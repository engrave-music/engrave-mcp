#pragma once

#include <engrave/audio.h>
#include <engrave/output.h>
#include <engrave/query.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace music::v2 {
// The engine retains its completed cache file throughout the sink callback.
// Reserve equal halves for that file and the staged copy, so their combined
// logical file sizes cannot exceed the job's output budget.
inline constexpr std::uint64_t audio_file_budget(std::uint64_t output_budget) noexcept {
    return output_budget / 2;
}

// Cumulative writes include header rewrites, unlike the file extent. This is a
// write-traffic limit, not additional disk space; the file cap remains strict.
inline constexpr std::uint64_t audio_write_budget(std::uint64_t file_budget) noexcept {
    constexpr std::uint64_t rewrite_slack = 1024 * 1024;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    return file_budget > maximum - rewrite_slack ? maximum : file_budget + rewrite_slack;
}

// Estimate the same stereo encoding settings used by the public engine API.
// Duration is either the nearest-second expanded outline or a millisecond
// position on the expanded timeline. Add half a second for rounding and the
// engine's three-second render tail. Lossy bitrates are nominal, not bounds.
inline std::uint64_t projected_audio_bytes(const engrave::AudioOptions& options,
                                           double score_seconds, std::int32_t sample_rate) noexcept {
    if (!std::isfinite(score_seconds) || score_seconds < 0 || sample_rate <= 0)
        return std::numeric_limits<std::uint64_t>::max();
    const long double seconds = static_cast<long double>(score_seconds) + 3.5L;
    long double bytes;
    if (options.format == engrave::AudioFormat::Wav || options.format == engrave::AudioFormat::Flac) {
        const int depth = options.bit_depth ? options.bit_depth :
            (options.format == engrave::AudioFormat::Wav ? 32 : 16);
        const long double frames = std::ceil(seconds * sample_rate);
        // WAV header slack; FLAC also gets 1 MiB for framing and metadata.
        bytes = frames * 2 * depth / 8 +
            (options.format == engrave::AudioFormat::Wav ? 4096 : 1024 * 1024);
    } else {
        const int bitrate = options.bitrate_kbps ? options.bitrate_kbps : 128;
        bytes = seconds * bitrate * 1000 / 8 + 1024 * 1024;
    }
    if (bytes >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(std::ceil(bytes));
}

// The renderer uses repeatList(false).ticks() for the unexpanded end, but
// converts it with the expanded tempo timeline. The public API does not expose
// that conversion directly. A measure position at the same unrolled tick is
// the engine's own millisecond-rounded conversion; use it only when every
// laid-out measure maps one-to-one to the outline and the end is a boundary.
// Otherwise the caller must omit this projection and rely on the engine's quota.
inline std::optional<double> unexpanded_audio_seconds(const engrave::Outline& outline,
                                                        const engrave::Positions& positions) {
    constexpr std::int64_t ticks_per_whole = 1920; // pinned engine: 480 ticks per quarter
    const auto ticks = [](const engrave::Fraction& value) -> std::optional<std::int64_t> {
        if (value.num < 0 || value.den <= 0 ||
            value.num > std::numeric_limits<std::int64_t>::max() / ticks_per_whole) return {};
        const auto scaled = value.num * ticks_per_whole;
        if (scaled % value.den) return {};
        return scaled / value.den;
    };
    if (outline.measures.empty() || positions.elements.size() != outline.measures.size() ||
        positions.events.empty()) return {};
    std::vector<std::int64_t> lengths;
    lengths.reserve(outline.measures.size());
    std::int64_t end = 0;
    for (std::size_t i = 0; i < outline.measures.size(); ++i) {
        const auto start = ticks(outline.measures[i].tick);
        const auto length = ticks(outline.measures[i].duration);
        if (!start || !length || *start != end || *length <= 0 ||
            *length > std::numeric_limits<std::int32_t>::max() - end ||
            positions.elements[i].id != static_cast<std::int32_t>(i)) return {};
        end += *length;
        lengths.push_back(*length);
    }
    std::int64_t unrolled = 0;
    std::int32_t previous_time = -1;
    for (const auto& event : positions.events) {
        if (event.elid < 0 || static_cast<std::size_t>(event.elid) >= lengths.size() ||
            event.position < 0 || event.position < previous_time) return {};
        if (unrolled == end) return static_cast<double>(event.position) / 1000.0;
        const auto length = lengths[static_cast<std::size_t>(event.elid)];
        if (length > end - unrolled) return {};
        unrolled += length;
        previous_time = event.position;
    }
    // The expanded and unexpanded timelines end at the same tick.
    if (unrolled == end) return outline.duration;
    return {};
}
}
