#include "export_job.h"
#include "api_json_internal.h"
#include "background_job.h"
#include "audio_budget.h"
#include "export_timing.h"
#include "worker_v2.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace music::v2 {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr std::size_t retained_jobs = 256;
struct Failure { HostError error; };

bool terminal(JobState s) {
    return s == JobState::Succeeded || s == JobState::Failed || s == JobState::Cancelled;
}
const char* state_name(JobState s) {
    switch (s) {
    case JobState::Queued: return "queued"; case JobState::Running: return "running";
    case JobState::Succeeded: return "succeeded"; case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
    }
    return "failed";
}
std::string session_id() {
    std::random_device random;
    std::ostringstream s; s << std::hex << random() << random() << random() << random(); return s.str();
}
void write_file(const fs::path& path, std::span<const std::byte> data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw Failure{{"Io", "cannot create job input"}};
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) throw Failure{{"Io", "cannot write job input"}};
}
void write_file(const fs::path& path, const std::string& data) {
    write_file(path, {reinterpret_cast<const std::byte*>(data.data()), data.size()});
}
std::string read_result(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Failure{{"WorkerFailed", "worker did not produce a result"}};
    std::string out; char buffer[4096];
    while (in) {
        in.read(buffer, sizeof(buffer)); auto n = static_cast<std::size_t>(in.gcount());
        if (n > 1024 * 1024 - out.size()) throw Failure{{"WorkerFailed", "worker result exceeds limit"}};
        out.append(buffer, n);
    }
    if (in.bad()) throw Failure{{"WorkerFailed", "cannot read worker result"}};
    return out;
}
std::string page_name(std::int32_t page, const char* extension) {
    std::ostringstream out; out << "page-" << std::setfill('0') << std::setw(4) << page + 1 << '.' << extension; return out.str();
}
fs::path destination_path(const Destination& destination) {
    if (destination.path.empty()) throw Failure{{"InvalidArgument", "destination is empty"}};
    std::error_code ec;
    if (destination.directory) {
        if (!fs::is_directory(destination.path, ec) || ec)
            throw Failure{{"InvalidArgument", "destination directory must exist"}};
        return fs::canonical(destination.path);
    }
    if (fs::is_directory(destination.path, ec) && !ec)
        throw Failure{{"InvalidArgument", "file destination is a directory"}};
    try { return normalize_output_path(destination.path); }
    catch (const std::invalid_argument& e) { throw Failure{{"InvalidArgument", e.what()}}; }
}

struct Job {
    ExportTiming timing{"isolated-job", {}};
    JobStatus status;
    ExportSnapshot snapshot;
    std::uint64_t snapshot_size = 0;
    Clock::time_point submitted;
    fs::path destination, directory, staging;
    bool reserved = false, cancel = false, timeout = false, audio_limit = false;
    std::unique_ptr<WorkerProcess> process;
    ExportJobs::Completion completion;
};
struct Shared {
    struct CaptureReservation { fs::path destination; std::uint64_t budget; };
    explicit Shared(JobLimits value) : limits(value), session(session_id()) {}
    JobLimits limits;
    std::string session;
    std::uint64_t next = 0, next_reservation = 0, snapshot_bytes = 0;
    std::size_t active = 0, running = 0;
    std::mutex mutex;
    std::condition_variable changed, callbacks_ready;
    std::atomic<bool> stopping{false};
    bool coordinator_done = false;
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs;
    std::unordered_map<std::uint64_t, CaptureReservation> captures;
    std::deque<std::string> order;
    std::deque<std::pair<ExportJobs::Completion, JobStatus>> callbacks;
};

