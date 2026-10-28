// SPDX-License-Identifier: GPL-3.0-only
//
// Headless driver for MuseScore 4's audio engine (roadmap step 5, part B1).
//
// This bypasses the desktop application/RPC main side and drives the Qt-free
// engine in-process, following the same call sequence as the desktop
// StartAudioController and PlaybackController (paths relative to
// vendor/musescore/src):
//   - framework/audio/engine/internal/enginecontroller.cpp:84-158
//   - playback/internal/playbackcontroller.cpp:74-86, 1105-1235, 1400-1460,
//     1497-1554
//   - notation/internal/notationplayback.cpp:77-82, 235-252
//   - engraving/playback/playbackmodel.cpp:65-105
//   - framework/audio/engine/internal/export/soundtrackwriter.cpp:60-140
//
// The engine keeps process-wide state (global IoC exports, static engine
// sequence ids, the global FluidSynth Vorbis decoder, the lock-free SoundFont
// cache) and we run it single-threaded: MUSE_THREADS_SUPPORT stays undefined,
// so the mixer never starts worker threads. Therefore:
//   - only one engine may exist per process;
//   - initialize_engine_audio() must be called once, before any export;
//   - every engine call (bootstrap, export, track listing) must happen on the
//     same OS thread. It is not safe to export from two threads or to run two
//     exports at the same time;
//   - the process must not use the retired MIDI->FluidSynth renderer
//     (audio_session) for the same score while the engine is initialised.
//
// initialize_engine_audio() needs the native runtime already set up
// (music::initialize_native_runtime): Uri::fromLocalFile resolves through
// IFileSystem and PlaybackModel resolves the MPE articulation profile
// repository from the global IoC.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "audio/common/rpc/irpcchannel.h"
#include "engraving/types/types.h"
#include "midi/miditypes.h"

namespace mu::engraving {
class MasterScore;
}

namespace music {

// No-op rpc::IRpcChannel for the in-process engine. The engine itself only
// sends notifications and queue-rendering calls SoundTrackWriter::rpcChannel()
// -> process() (soundtrackwriter.cpp:178,196,213); both are no-ops here.
class NullRpcChannel final : public muse::audio::rpc::IRpcChannel
{
public:
    void setupOnMain() override {}
    void setupOnEngine() override {}

    void process() override {}

    void send(const muse::audio::rpc::Msg&, const muse::audio::rpc::Handler& = nullptr) override {}
    void onMethod(muse::audio::rpc::Method, muse::audio::rpc::Handler) override {}
    void listenAll(muse::audio::rpc::Handler) override {}

