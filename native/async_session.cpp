#include "async_session.h"
#include "api_json_internal.h"
#include "export_job.h"
#include "host_api.h"
#include "mcp_transport.h"
#include "registry.h"
#include "export_timing.h"
#ifdef MUSIC_ASYNC_TEST_HOOKS
#include <engrave/testing/write470.h>
#include <cstdio>
#include <cstdlib>
#endif

#include <asio/async_result.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <thread>
#include <utility>

namespace music {
namespace {
using namespace v2;

struct HandlerContext {
    HostApi* host = nullptr;
    ExportJobs* jobs = nullptr;
    std::optional<ExportSnapshot> snapshot;
    std::optional<ExportJobs::Reservation> reservation;
    std::chrono::steady_clock::time_point submitted;
    std::shared_ptr<std::atomic<bool>> request_cancelled;
#ifdef MUSIC_ASYNC_TEST_HOOKS
    std::atomic<std::uint64_t>* capture_count = nullptr;
    std::atomic<bool>* capture_polling = nullptr;
    std::atomic<bool>* hold_capture_completion = nullptr;
#endif
};

Json open(void* p, const Json& a) { return static_cast<HandlerContext*>(p)->host->open(a); }
Json create(void* p, const Json& a) { return static_cast<HandlerContext*>(p)->host->create_document(a); }
Json close(void* p, const Json& a) { return static_cast<HandlerContext*>(p)->host->close(a); }
Json outline(void* p, const Json& a) { return static_cast<HandlerContext*>(p)->host->outline(a); }
Json query(void* p, const Json& a) { return static_cast<HandlerContext*>(p)->host->query(a); }
Json describe(void* p, const Json& a) { return static_cast<HandlerContext*>(p)->host->describe(a); }
Json edit(void* p, const Json& a) { return static_cast<HandlerContext*>(p)->host->edit(a); }
Json runtime_info(void* p, const Json& a) { return static_cast<HandlerContext*>(p)->host->runtime_info(a); }

// Both the runtime lease and the document bytes are captured by one owner task.
Json capture(void* p, const Json& a) {
    auto& context = *static_cast<HandlerContext*>(p);
    auto request = decode_export(a);
    if (!request) return error_envelope(request.error());
#ifdef MUSIC_ASYNC_TEST_HOOKS
    if (request->destination.path.string().find(".test-capture-late") != std::string::npos)
        context.hold_capture_completion->store(true, std::memory_order_release);
#endif
    auto submitted = context.submitted;
#ifdef MUSIC_ASYNC_TEST_HOOKS
    if (request->destination.path.string().find(".test-capture-deadline") != std::string::npos)
        submitted -= std::chrono::milliseconds(119500);
    if (request->destination.path.string().find(".test-470-deadline") != std::string::npos)
        submitted -= std::chrono::milliseconds(119000);
#endif
    auto reservation = context.jobs->reserve(*request, submitted);
    if (!reservation) return error_envelope(reservation.error());
    if (auto stopped = reservation->stop_reason()) return error_envelope(*stopped);
    auto runtime = context.host->capture_runtime();
    if (!runtime) return error_envelope(runtime.error());
    auto result = context.host->with_document(request->document, request->expected_revision,
        [&](engrave::Document& document) -> Result<void> {
#ifdef MUSIC_ASYNC_TEST_HOOKS
            ++*context.capture_count;
#endif
            engrave::CaptureOptions options;
            options.max_bytes = reservation->budget();
#ifdef MUSIC_ASYNC_TEST_HOOKS
            int polls = 0;
            const auto test_path = request->destination.path.string();
            const bool slow_capture = test_path.find(".test-capture-slow") != std::string::npos ||
                                      test_path.find(".test-capture-deadline") != std::string::npos;
            const bool conversion_probe = test_path.find(".test-470-") != std::string::npos;
            // Test binary only: stop at a real engine conversion poll, after
            // progress has advanced. Production contains neither probe nor pause.
            const bool converting_probe = test_path.find(".test-470-converting-") != std::string::npos &&
                std::getenv("ENGRAVE_MCP_TEST_DISABLE_470_PROBES") == nullptr;
            bool conversion_entered = false;
            engrave::testing::Write470Progress conversion_progress;
#endif
            options.cancelled = [&] {
#ifdef MUSIC_ASYNC_TEST_HOOKS
                const auto progress = engrave::testing::write470_progress();
                if ((converting_probe || (conversion_probe && test_path.find(".test-470-converting-") == std::string::npos)) && !conversion_entered &&
                    progress.stage == (converting_probe ? engrave::testing::Write470Progress::Stage::Converting :
                        engrave::testing::Write470Progress::Stage::Serializing) && progress.units >= 2) {
                    conversion_entered = true;
                    conversion_progress = progress;
                    std::fprintf(stderr, "WRITE470_%s units=%llu\n", converting_probe ? "CONVERTING" : "SERIALIZING",
                        static_cast<unsigned long long>(progress.units));
                    std::fflush(stderr);
                    // Expire only here: queued work cannot satisfy this probe.
                    if (converting_probe && test_path.find(".test-470-converting-deadline") != std::string::npos)
                        reservation->test_expire_capture();
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    while (!context.request_cancelled->load(std::memory_order_acquire) &&
                           !reservation->stop_reason() && std::chrono::steady_clock::now() < until)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (slow_capture && ++polls == 2) {
                    context.capture_polling->store(true, std::memory_order_release);
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    while (!context.request_cancelled->load(std::memory_order_acquire) &&
                           !reservation->stop_reason() && std::chrono::steady_clock::now() < until)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    context.capture_polling->store(false, std::memory_order_release);
                }
#endif
                return (context.request_cancelled && context.request_cancelled->load(std::memory_order_acquire)) ||
                       reservation->stop_reason().has_value();
            };
            auto snapshot = capture_export(document, std::move(*runtime), *request, options);
#ifdef MUSIC_ASYNC_TEST_HOOKS
            if (conversion_entered) {
                std::fprintf(stderr, "WRITE470_RETURN units=%llu error=%s\n",
                    static_cast<unsigned long long>(conversion_progress.units), snapshot ? "SUCCESS" : snapshot.error().code.c_str());
                std::fflush(stderr);
            }
#endif
            if (!snapshot) {
                if (snapshot.error().code == "Cancelled") {
                    if (context.request_cancelled && context.request_cancelled->load(std::memory_order_acquire))
                        return std::unexpected(HostError{"Cancelled", "export capture cancelled"});
                    if (auto stopped = reservation->stop_reason()) return std::unexpected(*stopped);
                }
                return std::unexpected(snapshot.error());
            }
            context.snapshot = std::move(*snapshot);
            return {};
        });
    if (!result) return error_envelope(result.error());
    context.reservation = std::move(*reservation);
    return {{"ok", true}}; // Internal acknowledgement; never exposed on the wire.
}

Json job(void* p, const Json& a) {
    auto& context = *static_cast<HandlerContext*>(p);
    try {
        detail::validate_tool("runtime_job", a);
        const auto id = a.at("job").get<std::string>();
        auto result = a.at("action") == "get" ? context.jobs->get(id) : context.jobs->cancel(id);
        if (!result) return error_envelope(result.error());
        return {{"ok", true}, {"status", encode(*result)}};
    } catch (const detail::DecodeError& e) {
        return error_envelope(e.error);
    } catch (const std::exception& e) {
        return error_envelope({"EngineFailure", e.what()});
    }
}

const HandlerTable handlers{open, create, close, outline, query, describe, edit, capture, runtime_info, job};

Json run_result(const JobStatus& status) {
    if (status.state != JobState::Succeeded)
        return error_envelope(status.error.value_or(HostError{"WorkerFailed", "job did not succeed"}));
    Json artifacts = Json::array();
    for (const auto& artifact : status.artifacts) artifacts.push_back(encode(artifact));
    return {{"ok", true}, {"action", "run"}, {"source_revision", std::to_string(status.source_revision)},
            {"artifacts", std::move(artifacts)}, {"export_warnings", encode(status.export_warnings)}};
}
}

struct AsyncSession::Impl : std::enable_shared_from_this<Impl> {
    struct OwnerResult {
        Json payload;
        std::optional<ExportSnapshot> snapshot;
        std::optional<ExportJobs::Reservation> reservation;
    };

