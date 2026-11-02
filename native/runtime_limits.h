#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace music::v2 {
// Shared by the live transport, export and runtime_info contract.
inline constexpr std::size_t transport_request_limit = 4 * 1024 * 1024;

struct ExportLimits {
    std::uint64_t output_bytes = 1024ull * 1024 * 1024;
    std::chrono::milliseconds deadline{120000};
};
}
