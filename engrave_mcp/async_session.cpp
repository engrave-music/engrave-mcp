#include "async_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
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

Json checkpoint(void *p, const Json &a) {
    auto &ctx = *static_cast<HandlerContext *>(p);
    return ctx.host->checkpoint(a, ctx.cancelled);
}

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

std::exception_ptr aborted() { return std::make_exception_ptr(asio::system_error(asio::error::operation_aborted)); }

struct FatalReportFailure {
    const char *reason = nullptr;
};

// One-shot test hook: if the env var names an existing file, remove it and return true.
bool consume_test_trigger(const char *env_name) {
    if (!test_hooks_enabled()) return false;
    const auto *value = std::getenv(env_name);
    if (!value || !*value) return false;
    std::error_code ec;
    const auto path = std::filesystem::u8path(value);
    if (!std::filesystem::exists(path, ec)) return false;
    std::filesystem::remove(path, ec);
    return true;
}

[[noreturn]] void fatal_audio_post_failure(RequestExecution *execution, const char *reason) noexcept {
    if (execution && execution->abort_transport) execution->abort_transport(reason);
    std::fprintf(stderr, "mcp transport terminal: %s; exit 74\n", reason);
    std::fflush(stderr);
    std::_Exit(74);
}

// Commits an export to publication: excludes the watchdog's exit 70,
// cancellation, and a deadline expiring while waiting for the gate. On
// COMMITTED, `publication` owns an active_publications token until the report
// finishes, with no mutex held across disk publication.
PublicationAdmission begin_publication(RequestExecution *execution, PublicationScope &publication,
                                       std::chrono::steady_clock::time_point deadline, FnRef<bool()> cancelled,
                                       FnRef<bool()> deadline_expired) {
    if (!execution) {
        if (cancelled && cancelled()) return PublicationAdmission::CANCELLED;
        if (deadline_expired && deadline_expired()) return PublicationAdmission::DEADLINE_EXCEEDED;
        return PublicationAdmission::COMMITTED;
    }
    auto check_rejected = [&]() -> std::optional<PublicationAdmission> {
        if (publication_gate.closing.load(std::memory_order_acquire)) return PublicationAdmission::CANCELLED;
        std::lock_guard response_lock(execution->response_mutex);
        if (execution->cancel_before_publication || (cancelled && cancelled())) return PublicationAdmission::CANCELLED;
        if (deadline_expired && deadline_expired()) return PublicationAdmission::DEADLINE_EXCEEDED;
        if (execution->state.load(std::memory_order_acquire) != ExecutionState::EXECUTING) return PublicationAdmission::CANCELLED;
        return std::nullopt;
    };

    std::unique_lock lock(publication_gate.state_mutex);
    if (auto rejected = check_rejected()) return *rejected;

    bool logged_waiting = false;
    while (publication_gate.active_publications > 0 || publication_gate.active_stdout_writes > 0) {
        if (test_hooks_enabled() && !logged_waiting) {
            logged_waiting = true;
            std::fprintf(stderr, "PUBLICATION_LOCK_WAITING\n");
            std::fflush(stderr);
        }
        const auto wake = [&] {
            return publication_gate.closing.load(std::memory_order_acquire) ||
                   (publication_gate.active_publications == 0 && publication_gate.active_stdout_writes == 0) ||
                   check_rejected().has_value();
        };
        if (publication_gate.state_cv.wait_until(lock, deadline, wake)) {
            if (auto rejected = check_rejected()) return *rejected;
        } else {
            if (auto rejected = check_rejected()) return *rejected;
            return PublicationAdmission::DEADLINE_EXCEEDED;
        }
    }

    if (publication_gate.closing.load(std::memory_order_acquire)) return PublicationAdmission::CANCELLED;
    {
        std::lock_guard response_lock(execution->response_mutex);
        if (execution->cancel_before_publication || (cancelled && cancelled())) return PublicationAdmission::CANCELLED;
        if (deadline_expired && deadline_expired()) return PublicationAdmission::DEADLINE_EXCEEDED;
        auto running = ExecutionState::EXECUTING;
        if (!execution->state.compare_exchange_strong(running, ExecutionState::PUBLISHING, std::memory_order_acq_rel)) {
            return PublicationAdmission::CANCELLED;
        }
    }
    ++publication_gate.active_publications;
    publication.adopt();
    return PublicationAdmission::COMMITTED;
}
} // namespace