    bool live_export = false;
    JobLimits limits;
    ExportJobs jobs{limits};
    asio::io_context owner_io{1}, jobs_io{1};
    asio::executor_work_guard<asio::io_context::executor_type> owner_work{asio::make_work_guard(owner_io)};
    asio::executor_work_guard<asio::io_context::executor_type> jobs_work{asio::make_work_guard(jobs_io)};
    std::unique_ptr<HostApi> host;
    std::jthread owner_thread, jobs_thread;
    std::thread::id session_thread = std::this_thread::get_id();
    std::size_t owner_pending = 0;
    bool closing = false;
    std::atomic<bool> stopping{false};
#ifdef MUSIC_ASYNC_TEST_HOOKS
    std::atomic<bool> test_owner_busy{false};
    std::atomic<std::uint64_t> test_capture_count{0};
    std::atomic<bool> test_capture_polling{false};
    std::atomic<bool> test_hold_completion{false}, test_capture_completion_pending{false};
#endif

    explicit Impl(engrave::EngineOptions options, bool live) : live_export(live) {
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
        try { jobs_thread = std::jthread([this] { jobs_io.run(); }); }
        catch (...) {
            owner_work.reset();
            if (owner_thread.joinable()) owner_thread.join();
            throw;
        }
    }

    ~Impl() {
        shutdown();
        if (owner_thread.joinable()) owner_thread.join();
        if (jobs_thread.joinable()) jobs_thread.join();
    }

