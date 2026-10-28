#include "audio_session.h"
#include "audio_synth_support.h"

#include <fluidsynth.h>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace music {

namespace {
void check_soundfont_path(const std::string& path) {
    if (path.empty() || path.find('\0') != std::string::npos)
        throw std::invalid_argument("SoundFont path is empty or contains NUL");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), ec))
        throw std::invalid_argument("SoundFont is not a regular file: " + path);
}

bool is_valid_mthd(std::string_view midi) {
    if (midi.size() < 14) {
        return false;
    }
    if (midi[0] != 'M' || midi[1] != 'T' || midi[2] != 'h' || midi[3] != 'd') {
        return false;
    }
    uint32_t header_len = (static_cast<uint32_t>(static_cast<uint8_t>(midi[4])) << 24) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(midi[5])) << 16) |
                          (static_cast<uint32_t>(static_cast<uint8_t>(midi[6])) << 8) |
                          static_cast<uint32_t>(static_cast<uint8_t>(midi[7]));
    if (header_len < 6 || header_len > midi.size() - 8) {
        return false;
    }
    return true;
}

void validate_output_params(const AudioOutputParams& params, const std::string& context) {
    if (!std::isfinite(params.volume_db) || params.volume_db < -120.0 || params.volume_db > 24.0) {
        throw std::invalid_argument(context + ": volume_db must be finite and in [-120, 24]");
    }
    if (!std::isfinite(params.balance) || params.balance < -1.0 || params.balance > 1.0) {
        throw std::invalid_argument(context + ": balance must be finite and in [-1, 1]");
    }
}

struct Stem {
    int64_t track_id = 0;
    std::string midi_data;
    fluid_settings_t* settings = nullptr;
    fluid_synth_t* synth = nullptr;
    fluid_player_t* player = nullptr;

    Stem(int64_t id, std::string midi)
        : track_id(id), midi_data(std::move(midi)) {}

    ~Stem() {
        reset();
    }

    void reset() noexcept {
        if (player) {
            fluid_player_stop(player);
            delete_fluid_player(player);
            player = nullptr;
        }
        if (synth) {
            delete_fluid_synth(synth);
            synth = nullptr;
        }
        if (settings) {
            delete_fluid_settings(settings);
            settings = nullptr;
        }
    }

    Stem(const Stem&) = delete;
    Stem& operator=(const Stem&) = delete;

    Stem(Stem&& other) noexcept
        : track_id(other.track_id),
          midi_data(std::move(other.midi_data)),
          settings(std::exchange(other.settings, nullptr)),
          synth(std::exchange(other.synth, nullptr)),
          player(std::exchange(other.player, nullptr)) {}

    Stem& operator=(Stem&& other) noexcept {
        if (this != &other) {
            reset();
            track_id = other.track_id;
            midi_data = std::move(other.midi_data);
            settings = std::exchange(other.settings, nullptr);
            synth = std::exchange(other.synth, nullptr);
            player = std::exchange(other.player, nullptr);
        }
        return *this;
    }
};

void initialize_stem_synth(Stem& stem, const std::string& soundfont) {
    stem.settings = new_fluid_settings();
    if (!stem.settings) throw std::runtime_error("Failed to allocate FluidSynth settings");
    if (fluid_settings_setnum(stem.settings, "synth.sample-rate", audio_sample_rate) != FLUID_OK
        || fluid_settings_setnum(stem.settings, "synth.gain", 0.2) != FLUID_OK
        || fluid_settings_setint(stem.settings, "synth.lock-memory", 0) != FLUID_OK
        || fluid_settings_setint(stem.settings, "synth.threadsafe-api", 0) != FLUID_OK
        || fluid_settings_setstr(stem.settings, "player.timing-source", "sample") != FLUID_OK)
        throw std::runtime_error("Failed to configure FluidSynth settings");
    stem.synth = new_fluid_synth(stem.settings);
    if (!stem.synth) throw std::runtime_error("Failed to allocate FluidSynth synth");
    if (fluid_synth_sfload(stem.synth, soundfont.c_str(), 1) < 0)
        throw std::runtime_error("Failed to load SoundFont: " + soundfont);
}

} // namespace

