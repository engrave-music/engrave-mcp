#pragma once

#include <engrave/audio.h>
#include <cmath>
#include <cstdint>
#include <limits>

namespace music::v2 {
// Estimate the same stereo encoding settings used by the public engine API.
// Outline duration is rounded to the nearest second and always includes
// repeats. Add half a second for that rounding and the engine's three-second
// render tail. Lossy bitrates are nominal, not strict codec bounds.
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
}