    void shutdown() noexcept {
        if (closing) return;
        closing = true;
        stopping.store(true, std::memory_order_release);
        owner_work.reset();
        // Shutdown may reap workers. Run it away from the session and owner
        // threads; callbacks post their terminal results to the session.
        try {
            asio::post(jobs_io, [this] { jobs.shutdown(); jobs_work.reset(); });
        } catch (...) {
            jobs_work.reset();
        }
    }

    template<class T, class Function>
    asio::awaitable<T> dispatch(asio::io_context& target, Function function, bool owner_task,
                                std::shared_ptr<std::atomic<bool>> running_cancel = {}, bool preserve_completed = false) {
        if (closing) throw asio::system_error(asio::error::operation_aborted);
        if (owner_task && owner_pending >= 64)
            throw ToolCallError("ResourceLimit", "Engine queue full (limit 64)");
        auto self = shared_from_this();
        auto token = asio::use_awaitable;
        co_return co_await asio::async_initiate<decltype(token), void(std::exception_ptr, T)>(
            [self, &target, function = std::move(function), owner_task, running_cancel, preserve_completed](auto handler) mutable {
                auto executor = asio::get_associated_executor(handler);
                auto work = asio::make_work_guard(executor);
                auto slot = asio::get_associated_cancellation_slot(handler);
                auto state = std::make_shared<std::atomic<int>>(0); // queued, running, cancelled
                if (slot.is_connected()) slot.assign([state, running_cancel](asio::cancellation_type type) {
                    if (type != asio::cancellation_type::none) {
                        if (running_cancel) running_cancel->store(true, std::memory_order_release);
                        int queued = 0;
                        state->compare_exchange_strong(queued, 2);
                    }
                });
                if (owner_task) ++self->owner_pending;
                try {
                    asio::post(target, [self, function = std::move(function), handler = std::move(handler),
                                        executor, work = std::move(work), slot, state, owner_task, running_cancel, preserve_completed]() mutable {
                        std::exception_ptr error;
                        T result{};
                        try {
                            int queued = 0;
                            if (!state->compare_exchange_strong(queued, 1) || self->stopping.load(std::memory_order_acquire))
                                throw asio::system_error(asio::error::operation_aborted);
                            result = function();
                            if (!preserve_completed && running_cancel && running_cancel->load(std::memory_order_acquire))
                                throw asio::system_error(asio::error::operation_aborted);
                        } catch (...) { error = std::current_exception(); }
#ifdef MUSIC_ASYNC_TEST_HOOKS
                        const bool hold_completion = self->test_hold_completion.exchange(false);
                        if (hold_completion && !error && running_cancel) {
                            self->test_capture_completion_pending.store(true, std::memory_order_release);
                            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                            while (!running_cancel->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until)
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            self->test_capture_completion_pending.store(false, std::memory_order_release);
                        }
#endif
                        asio::post(executor, [self, handler = std::move(handler), work = std::move(work), slot,
                                              owner_task, error, running_cancel, preserve_completed, result = std::move(result)]() mutable {
                            // Cancellation can arrive after the owner finished
                            // but before this completion runs on the session.
                            if (!preserve_completed && !error && running_cancel && running_cancel->load(std::memory_order_acquire))
                                error = std::make_exception_ptr(asio::system_error(asio::error::operation_aborted));
                            if (slot.is_connected()) slot.clear();
                            if (owner_task) --self->owner_pending;
                            handler(error, std::move(result));
                        });
                    });
                } catch (...) {
                    if (owner_task) --self->owner_pending;
                    if (slot.is_connected()) slot.clear();
                    throw;
                }
            }, token);
    }

