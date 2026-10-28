// SPDX-License-Identifier: GPL-3.0-only
//
// Headless driver for MuseScore 4's audio engine. See engine_audio.h for the
// lifetime/threading contract and for the upstream call sequence.

#include "engine_audio.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include <fluidsynth.h>

#include "audio/common/audiosanitizer.h"
#include "audio/common/audioutils.h"
#include "audio/common/soundfonttypes.h"
#include "audio/engine/iengineplayback.h"
#include "audio/engine/isoundfontrepository.h"
#include "audio/engine/internal/enginecontroller.h"

#include "engraving/dom/excerpt.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/playback/playbackmodel.h"

#include "score_audio.h"
#include "score_excerpts.h"

#include "global/async/asyncable.h"
#include "global/async/processevents.h"
#include "global/io/ifilesystem.h"
#include "global/io/path.h"
#include "global/modularity/ioc.h"
#include "global/types/ret.h"
#include "global/types/string.h"
#include "global/types/uri.h"

#include "midi/imidioutport.h"
#include "mpe/playbacksetupdata.h"

#include "log.h"

using namespace muse;
using namespace muse::audio;
using namespace muse::modularity;
using namespace mu::engraving;

namespace music {
namespace {
constexpr sample_rate_t kSampleRate = 44100;
constexpr samples_t kSamplesPerChannel = 4096;
constexpr audioch_t kAudioChannelCount = 2;
constexpr double kPlaybackTailSecs = 3.0;
constexpr gain_t kDefaultAuxSend = 0.30f;
constexpr std::int64_t kDefaultTimeoutMs = 10 * 60 * 1000;
constexpr const char* kReverbTrackName = "Reverb";
constexpr const char* kAuxTrackName = "Aux 2";
constexpr const char* kMetronomeTrackName = "Metronome";
constexpr const char* kChordsTrackNamePrefix = "Chords.";

// Capturing midi::IMidiOutPort. FluidSynth sends every event it handles (after
// the staff -> channel remap), which is the event stream later golden tests
// compare. A global hook keeps the sink reusable by the probe and by tests.
EngineAudioMidiEventHook s_midi_event_hook;

class CapturingMidiOutPort final : public midi::IMidiOutPort
{
public:
    midi::MidiDeviceList availableDevices() const override { return {}; }
    async::Notification availableDevicesChanged() const override { return {}; }

    Ret connect(const midi::MidiDeviceID&) override { return make_ok(); }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    midi::MidiDeviceID deviceID() const override { return "engine-capture"; }
    async::Notification deviceChanged() const override { return {}; }

    bool supportsMIDI20Output() const override { return false; }

