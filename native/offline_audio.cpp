#include "offline_audio.h"
#include "audio_synth_support.h"

#include "audio/engine/internal/codecs/vorbisdecoder.h"
#include "audio/thirdparty/fluidsynth/vorbis_decode.h"
#include <fluidsynth.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <string>
#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace music {
namespace detail {
void initialize_fluid_runtime() {
    static const auto owner = std::this_thread::get_id();
    if (std::this_thread::get_id() != owner)
        throw std::logic_error("FluidSynth requires one audio owner thread per process");
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        for (int level = FLUID_PANIC; level <= FLUID_DBG; ++level) {
            fluid_set_log_function(level, [](int severity, const char* message, void*) {
                std::fprintf(stderr, "FluidSynth[%d]: %s\n", severity, message ? message : "");
            }, nullptr);
        }
        static muse::audio::codec::VorbisDecoder decoder;
        muse::audio::fluid::FluidVorbisDecoder::decoder = &decoder;
    });
}
}
namespace {

struct SettingsDeleter {
    void operator()(fluid_settings_t* s) const noexcept {
        if (s) {
            delete_fluid_settings(s);
        }
    }
};

struct SynthDeleter {
    void operator()(fluid_synth_t* s) const noexcept {
        if (s) {
            delete_fluid_synth(s);
        }
    }
};

struct PlayerDeleter {
    void operator()(fluid_player_t* p) const noexcept {
        if (p) {
            fluid_player_stop(p);
            delete_fluid_player(p);
        }
    }
};

void append_u16_le(std::string& out, uint16_t val) {
    out.push_back(static_cast<char>(val & 0xFF));
    out.push_back(static_cast<char>((val >> 8) & 0xFF));
}

void append_u32_le(std::string& out, uint32_t val) {
    out.push_back(static_cast<char>(val & 0xFF));
    out.push_back(static_cast<char>((val >> 8) & 0xFF));
    out.push_back(static_cast<char>((val >> 16) & 0xFF));
    out.push_back(static_cast<char>((val >> 24) & 0xFF));
}

std::string make_riff_wave_header(uint32_t pcm_data_size) {
    std::string header;
    header.reserve(44);

    header.append("RIFF", 4);
    append_u32_le(header, 36 + pcm_data_size);
    header.append("WAVE", 4);

    header.append("fmt ", 4);
    append_u32_le(header, 16);      // Subchunk1Size (16 for PCM)
    append_u16_le(header, 1);       // AudioFormat (1 = PCM)
    append_u16_le(header, 2);       // NumChannels (stereo)
    append_u32_le(header, 44100);   // SampleRate
    append_u32_le(header, 176400);  // ByteRate (44100 * 2 * 2)
    append_u16_le(header, 4);       // BlockAlign (2 * 2)
    append_u16_le(header, 16);      // BitsPerSample

    header.append("data", 4);
    append_u32_le(header, pcm_data_size);
    return header;
}

} // namespace