void finish(Shared& shared, Job& job, JobState state, std::optional<HostError> error = {}) {
    job.process.reset(); // Reap before releasing asset files or reservations.
    if (job.reserved) { release_output_path(job.destination); job.reserved = false; }
    std::error_code ec;
    if (!job.directory.empty()) fs::remove_all(job.directory, ec);
    if (!job.staging.empty()) fs::remove_all(job.staging, ec);
    shared.snapshot_bytes -= job.snapshot_size; job.snapshot_size = 0;
    job.snapshot = {}; // Last asset lease may remove its immutable copies here.
    job.status.state = state; job.status.error = std::move(error);
    --shared.active;
    if (job.completion) {
        shared.callbacks.emplace_back(std::move(job.completion), job.status);
        shared.callbacks_ready.notify_one();
    }
}

void start(Shared& shared, Job& job) {
    job.timing.destination = job.destination;
    auto started = Clock::now();
    job.directory = create_private_directory(fs::temp_directory_path());
    job.staging = create_private_directory(job.snapshot.request.destination.directory
        ? job.destination : job.destination.parent_path());
    const auto& payload = job.snapshot.request.kind == ExportKind::Score ? job.snapshot.captured_score_bytes :
                          job.snapshot.request.kind == ExportKind::Positions ? job.snapshot.captured_positions_json : job.snapshot.mscz;
    write_file(job.directory / "payload", payload);
    const auto request = worker_request(job.snapshot, job.directory, job.staging, shared.limits.output_bytes).dump();
    if (request.size() > worker_request_limit)
        throw Failure{{"ResourceLimit", "worker request exceeds byte limit"}};
    write_file(job.directory / "request.json", request);
    job.process = std::make_unique<WorkerProcess>(current_executable_path(), job.directory / "request.json", job.directory / "worker.log");
    job.timing.add("stage_spawn", started);
    job.status.state = JobState::Running; ++shared.running;
}

bool audio_cache_exceeds(const Job& job, std::uint64_t limit) {
    const auto cache = job.directory / "cache";
    std::error_code ec;
    if (!fs::exists(cache, ec)) {
        if (ec) throw std::system_error(ec, "cannot inspect audio cache");
        return false;
    }
    std::uint64_t total = 0;
    fs::directory_iterator entries(cache, ec), end;
    if (ec) throw std::system_error(ec, "cannot inspect audio cache");
    for (; entries != end; entries.increment(ec)) {
        if (ec) throw std::system_error(ec, "cannot inspect audio cache");
        // The pinned engine writes its encoded audio as a direct child named
        // engrave-audio-<pid>-<index>.<format>. Do not traverse or charge the
        // runtime font cache, unrelated files, symlinks, or other node types.
        // The separately staged output is checked by the worker and publisher.
        if (!entries->path().filename().generic_u8string().starts_with(u8"engrave-audio-")) continue;
        const auto status = entries->symlink_status(ec);
        if (ec == std::errc::no_such_file_or_directory) { ec.clear(); continue; }
        if (ec) throw std::system_error(ec, "cannot inspect audio cache file");
        if (!fs::is_regular_file(status)) continue;
        const auto size = entries->file_size(ec);
        if (ec == std::errc::no_such_file_or_directory) { ec.clear(); continue; }
        if (ec) throw std::system_error(ec, "cannot measure audio cache file");
        if (size > limit - total) return true;
        total += size;
    }
    if (ec) throw std::system_error(ec, "cannot inspect audio cache");
    return false;
}

