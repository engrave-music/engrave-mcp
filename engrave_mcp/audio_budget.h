#pragma once

#include <cstdint>
#include <limits>

namespace music::api {

// PreparedAudio::run encodes straight into host staging, so the staged file
// is the only copy and may use the whole export budget.
inline constexpr std::uint64_t audio_file_budget(std::uint64_t output_budget) noexcept { return output_budget; }

// Cumulative writes include header rewrites, unlike the file extent. This is a
// write-traffic limit, not additional disk space; the file cap remains strict.
inline constexpr std::uint64_t audio_write_budget(std::uint64_t file_budget) noexcept {
    constexpr std::uint64_t rewrite_slack = 1024 * 1024;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return file_budget > maximum - rewrite_slack ? maximum : file_budget + rewrite_slack;
}

} // namespace music::api