void derive_force_mute(AudioMix& mix) noexcept
{
    bool any_solo = false;
    for (const auto& track : mix.tracks) {
        if (track.output.solo) {
            any_solo = true;
            break;
        }
    }
    for (auto& track : mix.tracks) {
        track.output.force_mute = any_solo && !track.output.solo;
    }
    // The master bus has no solo concept; explicit master mute is the only
    // way it is silenced.
    mix.master.force_mute = false;
}

bool is_neutral_mix(const AudioMix& mix) noexcept
{
    if (!(mix.master == AudioOutputParams{})) {
        return false;
    }
    for (const auto& track : mix.tracks) {
        if (!(track.output == AudioOutputParams{})) {
            return false;
        }
    }
    return true;
}

void validate_soundfont(const std::string& path) {
    check_soundfont_path(path);
    detail::initialize_fluid_runtime();
#ifdef __GLIBC__
    struct Reclaim { ~Reclaim() { malloc_trim(0); } } reclaim;
#endif
    Stem validator(-1, {});
    initialize_stem_synth(validator, path);
}

struct AudioSession::Impl {
public:
    Impl(std::vector<AudioTrack> tracks, std::string soundfont)
        : owner_thread_(std::this_thread::get_id()),
          soundfont_path_(std::move(soundfont)) {

        check_soundfont_path(soundfont_path_);

        if (tracks.empty() || tracks.size() > audio_max_tracks) {
            throw std::invalid_argument("Track count must be between 1 and 15");
        }

        constexpr size_t max_midi_bytes = 8 * 1024 * 1024;
        size_t total_midi_bytes = 0;

        for (const auto& track : tracks) {
            if (track.track_id < 0) {
                throw std::invalid_argument("Track ID must be non-negative: " + std::to_string(track.track_id));
            }
            if (!track_ids_.insert(track.track_id).second) {
                throw std::invalid_argument("Duplicate track ID: " + std::to_string(track.track_id));
            }
            if (track.midi.size() > max_midi_bytes) {
                throw std::invalid_argument("Track MIDI bytes exceed 8 MiB limit for track ID: " + std::to_string(track.track_id));
            }
            total_midi_bytes += track.midi.size();
            if (total_midi_bytes > max_midi_bytes) {
                throw std::invalid_argument("Total MIDI bytes exceed 8 MiB limit");
            }
            if (!is_valid_mthd(track.midi)) {
                throw std::invalid_argument("Invalid MIDI MThd header for track ID: " + std::to_string(track.track_id));
            }
        }

        mix_.master = AudioOutputParams{};
        mix_.tracks.reserve(tracks.size());
        for (const auto& track : tracks) {
            mix_.tracks.push_back(AudioTrackParams{ track.track_id, AudioOutputParams{} });
        }

        music::detail::initialize_fluid_runtime();

        stems_.reserve(tracks.size());
        stem_buffers_left_.resize(tracks.size(), std::vector<float>(audio_chunk_frames));
        stem_buffers_right_.resize(tracks.size(), std::vector<float>(audio_chunk_frames));

        for (auto& track : tracks) {
            stems_.emplace_back(track.track_id, std::move(track.midi));
            init_stem(stems_.back());
        }
    }

    ~Impl() {
        stems_.clear();
#if defined(__GLIBC__)
        malloc_trim(0);
#endif
    }

    void check_owner_thread() const {
        if (std::this_thread::get_id() != owner_thread_) {
            throw std::logic_error("AudioSession method called from non-owner thread");
        }
    }

    void cancel() noexcept {
        cancelled_.store(true, std::memory_order_release);
    }

    bool cancelled() const noexcept {
        return cancelled_.load(std::memory_order_acquire);
    }

    AudioChunk read_chunk() {
        check_owner_thread();
        if (cancelled() || eof_reached_) return make_terminal_empty_chunk();
        if (failure_) std::rethrow_exception(failure_);
        AudioChunk chunk;
        chunk.start_time = static_cast<double>(current_frame_) / audio_sample_rate;
        chunk.left.resize(audio_chunk_frames);
        chunk.right.resize(audio_chunk_frames);
        size_t count = 0;
        try { count = consume(audio_chunk_frames, &chunk); }
        catch (...) { failure_ = std::current_exception(); throw; }
        if (cancelled()) return make_terminal_empty_chunk();
        chunk.left.resize(count);
        chunk.right.resize(count);
        chunk.end_time = static_cast<double>(current_frame_) / audio_sample_rate;
        chunk.done = eof_reached_;
        return chunk;
    }