struct AsyncSession::Impl : std::enable_shared_from_this<Impl> {
    // One audio export from admission to completion. The session thread owns
    // its queue position; the owner thread, then the audio lane, run it.
    struct AudioRun {
        ExportRequest request;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::chrono::steady_clock::time_point submitted;
        std::shared_ptr<ExportTiming> timing;
        std::shared_ptr<RequestExecution> execution;
        std::shared_ptr<RequestExecution> state; // execution, or a private state without one
        std::function<void(std::exception_ptr, Json, bool)> complete;
        std::function<void()> release_lane;
        // Session thread only: prevents double release when complete() falls back after a release_lane() post failure.
        bool lane_released = false;
        std::optional<PreparedAudioExport> prepared;
    };

    ExportLimits limits;
    asio::io_context owner_io{1};
    asio::executor_work_guard<asio::io_context::executor_type> owner_work{asio::make_work_guard(owner_io)};
    // The audio lane runs prepared audio renders one at a time. Only the owner
    // thread posts to it, starts it and joins it, before destroying the Engine.
    asio::io_context audio_io{1};
    asio::executor_work_guard<asio::io_context::executor_type> audio_work{asio::make_work_guard(audio_io)};
    std::unique_ptr<HostApi> host;
    std::jthread audio_thread;
    std::jthread owner_thread;
    std::thread::id session_thread = std::this_thread::get_id();
    usize owner_pending = 0;
    bool closing = false;
    // Session thread: set from an audio export's dispatch to the owner until
    // its PreparedAudio is destroyed. Queued audio exports never wait on owner_io.
    bool audio_busy = false;
    std::deque<std::shared_ptr<AudioRun>> audio_queue;
    std::atomic<bool> stopping{false};
    std::atomic<bool> test_owner_busy{false};

    explicit Impl(engrave::EngineOptions options) {
        if (test_hooks_enabled()) {
            if (const auto *bytes = std::getenv("ENGRAVE_MCP_TEST_OUTPUT_BYTES")) limits.output_bytes = std::stoull(bytes);
            if (const auto *deadline_ms = std::getenv("ENGRAVE_MCP_TEST_DEADLINE_MS")) {
                limits.deadline = std::chrono::milliseconds(std::stoll(deadline_ms));
            }
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
                audio_thread = std::jthread([this] { audio_io.run(); });
                ready.set_value();
            } catch (...) {
                audio_work.reset();
                if (audio_thread.joinable()) audio_thread.join();
                host.reset();
                ready.set_exception(std::current_exception());
                return;
            }
            owner_io.run();
            // Only owner handlers post to the lane, so it now drains and stops:
            // every PreparedAudio is destroyed before the Engine.
            audio_work.reset();
            audio_thread.join();
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
        publication_gate.notify_state_change();
        auto queued = std::move(audio_queue);
        audio_queue.clear();
        for (const auto &run : queued) abort_queued(*run);
        owner_work.reset();
    }