void publish(Shared& shared, Job& job, int exit_code) {
    Json result;
    try { result = Json::parse(read_result(job.directory / "result.json")); }
    catch (const Failure&) { throw; }
    catch (const std::exception&) { throw Failure{{"WorkerFailed", "worker returned malformed JSON"}}; }
    try {
        if (!result.is_object() || result.at("version") != 2 || !result.at("ok").is_boolean())
            throw Failure{{"WorkerFailed", "worker returned an invalid result"}};
        if (!result.at("ok").get<bool>()) {
            const auto& error = result.at("error");
            const auto code = error.at("code").get<std::string>();
            const auto message = error.at("message").get<std::string>();
            static const std::vector<std::string> codes = {"InvalidArgument", "InvalidInput", "UnsupportedFormat",
                "NotFound", "StaleHandle", "ResourceLimit", "Unsupported", "Io", "RevisionConflict", "EditRejected",
                "Poisoned", "EngineFailure", "Cancelled", "DeadlineExceeded", "WorkerFailed"};
            if (std::find(codes.begin(), codes.end(), code) == codes.end() || message.size() > 65536)
                throw Failure{{"WorkerFailed", "worker returned an invalid error"}};
            throw Failure{{code, message}};
        }
        if (exit_code != 0) throw Failure{{"WorkerFailed", "worker exited unsuccessfully"}};
        const auto& artifacts = result.at("artifacts");
        const auto& request = job.snapshot.request;
        const bool pages = request.kind == ExportKind::Render && request.destination.directory;
        if (!artifacts.is_array() || artifacts.empty() || artifacts.size() > 4096 || (!pages && artifacts.size() != 1))
            throw Failure{{"WorkerFailed", "worker returned an invalid artifact count"}};
        if (pages && request.render_options.last_page >= 0 &&
            artifacts.size() != static_cast<std::size_t>(request.render_options.last_page - request.render_options.first_page + 1))
            throw Failure{{"WorkerFailed", "worker returned an incomplete page range"}};
        std::uint64_t total = 0;
        std::vector<Artifact> checked; checked.reserve(artifacts.size());
        for (std::size_t i = 0; i < artifacts.size(); ++i) {
            const auto& item = artifacts.at(i);
            const auto name = item.at("name").get<std::string>();
            const auto expected = pages ? page_name(request.render_options.first_page + static_cast<std::int32_t>(i),
                request.render_options.format == engrave::RenderFormat::Png ? "png" : "svg") : "output";
            if (name != expected) throw Failure{{"WorkerFailed", "worker returned an unexpected artifact name"}};
            const auto type = item.at("mime").get<std::string>();
            const auto size = item.at("size").get<std::uint64_t>();
            if (type.empty() || type.size() > 128 || size == 0 || size > shared.limits.output_bytes - total)
                throw Failure{{"ResourceLimit", "worker output exceeds configured limits"}};
            const auto staged = job.staging / name;
            std::error_code ec;
            if (fs::is_symlink(fs::symlink_status(staged, ec)) || ec || !fs::is_regular_file(staged, ec) || ec || fs::file_size(staged, ec) != size || ec)
                throw Failure{{"WorkerFailed", "worker artifact does not match its result"}};
            const auto destination = pages ? job.destination / fs::u8path(name) : job.destination;
            if (detail::path_text(destination).size() > 4096) throw Failure{{"ResourceLimit", "artifact path exceeds limit"}};
            checked.push_back({type, destination, size}); total += size;
        }
        if (Clock::now() - job.submitted >= shared.limits.deadline)
            throw Failure{{"DeadlineExceeded", "job deadline exceeded before publication"}};
        if (result.contains("phases_ms")) job.timing.phases.update(result.at("phases_ms"));
        auto publish_start = Clock::now();
        // The mutex remains held from the final cancellation check through
        // publication; a late cancellation observes the terminal result.
        for (std::size_t i = 0; i < checked.size(); ++i) {
            const auto name = artifacts.at(i).at("name").get<std::string>();
            try { publish_job_output(job.staging / name, checked[i].path); }
            catch (const std::exception& e) {
                HostError error{"Io", std::string("cannot publish artifact: ") + e.what()};
                for (const auto& published : job.status.artifacts) error.published_artifacts.push_back(detail::path_text(published.path));
                throw Failure{std::move(error)};
            }
            job.status.artifacts.push_back(checked[i]);
        }
        job.timing.add("publish", publish_start);
        job.timing.finish();
    } catch (const Failure&) { throw; }
      catch (const std::exception&) { throw Failure{{"WorkerFailed", "worker returned an invalid result"}}; }
}

