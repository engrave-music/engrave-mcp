#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace music {

inline constexpr std::size_t audio_chunk_frames = 512;
inline constexpr std::uint32_t audio_sample_rate = 44100;
inline constexpr std::size_t audio_max_tracks = 15;

// Caller-provided MIDI stems must have independent track identities and the same
// time origin. They may share MIDI channels: each stem has its own synthesizer.
struct AudioTrack {
    std::int64_t track_id = 0;
    std::string midi;
};
struct AudioOutputParams {
    double volume_db = 0;
    double balance = 0;
    bool solo = false;
    bool muted = false;
    bool force_mute = false;
    bool operator==(const AudioOutputParams&) const = default;
};
struct AudioTrackParams {
    std::int64_t track_id = 0;
    AudioOutputParams output;
    bool operator==(const AudioTrackParams&) const = default;
};
struct AudioMix {
    AudioOutputParams master;
    std::vector<AudioTrackParams> tracks;
    bool operator==(const AudioMix&) const = default;
};
struct AudioChunk {
    bool done = false;
    double start_time = 0;
    double end_time = 0;
    std::vector<float> left;
    std::vector<float> right;
};

// All sessions in a process share one audio owner OS thread. Only cancel() and
// cancelled() may be called from another thread. Use processes for parallel audio.
// Sessions retain MIDI and their loaded SoundFont; replacing a global SoundFont
// preference cannot change an existing session. Destroy sessions on their owner.
// Keep the SoundFont path immutable for this lifetime: backward seek reloads it.
// MusicCore supplies a frozen runtime asset; direct callers own this obligation.
class AudioSession {
public:
    AudioSession(std::vector<AudioTrack> tracks, std::string soundfont, double start_seconds = 0,
                 std::shared_ptr<const void> retained_assets = {});
    ~AudioSession();
    AudioSession(const AudioSession&) = delete;
    AudioSession& operator=(const AudioSession&) = delete;
    AudioSession(AudioSession&&) = delete;
    AudioSession& operator=(AudioSession&&) = delete;

    AudioChunk read_chunk();
    std::vector<AudioChunk> read_batch(std::size_t batch_size);
    // Exact frame seek by rendering/discarding; backward seeks reconstruct synths.
    // O(time), cooperatively cancellable between blocks. Cancellation is permanent.
    void seek(double seconds);
    void cancel() noexcept;
    bool cancelled() const noexcept;
    AudioMix mix() const;
    // Full snapshot replacement; validates all values and track IDs before changing state.
    void set_mix(const AudioMix& mix);

private:
    struct Impl;
    // Declared first so synths are destroyed before releasing frozen asset files.
    std::shared_ptr<const void> retained_assets_;
    std::unique_ptr<Impl> impl_;
};

// Loads the SoundFont through the actual synth, then releases it. Owner-thread only.
void validate_soundfont(const std::string& path);

} // namespace music
