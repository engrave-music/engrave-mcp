#pragma once
#include "api_output.h"
#include "runtime_limits.h"
#include <chrono>
#include <functional>
#include <memory>
namespace music::v2 {
enum class JobState { Queued, Running, Succeeded, Failed, Cancelled };
struct JobStatus {
    std::string job;
    JobState state=JobState::Queued;
    std::uint64_t source_revision=0;
    std::vector<Artifact> artifacts;
    std::optional<HostError> error;
};
// S3 implementation. Thread-safe facade for S4; only owned snapshots cross it.
// run and start submit the same path. S4 awaits through completion callback,
// never blocks the I/O or owner thread. Close does not cancel captured jobs.
// Reserve all destinations before launch. Stage then publish; partial publication
// is a failure carrying published_artifacts, and remaining staging is removed.
class ExportJobs {
public:
    using Completion=std::function<void(Result<JobStatus>)>;
    explicit ExportJobs(JobLimits);
    ~ExportJobs();
    ExportJobs(const ExportJobs&)=delete;
    ExportJobs& operator=(const ExportJobs&)=delete;
    Result<JobStatus> submit(ExportSnapshot, Completion = {});
    Result<JobStatus> get(std::string_view job) const;
    Result<JobStatus> cancel(std::string_view job);
    void shutdown(); // cancel, reap workers, release reservations before returning
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Json encode(const JobStatus&);
}