    AudioMix mix() const {
        check_owner_thread();
        return mix_;
    }

    void set_mix(const AudioMix& candidate) {
        check_owner_thread();

        validate_output_params(candidate.master, "Master mix output params");

        if (candidate.tracks.size() != stems_.size()) {
            throw std::invalid_argument("set_mix: track count mismatch");
        }

        std::unordered_set<int64_t> seen_ids;
        for (const auto& tp : candidate.tracks) {
            validate_output_params(tp.output, "Track " + std::to_string(tp.track_id) + " output params");
            if (!track_ids_.contains(tp.track_id)) {
                throw std::invalid_argument("set_mix: unknown track ID " + std::to_string(tp.track_id));
            }
            if (!seen_ids.insert(tp.track_id).second) {
                throw std::invalid_argument("set_mix: duplicate track ID " + std::to_string(tp.track_id));
            }
        }

        AudioMix replacement = candidate;
        std::swap(mix_, replacement);
    }

private:
    void init_stem(Stem& stem) {
        initialize_stem_synth(stem, soundfont_path_);

        stem.player = new_fluid_player(stem.synth);
        if (!stem.player) {
            throw std::runtime_error("Failed to allocate FluidSynth player");
        }

        if (fluid_player_add_mem(stem.player, stem.midi_data.data(), stem.midi_data.size()) != FLUID_OK) {
            throw std::runtime_error("Failed to add MIDI memory buffer to FluidSynth player");
        }

        if (fluid_player_play(stem.player) != FLUID_OK) {
            throw std::runtime_error("Failed to start FluidSynth player");
        }
    }

    bool all_players_done() const {
        bool done = true;
        for (const auto& stem : stems_) {
            int status = fluid_player_get_status(stem.player);
            if (status != FLUID_PLAYER_PLAYING && status != FLUID_PLAYER_DONE)
                throw std::runtime_error("Unexpected FluidSynth player state: " + std::to_string(status));
            done = done && status == FLUID_PLAYER_DONE;
        }
        return done;
    }

    AudioChunk make_terminal_empty_chunk() const {
        AudioChunk chunk;
        chunk.done = true;
        chunk.start_time = static_cast<double>(current_frame_) / static_cast<double>(audio_sample_rate);
        chunk.end_time = chunk.start_time;
        chunk.left.clear();
        chunk.right.clear();
        return chunk;
    }

    // Synths always render fixed blocks from time zero. Public reads/seek consume
    // this raw-stem cache, so arbitrary-frame seek does not change render timing,
    // release-tail length, or mixing of already buffered samples.
    bool render_block() {
        if (all_players_done()) {
            backend_done_ = true;
            eof_reached_ = true;
            return false;
        }
        if (rendered_frame_ >= max_rendered_frames_)
            throw std::runtime_error("Audio exceeds the 600-second rendering limit");
        const auto frames = static_cast<size_t>(std::min<int64_t>(audio_chunk_frames,
                                                                max_rendered_frames_ - rendered_frame_));
        for (size_t i = 0; i < stems_.size(); ++i) {
            std::fill(stem_buffers_left_[i].begin(), stem_buffers_left_[i].end(), 0.0f);
            std::fill(stem_buffers_right_[i].begin(), stem_buffers_right_[i].end(), 0.0f);
            if (fluid_player_get_status(stems_[i].player) == FLUID_PLAYER_DONE) continue;
            if (fluid_synth_write_float(stems_[i].synth, static_cast<int>(frames),
                                        stem_buffers_left_[i].data(), 0, 1,
                                        stem_buffers_right_[i].data(), 0, 1) != FLUID_OK)
                throw std::runtime_error("fluid_synth_write_float failed");
        }
        rendered_frame_ += frames;
        backend_done_ = all_players_done();
        if (rendered_frame_ >= max_rendered_frames_ && !backend_done_)
            throw std::runtime_error("Audio exceeds the 600-second rendering limit");
        pending_offset_ = 0;
        pending_frames_ = frames;
        return true;
    }

