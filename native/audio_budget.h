#pragma once

#include <cstdint>
#include <limits>

namespace music::v2 {
// The engine retains its completed cache file throughout the sink callback.
// Reserve equal halves for that file and the staged copy, so their combined
// logical file sizes cannot exceed the export's output budget.
inline constexpr std::uint64_t audio_file_budget(std::uint64_t output_budget) noexcept { return output_budget / 2; }

// Cumulative writes include header rewrites, unlike the file extent. This is a
// write-traffic limit, not additional disk space; the file cap remains strict.
inline constexpr std::uint64_t audio_write_budget(std::uint64_t file_budget) noexcept {
    constexpr std::uint64_t rewrite_slack = 1024 * 1024;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return file_budget > maximum - rewrite_slack ? maximum : file_budget + rewrite_slack;
}

} // namespace music::v2
