#include "async_session.h"
#include "atomic_file.h"
#include "background_job.h"
#include "engine.h"
#include "mcp_transport.h"
#include "music_core.h"
#include "worker_protocol.h"
#include <asio/co_spawn.hpp>
#include <asio/async_result.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/detached.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#include <future>
#include <atomic>
#include <functional>
#include <charconv>
#include <cmath>

namespace music {
namespace {
using Json = nlohmann::json;
const char* core_error_code(CoreError::Code code) {
    switch (code) {
    case CoreError::Code::invalid_argument: return "invalid_argument";
    case CoreError::Code::not_found: return "not_found";
    case CoreError::Code::resource_limit: return "resource_limit";
    case CoreError::Code::wrong_thread: return "wrong_thread";
    case CoreError::Code::conflict: return "conflict";
    case CoreError::Code::stale_revision: return "stale_revision";
    case CoreError::Code::poisoned_document: return "poisoned_document";
    default: return "engine_failure";
    }
}
std::string text_arg(const Json& args, const char* key) {
    if (!args.contains(key) || !args.at(key).is_string())
        throw std::invalid_argument(std::string(key) + " must be a string");
    auto value = args.at(key).get<std::string>();
    if (value.empty() || value.find('\0') != std::string::npos)
        throw std::invalid_argument(std::string(key) + " is empty or contains NUL");
    return value;
}
void only_keys(const Json& args, std::initializer_list<std::string_view> keys) {
    if (!args.is_object()) throw std::invalid_argument("Arguments must be an object");
    for (const auto& [key, _] : args.items())
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            throw std::invalid_argument("Unknown argument: " + key);
}
double finite_seconds(const Json& value) {
    if (!value.is_number()) throw std::invalid_argument("Time must be a number");
    const auto seconds = value.get<double>();
    if (!std::isfinite(seconds) || seconds < 0 || seconds >= 600)
        throw std::invalid_argument("Time must be finite and in [0, 600) seconds");
    return seconds;
}
std::filesystem::path private_directory(const std::filesystem::path& parent) {
    std::random_device random;
    for (int i = 0; i != 16; ++i) {
        auto path = parent / (".musescore-job-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::error_code error;
        if (std::filesystem::create_directory(path, error)) {
#ifndef _WIN32
            std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
            if (error) {
                std::filesystem::remove(path);
                throw std::system_error(error, "Cannot restrict job directory permissions");
            }
#endif
            return path;
        }
        if (error && error != std::errc::file_exists)
            throw std::system_error(error, "Cannot create job directory");
    }
    throw std::runtime_error("Cannot allocate unique job directory");
}
}

struct AsyncSession::Impl : std::enable_shared_from_this<Impl> {
    struct Job {
        std::string id;
        DocumentId document;
        Revision revision;
        ExportFormat format;
        std::filesystem::path destination, directory, staging_directory;
        std::string state = "capturing", error;
        std::size_t captured_bytes = 0;
        std::uintmax_t bytes = 0;
        bool cancel_requested = false;
        bool stream = false, command_busy = false, ready = false;
        std::uint64_t sequence = 0;
        std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
        std::unique_ptr<WorkerProcess> process;
        bool reserved = false;
        bool terminal() const { return state == "completed" || state == "failed" || state == "cancelled"; }
        void cleanup() noexcept {
            process.reset();
            if (reserved) {
                release_output_path(destination);
                reserved = false;
            }
            std::error_code ignored;
            // Both directories were created exclusively by this Job; never client-provided paths.
            if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
            if (!staging_directory.empty()) std::filesystem::remove_all(staging_directory, ignored);
            directory.clear();
            staging_directory.clear();
        }
        ~Job() { cleanup(); }
    };
    struct OwnerResult {
        OwnerResult() = default;
        // Reported to the caller after the cache updates below are published,
        // e.g. a font relayout that poisoned some documents.
        std::exception_ptr deferred_error;
        Json payload;
        std::string closed_document;
        bool invalidate_snapshots = false;
        std::vector<std::string> invalidated_documents;
        std::vector<std::string> poisoned_documents;
        std::vector<std::shared_ptr<const ScoreReadSnapshot>> snapshots;
        ExportData snapshot;
        RuntimeOptions runtime;
        std::size_t snapshot_size = 0;
    };
    asio::io_context owner_io{1};
    asio::executor_work_guard<asio::io_context::executor_type> owner_work{asio::make_work_guard(owner_io)};
    std::unique_ptr<Engine> owner_engine;
    std::jthread owner_thread;
    std::size_t owner_pending = 0;
    std::map<std::string, std::shared_ptr<const ScoreReadSnapshot>> read_snapshots;
    std::set<std::string> poisoned_documents;
    std::size_t snapshot_bytes = 0;
    std::filesystem::path executable;
    std::thread::id owner = std::this_thread::get_id();
    std::map<std::string, std::shared_ptr<Job>> jobs;
    std::deque<std::string> order;
    std::deque<std::shared_ptr<Impl>> owner_keep_alive;
    std::uint64_t serial = 0;
    std::size_t running_streams = 0, running_exports = 0;
    bool closing = false;
    std::atomic<bool> owner_closing{false};
#ifdef MUSIC_ASYNC_TEST_HOOKS
    std::atomic<bool> test_owner_busy{false};
#endif
    // Streams live for the whole audio session, so exporting shares the active
    // job cap but has its own worker budget rather than starving behind them.
    static constexpr std::size_t max_running_streams = 2, max_running_exports = 2,
                                 max_active = 16, max_retained = 64;