    size_t consume(size_t requested, AudioChunk* output) {
        size_t consumed = 0;
        while (consumed < requested && !cancelled() && !eof_reached_) {
            if (pending_offset_ == pending_frames_) {
                if (backend_done_ || !render_block()) {
                    eof_reached_ = true;
                    break;
                }
            }
            if (cancelled()) break;
            const auto count = std::min(requested - consumed, pending_frames_ - pending_offset_);
            if (output) apply_mix(*output, count, pending_offset_, consumed);
            pending_offset_ += count;
            current_frame_ += count;
            consumed += count;
            if (pending_offset_ == pending_frames_ && backend_done_) eof_reached_ = true;
        }
        return consumed;
    }

    void apply_mix(AudioChunk& chunk, size_t frames, size_t source_offset, size_t output_offset) {
        bool any_solo = false;
        for (const auto& tp : mix_.tracks) {
            if (tp.output.solo) {
                any_solo = true;
                break;
            }
        }

        std::vector<float> track_gain_l(stems_.size(), 0.0f);
        std::vector<float> track_gain_r(stems_.size(), 0.0f);

        for (size_t s = 0; s < stems_.size(); ++s) {
            int64_t tid = stems_[s].track_id;
            const AudioOutputParams* params = nullptr;
            for (const auto& tp : mix_.tracks) {
                if (tp.track_id == tid) {
                    params = &tp.output;
                    break;
                }
            }
            if (!params) {
                continue;
            }

            bool audible = true;
            if (params->muted || params->force_mute) {
                audible = false;
            } else if (any_solo && !params->solo) {
                audible = false;
            }

            if (audible) {
                double lin = std::pow(10.0, params->volume_db / 20.0);
                track_gain_l[s] = static_cast<float>(lin * (1.0 - params->balance));
                track_gain_r[s] = static_cast<float>(lin * (1.0 + params->balance));
            }
        }

        bool master_silenced = mix_.master.muted || mix_.master.force_mute;
        float master_gain_l = 0.0f;
        float master_gain_r = 0.0f;
        if (!master_silenced) {
            double m_lin = std::pow(10.0, mix_.master.volume_db / 20.0);
            master_gain_l = static_cast<float>(m_lin * (1.0 - mix_.master.balance));
            master_gain_r = static_cast<float>(m_lin * (1.0 + mix_.master.balance));
        }

        for (size_t f = 0; f < frames; ++f) {
            float sum_l = 0.0f;
            float sum_r = 0.0f;
            for (size_t s = 0; s < stems_.size(); ++s) {
                sum_l += stem_buffers_left_[s][source_offset + f] * track_gain_l[s];
                sum_r += stem_buffers_right_[s][source_offset + f] * track_gain_r[s];
            }
            chunk.left[output_offset + f] = sum_l * master_gain_l;
            chunk.right[output_offset + f] = sum_r * master_gain_r;
        }
    }

    std::thread::id owner_thread_;
    std::atomic<bool> cancelled_{false};
    std::exception_ptr failure_;
    std::string soundfont_path_;
    std::vector<Stem> stems_;
    std::unordered_set<int64_t> track_ids_;
    AudioMix mix_;
    int64_t current_frame_ = 0;
    int64_t rendered_frame_ = 0;
    size_t pending_offset_ = 0;
    size_t pending_frames_ = 0;
    bool backend_done_ = false;
    bool eof_reached_ = false;
    static constexpr int64_t max_rendered_frames_ = 600 * static_cast<int64_t>(audio_sample_rate);
    std::vector<std::vector<float>> stem_buffers_left_;
    std::vector<std::vector<float>> stem_buffers_right_;
};

AudioSession::AudioSession(std::vector<AudioTrack> tracks, std::string soundfont,
                           std::shared_ptr<const void> retained_assets)
    : retained_assets_(std::move(retained_assets)),
      impl_(std::make_unique<Impl>(std::move(tracks), std::move(soundfont))) {}

AudioSession::~AudioSession() = default;

AudioChunk AudioSession::read_chunk() {
    return impl_->read_chunk();
}

void AudioSession::cancel() noexcept {
    impl_->cancel();
}

bool AudioSession::cancelled() const noexcept {
    return impl_->cancelled();
}

AudioMix AudioSession::mix() const {
    return impl_->mix();
}

void AudioSession::set_mix(const AudioMix& mix) {
    impl_->set_mix(mix);
}

} // namespace music
