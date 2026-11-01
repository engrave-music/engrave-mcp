#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace music::v2 {
// Shared by the live transport, job manager and runtime_info contract.
inline constexpr std::size_t transport_request_limit = 4 * 1024 * 1024;
inline constexpr std::size_t export_worker_count = 2;

struct JobLimits {
    std::size_t queue_capacity = 8;
    std::uint64_t snapshot_bytes = 64 * 1024 * 1024;
    std::uint64_t output_bytes = 1024ull * 1024 * 1024;
    std::chrono::milliseconds deadline{120000};
};
}