    asio::awaitable<Json> submit(ExportSnapshot snapshot, ExportJobs::Reservation reservation) {
        if (closing) throw asio::system_error(asio::error::operation_aborted);
        auto self = shared_from_this();
        auto token = asio::use_awaitable;
        co_return co_await asio::async_initiate<decltype(token), void(std::exception_ptr, Json)>(
            [self, snapshot = std::move(snapshot), reservation = std::move(reservation)](auto handler) mutable {
                struct State {
                    std::atomic<bool> cancelled{false}, completed{false};
                    std::string job; // Read and written only on jobs_io.
                };
                auto state = std::make_shared<State>();
                auto executor = asio::get_associated_executor(handler);
                auto work = std::make_shared<decltype(asio::make_work_guard(executor))>(asio::make_work_guard(executor));
                auto slot = asio::get_associated_cancellation_slot(handler);
                auto completion_handler = std::make_shared<decltype(handler)>(std::move(handler));
                auto complete = [state, executor, work, slot, completion_handler](std::exception_ptr error, Json result) mutable {
                    if (state->completed.exchange(true)) return;
                    asio::post(executor, [work, slot, completion_handler, error, result = std::move(result)]() mutable {
                        if (slot.is_connected()) slot.clear();
                        (*completion_handler)(error, std::move(result));
                    });
                };
                const bool run = snapshot.request.action == ExportAction::Run;
                if (slot.is_connected()) slot.assign([self, state, run](asio::cancellation_type type) {
                    if (type == asio::cancellation_type::none) return;
                    state->cancelled.store(true, std::memory_order_release);
                    if (run) asio::post(self->jobs_io, [self, state] {
                        if (!state->completed.load() && !state->job.empty()) (void)self->jobs.cancel(state->job);
                    });
                });
                asio::post(self->jobs_io, [self, state, complete, run, snapshot = std::move(snapshot),
                                           reservation = std::move(reservation)]() mutable {
                    if (state->cancelled.load(std::memory_order_acquire) || self->stopping.load(std::memory_order_acquire)) {
                        complete(std::make_exception_ptr(asio::system_error(asio::error::operation_aborted)), {});
                        return;
                    }
                    ExportJobs::Completion callback;
                    if (run) callback = [state, complete](Result<JobStatus> result) mutable {
                        if (!result) { complete({}, error_envelope(result.error())); return; }
                        if (result->state == JobState::Cancelled && state->cancelled.load(std::memory_order_acquire)) {
                            complete(std::make_exception_ptr(asio::system_error(asio::error::operation_aborted)), {});
                            return;
                        }
                        complete({}, run_result(*result));
                    };
                    auto submitted = self->jobs.submit(std::move(reservation), std::move(snapshot), std::move(callback));
                    if (!submitted) { complete({}, error_envelope(submitted.error())); return; }
                    state->job = submitted->job;
                    if (!run) {
                        complete({}, {{"ok", true}, {"action", "start"}, {"job", submitted->job},
                                      {"source_revision", std::to_string(submitted->source_revision)}});
                    } else if (state->cancelled.load(std::memory_order_acquire)) {
                        (void)self->jobs.cancel(state->job);
                    }
                });
            }, token);
    }
};

AsyncSession::AsyncSession(engrave::EngineOptions options, bool live)
    : impl_(std::make_shared<Impl>(std::move(options), live)) {}
AsyncSession::~AsyncSession() { shutdown(); }
void AsyncSession::shutdown() noexcept { if (impl_) impl_->shutdown(); }

asio::awaitable<Json> AsyncSession::call(std::string name, Json arguments) {
    auto self = impl_;
    if (std::this_thread::get_id() != self->session_thread) throw std::logic_error("AsyncSession called from non-session thread");
    if (self->closing) throw asio::system_error(asio::error::operation_aborted);
#ifdef MUSIC_ASYNC_TEST_HOOKS
    if (name == "test_capture_count") co_return Json{{"ok", true}, {"count", self->test_capture_count.load()}};
    if (name == "test_capture_state") co_return Json{{"ok", true}, {"polling", self->test_capture_polling.load()},
        {"completion_pending", self->test_capture_completion_pending.load()}};
    if (name == "test_owner_state") co_return Json{{"ok", true}, {"busy", self->test_owner_busy.load()}};
    if (name == "test_owner_sleep") {
        co_return co_await self->dispatch<Json>(self->owner_io, [self] {
            self->test_owner_busy.store(true);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            self->test_owner_busy.store(false);
            return Json{{"ok", true}, {"slept_ms", 2000}};
        }, true);
    }
#endif
    const auto* spec = find_tool(name);
    if (!spec) throw ToolCallError("InvalidArgument", "Unknown tool");
    const auto handler = handlers[static_cast<std::size_t>(spec->handler)];
    if (spec->scheduling == Scheduling::JobManager) {
        co_return co_await self->dispatch<Json>(self->jobs_io, [self, handler, arguments = std::move(arguments)] {
            HandlerContext context; context.jobs = &self->jobs;
            return handler(&context, arguments);
        }, false);
    }
    const auto submitted = std::chrono::steady_clock::now();
    auto capture_cancel = spec->scheduling == Scheduling::ExportJob ? std::make_shared<std::atomic<bool>>(false) : nullptr;
    if (self->live_export && spec->scheduling == Scheduling::ExportJob) {
        auto request = decode_export(arguments);
        if (!request) co_return error_envelope(request.error());
        if (request->action == ExportAction::Run && request->kind != ExportKind::Audio) {
            auto timing = std::make_shared<ExportTiming>("live", request->destination.path);
            auto payload = co_await self->dispatch<Json>(self->owner_io, [self, request = *request, capture_cancel, timing, submitted] {
                timing->add("owner_dispatch", submitted);
                Json payload;
                auto result = self->host->with_document(request.document, request.expected_revision,
                    [&](engrave::Document& document) -> Result<void> {
                        auto output = live_export(document, request, self->limits, [&] {
                            return capture_cancel->load(std::memory_order_acquire) || self->stopping.load(std::memory_order_acquire);
                        }, submitted, *timing);
                        if (!output) return std::unexpected(output.error());
                        payload = std::move(*output);
                        return {};
                    });
                if (!result) {
                    if (result.error().code == "Cancelled") throw asio::system_error(asio::error::operation_aborted);
                    return error_envelope(result.error());
                }
                return payload;
            }, true, capture_cancel, true);
            timing->finish();
            co_return payload;
        }
    }
    std::filesystem::path timing_path;
    if (capture_cancel) {
        auto request = decode_export(arguments);
        if (request) timing_path = request->destination.path;
    }
    auto isolated_timing = std::make_shared<ExportTiming>("isolated", timing_path);
    auto result = co_await self->dispatch<Impl::OwnerResult>(self->owner_io, [self, handler, submitted, capture_cancel, isolated_timing, arguments = std::move(arguments)] {
        HandlerContext context; context.host = self->host.get(); context.jobs = &self->jobs; context.submitted = submitted;
        context.request_cancelled = capture_cancel;
#ifdef MUSIC_ASYNC_TEST_HOOKS
        context.capture_count = &self->test_capture_count;
        context.capture_polling = &self->test_capture_polling;
        context.hold_capture_completion = &self->test_hold_completion;
#endif
        if (capture_cancel) isolated_timing->add("owner_dispatch", submitted);
        auto started = ExportTiming::Clock::now();
        auto payload = handler(&context, arguments);
        if (capture_cancel) isolated_timing->add("capture", started);
        return Impl::OwnerResult{std::move(payload), std::move(context.snapshot), std::move(context.reservation)};
    }, true, capture_cancel);
    if (spec->scheduling != Scheduling::ExportJob || !result.snapshot) co_return std::move(result.payload);
    if (!result.reservation) co_return error_envelope({"EngineFailure", "captured export has no reservation"});
    auto payload = co_await self->submit(std::move(*result.snapshot), std::move(*result.reservation));
    isolated_timing->add("job_wait", isolated_timing->last);
    isolated_timing->finish();
    co_return payload;
}
}