void coordinator(std::shared_ptr<Shared> shared) {
    std::unique_lock lock(shared->mutex);
    for (;;) {
        const auto now = Clock::now();
        for (const auto& id : shared->order) {
            auto job = shared->jobs.at(id);
            if (terminal(job->status.state)) continue;
            if (shared->stopping) job->cancel = true;
            if (!job->cancel && now - job->submitted >= shared->limits.deadline) job->timeout = true;
            if (job->status.state == JobState::Queued) {
                if (job->cancel || job->timeout) {
                    finish(*shared, *job, job->cancel ? JobState::Cancelled : JobState::Failed,
                           HostError{job->cancel ? "Cancelled" : "DeadlineExceeded", job->cancel ? "job cancelled" : "job deadline exceeded"});
                } else if (job->snapshot.capture_error) {
                    finish(*shared, *job, JobState::Failed, *job->snapshot.capture_error);
                } else if (shared->running < export_worker_count) {
                    try { start(*shared, *job); }
                    catch (const Failure& e) { finish(*shared, *job, JobState::Failed, e.error); }
                    catch (const std::exception& e) { finish(*shared, *job, JobState::Failed, HostError{"WorkerFailed", e.what()}); }
                }
            }
            if (job->status.state != JobState::Running) continue;
            try {
                if (!job->cancel && Clock::now() - job->submitted >= shared->limits.deadline) job->timeout = true;
                if (!job->cancel && !job->timeout && job->snapshot.request.kind == ExportKind::Audio &&
                    audio_cache_exceeds(*job, audio_file_budget(shared->limits.output_bytes))) job->audio_limit = true;
                if (job->cancel || job->timeout || job->audio_limit) job->process->terminate();
                auto exited = job->process->poll();
                if (!exited) continue;
                --shared->running;
                if (job->audio_limit && !job->cancel && !job->timeout) {
                    finish(*shared, *job, JobState::Failed, HostError{"ResourceLimit", "audio cache exceeds output byte limit"});
                } else if (job->cancel || job->timeout) {
                    finish(*shared, *job, job->cancel ? JobState::Cancelled : JobState::Failed,
                           HostError{job->cancel ? "Cancelled" : "DeadlineExceeded", job->cancel ? "job cancelled" : "job deadline exceeded"});
                } else {
                    try { publish(*shared, *job, *exited); finish(*shared, *job, JobState::Succeeded); }
                    catch (const Failure& e) { finish(*shared, *job, e.error.code == "Cancelled" ? JobState::Cancelled : JobState::Failed, e.error); }
                }
            } catch (const std::exception& e) {
                --shared->running;
                if (job->audio_limit && !job->cancel && !job->timeout)
                    finish(*shared, *job, JobState::Failed, HostError{"ResourceLimit", "audio cache exceeds output byte limit"});
                else if (job->cancel || job->timeout)
                    finish(*shared, *job, job->cancel ? JobState::Cancelled : JobState::Failed,
                           HostError{job->cancel ? "Cancelled" : "DeadlineExceeded", job->cancel ? "job cancelled" : "job deadline exceeded"});
                else finish(*shared, *job, JobState::Failed, HostError{"WorkerFailed", e.what()});
            }
        }
        if (shared->stopping && shared->active == 0) break;
        shared->changed.wait_for(lock, std::chrono::milliseconds(shared->running ? 5 : 20));
    }
    shared->coordinator_done = true;
    shared->callbacks_ready.notify_all();
}
void callback_loop(std::shared_ptr<Shared> shared) {
    std::unique_lock lock(shared->mutex);
    for (;;) {
        shared->callbacks_ready.wait(lock, [&] { return shared->coordinator_done || !shared->callbacks.empty(); });
        if (shared->callbacks.empty() && shared->coordinator_done) return;
        auto [callback, status] = std::move(shared->callbacks.front()); shared->callbacks.pop_front();
        lock.unlock();
        try { callback(std::move(status)); } catch (...) {}
        lock.lock();
    }
}
}

