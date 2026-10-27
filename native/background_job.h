#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>

namespace music {
bool same_output_path(const std::filesystem::path& first, const std::filesystem::path& second);

[[nodiscard]] std::filesystem::path current_executable_path();
std::uint64_t current_process_id() noexcept;
void enforce_worker_parent(std::uint64_t expected_parent);
void publish_job_output(const std::filesystem::path& staged, const std::filesystem::path& destination);

class WorkerProcess {
public:
    explicit WorkerProcess(const std::filesystem::path& executable,
                           const std::filesystem::path& request_json,
                           const std::filesystem::path& stderr_log);
    ~WorkerProcess() noexcept;

    WorkerProcess(WorkerProcess&&) noexcept;
    WorkerProcess& operator=(WorkerProcess&&) noexcept;

    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;

    [[nodiscard]] std::optional<int> poll();
    void terminate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace music