    bool audio_cancelled(const AudioRun &run) const noexcept {
        return run.cancel->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire) ||
               publication_gate.closing.load(std::memory_order_acquire);
    }

    // Session thread.
    static void abort_queued(AudioRun &run) noexcept {
        auto queued = ExecutionState::QUEUED;
        if (!run.state->state.compare_exchange_strong(queued, ExecutionState::CANCELLED_QUEUED)) return;
        try {
            run.complete(aborted(), Json{}, false);
        } catch (...) {
            fatal_audio_post_failure(run.execution.get(), "audio queued abort completion post failed");
        }
    }

    // Session thread: hands the next queued audio export to the owner.
    void start_audio() noexcept {
        while (!audio_busy && !audio_queue.empty()) {
            auto run = std::move(audio_queue.front());
            audio_queue.pop_front();
            if (closing || publication_gate.closing.load(std::memory_order_acquire)) {
                abort_queued(*run);
                continue;
            }
            try {
                asio::post(owner_io, [this, run] { prepare_audio(run); });
            } catch (...) {
                auto queued = ExecutionState::QUEUED;
                if (run->state->state.compare_exchange_strong(queued, ExecutionState::CANCELLED_QUEUED)) {
                    try {
                        run->complete(std::current_exception(), Json{}, false);
                    } catch (...) {
                        fatal_audio_post_failure(run->execution.get(), "audio dispatch failure completion post failed");
                    }
                }
                continue;
            }
            audio_busy = true;
            if (test_hooks_enabled()) {
                std::fprintf(stderr, "OWNER_ENQUEUED\n");
                std::fflush(stderr);
            }
        }
    }

    // Session thread.
    void release_audio_lane() noexcept {
        audio_busy = false;
        start_audio();
    }

    // Ends an audio export's active task on the thread that ran it last. Any
    // PreparedAudio is already destroyed, so the lane may be reused.
    static void finish_audio_task(AudioRun &run, std::exception_ptr error, Json payload) noexcept {
        {
            std::lock_guard lock(publication_gate.state_mutex);
            --publication_gate.active_tasks;
            run.state->state.store(ExecutionState::FINISHED, std::memory_order_release);
        }
        publication_gate.state_cv.notify_all();
        bool release_posted = false;
        try {
            run.release_lane();
            release_posted = true;
        } catch (...) {
            // Fall back to releasing the lane inside complete() on the session thread.
        }
        try {
            run.complete(error, std::move(payload), !release_posted);
        } catch (...) {
            fatal_audio_post_failure(run.execution.get(), "audio lane completion post failed");
        }
    }

    // Owner thread: setup, the only part of an audio export that reads the Document.
    void prepare_audio(const std::shared_ptr<AudioRun> &run) noexcept {
        auto queued = ExecutionState::QUEUED;
        enum class PrepareGate { PROCEED, DROPPED_CLOSING, ALREADY_CANCELLED };
        PrepareGate gate = PrepareGate::PROCEED;
        bool need_complete = false;
        {
            std::lock_guard lock(publication_gate.state_mutex);
            if (publication_gate.closing.load(std::memory_order_acquire)) {
                gate = PrepareGate::DROPPED_CLOSING;
                need_complete = run->state->state.compare_exchange_strong(queued, ExecutionState::CANCELLED_QUEUED);
            } else if (!run->state->state.compare_exchange_strong(queued, ExecutionState::EXECUTING)) {
                gate = PrepareGate::ALREADY_CANCELLED;
            } else {
                ++publication_gate.active_tasks;
            }
        }
        if (gate == PrepareGate::DROPPED_CLOSING) {
            if (test_hooks_enabled()) {
                std::fprintf(stderr, "OWNER_DROPPED_CLOSING\n");
                std::fflush(stderr);
            }
            bool release_posted = false;
            try {
                run->release_lane();
                release_posted = true;
            } catch (...) {}
            if (need_complete) {
                try {
                    run->complete(aborted(), Json{}, !release_posted);
                } catch (...) {}
            }
            return;
        }
        if (gate == PrepareGate::ALREADY_CANCELLED) {
            // Cancelled while queued on owner_io; the session already completed it.
            try {
                run->release_lane();
            } catch (...) {
                fatal_audio_post_failure(run->execution.get(), "audio lane release post failed");
            }
            return;
        }
        std::exception_ptr error;
        Json payload{};
        try {
            run->timing->add("owner_dispatch", run->submitted);
            if (stopping.load(std::memory_order_acquire)) throw asio::system_error(asio::error::operation_aborted);
            auto runtime = host->runtime_options();
            if (!runtime) {
                payload = error_envelope(runtime.error());
            } else {
                auto cancelled = [&] { return audio_cancelled(*run); };
                auto result = host->with_document(
                    run->request.document, run->request.expected_revision, [&](engrave::Document &document) -> Result<void> {
                        auto prepared =
                            prepare_audio_export(document, run->request, limits, cancelled, run->submitted, *run->timing, *runtime);
                        if (!prepared) return std::unexpected(std::move(prepared.error()));
                        run->prepared.emplace(std::move(*prepared));
                        return {};
                    });
                if (!result) {
                    if (result.error().code == "Cancelled") throw asio::system_error(asio::error::operation_aborted);
                    payload = error_envelope(result.error());
                }
            }
        } catch (...) {
            error = std::current_exception();
        }
        if (run->prepared) {
            try {
                // The active task carries over: active_tasks and EXECUTING stay
                // set until the lane finishes, for exit 74 and the watchdog.
                asio::post(audio_io, [this, run] { finish_audio(run); });
                return;
            } catch (...) {
                run->prepared.reset();
                error = std::current_exception();
            }
        }
        finish_audio_task(*run, error, std::move(payload));
    }

    // Audio lane: renders into staging, then publishes and reports.
    void finish_audio(const std::shared_ptr<AudioRun> &run) noexcept {
        std::exception_ptr error;
        Json payload{};
        const char *fatal_report_failure = nullptr;
        std::vector<std::string> visible_paths;
        PublicationScope publication;
        auto *execution = run->execution.get();
        const auto deadline = run->submitted + limits.deadline;
        try {
            auto prepared = std::move(*run->prepared);
            run->prepared.reset();
            auto cancelled = [&] { return audio_cancelled(*run); };
            auto output = finish_audio_export(
                std::move(prepared), cancelled, *run->timing,
                [&](FnRef<bool()> deadline_expired) {
                    return begin_publication(execution, publication, deadline, cancelled, deadline_expired);
                },
                &visible_paths);
            if (!output) {
                if (output.error().code == "Cancelled") throw asio::system_error(asio::error::operation_aborted);
                if (output.error().published_artifacts.empty()) output.error().published_artifacts = std::move(visible_paths);
                payload = error_envelope(output.error());
            } else {
                payload = std::move(*output);
            }
            if (publication.active() && execution && execution->report) {
                fatal_report_failure = execution->report(std::move(payload));
                if (!fatal_report_failure) {
                    execution->mark_report_sent();
                    payload = Json{};
                }
            }
        } catch (...) {
            error = std::current_exception();
        }
        if (error && publication.active() && execution && execution->state.load() == ExecutionState::PUBLISHING && execution->report) {
            fatal_report_failure = execution->report(Json{}); // terminal: trustworthy report unavailable
        }
        run->prepared.reset();
        publication.release();
        if (fatal_report_failure) {
            {
                std::lock_guard lock(publication_gate.state_mutex);
                --publication_gate.active_tasks;
                run->state->state.store(ExecutionState::FINISHED, std::memory_order_release);
            }
            publication_gate.state_cv.notify_all();
            if (execution && execution->abort_transport) execution->abort_transport(fatal_report_failure);
            std::_Exit(74);
        }
        finish_audio_task(*run, error, std::move(payload));
    }

    // Admission matches dispatch(). While the lane is busy the export waits in
    // audio_queue, so cancelling it never touches the owner or the lane.
    asio::awaitable<Json> audio_export(ExportRequest request, std::shared_ptr<std::atomic<bool>> request_cancel,
                                       std::chrono::steady_clock::time_point submitted, std::shared_ptr<ExportTiming> timing,
                                       std::shared_ptr<RequestExecution> execution) {
        if (closing) throw asio::system_error(asio::error::operation_aborted);
        if (publication_gate.closing.load(std::memory_order_acquire)) {
            if (test_hooks_enabled()) {
                std::fprintf(stderr, "ADMISSION_REFUSED_CLOSING\n");
                std::fflush(stderr);
            }
            throw asio::system_error(asio::error::operation_aborted);
        }
        if (owner_pending >= 64) co_return error_envelope(HostError{"ResourceLimit", "Engine queue full (limit 64)"});
        auto run = std::make_shared<AudioRun>();
        run->request = std::move(request);
        run->cancel = std::move(request_cancel);
        run->submitted = submitted;
        run->timing = std::move(timing);
        run->execution = execution;
        run->state = execution ? std::move(execution) : std::make_shared<RequestExecution>();
        auto self = shared_from_this();
        auto token = asio::use_awaitable;
        co_return co_await asio::async_initiate<decltype(token), void(std::exception_ptr, Json)>(
            [self, run](auto handler) mutable {
                auto executor = asio::get_associated_executor(handler);
                auto guard = std::make_shared<decltype(asio::make_work_guard(executor))>(asio::make_work_guard(executor));
                auto slot = asio::get_associated_cancellation_slot(handler);
                auto completion_handler = std::make_shared<decltype(handler)>(std::move(handler));
                // Weak: the owner or lane thread may drop the last AudioRun, and
                // ~Impl must never run there (it joins those threads).
                const std::weak_ptr<Impl> weak = self;
                const std::weak_ptr<AudioRun> weak_run = run;
                run->complete = [weak, weak_run, executor, guard, slot, completion_handler](std::exception_ptr error, Json result,
                                                                                            bool release_fallback) mutable {
                    if (consume_test_trigger("ENGRAVE_MCP_TEST_FAIL_AUDIO_COMPLETE_POST")) throw std::bad_alloc();
                    asio::post(executor, [weak, weak_run, guard, slot, completion_handler, error, release_fallback,
                                          result = std::move(result)]() mutable {
                        if (const auto session = weak.lock()) {
                            if (release_fallback) {
                                if (const auto locked_run = weak_run.lock(); !locked_run || !locked_run->lane_released) {
                                    if (locked_run) locked_run->lane_released = true;
                                    session->release_audio_lane();
                                }
                            }
                            if (slot.is_connected()) slot.clear();
                            --session->owner_pending;
                        } else if (slot.is_connected()) {
                            slot.clear();
                        }
                        (*completion_handler)(error, std::move(result));
                    });
                };
                // Posted before the completion, so the lane is free when it resumes.
                run->release_lane = [weak, weak_run, executor] {
                    if (consume_test_trigger("ENGRAVE_MCP_TEST_FAIL_AUDIO_RELEASE_POST")) throw std::bad_alloc();
                    asio::post(executor, [weak, weak_run] {
                        if (const auto session = weak.lock()) {
                            if (const auto locked_run = weak_run.lock()) {
                                if (locked_run->lane_released) return;
                                locked_run->lane_released = true;
                            }
                            session->release_audio_lane();
                        }
                    });
                };
                struct AudioEnqueueGuard {
                    std::shared_ptr<Impl> &self;
                    std::shared_ptr<AudioRun> &run;
                    decltype(slot) &cancel_slot;
                    bool queued = false;
                    bool slot_assigned = false;
                    bool committed = false;

                    ~AudioEnqueueGuard() noexcept {
                        if (!committed) {
                            if (slot_assigned && cancel_slot.is_connected()) cancel_slot.clear();
                            if (queued) std::erase(self->audio_queue, run);
                        }
                    }
                } enqueue_guard{self, run, slot};
                self->audio_queue.push_back(run);
                enqueue_guard.queued = true;
                if (slot.is_connected()) {
                    slot.assign([self, run](asio::cancellation_type type) {
                        if (type == asio::cancellation_type::none) return;
                        run->cancel->store(true, std::memory_order_release);
                        auto state = ExecutionState::QUEUED;
                        if (!run->state->state.compare_exchange_strong(state, ExecutionState::CANCELLED_QUEUED)) return;
                        // Still in audio_queue, or queued on owner_io where
                        // prepare_audio observes the cancellation and frees the lane.
                        std::erase(self->audio_queue, run);
                        try {
                            run->complete(aborted(), Json{}, false);
                        } catch (...) {
                            fatal_audio_post_failure(run->execution.get(), "audio queued cancel completion post failed");
                        }
                    });
                    enqueue_guard.slot_assigned = true;
                }
                ++self->owner_pending;
                enqueue_guard.committed = true;
                if (test_hooks_enabled() && self->audio_busy) {
                    std::fprintf(stderr, "AUDIO_LANE_QUEUED\n");
                    std::fflush(stderr);
                }
                self->start_audio();
            },
            token);
    }

    // Keeps catch (...) limited to std::current_exception() (reporting and
    // rethrow run outside catch$) so Win64 ASan unwinds cleanly alongside
    // -mllvm -asan-stack-dynamic-alloca=0.
    [[gnu::noinline]] Json run_live_export(const ExportRequest &request, const std::shared_ptr<std::atomic<bool>> &request_cancel,
                                           const std::shared_ptr<ExportTiming> &timing, std::chrono::steady_clock::time_point submitted,
                                           const std::shared_ptr<RequestExecution> &execution) {
        timing->add("owner_dispatch", submitted);
        Json payload;
        std::vector<std::string> visible_paths;
        PublicationScope publication;
        const auto deadline = submitted + limits.deadline;
        auto runtime = host->runtime_options();
        if (!runtime) return error_envelope(runtime.error());
        std::exception_ptr caught;
        const char *fatal_failure = nullptr;
        bool cancelled_result = false;
        try {
            auto result =
                host->with_document(request.document, request.expected_revision, [&](engrave::Document &document) -> Result<void> {
                    auto cancelled = [&] {
                        return request_cancel->load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire) ||
                               publication_gate.closing.load(std::memory_order_acquire);
                    };
                    auto output = live_export(
                        document, request, limits, cancelled, submitted, *timing, *runtime,
                        [&](FnRef<bool()> deadline_expired) {
                            return begin_publication(execution.get(), publication, deadline, cancelled, deadline_expired);
                        },
                        &visible_paths);
                    if (test_hooks_enabled() && !visible_paths.empty() && std::getenv("ENGRAVE_MCP_TEST_THROW_DOCUMENT_CALLBACK")) {
                        throw 42; // exercise the non-standard exception boundary too
                    }
                    if (!output) return std::unexpected(std::move(output.error()));
                    payload = std::move(*output);
                    return {};
                });
            if (!result) {
                if (result.error().code == "Cancelled") {
                    cancelled_result = true;
                } else {
                    if (result.error().published_artifacts.empty()) result.error().published_artifacts = std::move(visible_paths);
                    payload = error_envelope(result.error());
                }
            }
            if (!cancelled_result && publication.active() && execution && execution->report) {
                if (const char *failure = execution->report(std::move(payload))) {
                    fatal_failure = failure;
                } else {
                    execution->mark_report_sent();
                    return Json{};
                }
            }
        } catch (...) {
            caught = std::current_exception();
        }
        if ((caught || cancelled_result) && publication.active() && execution && execution->state.load() == ExecutionState::PUBLISHING &&
            execution->report) {
            fatal_failure = execution->report(Json{});
        }
        if (fatal_failure) throw FatalReportFailure{fatal_failure};
        if (caught) std::rethrow_exception(caught);
        if (cancelled_result) throw asio::system_error(asio::error::operation_aborted);
        return payload;
    }

    template <class Function>
    asio::awaitable<Json> dispatch(Function function, std::shared_ptr<std::atomic<bool>> running_cancel = {},
                                   bool preserve_completed = false, std::shared_ptr<RequestExecution> execution = {}) {
        if (closing) throw asio::system_error(asio::error::operation_aborted);
        if (publication_gate.closing.load(std::memory_order_acquire)) {
            if (test_hooks_enabled()) {
                std::fprintf(stderr, "ADMISSION_REFUSED_CLOSING\n");
                std::fflush(stderr);
            }
            throw asio::system_error(asio::error::operation_aborted);
        }
        if (owner_pending >= 64) co_return error_envelope(HostError{"ResourceLimit", "Engine queue full (limit 64)"});
        auto self = shared_from_this();
        auto token = asio::use_awaitable;
        co_return co_await asio::async_initiate<decltype(token), void(std::exception_ptr, Json)>(
            [self, function = std::move(function), running_cancel, preserve_completed, execution](auto handler) mutable {
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
                auto complete = [self, executor, guard, slot, completion_handler, running_cancel,
                                 preserve_completed](std::exception_ptr error, Json result) mutable {
                    asio::post(executor, [self, guard, slot, completion_handler, error, running_cancel, preserve_completed,
                                          result = std::move(result)]() mutable {
                        if (!preserve_completed && !error && running_cancel && running_cancel->load(std::memory_order_acquire)) {
                            error = aborted();
                        }
                        if (slot.is_connected()) slot.clear();
                        --self->owner_pending;
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
                            complete(aborted(), Json{});
                        }
                    });
                }
                ++self->owner_pending;
                struct PostGuard {
                    std::shared_ptr<Impl> &self;
                    decltype(slot) &cancel_slot;
                    bool committed = false;

                    ~PostGuard() noexcept {
                        if (!committed) {
                            --self->owner_pending;
                            if (cancel_slot.is_connected()) cancel_slot.clear();
                        }
                    }
                } post_guard{self, slot};
                asio::post(self->owner_io, [self, task, complete, state, running_cancel, preserve_completed]() mutable {
                    auto queued = ExecutionState::QUEUED;
                    bool dropped_closing = false;
                    bool need_complete = false;
                    bool proceed = false;
                    {
                        std::lock_guard lock(publication_gate.state_mutex);
                        if (publication_gate.closing.load(std::memory_order_acquire)) {
                            dropped_closing = true;
                            need_complete = state->state.compare_exchange_strong(queued, ExecutionState::CANCELLED_QUEUED);
                        } else if (state->state.compare_exchange_strong(queued, ExecutionState::EXECUTING)) {
                            ++publication_gate.active_tasks;
                            proceed = true;
                        }
                    }
                    if (dropped_closing) {
                        if (test_hooks_enabled()) {
                            std::fprintf(stderr, "OWNER_DROPPED_CLOSING\n");
                            std::fflush(stderr);
                        }
                        if (need_complete) {
                            task->reset();
                            complete(aborted(), Json{});
                        }
                        return;
                    }
                    if (!proceed) return;
                    std::exception_ptr error;
                    Json result{};
                    const char *fatal_report_failure = nullptr;
                    try {
                        if (self->stopping.load(std::memory_order_acquire)) throw asio::system_error(asio::error::operation_aborted);
                        result = (**task)();
                        if (!preserve_completed && running_cancel && running_cancel->load(std::memory_order_acquire)) {
                            throw asio::system_error(asio::error::operation_aborted);
                        }
                    } catch (const FatalReportFailure &fatal) {
                        fatal_report_failure = fatal.reason;
                    } catch (...) {
                        error = std::current_exception();
                    }
                    task->reset();

                    {
                        std::lock_guard lock(publication_gate.state_mutex);
                        --publication_gate.active_tasks;
                        state->state.store(ExecutionState::FINISHED, std::memory_order_release);
                    }
                    publication_gate.state_cv.notify_all();
                    if (fatal_report_failure) {
                        if (state->abort_transport) state->abort_transport(fatal_report_failure);
                        std::_Exit(74);
                    }
                    complete(error, std::move(result));
                });
                post_guard.committed = true;
                if (test_hooks_enabled()) {
                    std::fprintf(stderr, "OWNER_ENQUEUED\n");
                    std::fflush(stderr);
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
            co_return co_await self->dispatch([self, ms, barrier = std::move(barrier)] {
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
            });
        }
    }
    const auto *spec = find_tool(name);
    if (!spec) co_return error_envelope(HostError{"InvalidArgument", "Unknown tool"});
    const auto handler = handlers[static_cast<std::size_t>(spec->handler)];
    const auto submitted = std::chrono::steady_clock::now();
    auto request_cancel = (spec->handler == HandlerId::EXPORT || spec->handler == HandlerId::EDIT ||
                           spec->handler == HandlerId::WRITE_PHRASE || spec->handler == HandlerId::CHECKPOINT) ?
                              std::make_shared<std::atomic<bool>>(false) :
                              nullptr;
    if (spec->handler == HandlerId::EXPORT) {
        auto request = decode_export(arguments);
        if (!request) co_return error_envelope(request.error());
        auto timing = std::make_shared<ExportTiming>("live", request->destination.path);
        Json payload;
        if (request->kind == ExportKind::AUDIO) {
            payload = co_await self->audio_export(std::move(*request), request_cancel, submitted, timing, execution);
        } else {
            payload = co_await self->dispatch(
                [self, request = *request, request_cancel, timing, submitted, execution] {
                    return self->run_live_export(request, request_cancel, timing, submitted, execution);
                },
                request_cancel, true, execution);
        }
        if (test_hooks_enabled() && execution && execution->was_reported() && std::getenv("ENGRAVE_MCP_TEST_THROW_AFTER_REPORT")) {
            throw std::runtime_error("Injected post-report completion failure");
        }
        timing->finish();
        co_return payload;
    }
    co_return co_await self->dispatch(
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
        request_cancel, true);
}
} // namespace music