struct ExportJobs::Reservation::State {
    std::shared_ptr<Shared> shared;
    fs::path destination;
    Destination requested_destination;
    ExportKind kind = ExportKind::Score;
    std::uint64_t budget = 0;
    std::uint64_t id = 0;
    Clock::time_point submitted;
    ~State() {
        if (!id) return;
        std::lock_guard lock(shared->mutex);
        const auto entry = shared->captures.find(id);
        if (entry == shared->captures.end()) return;
        release_output_path(entry->second.destination);
        shared->snapshot_bytes -= entry->second.budget;
        --shared->active;
        shared->captures.erase(entry);
        shared->changed.notify_all();
    }
};

ExportJobs::Reservation::Reservation() = default;
ExportJobs::Reservation::~Reservation() = default;
ExportJobs::Reservation::Reservation(Reservation&&) noexcept = default;
ExportJobs::Reservation& ExportJobs::Reservation::operator=(Reservation&&) noexcept = default;
ExportJobs::Reservation::Reservation(std::unique_ptr<State> state) : state_(std::move(state)) {}
std::uint64_t ExportJobs::Reservation::budget() const { return state_ ? state_->budget : 0; }
std::optional<HostError> ExportJobs::Reservation::stop_reason() const {
    if (!state_ || state_->shared->stopping.load(std::memory_order_acquire))
        return HostError{"Cancelled", "job manager is shutting down"};
    if (Clock::now() - state_->submitted >= state_->shared->limits.deadline)
        return HostError{"DeadlineExceeded", "job deadline exceeded during capture"};
    return {};
}

#ifdef MUSIC_ASYNC_TEST_HOOKS
void ExportJobs::Reservation::test_expire_capture() {
    state_->submitted = Clock::now() - state_->shared->limits.deadline;
}
#endif

struct ExportJobs::Impl {
    explicit Impl(JobLimits limits) : shared(std::make_shared<Shared>(limits)) {
        callbacks = std::thread(callback_loop, shared);
        callback_id = callbacks.get_id();
        try { worker = std::thread(coordinator, shared); }
        catch (...) {
            { std::lock_guard lock(shared->mutex); shared->coordinator_done = true; shared->callbacks_ready.notify_all(); }
            callbacks.join();
            throw;
        }
    }
    std::shared_ptr<Shared> shared;
    std::thread worker, callbacks;
    std::thread::id callback_id;
    std::mutex shutdown_mutex;
    std::condition_variable shutdown_done;
    bool joining = false, joined = false;
    void shutdown() {
        std::unique_lock lock(shutdown_mutex);
        if (joined) return;
        if (joining) {
            // The joining thread may be waiting for this callback to return.
            if (callback_id == std::this_thread::get_id()) return;
            shutdown_done.wait(lock, [&] { return joined; }); return;
        }
        joining = true; lock.unlock();
        {
            std::lock_guard state_lock(shared->mutex);
            shared->stopping = true;
            // Owner work may still be running or its response may never be
            // consumed. Revoke provisional reservations without waiting for it.
            for (const auto& [id, capture] : shared->captures) {
                release_output_path(capture.destination);
                shared->snapshot_bytes -= capture.budget;
                --shared->active;
            }
            shared->captures.clear();
            shared->changed.notify_all();
        }
        if (worker.joinable()) worker.join();
        if (callbacks.joinable()) {
            if (callback_id == std::this_thread::get_id()) callbacks.detach();
            else callbacks.join();
        }
        lock.lock(); joined = true; lock.unlock(); shutdown_done.notify_all();
    }
};

