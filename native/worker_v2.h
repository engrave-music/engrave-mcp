#pragma once
#include "api_output.h"

namespace music::v2 {
inline constexpr std::size_t worker_request_limit = 4 * 1024 * 1024;
// The lease stays in the parent; only its immutable paths cross the process boundary.
Json worker_request(const ExportSnapshot&, const std::filesystem::path& job_directory,
                    const std::filesystem::path& staging_directory, std::uint64_t output_limit);
int run_worker(const std::filesystem::path& request_path) noexcept;
}