std::string render_midi_wav(const std::string& midi, const std::string& soundfont) {
#ifdef __GLIBC__
    // Construct before the synth/player so their large SoundFont allocations
    // are freed first, then return unused heap pages after this offline job.
    // This releases only free pages; live allocations remain measurable.
    struct ReleaseUnusedPages { ~ReleaseUnusedPages() { malloc_trim(0); } } release_unused_pages;
#endif
    detail::initialize_fluid_runtime();
    if (soundfont.empty()) {
        throw std::runtime_error("Soundfont path cannot be empty");
    }
    if (midi.empty()) {
        throw std::runtime_error("MIDI data cannot be empty");
    }
    constexpr size_t MAX_MIDI_SIZE = 8 * 1024 * 1024; // 8 MiB
    if (midi.size() > MAX_MIDI_SIZE) {
        throw std::runtime_error("MIDI data exceeds maximum permitted size of 8 MiB");
    }

    // 1. Settings configuration
    std::unique_ptr<fluid_settings_t, SettingsDeleter> settings(new_fluid_settings());
    if (!settings) {
        throw std::runtime_error("Failed to allocate FluidSynth settings");
    }

    if (fluid_settings_setnum(settings.get(), "synth.sample-rate", 44100.0) != FLUID_OK) {
        throw std::runtime_error("Failed to set sample-rate setting");
    }
    if (fluid_settings_setnum(settings.get(), "synth.gain", 0.2) != FLUID_OK) {
        throw std::runtime_error("Failed to set synth.gain setting");
    }
    if (fluid_settings_setint(settings.get(), "synth.lock-memory", 0) != FLUID_OK) {
        throw std::runtime_error("Failed to set synth.lock-memory setting");
    }
    if (fluid_settings_setint(settings.get(), "synth.threadsafe-api", 0) != FLUID_OK) {
        throw std::runtime_error("Failed to set synth.threadsafe-api setting");
    }
    if (fluid_settings_setstr(settings.get(), "player.timing-source", "sample") != FLUID_OK) {
        throw std::runtime_error("Failed to set player.timing-source setting");
    }

    // 2. Shared decoder/log setup was completed by initialize_fluid_runtime().

    // 3. Synth creation & SoundFont loading
    std::unique_ptr<fluid_synth_t, SynthDeleter> synth(new_fluid_synth(settings.get()));
    if (!synth) {
        throw std::runtime_error("Failed to allocate FluidSynth synth");
    }

    if (fluid_synth_sfload(synth.get(), soundfont.c_str(), 1) < 0) {
        throw std::runtime_error("Failed to load soundfont: " + soundfont);
    }

    // 4. Player creation & MIDI feed
    std::unique_ptr<fluid_player_t, PlayerDeleter> player(new_fluid_player(synth.get()));
    if (!player) {
        throw std::runtime_error("Failed to allocate FluidSynth player");
    }

    if (fluid_player_add_mem(player.get(), midi.data(), midi.size()) != FLUID_OK) {
        throw std::runtime_error("Failed to load MIDI memory into player");
    }
    if (fluid_player_play(player.get()) != FLUID_OK) {
        throw std::runtime_error("Failed to begin MIDI playback");
    }

    // 5. Render audio blocks (10 minutes max total duration including 3s tail)
    constexpr size_t SAMPLE_RATE = 44100;
    constexpr size_t MAX_TOTAL_FRAMES = SAMPLE_RATE * 60 * 10; // 10 minutes
    // FluidSynth's player already waits for voice release and its reverb grace period.
    constexpr size_t TAIL_FRAMES = 0;
    constexpr size_t CHUNK_FRAMES = 256;

    std::array<int16_t, CHUNK_FRAMES * 2> chunk_buf;
    std::string pcm_data;
    pcm_data.reserve(CHUNK_FRAMES * 4 * 16);

    size_t total_frames = 0;

    auto render_chunk = [&](size_t frames_to_render) {
        if (fluid_synth_write_s16(synth.get(), static_cast<int>(frames_to_render),
                                  chunk_buf.data(), 0, 2,
                                  chunk_buf.data(), 1, 2) != FLUID_OK) {
            throw std::runtime_error("fluid_synth_write_s16 render block failed");
        }
        for (size_t i = 0; i < frames_to_render * 2; ++i) {
            append_u16_le(pcm_data, static_cast<uint16_t>(chunk_buf[i]));
        }
        total_frames += frames_to_render;
    };

    // Render while player is actively playing
    while (fluid_player_get_status(player.get()) == FLUID_PLAYER_PLAYING &&
           total_frames < MAX_TOTAL_FRAMES) {
        size_t frames = std::min(CHUNK_FRAMES, MAX_TOTAL_FRAMES - total_frames);
        if (frames == 0) {
            break;
        }
        render_chunk(frames);
    }

    if (fluid_player_get_status(player.get()) != FLUID_PLAYER_DONE || total_frames + TAIL_FRAMES > MAX_TOTAL_FRAMES) {
        throw std::runtime_error("Audio exceeds the 10-minute rendering limit");
    }
    // Render the release tail after MIDI playback completes.
    size_t tail_frames_rendered = 0;
    while (tail_frames_rendered < TAIL_FRAMES && total_frames < MAX_TOTAL_FRAMES) {
        size_t frames = std::min(CHUNK_FRAMES, TAIL_FRAMES - tail_frames_rendered);
        frames = std::min(frames, MAX_TOTAL_FRAMES - total_frames);
        if (frames == 0) {
            break;
        }
        render_chunk(frames);
        tail_frames_rendered += frames;
    }

    if (pcm_data.size() > 0xFFFFFFFFULL - 36ULL) {
        throw std::runtime_error("Rendered audio data exceeds 32-bit RIFF capacity");
    }

    std::string header = make_riff_wave_header(static_cast<uint32_t>(pcm_data.size()));
    header.append(pcm_data);
    return header;
}

} // namespace music