    Impl(RuntimeOptions options, std::filesystem::path executable) : executable(std::move(executable)) {
        std::promise<void> ready;
        auto initialized = ready.get_future();
        owner_thread = std::jthread([this, options = std::move(options), ready = std::move(ready)]() mutable {
            try {
                int argc = 1;
                auto executable_text = path_utf8(this->executable);
                char* argv[] = {executable_text.data(), nullptr};
                owner_engine = std::make_unique<Engine>(argc, argv, options.soundfont, options.resources_path);
                ready.set_value();
            } catch (...) {
                ready.set_exception(std::current_exception());
                return;
            }
            owner_io.run();
            owner_engine.reset(); // The DOM and global-module facade die on their owner thread.
        });
        initialized.get();
    }
    ~Impl() {
        shutdown();
        for (auto& [_, job] : jobs) job->cleanup();
    }
    asio::awaitable<OwnerResult> on_owner(std::function<OwnerResult(Engine&)> function, std::string target_document = "") {
        check_owner();
        if (owner_pending >= 64) throw CoreError(CoreError::Code::resource_limit, "Engine queue full (limit 64)");
        auto self = shared_from_this();
        auto token = asio::use_awaitable;
        co_return co_await asio::async_initiate<decltype(token), void(std::exception_ptr, OwnerResult)>(
            [self, function = std::move(function), target_document = std::move(target_document)](auto handler) mutable {
                auto executor = asio::get_associated_executor(handler);
                auto work = asio::make_work_guard(executor);
                auto state = std::make_shared<std::atomic<int>>(0); // 0 queued, 1 running, 2 cancelled
                auto slot = asio::get_associated_cancellation_slot(handler);
                if (slot.is_connected()) slot.assign([state](asio::cancellation_type type) {
                    if (type != asio::cancellation_type::none) {
                        int queued = 0;
                        state->compare_exchange_strong(queued, 2);
                    }
                });
                ++self->owner_pending;
                self->owner_keep_alive.push_back(self);
                try {
                asio::post(self->owner_io, [self, function = std::move(function), handler = std::move(handler),
                                           executor, work = std::move(work), state, slot,
                                           target_document = std::move(target_document)]() mutable {
                    OwnerResult result;
                    std::exception_ptr error;
                    try {
                        int queued = 0;
                        if (!state->compare_exchange_strong(queued, 1))
                            throw asio::system_error(asio::error::operation_aborted);
                        if (self->owner_closing.load(std::memory_order_acquire))
                            throw asio::system_error(asio::error::operation_aborted);
                        result = function(*self->owner_engine);
                        if (result.deferred_error) error = result.deferred_error;
                    } catch (const CoreError& failure) {
                        if (failure.code() == CoreError::Code::poisoned_document) {
                            if (!target_document.empty()) {
                                result.poisoned_documents.push_back(target_document);
                            } else if (result.poisoned_documents.empty() && result.snapshots.empty()) {
                                result.invalidate_snapshots = true;
                            }
                        }
                        error = std::current_exception();
                    } catch (...) { error = std::current_exception(); }
                    function = nullptr;
                    std::shared_ptr<Impl> owner_self = std::move(self);
                    try {
                        asio::post(executor, [self = owner_self, handler = std::move(handler), result = std::move(result), error,
                                               work = std::move(work), slot]() mutable {
                            if (slot.is_connected()) slot.clear();
                            --self->owner_pending;
                            if (!self->owner_keep_alive.empty()) self->owner_keep_alive.pop_front();
                            try {
                                if (result.invalidate_snapshots) self->read_snapshots.clear();
                                for (const auto& doc : result.invalidated_documents) {
                                    self->read_snapshots.erase(doc);
                                }
                                for (const auto& doc : result.poisoned_documents) {
                                    self->read_snapshots.erase(doc);
                                    self->poisoned_documents.insert(doc);
                                }
                                if (!result.closed_document.empty()) {
                                    self->read_snapshots.erase(result.closed_document);
                                    self->poisoned_documents.erase(result.closed_document);
                                }
                                for (auto& snapshot : result.snapshots) {
                                    const auto doc_id = document_id_to_string(snapshot->info.id);
                                    try {
                                        self->poisoned_documents.erase(doc_id);
                                        self->read_snapshots[doc_id] = std::move(snapshot);
                                    } catch (...) {
                                        self->read_snapshots.erase(doc_id);
                                    }
                                }
                            } catch (...) {
                                // Cache publication cannot invalidate an already committed operation.
                            }
                            handler(error, std::move(result));
                        });
                        owner_self.reset();
                    } catch (...) {
                        auto post_error = std::current_exception();
                        try {
                            asio::post(executor, [self = std::move(owner_self), handler = std::move(handler),
                                                   post_error, work = std::move(work), slot]() mutable noexcept {
                                if (slot.is_connected()) slot.clear();
                                --self->owner_pending;
                                if (!self->owner_keep_alive.empty()) self->owner_keep_alive.pop_front();
                                try {
                                    OwnerResult empty_result;
                                    handler(post_error, std::move(empty_result));
                                } catch (...) {}
                            });
                        } catch (...) {
                            // If minimal post throws, self is still kept alive by owner_keep_alive on session thread.
                        }
                    }
                });
                } catch (...) {
                    --self->owner_pending;
                    if (!self->owner_keep_alive.empty()) self->owner_keep_alive.pop_back();
                    if (slot.is_connected()) slot.clear();
                    throw;
                }
            }, token);
    }
    void check_owner() const {
        if (std::this_thread::get_id() != owner) throw std::logic_error("AsyncSession called from non-owner thread");
    }
    Json describe(const Job& job) const {
        Json result = {{"jobId", job.id}, {"documentId", document_id_to_string(job.document)},
                       {"revision", job.revision.value}, {"state", job.state},
                       {"format", format_name(job.format)}, {"path", path_utf8(job.destination)}};
        if (job.stream) {
            result.erase("path");
            result.erase("format");
            result["kind"] = "audio";
            result["ready"] = job.ready;
            result["sampleRate"] = 44100;
            result["chunkFrames"] = 512;
        } else result["kind"] = "export";
        if (job.state == "completed") result["bytes"] = job.bytes;
        if (!job.error.empty()) result["error"] = job.error;
        if (job.cancel_requested && !job.terminal()) result["cancelRequested"] = true;
        return result;
    }
    std::shared_ptr<Job> find(const Json& args) {
        const auto id = text_arg(args, "jobId");
        const auto it = jobs.find(id);
        if (it == jobs.end()) {
            std::uint64_t number = 0;
            const auto parsed = id.starts_with("job-")
                ? std::from_chars(id.data() + 4, id.data() + id.size(), number) : std::from_chars_result{};
            if (id.starts_with("job-") && parsed.ec == std::errc{} && parsed.ptr == id.data() + id.size()
                && number > 0 && number <= serial)
                throw ToolCallError("expired", "Job history has expired");
            throw ToolCallError("not_found", "Unknown jobId");
        }
        return it->second;
    }
    void prune() {
        for (auto it = order.begin(); jobs.size() >= max_retained && it != order.end();) {
            const auto found = jobs.find(*it);
            if (found != jobs.end() && found->second->terminal()) {
                jobs.erase(found);
                it = order.erase(it);
            } else ++it;
        }
    }
    asio::awaitable<std::shared_ptr<Job>> create(Json args, bool stream = false) {
        if (closing) throw std::runtime_error("Session is shutting down");
        if (stream) only_keys(args, {"documentId", "excerptId", "expectedRevision", "startTime", "expandRepeats", "exportRPNs"});
        else only_keys(args, {"documentId", "format", "path", "page", "pngDpi", "drawPageBackground",
                              "transparent", "expandRepeats", "exportRPNs", "expectedRevision", "excerptId"});
        const double start = stream && args.contains("startTime") ? finite_seconds(args.at("startTime")) : 0;
        const auto active = std::count_if(jobs.begin(), jobs.end(), [](const auto& pair) { return !pair.second->terminal(); });
        if (active >= max_active) throw CoreError(CoreError::Code::resource_limit, "Background job queue full (limit 16)");
        const auto format = stream ? std::optional<ExportFormat>{ExportFormat::Wav} : export_format_from_name(text_arg(args, "format"));
        if (!format) throw std::invalid_argument("Unsupported export format");
        const auto options = parse_export_options(args, *format);
        std::filesystem::path destination;
        if (!stream) {
            const auto raw_path = std::filesystem::absolute(std::filesystem::u8path(text_arg(args, "path")));
            if (raw_path.filename().empty() || !std::filesystem::is_directory(raw_path.parent_path()))
                throw std::invalid_argument("Export parent directory must exist");
            destination = std::filesystem::canonical(raw_path.parent_path()) / raw_path.filename();
            if (is_output_path_reserved(destination))
                throw ToolCallError("busy", "Another job owns this export destination");
            for (const auto& [_, job] : jobs)
                if (!job->terminal() && !job->stream && same_output_path(job->destination, destination))
                    throw ToolCallError("busy", "Another job owns this export destination");
        }
        const auto document = document_id_from_string(text_arg(args, "documentId"));
        constexpr std::size_t maximum_snapshot = 8 * 1024 * 1024;
        if (maximum_snapshot > 64 * 1024 * 1024 - snapshot_bytes)
            throw CoreError(CoreError::Code::resource_limit, "Captured snapshots exceed byte budget");
        auto job = std::make_shared<Job>();
        job->id = "job-" + std::to_string(++serial);
        job->document = document;
        job->format = *format;
        job->stream = stream;
        job->destination = destination;
        // Reserved only once the job owns the path, so Job::cleanup always releases it.
        if (!destination.empty()) {
            reserve_output_path(destination);
            job->reserved = true;
        }
        prune();
        jobs.emplace(job->id, job);
        order.push_back(job->id);
        job->captured_bytes = maximum_snapshot;
        snapshot_bytes += maximum_snapshot;
        try {
            job->directory = private_directory(std::filesystem::absolute(std::filesystem::temp_directory_path()));
            if (!stream) job->staging_directory = private_directory(destination.parent_path());
            const auto directory = job->directory;
            const auto staging_directory = job->staging_directory;
            const auto target_doc = document_id_to_string(document);
            auto captured = co_await on_owner([document, args = std::move(args), directory, staging_directory,
                                              format = *format, options, stream, start](Engine& engine) {
                const auto info = engine.core().describe(document);
                if (args.contains("expectedRevision")) {
                    const auto& revision = args.at("expectedRevision");
                    if (!revision.is_number_integer() || revision < 1 || revision > std::numeric_limits<std::uint64_t>::max())
                        throw std::invalid_argument("expectedRevision must be a positive integer");
                    if (revision.get<std::uint64_t>() != info.revision.value)
                        throw CoreError(CoreError::Code::stale_revision, "Export revision is no longer current");
                }
                OwnerResult result;
                result.snapshot = engine.core().snapshot(document);
                result.runtime = engine.core().runtime_options();
                result.snapshot_size = result.snapshot.bytes.size();
                if (result.snapshot_size > 8 * 1024 * 1024)
                    throw CoreError(CoreError::Code::resource_limit, "Snapshot exceeds worker input limit");
                write_atomic(directory / "snapshot.mscz", result.snapshot.bytes);
                ExportWorkerRequest request{directory / "snapshot.mscz", staging_directory / "output",
                                            directory / "result.json", result.runtime, format, options};
                request.cache_directory = directory / "runtime-cache";
                request.stream = stream;
                request.start_seconds = start;
                if (!is_audio_format(format) && !stream) {
                    request.runtime.soundfont.clear();
                }
                if (stream) request.output_path = directory / "unused-output";
                if (is_audio_format(format) || format == ExportFormat::Metadata) {
                    request.mix = engine.core().audio_mix(document, options.excerpt_id).mix;
                    request.has_mix = true;
                }
                write_atomic(directory / "request.json", worker_request_json(request).dump());
                result.snapshot.bytes.clear();
                return result;
            }, target_doc);
            if (closing) throw std::runtime_error("Session is shutting down");
            if (job->cancel_requested) throw ToolCallError("cancelled", "Job was cancelled");
            const auto size = captured.snapshot_size;
            snapshot_bytes -= job->captured_bytes - size;
            job->captured_bytes = size;
            job->revision = captured.snapshot.revision;
            job->state = "queued";
            co_return job;
        } catch (const asio::system_error& error) {
            const bool aborted = error.code() == asio::error::operation_aborted;
            job->state = (closing || job->cancel_requested || aborted) ? "cancelled" : "failed";
            if (aborted && job->error.empty()) job->error = "Capture aborted";
            release(job);
            if (aborted) throw ToolCallError("cancelled", "Job capture was aborted");
            throw;
        } catch (...) {
            job->state = (closing || job->cancel_requested) ? "cancelled" : "failed";
            release(job);
            throw;
        }
    }
    void release(const std::shared_ptr<Job>& job) noexcept {
        snapshot_bytes -= job->captured_bytes;
        job->captured_bytes = 0;
        job->cleanup();
    }
    void cancel(const std::shared_ptr<Job>& job) {
        if (job->terminal()) return; // Publication is a commit; late cancellation cannot undo it.
        job->cancel_requested = true;
        if (job->state == "capturing") return; // Owner owns capture files until its completion.
        if (job->process) job->process->terminate();
        else {
            job->state = "cancelled";
            release(job);
        }
    }
    asio::awaitable<void> monitor(std::shared_ptr<Job> job) {
        const auto executor = co_await asio::this_coro::executor;
        try {
            asio::steady_timer timer(executor);
            std::optional<int> status;
            while (!(status = job->process->poll())) {
                if (job->stream && !job->cancel_requested) {
                    if (!job->ready && std::filesystem::exists(job->directory / "result.json")) {
                        const auto ready = Json::parse(read_bounded_file(job->directory / "result.json", 64 * 1024));
                        if (!ready.value("ok", false)) throw std::runtime_error(ready.value("error", "Audio worker failed"));
                        job->ready = ready.value("ready", false);
                        job->last_activity = std::chrono::steady_clock::now();
                    }
                    if (!job->command_busy && std::chrono::steady_clock::now() - job->last_activity > std::chrono::seconds(120)) {
                        job->error = "Audio session idle timeout";
                        job->cancel_requested = true;
                    }
                }
                if (job->cancel_requested || closing) job->process->terminate();
                timer.expires_after(std::chrono::milliseconds(20));
                co_await timer.async_wait(asio::use_awaitable);
            }
            if (job->cancel_requested || closing) job->state = "cancelled";
            else if (job->stream) {
                if (*status == 0 && (std::chrono::steady_clock::now() - job->last_activity >= std::chrono::seconds(120))) {
                    job->state = "cancelled";
                    if (job->error.empty()) job->error = "Audio session idle timeout";
                } else {
                    throw std::runtime_error("Audio worker exited before cancellation");
                }
            } else {
                const auto result = Json::parse(read_bounded_file(job->directory / "result.json", 64 * 1024));
                if (*status != 0 || !result.value("ok", false))
                    throw std::runtime_error(result.value("error", "Export worker failed"));
                const auto staged = job->staging_directory / "output";
                const auto bytes = std::filesystem::file_size(staged);
                if (!bytes || bytes > 256 * 1024 * 1024 || bytes != result.at("bytes").get<std::uintmax_t>())
                    throw std::runtime_error("Worker output size does not match result");
                // No co_await between final cancellation check, atomic publication and terminal state.
                publish_job_output(staged, job->destination);
                job->bytes = bytes;
                job->state = "completed";
            }
        } catch (const std::exception& error) {
            job->state = (job->cancel_requested || closing) ? "cancelled" : "failed";
            if (job->state == "failed") job->error = std::string(error.what()).substr(0, 2048);
        } catch (...) {
            job->state = (job->cancel_requested || closing) ? "cancelled" : "failed";
            if (job->state == "failed") job->error = "Unknown export worker failure";
        }
        release(job);
        if (job->stream) --running_streams;
        else --running_exports;
        launch(executor);
    }
    asio::awaitable<Json> audio_command(const std::string& name, Json args) {
        if (name == "score_audio_read") only_keys(args, {"jobId", "batchSize"});
        else only_keys(args, {"jobId", "seconds"});
        auto job = find(args);
        if (!job->stream) throw std::invalid_argument("Job is not an audio session");
        if (job->terminal() || job->cancel_requested) throw ToolCallError("not_found", "Audio session has ended");
        if (job->command_busy) throw ToolCallError("busy", "Audio session already has a pending command");
        if (job->state != "running") throw ToolCallError("busy", job->state == "queued"
            ? "Audio session is waiting for a worker slot" : "Audio session is preparing");
        Json command;
        if (name == "score_audio_read") {
            const auto count = args.value("batchSize", Json(1));
            if (!count.is_number_integer() || count < 1 || count > 16)
                throw std::invalid_argument("batchSize must be an integer between 1 and 16");
            command = {{"operation", "read"}, {"batchSize", count}};
        } else {
            if (!args.contains("seconds")) throw std::invalid_argument("seconds is required");
            if (!args.at("seconds").is_number()) throw std::invalid_argument("seconds must be numeric");
            const auto sec = args.at("seconds").get<double>();
            if (!std::isfinite(sec)) throw std::invalid_argument("seconds must be finite");
            command = {{"operation", "seek"}, {"seconds", sec}};
        }
        job->command_busy = true;
        struct CommandGuard {
            std::shared_ptr<Job> job;
            ~CommandGuard() { job->command_busy = false; job->last_activity = std::chrono::steady_clock::now(); }
        } guard{job};
        const auto executor = co_await asio::this_coro::executor;
        asio::steady_timer timer(executor);
        const auto directory = job->directory;
        bool worker_error = false;
        std::string worker_error_message;
        try {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            while (!job->ready) {
                if (job->terminal() || job->cancel_requested) throw ToolCallError("cancelled", "Audio session ended during startup");
                if (std::chrono::steady_clock::now() > deadline) throw ToolCallError("timeout", "Audio worker startup timed out");
                timer.expires_after(std::chrono::milliseconds(5));
                co_await timer.async_wait(asio::use_awaitable);
            }
            command["sequence"] = ++job->sequence;
            write_atomic(directory / "command.json", command.dump());
            for (;;) {
                if (job->terminal() || job->cancel_requested) throw ToolCallError("cancelled", "Audio command cancelled");
                if (std::filesystem::exists(directory / "reply.json")) {
                    auto reply = Json::parse(read_bounded_file(directory / "reply.json", 1024 * 1024));
                    std::filesystem::remove(directory / "reply.json");
                    if (reply.at("sequence").get<std::uint64_t>() != job->sequence)
                        throw std::runtime_error("Audio worker sequence mismatch");
                    if (!reply.value("ok", false)) {
                        worker_error = true;
                        worker_error_message = reply.value("error", "Audio command failed");
                        if (!reply.value("recoverable", false)) {
                            cancel(job);
                        }
                        break;
                    }
                    reply.erase("version"); reply.erase("ok"); reply.erase("sequence");
                    reply["jobId"] = job->id;
                    reply["revision"] = job->revision.value;
                    reply["sampleRate"] = 44100;
                    co_return reply;
                }
                if (std::chrono::steady_clock::now() > deadline) throw ToolCallError("timeout", "Audio command timed out");
                timer.expires_after(std::chrono::milliseconds(5));
                co_await timer.async_wait(asio::use_awaitable);
            }
        } catch (...) {
            // Abandoning a command invalidates its sequence; kill only this owned child.
            cancel(job);
            throw;
        }
        if (worker_error) {
            throw ToolCallError("engine_failure", worker_error_message);
        }
    }
    void launch(asio::any_io_executor executor) {
        if (closing) return;
        for (const auto& id : order) {
            auto job = jobs.at(id);
            if (job->state != "queued") continue;
            // A full stream budget must not hold back queued exports, and vice versa.
            if (job->stream ? running_streams >= max_running_streams
                            : running_exports >= max_running_exports) {
                continue;
            }
            try {
                job->process = std::make_unique<WorkerProcess>(executable, job->directory / "request.json", job->directory / "worker.log");
                job->state = "running";
                job->last_activity = std::chrono::steady_clock::now();
                if (job->stream) ++running_streams;
                else ++running_exports;
                auto self = shared_from_this();
                asio::co_spawn(executor, [self, job]() -> asio::awaitable<void> { co_await self->monitor(job); }, asio::detached);
            } catch (const std::exception& error) {
                if (job->state == "running") {
                    if (job->stream) --running_streams;
                    else --running_exports;
                }
                job->state = "failed";
                job->error = std::string(error.what()).substr(0, 2048);
                release(job);
            }
        }
    }
    void shutdown() noexcept {
        bool expected = false;
        if (!owner_closing.compare_exchange_strong(expected, true)) {
            if (owner_thread.joinable() && std::this_thread::get_id() != owner_thread.get_id()) {
                owner_thread.join();
            }
            return;
        }
        closing = true;
        for (const auto& [_, job] : jobs) {
            try { cancel(job); }
            catch (...) { job->cancel_requested = true; }
        }
        owner_work.reset();
        if (owner_thread.joinable() && std::this_thread::get_id() != owner_thread.get_id()) {
            owner_thread.join();
        }
        owner_keep_alive.clear();
    }
};

AsyncSession::AsyncSession(RuntimeOptions options, std::filesystem::path executable)
    : impl_(std::make_shared<Impl>(std::move(options), std::move(executable))) {}
AsyncSession::~AsyncSession() {
    shutdown();
    impl_.reset();
}
void AsyncSession::shutdown() noexcept {
    if (impl_) impl_->shutdown();
}
asio::awaitable<nlohmann::json> AsyncSession::call(std::string name, Json arguments) {
    auto self = impl_;
    self->check_owner();
    const auto executor = co_await asio::this_coro::executor;
    co_await asio::post(executor, asio::use_awaitable);
    const auto context_document = arguments.contains("documentId") && arguments.at("documentId").is_string()
        ? arguments.at("documentId").get<std::string>() : std::string{};
    try {
#ifdef MUSIC_ASYNC_TEST_HOOKS
    if (name == "test_owner_state")
        co_return Json{{"busy", self->test_owner_busy.load(std::memory_order_acquire)}};
    if (name == "test_owner_sleep") {
        auto result = co_await self->on_owner([&busy = self->test_owner_busy](Engine&) {
            busy.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            busy.store(false, std::memory_order_release);
            Impl::OwnerResult result;
            result.payload = {{"sleptMs", 2000}};
            return result;
        });
        co_return result.payload;
    }
    if (name == "test_poison_document") {
        only_keys(arguments, {"documentId"});
        const auto id_str = text_arg(arguments, "documentId");
        const auto id = document_id_from_string(id_str);
        auto result = co_await self->on_owner([id, id_str](Engine& engine) {
            engine.core().poison_document_for_testing(id);
            Impl::OwnerResult result;
            result.poisoned_documents.push_back(id_str);
            result.payload = {{"poisoned", true}};
            return result;
        }, id_str);
        co_return result.payload;
    }
#endif
    if (name == "score_export") {
        if (arguments.contains("path") && arguments.at("path").is_string()) {
            const auto raw_path = std::filesystem::absolute(std::filesystem::u8path(text_arg(arguments, "path")));
            if (!raw_path.filename().empty() && std::filesystem::is_directory(raw_path.parent_path())) {
                const auto destination = std::filesystem::canonical(raw_path.parent_path()) / raw_path.filename();
                if (is_output_path_reserved(destination))
                    throw ToolCallError("busy", "Another job owns this export destination");
            }
        }
    }
    if (name == "score_export_start" || name == "score_audio_start") {
        auto job = co_await self->create(std::move(arguments), name == "score_audio_start");
        self->launch(executor);
        co_return self->describe(*job);
    }
    if (name == "score_audio_read" || name == "score_audio_seek")
        co_return co_await self->audio_command(name, std::move(arguments));
    if (name == "score_job_get" || name == "score_job_cancel") {
        only_keys(arguments, {"jobId"});
        auto job = self->find(arguments);
        if (name == "score_job_cancel") self->cancel(job);
        co_return self->describe(*job);
    }
    if (name == "score_query_region") {
        only_keys(arguments, {"documentId", "partIndex", "firstMeasure", "measureCount", "voice"});
        const auto id = text_arg(arguments, "documentId");
        if (self->poisoned_documents.contains(id))
            throw CoreError(CoreError::Code::poisoned_document, "Document projection is poisoned");
        const auto found = self->read_snapshots.find(id);
        if (found == self->read_snapshots.end())
            throw CoreError(CoreError::Code::not_found, "No published score projection for document");
        ScoreRegion region;
        const auto integer = [&](const char* name, std::size_t maximum) {
            if (!arguments.contains(name) || !arguments.at(name).is_number_integer()
                || arguments.at(name) < 0 || arguments.at(name) > maximum)
                throw std::invalid_argument(std::string(name) + " is out of range");
            return arguments.at(name).get<std::size_t>();
        };
        region.part_index = integer("partIndex", 255);
        region.first_measure = integer("firstMeasure", 1023);
        region.measure_count = integer("measureCount", 128);
        if (!region.measure_count) throw std::invalid_argument("measureCount must be positive");
        if (arguments.contains("voice")) region.voice = static_cast<unsigned>(integer("voice", 3));
        if (!found->second->projection.complete)
            throw CoreError(CoreError::Code::resource_limit, found->second->projection.unavailable_reason);
        const auto value = query_score_projection(found->second->projection, region);
        Json notes = Json::array(), measures = Json::array();
        for (const auto& note : value.notes)
            notes.push_back({{"id", note.id}, {"measureId", note.measure_id}, {"partIndex", note.part_index},
                {"staffIndex", note.staff_index}, {"measureIndex", note.measure_index}, {"voice", note.voice},
                {"tick", note.tick}, {"durationTicks", note.duration_ticks}, {"pitch", note.pitch},
                {"tied", note.tied}, {"spanned", note.spanned}});
        for (const auto& measure : value.measures)
            measures.push_back({{"id", measure.id}, {"index", measure.index}, {"tick", measure.tick},
                                {"durationTicks", measure.duration_ticks}});
        co_return Json{{"revision", found->second->info.revision.value}, {"consistency", "lastPublished"},
                       {"notes", std::move(notes)}, {"measures", std::move(measures)},
                       {"hasCrossRegionSpanners", value.has_cross_region_spanners}};
    }
    std::vector<DocumentId> refresh;
    if (name == "runtime_add_font" || name == "runtime_set_soundfont")
        for (const auto& [_, snapshot] : self->read_snapshots) refresh.push_back(snapshot->info.id);
    auto result = co_await self->on_owner([name = std::move(name), arguments = std::move(arguments),
                                          refresh = std::move(refresh)](Engine& engine) mutable {
        Impl::OwnerResult result;
        std::exception_ptr call_error;
        try {
            result.payload = Json::parse(engine.call(name, arguments.dump()));
        } catch (const CoreError& failure) {
            if (failure.code() == CoreError::Code::poisoned_document) {
                call_error = std::current_exception();
            } else {
                throw;
            }
        }
        if (name == "score_close") result.closed_document = text_arg(arguments, "documentId");
        else if (result.payload.is_object() && result.payload.contains("documentId"))
            refresh.push_back(document_id_from_string(result.payload.at("documentId").get<std::string>()));
        else if (name == "score_generate_excerpts" || name == "score_audio_set_mix")
            refresh.push_back(document_id_from_string(text_arg(arguments, "documentId")));
        // A projection failure must not turn an already committed edit into a reported failure.
        for (const auto id : refresh) {
            try { result.snapshots.push_back(engine.core().read_snapshot(id)); }
            catch (const CoreError& failure) {
                const auto doc_str = document_id_to_string(id);
                if (failure.code() == CoreError::Code::poisoned_document) {
                    result.poisoned_documents.push_back(doc_str);
                } else {
                    result.invalidated_documents.push_back(doc_str);
                }
            } catch (...) {
                result.invalidated_documents.push_back(document_id_to_string(id));
            }
        }
        result.deferred_error = call_error;
        return result;
    }, context_document);
    co_return std::move(result.payload);
    } catch (const CoreError& error) {
        Json details = Json::object();
        if (!context_document.empty()) {
            details["documentId"] = context_document;
            const auto found = self->read_snapshots.find(context_document);
            if (found != self->read_snapshots.end()) details["currentRevision"] = found->second->info.revision.value;
        }
        throw ToolCallError(core_error_code(error.code()), error.what(), std::move(details));
    } catch (const ScoreQueryLimit& error) {
        throw ToolCallError("resource_limit", error.what());
    } catch (const std::invalid_argument& error) {
        throw ToolCallError("invalid_argument", error.what());
    }
}
}