ExportJobs::ExportJobs(JobLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
ExportJobs::~ExportJobs() { shutdown(); }
void ExportJobs::shutdown() { if (impl_) impl_->shutdown(); }

Result<ExportJobs::Reservation> ExportJobs::reserve(const ExportRequest& request, Clock::time_point submitted) {
    try {
        const bool pages = request.kind == ExportKind::Render &&
            request.render_options.format != engrave::RenderFormat::Pdf;
        if (request.destination.directory != pages)
            return std::unexpected(HostError{"InvalidArgument", "destination kind does not match export"});
        auto state = std::make_unique<Reservation::State>();
        state->shared = impl_->shared;
        state->destination = destination_path(request.destination);
        state->requested_destination = request.destination;
        state->kind = request.kind;
        state->submitted = submitted;
        auto shared = state->shared;
        std::lock_guard lock(shared->mutex);
        if (shared->stopping) return std::unexpected(HostError{"Cancelled", "job manager is shutting down"});
        if (Clock::now() - submitted >= shared->limits.deadline)
            return std::unexpected(HostError{"DeadlineExceeded", "job deadline exceeded before capture"});
        if (shared->active >= shared->limits.queue_capacity || shared->snapshot_bytes >= shared->limits.snapshot_bytes)
            return std::unexpected(HostError{"ResourceLimit", "job queue or snapshot budget is full"});
        if (!try_reserve_output_path(state->destination, request.destination.directory))
            return std::unexpected(HostError{"InvalidArgument", "destination is reserved by another job"});
        // A capture's size is unknown until the public API returns it. Charge
        // all remaining payload budget until the snapshot can be measured.
        state->budget = shared->limits.snapshot_bytes - shared->snapshot_bytes;
        const auto id = ++shared->next_reservation;
        if (!id) {
            release_output_path(state->destination);
            return std::unexpected(HostError{"ResourceLimit", "capture reservation IDs exhausted"});
        }
        try { shared->captures.emplace(id, Shared::CaptureReservation{state->destination, state->budget}); }
        catch (...) { release_output_path(state->destination); throw; }
        state->id = id;
        shared->snapshot_bytes += state->budget;
        ++shared->active;
        return Reservation(std::move(state));
    } catch (const Failure& e) { return std::unexpected(e.error); }
      catch (const fs::filesystem_error& e) { return std::unexpected(HostError{"Io", e.what()}); }
      catch (const std::exception& e) { return std::unexpected(HostError{"EngineFailure", e.what()}); }
}

Result<JobStatus> ExportJobs::submit(ExportSnapshot snapshot, Completion completion) {
    auto reservation = reserve(snapshot.request);
    if (!reservation) return std::unexpected(reservation.error());
    return submit(std::move(*reservation), std::move(snapshot), std::move(completion));
}

Result<JobStatus> ExportJobs::submit(Reservation reservation, ExportSnapshot snapshot, Completion completion) {
    try {
        auto* state = reservation.state_.get();
        if (!state || !state->id || state->shared != impl_->shared || state->kind != snapshot.request.kind ||
            state->requested_destination.directory != snapshot.request.destination.directory ||
            state->requested_destination.path != snapshot.request.destination.path)
            return std::unexpected(HostError{"InvalidArgument", "snapshot does not match its reservation"});
        snapshot.request.destination.path = state->destination;
        std::uint64_t size = 0;
        for (const auto n : {snapshot.mscz.size(), snapshot.captured_score_bytes.size(), snapshot.captured_positions_json.size()}) {
            if (n > state->budget - size)
                return std::unexpected(HostError{"ResourceLimit", "snapshot exceeds reserved byte budget"});
            size += n;
        }
        // No-repeat audio may capture positions in the worker to estimate its
        // duration. Keep a separate part of the original reservation charged
        // for that concurrent payload until the job finishes. If it is too
        // small, the worker skips projection and relies on the audio quota.
        const auto position_budget = snapshot.request.kind == ExportKind::Audio && !snapshot.request.audio_options.use_repeats
            ? std::min(size, state->budget - size) : 0;
        snapshot.capture_budget = position_budget;
        if ((snapshot.request.kind == ExportKind::Render || snapshot.request.kind == ExportKind::Audio) && snapshot.mscz.size() > engrave::max_input_bytes)
            return std::unexpected(HostError{"ResourceLimit", "snapshot exceeds engine input limit"});
        if ((snapshot.request.kind == ExportKind::Score && snapshot.captured_score_bytes.size() > impl_->shared->limits.output_bytes) ||
            (snapshot.request.kind == ExportKind::Positions && snapshot.captured_positions_json.size() > impl_->shared->limits.output_bytes))
            return std::unexpected(HostError{"ResourceLimit", "captured artifact exceeds output limit"});
        auto shared = impl_->shared;
        std::lock_guard lock(shared->mutex);
        if (shared->stopping) return std::unexpected(HostError{"Cancelled", "job manager is shutting down"});
        if (!shared->captures.contains(state->id))
            return std::unexpected(HostError{"Cancelled", "capture reservation was revoked"});
        if (Clock::now() - state->submitted >= shared->limits.deadline)
            return std::unexpected(HostError{"DeadlineExceeded", "job deadline exceeded during capture"});
        auto job = std::make_shared<Job>();
        job->destination = state->destination; job->snapshot = std::move(snapshot);
        job->snapshot_size = size + position_budget; job->submitted = state->submitted; job->completion = std::move(completion);
        job->status.job = "job-" + shared->session + "-" + std::to_string(++shared->next);
        job->status.source_revision = job->snapshot.source_revision;
        job->status.snapshot_budget_enforced = job->snapshot.snapshot_budget_enforced;
        job->status.export_warnings = job->snapshot.export_warnings;
        JobStatus status = job->status;
        while (shared->order.size() >= retained_jobs) {
            auto oldest = std::find_if(shared->order.begin(), shared->order.end(), [&](const auto& id) { return terminal(shared->jobs.at(id)->status.state); });
            if (oldest == shared->order.end()) break;
            shared->jobs.erase(*oldest); shared->order.erase(oldest);
        }
        shared->jobs.emplace(job->status.job, job);
        try { shared->order.push_back(job->status.job); }
        catch (...) { shared->jobs.erase(job->status.job); throw; }
        shared->snapshot_bytes -= state->budget - job->snapshot_size;
        shared->captures.erase(state->id);
        job->reserved = true;
        shared->changed.notify_all();
        return status;
    } catch (const Failure& e) { return std::unexpected(e.error); }
      catch (const fs::filesystem_error& e) { return std::unexpected(HostError{"Io", e.what()}); }
      catch (const std::exception& e) { return std::unexpected(HostError{"EngineFailure", e.what()}); }
}
Result<JobStatus> ExportJobs::get(std::string_view id) const {
    auto shared = impl_->shared; std::lock_guard lock(shared->mutex);
    const auto it = shared->jobs.find(std::string(id));
    if (it == shared->jobs.end()) return std::unexpected(HostError{"NotFound", "unknown job"});
    return it->second->status;
}
Result<JobStatus> ExportJobs::cancel(std::string_view id) {
    auto shared = impl_->shared; std::lock_guard lock(shared->mutex);
    const auto it = shared->jobs.find(std::string(id));
    if (it == shared->jobs.end()) return std::unexpected(HostError{"NotFound", "unknown job"});
    if (!terminal(it->second->status.state)) { it->second->cancel = true; shared->changed.notify_all(); }
    return it->second->status;
}
Json encode(const JobStatus& status) {
    Json artifacts = Json::array(); for (const auto& a : status.artifacts) artifacts.push_back(encode(a));
    return {{"job", status.job}, {"state", state_name(status.state)}, {"source_revision", std::to_string(status.source_revision)},
            {"artifacts", std::move(artifacts)}, {"export_warnings", encode(status.export_warnings)},
            {"error", status.error ? error_envelope(*status.error).at("error") : Json(nullptr)}};
}
} // namespace music::v2
