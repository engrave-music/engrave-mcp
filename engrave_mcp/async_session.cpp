#include "async_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <optional>
#include <thread>
#include <utility>

#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include "api_output.h"
#include "export_timing.h"
#include "host_api.h"
#include "mcp_transport.h"
#include "publication_gate.h"
#include "registry.h"
#include "test_hooks.h"

namespace music {

namespace {
using namespace api;

struct HandlerContext {
    HostApi *host = nullptr;
    const std::atomic<bool> *cancelled = nullptr;
};

Json open(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->open(a); }

Json create(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->create_document(a); }

Json close(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->close(a); }

Json outline(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->outline(a); }

Json query(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->query(a); }

Json read_music(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->read_music(a); }

Json write_phrase(void *p, const Json &a) {
    auto &ctx = *static_cast<HandlerContext *>(p);
    return ctx.host->write_phrase(a, ctx.cancelled);
}

Json configure(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->configure(a); }

Json edit(void *p, const Json &a) {
    auto &ctx = *static_cast<HandlerContext *>(p);
    return ctx.host->edit(a, ctx.cancelled);
}

Json runtime_info(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->runtime_info(a); }

Json checkpoint(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->checkpoint(a); }

Json restore_checkpoint(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->restore_checkpoint(a); }

Json drop_checkpoint(void *p, const Json &a) { return static_cast<HandlerContext *>(p)->host->drop_checkpoint(a); }

Json export_request(void *, const Json &) { throw std::logic_error("export must route through owner"); }

const HandlerTable handlers{open,
                            create,
                            close,
                            outline,
                            query,
                            read_music,
                            edit,
                            export_request,
                            runtime_info,
                            checkpoint,
                            restore_checkpoint,
                            drop_checkpoint,
                            write_phrase,
                            configure};
} // namespace

struct AsyncSession::Impl : std::enable_shared_from_this<Impl> {
    ExportLimits limits;
    asio::io_context owner_io{1};
    asio::executor_work_guard<asio::io_context::executor_type> owner_work{asio::make_work_guard(owner_io)};
    std::unique_ptr<HostApi> host;
    std::jthread owner_thread;
    std::thread::id session_thread = std::this_thread::get_id();
    usize owner_pending = 0;
    bool closing = false;
    std::atomic<bool> stopping{false};
    std::atomic<bool> test_owner_busy{false};

    explicit Impl(engrave::EngineOptions options) {
        if (test_hooks_enabled()) {
            if (const auto *bytes = std::getenv("ENGRAVE_MCP_TEST_OUTPUT_BYTES")) limits.output_bytes = std::stoull(bytes);
        }
        validate_registry();
        validate_handlers(handlers);
        std::promise<void> ready;
        auto initialized = ready.get_future();
        owner_thread = std::jthread([this, options = std::move(options), ready = std::move(ready)]() mutable {
            try {
                auto created = HostApi::create(options, limits);
                if (!created) throw std::runtime_error(created.error().code + ": " + created.error().message);
                host = std::move(*created);
                ready.set_value();
            } catch (...) {
                ready.set_exception(std::current_exception());
                return;
            }
            owner_io.run();
            host.reset(); // Engine and documents always die on their owner thread.
        });
        initialized.get();
    }

    ~Impl() {
        shutdown();
        if (owner_thread.joinable()) owner_thread.join();
    }

    void shutdown() noexcept {
        if (closing) return;
        closing = true;
        stopping.store(true, std::memory_order_release);
        owner_work.reset();
    }