    Ret sendEvent(const midi::Event& event) override
    {
        if (s_midi_event_hook) {
            s_midi_event_hook(event);
        }

        return make_ok();
    }
};

// Process-wide state. The EngineController is intentionally leaked: the engine
// owns global IoC exports and static synthesizer state, and must outlive every
// static destructor (FluidResolver's Vorbis decoder is a process-wide pointer).
struct EngineState {
    std::shared_ptr<NullRpcChannel> rpcChannel;
    std::shared_ptr<CapturingMidiOutPort> midiOutPort;
    audio::engine::EngineController* controller = nullptr;
    AudioInputParams inputParams;
    std::string soundfontPath;
    std::string soundfontUri;
    std::string soundfontId;
};

EngineState& engine_state()
{
    static EngineState state;
    return state;
}

OutputSpec output_spec()
{
    OutputSpec spec;
    spec.sampleRate = kSampleRate;
    spec.samplesPerChannel = kSamplesPerChannel;
    spec.audioChannelCount = kAudioChannelCount;
    return spec;
}

audio::engine::IEnginePlayback* ensure_playback(std::string& error)
{
    auto service = globalIoc()->resolve<audio::engine::IEnginePlayback>("engine_audio");
    if (!service) {
        error = "audio engine playback service is not registered";
        return nullptr;
    }

    return service.get();
}

// Mirrors playbackcontroller.cpp:74-86 (makeReverbOutputParams).
muse::audio::AudioOutputParams make_reverb_output_params()
{
    AudioFxParams reverbParams;
    reverbParams.resourceMeta = makeReverbMeta();
    reverbParams.categories.insert(AudioFxCategory::FxReverb);
    reverbParams.chainOrder = 0;
    reverbParams.active = true;

    muse::audio::AudioOutputParams result;
    result.fxChain.emplace(reverbParams.chainOrder, std::move(reverbParams));
    return result;
}

SoundTrackFormat make_soundtrack_format(EngineAudioFormat format)
{
    SoundTrackFormat result;
    result.outputSpec = output_spec();

    switch (format) {
    case EngineAudioFormat::Wav:
        result.type = SoundTrackType::WAV;
        result.sampleFormat = AudioSampleFormat::Float32;
        result.bitRate = 0;
        break;
    case EngineAudioFormat::Flac:
        result.type = SoundTrackType::FLAC;
        result.sampleFormat = AudioSampleFormat::Int16;
        result.bitRate = 0;
        break;
    case EngineAudioFormat::Ogg:
        // Desktop OggWriter hard-codes 128 kbps (oggwriter.cpp:42).
        result.type = SoundTrackType::OGG;
        result.sampleFormat = AudioSampleFormat::Undefined;
        result.bitRate = 128;
        break;
    case EngineAudioFormat::Mp3:
        // Desktop default exportMp3Bitrate is 128
        // (audioexportconfiguration.cpp:44).
        result.type = SoundTrackType::MP3;
        result.sampleFormat = AudioSampleFormat::Undefined;
        result.bitRate = 128;
        break;
    case EngineAudioFormat::Aac:
        // Desktop AacWriter reuses exportMp3Bitrate for the AAC bitrate.
        result.type = SoundTrackType::AAC;
        result.sampleFormat = AudioSampleFormat::Undefined;
        result.bitRate = 128;
        break;
    }

    return result;
}

// Mirrors the desktop mixer title rules in playbackcontroller.cpp:1072-1103
// and resolveAuxTrackTitle() (:88-100). Translation is not available in the
// native host, so the English labels are used verbatim.
std::string track_name(const MasterScore* master, const PlaybackModel& model, const InstrumentTrackId& id)
{
    if (id == model.metronomeTrackId()) {
        return kMetronomeTrackName;
    }

    const Part* part = master->partById(id.partId);
    if (!part) {
        return id.instrumentId.toStdString();
    }

    if (model.isChordSymbolsTrack(id)) {
        return std::string(kChordsTrackNamePrefix) + part->partName().toStdString();
    }

    const Instrument* primary = part->instrument();
    if (primary && primary->id() == id.instrumentId) {
        return part->partName().toStdString();
    }

    if (const Instrument* instrument = part->instrumentById(id.instrumentId)) {
        return "(" + instrument->trackName().toStdString() + ")";
    }

    return id.instrumentId.toStdString();
}

void set_track_mix(muse::audio::AudioOutputParams& out, const EngineAudioTrackParams& params)
{
    out.volume = static_cast<volume_db_t>(params.volume_db);
    out.balance = static_cast<balance_t>(params.balance);
    out.solo = params.solo;
    out.muted = params.muted;
}

// Waits for EnginePlayback::saveSoundTrack()'s promise. The promise body runs
// from async::processMessages() on this thread, and the writer renders the
// whole file inside that body, so the waiter only observes the outcome.
class ExportWaiter final : public async::Asyncable
{
public:
    bool settled = false;
    bool ok = false;
    bool timedOut = false;
    std::string message;
};

struct SequenceGuard {
    audio::engine::IEnginePlayback* playback = nullptr;
    TrackSequenceId id = -1;