    void addStream(std::shared_ptr<muse::audio::rpc::IRpcStream>) override {}
    void removeStream(muse::audio::rpc::StreamId) override {}
    void sendStream(const muse::audio::rpc::StreamMsg&) override {}
    void onStream(muse::audio::rpc::StreamId, muse::audio::rpc::StreamHandler) override {}
};

// Hook for MIDI event goldens. The engine registers a capturing IMidiOutPort;
// every event FluidSynth actually handles (after the staff % 16 channel remap,
// fluidsynth.cpp:251-262) is forwarded to the hook synchronously from the
// render call. Events applied by FluidSynth outside handleEvent are not
// captured: setup-time program/bank/CC7/CC11 from setupSound(), and the
// expression/pitch-bend changes from flushSound()/setPlaybackPosition()
// (fluidsynth.cpp:328-347,483-516). An empty function disables capture. Events
// are sent on the engine thread; the hook must not re-enter the engine.
using EngineAudioMidiEventHook = std::function<void(const muse::midi::Event&)>;
void set_engine_audio_midi_event_hook(EngineAudioMidiEventHook hook);

// Initialises the process-wide engine and loads one SoundFont:
//   AudioSanitizer setup, NullRpcChannel registered as rpc::IRpcChannel,
//   capturing IMidiOutPort registered, EngineController::registerExports(),
//   onStartRunning(), init({44100, 4096, 2}, {useSoundFontLowPassFilter=true}),
//   ISoundFontRepository::addSoundFont(Uri::fromLocalFile(path)).
// The SoundFont resource id is its complete basename ("FluidR3Mono_GM" for
// ".../FluidR3Mono_GM.sf3"), matching FluidSoundFontParser::parseSoundFont
// (fluidsoundfontparser.cpp:70) and FluidResolver::refresh
// (fluidresolver.cpp:113-125).
//
// Returns false and fills `error` (when not null) if the engine was already
// initialised with a different SoundFont, the path is not .sf2/.sf3, the
// native runtime/IFileSystem is missing, or the font cannot be parsed. A
// failed call after EngineController was constructed leaves the process
// without a usable engine; exit instead of retrying.
bool initialize_engine_audio(const std::string& soundfont_path, std::string* error = nullptr);
bool engine_audio_initialized();

// SoundFont identity of the successfully initialised engine (empty when not
// initialised). The URI is the encoded "file://" form used as the repository
// key; it lets callers verify the Windows drive-letter/UTF-8 round trip.
std::string engine_soundfont_resource_id();
std::string engine_soundfont_uri();

enum class EngineAudioFormat {
    Wav,
    Flac,
    Ogg,
    Mp3,
    Aac,
};

// Per-track mix override, keyed by mu::engraving::InstrumentTrackId. Tracks
// without an entry use neutral values. `muted` is folded with the derived
// forceMute (any solo and this track is not solo) and with the engine's
// chord-symbol/metronome mute rules; see export_score_audio().
struct EngineAudioTrackParams {
    double volume_db = 0.0;
    double balance = 0.0;
    bool solo = false;
    bool muted = false;
};

struct AudioRenderRequest {
    EngineAudioFormat format = EngineAudioFormat::Wav;
    std::string output_path;
    // -1 renders the master score. A non-negative value renders the master
    // score with every part that is absent from, or invisible in, that excerpt
    // muted, like MasterNotation::initNotationSoloMuteState
    // (masternotation.cpp:748-770).
    int excerpt_id = -1;
    bool use_repeats = true;
    double master_volume_db = 0.0;
    double master_balance = 0.0;
    std::map<mu::engraving::InstrumentTrackId, EngineAudioTrackParams> tracks;
    // Wait budget for the saveSoundTrack promise, in milliseconds; <= 0 uses
    // 10 minutes. The writer itself renders synchronously while the promise
    // body is pumped, so the timeout only catches a promise that never runs;
    // on expiry the export is aborted at the next pump boundary.
    std::int64_t timeout_ms = 0;
};

struct AudioRenderResult {
    bool ok = false;
    std::string error;
    // Total sample frames the writer emits: (repeat-expanded play time + 3 s,
    // truncated to milliseconds) at 44100 Hz. Computed from the same duration
    // handed to SoundTrackWriter (soundtrackwriter.cpp:74-82); the writer has
    // no frames-written return value.
    std::uint64_t frames = 0;
    // Repeat-expanded score play time, without the 3 s tail.
    double play_time_secs = 0.0;
    // Audio span of the written file: play_time_secs + 3 s, in milliseconds.
    double duration_secs = 0.0;
    // File size after a successful export; 0 when the file does not exist.
    std::uint64_t bytes = 0;
};

// Renders `master` with the engine and writes one audio file. The request
// format selects the upstream encoder defaults:
//   WAV  Float32, FLAC Int16, MP3 128 kbps, OGG 128 kbps (Opus in Ogg),
//   AAC  128 kbps (ADTS; desktop AacWriter reuses exportMp3Bitrate).
// The engine renders 44100 Hz stereo in 4096-frame chunks. Ogg/Opus is
// resampled to 48 kHz inside libopusenc (upstream OggWriter behavior too), so
// the decoded Ogg stream is 48 kHz while its duration follows the engine
// render.
//
// Setup mirrors desktop playback/export:
//   PlaybackModel on `master` (repeats as requested, chord symbols off,
//   metronome off) kept alive for the whole export;
//   addSequence; addTrack per playable InstrumentTrackId with
//   resolveTrackPlaybackData() and an explicit Fluid input resource (so a
//   track never falls back to the nonexistent default "MS Basic" resource);
//   out.auxSends = {{0.30,true},{0.30,true}}; aux 0 is Muse Reverb (copied
//   from playbackcontroller.cpp:74-86), aux 1 is an empty output;
//   setMasterOutputParams; setOutputParams per track with muted folded;
//   setDuration(play time + 3 s); saveSoundTrack() and processMessages()
//   until the promise resolves.
//
// The metronome and chord-symbol tracks are added when the PlaybackModel
// reports them, but are always muted (the metronome/chord playback switches
// are off, like PlaybackController::trackOutputParams).
//
// Never throws for a bad score/format/engine state; inspect AudioRenderResult.
AudioRenderResult export_score_audio(mu::engraving::MasterScore* master, const AudioRenderRequest& request);

// One playable track for score_audio_tracks. Part and instrument ids are the
// raw ids from the score (muse::ID hex string / muse::String). The
// chord-symbol pseudo-track has chord_symbols == true and an empty
// instrument_name. The metronome pseudo-track is not listed because it is not
// addressable through a part/instrument identity.
struct EngineAudioTrackInfo {
    mu::engraving::InstrumentTrackId id;
    std::string part_id;
    std::string instrument_id;
    std::string part_name;
    std::string instrument_name;
    bool chord_symbols = false;
    // Zero-based index in MasterScore::parts().
    std::size_t part_index = 0;
};

// Lists parts, their instrument tracks and chord-symbol pseudo-tracks in score
// order (no rendering). Throws std::invalid_argument when master is null.
std::vector<EngineAudioTrackInfo> engine_audio_tracks(mu::engraving::MasterScore* master);

} // namespace music