    template <class T, class Function>
    asio::awaitable<T> dispatch(asio::io_context &target, Function function, bool owner_task,
                                std::shared_ptr<std::atomic<bool>> running_cancel = {}, bool preserve_completed = false,
                                std::shared_ptr<RequestExecution> execution = {}) {
        if (closing) throw asio::system_error(asio::error::operation_aborted);
        if (publication_gate.closing.load(std::memory_order_acquire)) {
            if (test_hooks_enabled()) {
                std::fprintf(stderr, "ADMISSION_REFUSED_CLOSING\n");
                std::fflush(stderr);
            }
            throw asio::system_error(asio::error::operation_aborted);
        }
        if (owner_task && owner_pending >= 64) throw ToolCallError("ResourceLimit", "Engine queue full (limit 64)");
        auto self = shared_from_this();
        auto token = asio::use_awaitable;
        co_return co_await asio::async_initiate<decltype(token), void(std::exception_ptr, T)>(
            [self, &target, function = std::move(function), owner_task, running_cancel, preserve_completed,
             execution](auto handler) mutable {
                auto executor = asio::get_associated_executor(handler);
                auto work = asio::make_work_guard(executor);
                auto slot = asio::get_associated_cancellation_slot(handler);
                // Each dispatch owns its cancellation, execution state and completion.
                // The session thread releases queued closures immediately; a stale
                // owner-queue entry can only observe Cancelled and return.
                auto state = execution ? execution : std::make_shared<RequestExecution>();
                auto task = std::make_shared<std::optional<Function>>(std::move(function));
                auto completion_handler = std::make_shared<decltype(handler)>(std::move(handler));
                auto guard = std::make_shared<decltype(work)>(std::move(work));
                auto complete = [self, executor, guard, slot, completion_handler, owner_task, running_cancel,
                                 preserve_completed](std::exception_ptr error, T result) mutable {
                    asio::post(executor, [self, guard, slot, completion_handler, owner_task, error, running_cancel, preserve_completed,
                                          result = std::move(result)]() mutable {
                        if (!preserve_completed && !error && running_cancel && running_cancel->load(std::memory_order_acquire)) {
                            error = std::make_exception_ptr(asio::system_error(asio::error::operation_aborted));
                        }
                        if (slot.is_connected()) slot.clear();
                        if (owner_task) --self->owner_pending;
                        (*completion_handler)(error, std::move(result));
                    });
                };
                if (slot.is_connected()) {
                    slot.assign([state, task, complete, running_cancel](asio::cancellation_type type) mutable {
                        if (type == asio::cancellation_type::none) return;
                        if (running_cancel) running_cancel->store(true, std::memory_order_release);
                        auto queued = ExecutionState::QUEUED;
                        if (state->state.compare_exchange_strong(queued, ExecutionState::CANCELLED_QUEUED)) {
                            task->reset();
                            complete(std::make_exception_ptr(asio::system_error(asio::error::operation_aborted)), T{});
                        }
                    });
                }
                if (owner_task) ++self->owner_pending;
                try {
                    asio::post(target, [self, task, complete, state, running_cancel, preserve_completed]() mutable {
                        auto queued = ExecutionState::QUEUED;
                        {
                            std::lock_guard lock(publication_gate.mutex);
                            if (publication_gate.closing) {
                                if (test_hooks_enabled()) {
                                    std::fprintf(stderr, "OWNER_DROPPED_CLOSING\n");
                                    std::fflush(stderr);
                                }
                                if (!state->state.compare_exchange_strong(queued, ExecutionState::CANCELLED_QUEUED)) return;
                                task->reset();
                                complete(std::make_exception_ptr(asio::system_error(asio::error::operation_aborted)), T{});
                                return;
                            }
                            if (!state->state.compare_exchange_strong(queued, ExecutionState::EXECUTING)) return;
                            ++publication_gate.owner_tasks;
                        }
                        std::exception_ptr error;
                        T result{};
                        try {
                            if (self->stopping.load(std::memory_order_acquire)) throw asio::system_error(asio::error::operation_aborted);
                            result = (**task)();
                            if (!preserve_completed && running_cancel && running_cancel->load(std::memory_order_acquire)) {
                                throw asio::system_error(asio::error::operation_aborted);
                            }
                        } catch (...) {
                            error = std::current_exception();
                        }
                        task->reset();

                        {
                            std::lock_guard lock(publication_gate.mutex);
                            --publication_gate.owner_tasks;
                            state->state.store(ExecutionState::FINISHED, std::memory_order_release);
                        }
                        publication_gate.changed.notify_all();
                        complete(error, std::move(result));
                    });
                    if (test_hooks_enabled() && owner_task) {
                        std::fprintf(stderr, "OWNER_ENQUEUED\n");
                        std::fflush(stderr);
                    }
                } catch (...) {
                    if (owner_task) --self->owner_pending;
                    if (slot.is_connected()) slot.clear();
                    throw;
                }
            },
            token);
    }
};

AsyncSession::AsyncSession(engrave::EngineOptions options) : impl_(std::make_shared<Impl>(std::move(options))) {}

AsyncSession::~AsyncSession() { shutdown(); }

void AsyncSession::shutdown() noexcept {
    if (impl_) impl_->shutdown();
}

asio::awaitable<Json> AsyncSession::call(std::string name, Json arguments, std::shared_ptr<RequestExecution> execution) {
    auto self = impl_;
    if (std::this_thread::get_id() != self->session_thread) throw std::logic_error("AsyncSession called from non-session thread");
    if (self->closing) throw asio::system_error(asio::error::operation_aborted);
    if (test_hooks_enabled()) {
        if (name == "test_owner_state") co_return Json{{"ok", true}, {"busy", self->test_owner_busy.load()}};
        if (name == "test_owner_sleep") {
            std::string barrier;
            if (arguments.is_object() && arguments.contains("barrier") && arguments.at("barrier").is_string()) {
                barrier = arguments.at("barrier").get_string();
            }
            std::int64_t ms = 2000;
            if (arguments.is_object() && arguments.contains("ms") && arguments.at("ms").is_number()) {
                ms = static_cast<std::int64_t>(std::clamp(arguments.at("ms").as<double>(), 1.0, 60000.0));
            }
            co_return co_await self->dispatch<Json>(
                self->owner_io,
                [self, ms, barrier = std::move(barrier)] {
                    struct ClearBusy {
                        std::atomic<bool> &busy;
                        ~ClearBusy() { busy.store(false); }
                    };
                    self->test_owner_busy.store(true);
                    const ClearBusy clear_busy{self->test_owner_busy};
                    if (!barrier.empty()) {
                        const auto barrier_path = std::filesystem::u8path(barrier);
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                        while (!std::filesystem::exists(barrier_path)) {
                            if (self->stopping.load(std::memory_order_acquire)) {
                                throw asio::system_error(asio::error::operation_aborted);
                            }
                            if (std::chrono::steady_clock::now() >= deadline) {
                                throw std::runtime_error("test owner barrier timed out");
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                        return Json{{"ok", true}, {"barrier", barrier}};
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                    return Json{{"ok", true}, {"slept_ms", ms}};
                },
                true);
        }
    }
    const auto *spec = find_tool(name);
    if (!spec) throw ToolCallError("InvalidArgument", "Unknown tool");
    const auto handler = handlers[static_cast<std::size_t>(spec->handler)];
    const auto submitted = std::chrono::steady_clock::now();
    auto request_cancel =
        (spec->handler == HandlerId::EXPORT || spec->handler == HandlerId::EDIT || spec->handler == HandlerId::WRITE_PHRASE) ?
            std::make_shared<std::atomic<bool>>(false) :
            nullptr;
    if (spec->handler == HandlerId::EXPORT) {
        auto request = decode_export(arguments);
        if (!request) co_return error_envelope(request.error());
        auto timing = std::make_shared<ExportTiming>("live", request->destination.path);
        auto payload = co_await self->dispatch<Json>(
            self->owner_io,
            [self, request = *request, request_cancel, timing, submitted, execution] {
                timing->add("owner_dispatch", submitted);
                Json payload;
                std::vector<std::string> visible_paths;
                std::unique_lock publication(publication_gate.mutex, std::defer_lock);
                auto runtime = self->host->runtime_options();
                if (!runtime) return error_envelope(runtime.error());
                try {
                    auto result = self->host->with_document(
                        request.document, request.expected_revision,
                        [&](engrave::Document &document) -> Result<void> {
                            auto output = live_export(
                                document, request, self->limits,
                                [&] {
                                    return request_cancel->load(std::memory_order_acquire) ||
                                           self->stopping.load(std::memory_order_acquire);
                                },
                                submitted, *timing, *runtime,
                                [execution, &publication] {
                                    if (!execution) return true;
                                    if (test_hooks_enabled()) {
                                        if (!publication.try_lock()) {
                                            std::fprintf(stderr, "PUBLICATION_LOCK_WAITING\n");
                                            std::fflush(stderr);
                                            publication.lock();
                                        }
                                    } else {
                                        publication.lock();
                                    }
                                    if (publication_gate.closing) {
                                        publication.unlock();
                                        return false;
                                    }
                                    {
                                        std::lock_guard lock(execution->response_mutex);
                                        if (execution->cancel_before_publication) {
                                            publication.unlock();
                                            return false;
                                        }
                                        auto running = ExecutionState::EXECUTING;
                                        if (!execution->state.compare_exchange_strong(running, ExecutionState::PUBLISHING,
                                                                                      std::memory_order_acq_rel)) {
                                            publication.unlock();
                                            return false;
                                        }
                                    }
                                    return true;
                                },
                                &visible_paths);
                            if (test_hooks_enabled() && !visible_paths.empty() && std::getenv("ENGRAVE_MCP_TEST_THROW_DOCUMENT_CALLBACK")) {
                                throw 42; // exercise the non-standard exception boundary too
                            }
                            if (!output) return std::unexpected(output.error());
                            payload = std::move(*output);
                            return {};
                        },
                        &visible_paths);
                    if (!result) {
                        if (result.error().code == "Cancelled") throw asio::system_error(asio::error::operation_aborted);
                        payload = error_envelope(result.error());
                    }
                    if (publication.owns_lock() && execution && execution->report.has_value()) {
                        (*execution->report)(std::move(payload));
                        execution->mark_report_sent();
                        return Json{};
                    }
                    return payload;
                } catch (...) {
                    if (publication.owns_lock() && execution && execution->state.load() == ExecutionState::PUBLISHING &&
                        execution->report.has_value()) {
                        (*execution->report)(Json{}); // terminal: trustworthy report unavailable
                    }
                    throw;
                }
            },
            true, request_cancel, true, execution);
        if (test_hooks_enabled() && execution && execution->was_reported() && std::getenv("ENGRAVE_MCP_TEST_THROW_AFTER_REPORT")) {
            throw std::runtime_error("Injected post-report completion failure");
        }
        timing->finish();
        co_return payload;
    }
    co_return co_await self->dispatch<Json>(
        self->owner_io,
        [self, handler, request_cancel, arguments = std::move(arguments)] {
            HandlerContext context;
            context.host = self->host.get();
            context.cancelled = request_cancel.get();
            auto result = handler(&context, arguments);
            if (request_cancel && result.contains("error") && result.at("error").at("code") == "Cancelled") {
                throw asio::system_error(asio::error::operation_aborted);
            }
            return result;
        },
        true, request_cancel, true);
}
} // namespace music