    ~SequenceGuard()
    {
        if (playback && id >= 0) {
            playback->removeSequence(id);
        }
    }
};

std::uint64_t file_size(const std::string& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::u8path(path), ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

void fluid_log_to_logger(int level, const char* message, void*)
{
    switch (level) {
    case FLUID_PANIC:
    case FLUID_ERR:
        LOGE() << message;
        break;
    case FLUID_WARN:
        LOGW() << message;
        break;
    default:
        LOGI() << message;
        break;
    }
}
} // namespace

void set_engine_audio_midi_event_hook(EngineAudioMidiEventHook hook)
{
    s_midi_event_hook = std::move(hook);
}

bool initialize_engine_audio(const std::string& soundfont_path, std::string* error)
{
    EngineState& state = engine_state();
    const auto fail = [error](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        return false;
    };

    if (state.controller) {
        if (!state.soundfontId.empty() && state.soundfontPath == soundfont_path) {
            return true;
        }
        if (state.soundfontId.empty()) {
            return fail("audio engine initialisation failed earlier; restart the process");
        }
        return fail("audio engine is already initialised with SoundFont '" + state.soundfontPath
                    + "' (only one engine and one SoundFont per process)");
    }

    if (soundfont_path.empty() || !synth::isSoundFont(soundfont_path)) {
        return fail("SoundFont must be an existing .sf2 or .sf3 file: " + soundfont_path);
    }

    // Uri::fromLocalFile() resolves the absolute path through global IFileSystem.
    if (!globalIoc()->resolve<io::IFileSystem>("engine_audio")) {
        return fail("native runtime (IFileSystem) is not initialised; call initialize_native_runtime() first");
    }

    // 1. Thread identity. Everything below (and every later engine call) runs
    // on this thread; the mixer stays single-threaded because
    // MUSE_THREADS_SUPPORT is undefined in the generated config.
    AudioSanitizer::setupMainThread();
    AudioSanitizer::setupEngineThread();

    // 2. Null rpc channel + capturing MIDI out. Replace pre-registered stubs
    // (e.g. a MIDI module) so the engine and FluidSynth resolve our exports.
    state.rpcChannel = std::make_shared<NullRpcChannel>();
    globalIoc()->unregister<rpc::IRpcChannel>("engine_audio");
    globalIoc()->registerExport<rpc::IRpcChannel>("engine_audio", state.rpcChannel);

    state.midiOutPort = std::make_shared<CapturingMidiOutPort>();
    globalIoc()->unregister<midi::IMidiOutPort>("engine_audio");
    globalIoc()->registerExport<midi::IMidiOutPort>("engine_audio", state.midiOutPort);

    // 3. Engine services and output configuration. The output spec must match
    // the export spec: FluidSynth is rebuilt on outputSpec changes
    // (fluidsynth.cpp:268-290).
    auto* controller = new audio::engine::EngineController(state.rpcChannel, modularity::globalCtx());
    state.controller = controller;
    controller->registerExports();
    controller->onStartRunning();

    AudioEngineConfig config;
    config.useSoundFontLowPassFilter = true; // desktop parity: audioconfiguration.cpp:100
    controller->init(output_spec(), config);

    // 4. SoundFont. addSoundFont() is synchronous for file: URIs
    // (soundfontrepository.cpp:145-147) and refreshes FluidResolver through
    // soundFontsChanged (fluidresolver.cpp:43-48).
    auto repository = globalIoc()->resolve<synth::ISoundFontRepository>("engine_audio");
    if (!repository) {
        return fail("audio engine SoundFont repository is not registered");
    }

    // The pinned FluidSynth default sink writes parser errors to stdout, which
    // corrupts a JSON CLI's output. FluidSynth::init() installs its own
    // per-synth callbacks later, so this only covers parsing here.
    fluid_set_log_function(FLUID_PANIC, fluid_log_to_logger, nullptr);
    fluid_set_log_function(FLUID_ERR, fluid_log_to_logger, nullptr);
    fluid_set_log_function(FLUID_WARN, fluid_log_to_logger, nullptr);
    fluid_set_log_function(FLUID_INFO, fluid_log_to_logger, nullptr);

    const Uri uri = Uri::fromLocalFile(io::path_t(soundfont_path));
    repository->addSoundFont(uri);

    const synth::SoundFontsMap& soundfonts = repository->soundFonts();
    auto it = soundfonts.find(uri);
    if (it == soundfonts.end()) {
        return fail("failed to load SoundFont '" + soundfont_path + "'");
    }

    state.soundfontPath = soundfont_path;
    state.soundfontUri = uri.toString();
    state.soundfontId = it->second.name;

    // 5. Explicit input resource for every track. Without it a track would
    // resolve to the engine default ("MS Basic", audioengineconfiguration.cpp:35-45),
    // which is absent from this process, and the track would be silent.
    AudioResourceMeta meta;
    meta.id = state.soundfontId;
    meta.vendor = "Fluid";
    meta.type = AudioResourceType::FluidSoundfont;
    meta.attributes = {
        { PLAYBACK_SETUP_DATA_ATTRIBUTE, mpe::GENERIC_SETUP_DATA_STRING },
        { synth::SOUNDFONT_NAME_ATTRIBUTE, String::fromStdString(state.soundfontId) },
    };
    meta.hasNativeEditorSupport = false;

    state.inputParams = AudioInputParams();
    state.inputParams.resourceMeta = std::move(meta);

    return true;
}

bool engine_audio_initialized()
{
    const EngineState& state = engine_state();
    return state.controller != nullptr && !state.soundfontId.empty();
}

std::string engine_soundfont_resource_id()
{
    return engine_state().soundfontId;
}

std::string engine_soundfont_uri()
{
    return engine_state().soundfontUri;
}

AudioRenderResult export_score_audio(MasterScore* master, const AudioRenderRequest& request)
{
    AudioRenderResult result;
    const auto fail = [&result](std::string message) {
        result.ok = false;
        result.error = std::move(message);
        return result;
    };

    const EngineState& state = engine_state();
    if (!state.controller || state.soundfontId.empty()) {
        return fail("audio engine is not initialised");
    }
    if (!master) {
        return fail("score is null");
    }
    if (request.output_path.empty()) {
        return fail("output path is empty");
    }

    MasterScore* masterScore = master;

    // Excerpt selection mutes every master part that is absent from, or
    // invisible in, the excerpt (masternotation.cpp:748-770).
    std::set<ID> mutedParts;
    bool excerptSelected = false;
    if (request.excerpt_id >= 0) {
        const std::vector<Excerpt*>& excerpts = masterScore->excerpts();
        if (static_cast<std::size_t>(request.excerpt_id) >= excerpts.size()) {
            return fail("excerpt id is out of range");
        }

        const Excerpt* excerpt = excerpts[static_cast<std::size_t>(request.excerpt_id)];
        if (!excerpt) {
            return fail("excerpt entry is null");
        }

        const Score* excerptScore = excerpt->excerptScore();
        if (!excerptScore || excerptScore->masterScore() != masterScore) {
            return fail("excerpt has no score owned by this master score");
        }

        for (const Part* part : masterScore->parts()) {
            if (!part) {
                continue;
            }
            const Part* excerptPart = excerptScore->partById(part->id());
            if (!excerptPart || !excerptPart->isVisible()) {
                mutedParts.insert(part->id());
            }
        }
        excerptSelected = true;
    }

    // PlaybackModel stays alive until the sequence has been removed; the audio
    // tracks reference its playback data streams.
    PlaybackModel model(modularity::globalCtx());
    model.setPlayRepeats(request.use_repeats);
    model.setPlayChordSymbols(false);
    model.setIsMetronomeEnabled(false);
    model.load(masterScore);

    const InstrumentTrackIdSet existingTrackIds = model.existingTrackIdSet();
    if (existingTrackIds.empty()) {
        return fail("score has no playable tracks");
    }

    // Repeat-expanded play time plus the upstream 3 s tail
    // (notationplayback.cpp:235-252). The tail contains reverb/note decay; the
    // writer truncates everything after it (soundtrackwriter.cpp:199-214).
    const int lastUtick = masterScore->repeatList(request.use_repeats).ticks();
    const double playTime = masterScore->utick2utime(lastUtick);
    const msecs_t totalDurationMs = secsToMilisecs(playTime + kPlaybackTailSecs);
    if (totalDurationMs <= 0) {
        return fail("score has no audio duration");
    }

    const std::uint64_t totalFrames = static_cast<std::uint64_t>(totalDurationMs) * 1000ULL
                                      * kSampleRate / 1000000ULL;

    std::string error;
    audio::engine::IEnginePlayback* enginePlayback = ensure_playback(error);
    if (!enginePlayback) {
        return fail(std::move(error));
    }

    const TrackSequenceId sequenceId = enginePlayback->addSequence();
    SequenceGuard sequenceGuard{ enginePlayback, sequenceId };

    muse::audio::AudioOutputParams masterOutput;
    masterOutput.volume = static_cast<volume_db_t>(request.master_volume_db);
    masterOutput.balance = static_cast<balance_t>(request.master_balance);
    enginePlayback->setMasterOutputParams(masterOutput);

    // The public mix is indexed by the same deterministic track list that
    // score_audio_tracks reports. Keep the list for the selected excerpt: for
    // a part export it contains only the excerpt's parts, while master tracks
    // outside it keep neutral values and are muted below.
    const std::vector<EngineAudioTrackInfo> mixTracks = engine_audio_tracks(masterScore, request.excerpt_id);
    std::map<InstrumentTrackId, std::size_t> mixIndex;
    for (std::size_t index = 0; index < mixTracks.size(); ++index) {
        mixIndex.emplace(mixTracks[index].id, index);
    }
    const auto trackParams = [&request](std::size_t index) {
        return index < request.tracks.size() ? request.tracks[index] : EngineAudioTrackParams{};
    };

    // updateSoloMuteStates() derives forceMute from any non-metronome solo.
    bool hasSolo = false;
    for (std::size_t index = 0; index < mixTracks.size(); ++index) {
        if (mixTracks[index].id == model.metronomeTrackId()) {
            continue;
        }
        if (trackParams(index).solo) {
            hasSolo = true;
            break;
        }
    }

    // Deterministic track order: upstream iterates the unordered track set, but
    // deterministic ids/mix order are needed for byte-identical exports.
    std::vector<InstrumentTrackId> orderedTrackIds(existingTrackIds.cbegin(), existingTrackIds.cend());
    std::sort(orderedTrackIds.begin(), orderedTrackIds.end());

    for (const InstrumentTrackId& trackId : orderedTrackIds) {
        const mpe::PlaybackData& playbackData = model.resolveTrackPlaybackData(trackId);
        if (!playbackData.isValid()) {
            continue;
        }

        const bool isMetronome = trackId == model.metronomeTrackId();
        const bool isChordSymbols = model.isChordSymbolsTrack(trackId);

        const auto index = mixIndex.find(trackId);
        EngineAudioTrackParams mix;
        if (index != mixIndex.end()) {
            mix = trackParams(index->second);
        }

        // Fold forceMute as PlaybackController::updateSoloMuteStates does:
        // params.muted = mute || forceMute (the engine only honours `muted`,
        // mixerchannel.cpp:67-69,215). Chord-symbol and metronome tracks are
        // force-muted because chord playback and the metronome are off.
        const bool forceMute = !isMetronome && hasSolo && !mix.solo;

        muse::audio::AudioOutputParams out;
        set_track_mix(out, mix);
        out.muted = mix.muted || forceMute || isChordSymbols || isMetronome
                    || (excerptSelected && mutedParts.count(trackId.partId) > 0);
        out.forceMute = forceMute;
        if (!isMetronome) {
            // Desktop adds the two Fluid default sends only for non-metronome
            // tracks (playbackcontroller.cpp:1137-1145).
            out.auxSends = { AuxSendParams { kDefaultAuxSend, true }, AuxSendParams { kDefaultAuxSend, true } };
        }

        AudioParams params;
        params.in = state.inputParams;
        params.out = out;

        const std::string name = track_name(masterScore, model, trackId);
        RetVal2<TrackId, AudioParams> added = enginePlayback->addTrack(sequenceId, name, playbackData, params);
        if (!added.ret) {
            return fail("addTrack failed: " + added.ret.toString());
        }

        enginePlayback->setOutputParams(sequenceId, added.val1, out);
    }

    // Aux 0 is the Muse Reverb converter-parity send target; aux 1 is empty
    // (desktop adds AUX_CHANNEL_NUM tracks, playbackcontroller.cpp:1432-1434).
    RetVal2<TrackId, muse::audio::AudioOutputParams> reverb = enginePlayback->addAuxTrack(sequenceId, kReverbTrackName,
                                                                            make_reverb_output_params());
    if (!reverb.ret) {
        return fail("addAuxTrack(reverb) failed: " + reverb.ret.toString());
    }
    RetVal2<TrackId, muse::audio::AudioOutputParams> aux2 = enginePlayback->addAuxTrack(sequenceId, kAuxTrackName,
                                                                          muse::audio::AudioOutputParams());
    if (!aux2.ret) {
        return fail("addAuxTrack(aux 2) failed: " + aux2.ret.toString());
    }

    enginePlayback->setDuration(sequenceId, totalDurationMs);

    // libopusenc picks the Ogg stream serial and the Opus encoder dithers with
    // rand(). Seed after all engine setup (synth/sequence creation) and before
    // the writer starts, so repeated exports in one worker process - and the
    // same request in sync and async workers - are byte-identical.
    std::srand(0);

    ExportWaiter waiter;
    enginePlayback->saveSoundTrack(sequenceId, io::path_t(request.output_path), make_soundtrack_format(request.format))
    .onResolve(&waiter, [&waiter](const Ret& ret) {
        waiter.settled = true;
        waiter.ok = static_cast<bool>(ret);
        waiter.message = ret.toString();
    });

    const std::int64_t timeoutMs = request.timeout_ms > 0 ? request.timeout_ms : kDefaultTimeoutMs;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!waiter.settled) {
        // The promise body (and therefore the whole writer) runs inside this
        // pump; onResolve is delivered synchronously from resolve().
        async::processMessages();

        if (waiter.settled) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            waiter.timedOut = true;
            enginePlayback->abortSavingAllSoundTracks();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (waiter.timedOut) {
        return fail("timed out waiting for the audio export");
    }
    if (!waiter.ok) {
        return fail(waiter.message.empty() ? "audio export failed" : waiter.message);
    }

    result.ok = true;
    result.frames = totalFrames;
    result.play_time_secs = playTime;
    result.duration_secs = static_cast<double>(totalDurationMs) / 1000.0;
    result.bytes = file_size(request.output_path);
    if (result.bytes == 0) {
        result.ok = false;
        result.error = "audio export reported success but wrote no data";
    }

    return result;
}

std::vector<EngineAudioTrackInfo> engine_audio_tracks(MasterScore* master, int excerpt_id)
{
    if (!master) {
        throw std::invalid_argument("engine_audio_tracks: master score is null");
    }

    mu::engraving::Score* selected = nullptr;
    try {
        selected = select_score(master, excerpt_id);
    } catch (const std::out_of_range& error) {
        throw std::invalid_argument(error.what());
    }

    std::vector<EngineAudioTrackInfo> result;
    for (const ScoreAudioTrackInfo& description : score_audio_track_info(selected)) {
        EngineAudioTrackInfo info;
        info.id = description.id;
        info.part_id = description.part_id;
        info.instrument_id = description.instrument_id;
        info.part_name = description.part_name;
        info.instrument_name = description.instrument_name;
        info.chord_symbols = description.chord_symbols;
        info.part_index = description.part_index;
        result.push_back(std::move(info));
    }

    return result;
}
} // namespace music
