// Native score -> audio-stem helpers.
//
// The musical rendering is entirely upstream: ExportMidi renders the score and
// MidiFile serializes the result. This file only partitions that rendered MIDI
// by score part and adapts a few in-memory representations to MidiFile's
// read/write conventions. It deliberately contains no byte-level MIDI parser.

#include "score_audio.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "global/io/buffer.h"
#include "global/io/iodevice.h"
#include "global/serialization/json.h"
#include "global/serialization/zipreader.h"

#include "engraving/compat/midi/midicoreevent.h"
#include "engraving/dom/instrument.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/part.h"
#include "engraving/dom/score.h"
#include "engraving/dom/staff.h"

#include "importexport/midi/internal/midiexport/exportmidi.h"
#include "importexport/midi/internal/midishared/midifile.h"

namespace music {
namespace {

using mu::engraving::Part;
using mu::engraving::Score;
using mu::engraving::Staff;
using mu::iex::midi::MidiEvent;
using mu::iex::midi::MidiFile;
using mu::iex::midi::MidiTrack;

// Mirrors the total MIDI budget enforced by AudioSession::Impl.
constexpr std::size_t max_total_midi_bytes = 8 * 1024 * 1024;
// audiosettings.json is tiny; reject pathological archives before parsing.
constexpr std::size_t max_audio_settings_bytes = 1024 * 1024;
constexpr std::size_t max_audio_settings_tracks = 4096;

std::optional<double> settings_number(const muse::JsonObject& object, const char* key)
{
    if (!object.contains(key)) return std::nullopt;
    const auto value = object.value(key);
    if (!value.isNumber()) return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number)) return std::nullopt;
    return number;
}

std::optional<bool> settings_bool(const muse::JsonObject& object, const char* key)
{
    if (!object.contains(key)) return std::nullopt;
    const auto value = object.value(key);
    if (!value.isBool()) return std::nullopt;
    return value.toBool();
}

// Unknown fields are ignored by design. Out-of-range known fields are ignored
// individually so one corrupt value cannot discard the rest of the mix.
void apply_settings_output(AudioOutputParams& output, const muse::JsonObject& object)
{
    if (const auto volume = settings_number(object, "volumeDb");
        volume && *volume >= -120.0 && *volume <= 24.0) {
        output.volume_db = *volume;
    }
    if (const auto balance = settings_number(object, "balance");
        balance && *balance >= -1.0 && *balance <= 1.0) {
        output.balance = *balance;
    }
}

struct PartPlan {
    std::int64_t track_id = 0;
    Part* part = nullptr;
    std::string part_id;
    std::string instrument_id;
    std::string name;
    std::vector<std::size_t> staff_track_indexes;
};

std::string part_label(std::size_t index, const Part& part)
{
    return "part " + std::to_string(index) + " ('" + part.partName().toStdString() + "')";
}

std::string describe_instruments(const Part& part)
{
    std::string result;
    for (const auto& entry : part.instruments()) {
        if (!result.empty()) {
            result += ", ";
        }
        result += entry.second ? entry.second->id().toStdString() : std::string("<null>");
        result += "@" + std::to_string(entry.first);
    }
    return result;
}

// A score is represented by one stem per part, and therefore by one logical
// instrument per part. ExportMidi iterates every instrument and every
// articulation channel of a part and writes all of them into that part's staff
// tracks, with initial controller/program setup at tick 0. Splitting those
// tracks requires instrument/channel ownership mapping that this helper does
// not yet provide, so reject the ambiguous case instead of mixing instruments.
std::vector<PartPlan> build_plans(Score* score)
{
    if (!score) {
        throw std::invalid_argument("score_audio: score must not be null");
    }

    const std::vector<Part*>& parts = score->parts();
    const std::vector<Staff*>& staves = score->staves();

    if (parts.empty()) {
        if (!staves.empty()) {
            throw std::runtime_error("score_audio: score has staves but no parts");
        }
        return {};
    }

    if (parts.size() > audio_max_tracks) {
        throw std::invalid_argument("score_audio: score has " + std::to_string(parts.size())
                                    + " parts; at most " + std::to_string(audio_max_tracks)
                                    + " audio stems are supported");
    }
    if (staves.empty()) {
        throw std::runtime_error("score_audio: score has parts but no staves");
    }

    std::vector<bool> staff_seen(staves.size(), false);
    std::vector<PartPlan> plans;
    plans.reserve(parts.size());

    for (std::size_t part_index = 0; part_index < parts.size(); ++part_index) {
        Part* part = parts[part_index];
        if (!part) {
            throw std::runtime_error("score_audio: part index " + std::to_string(part_index) + " is null");
        }

        if (part->staves().empty()) {
            throw std::runtime_error("score_audio: " + part_label(part_index, *part) + " has no staves");
        }

        for (const auto& instrument_entry : part->instruments()) {
            if (!instrument_entry.second) {
                throw std::runtime_error("score_audio: " + part_label(part_index, *part)
                                         + " contains a null instrument");
            }
        }

        const mu::engraving::InstrumentTrackIdList instrument_tracks = part->instrumentTrackIdList();
        if (instrument_tracks.empty()) {
            throw std::runtime_error("score_audio: " + part_label(part_index, *part) + " has no instrument");
        }
        if (instrument_tracks.size() > 1) {
            throw std::runtime_error("score_audio: " + part_label(part_index, *part)
                                     + " has " + std::to_string(instrument_tracks.size())
                                     + " logical instruments (" + describe_instruments(*part)
                                     + "); multi-instrument parts are unsupported because ExportMidi"
                                       " merges their channels into the part's staff tracks");
        }

        PartPlan plan;
        plan.track_id = static_cast<std::int64_t>(part_index);
        plan.part = part;
        plan.part_id = part->id().toStdString();
        // Tracks are keyed by part index; the instrument id is descriptive only
        // and some importers (Guitar Pro) leave it empty.
        plan.instrument_id = instrument_tracks.front().instrumentId.toStdString();
        plan.name = part->partName().toStdString();

        std::vector<std::size_t> indexes;
        indexes.reserve(part->staves().size());
        for (Staff* staff : part->staves()) {
            if (!staff) {
                throw std::runtime_error("score_audio: " + part_label(part_index, *part)
                                         + " contains a null staff");
            }
            if (staff->part() != part) {
                throw std::runtime_error("score_audio: " + part_label(part_index, *part)
                                         + " references a staff owned by a different part");
            }

            const mu::engraving::staff_idx_t staff_index = score->staffIdx(staff);
            if (staff_index >= staves.size() || staves[staff_index] != staff) {
                throw std::runtime_error("score_audio: " + part_label(part_index, *part)
                                         + " references a staff that is not in Score::staves()");
            }
            if (staff_seen[staff_index]) {
                throw std::runtime_error("score_audio: staff " + std::to_string(staff_index)
                                         + " belongs to more than one part");
            }
            staff_seen[staff_index] = true;
            indexes.push_back(static_cast<std::size_t>(staff_index));
        }

        std::sort(indexes.begin(), indexes.end());
        if (std::adjacent_find(indexes.begin(), indexes.end()) != indexes.end()) {
            throw std::runtime_error("score_audio: " + part_label(part_index, *part)
                                     + " lists the same staff more than once");
        }
        plan.staff_track_indexes = std::move(indexes);
        plans.push_back(std::move(plan));
    }

    for (std::size_t i = 0; i < staff_seen.size(); ++i) {
        if (!staff_seen[i]) {
            throw std::runtime_error("score_audio: staff " + std::to_string(i)
                                     + " does not belong to any score part");
        }
    }

    return plans;
}

// ExportMidi indexes midiMapping with InstrChannel::channel() without a bounds
// check. Validate the mapping first so an uninitialized or broken score gets a
// descriptive exception instead of undefined behaviour inside the exporter.
void validate_export_channels(Score* score, const std::vector<PartPlan>& plans)
{
    mu::engraving::MasterScore* master = score->masterScore();
    if (!master) {
        throw std::runtime_error("score_audio: score has no master score for MIDI mapping");
    }
    const std::vector<mu::engraving::MidiMapping>& mapping = master->midiMapping();
    if (mapping.empty()) {
        throw std::runtime_error("score_audio: score MIDI mapping is not initialized");
    }

    for (const PartPlan& plan : plans) {
        if (plan.part->instruments().empty()) {
            throw std::runtime_error("score_audio: " + part_label(static_cast<std::size_t>(plan.track_id), *plan.part)
                                     + " has no instrument");
        }
        for (const auto& instrument_entry : plan.part->instruments()) {
            const mu::engraving::Instrument* instrument = instrument_entry.second;
            if (!instrument) {
                throw std::runtime_error("score_audio: " + part_label(static_cast<std::size_t>(plan.track_id), *plan.part)
                                         + " contains a null instrument");
            }
            if (instrument->channel().empty()) {
                throw std::runtime_error("score_audio: " + part_label(static_cast<std::size_t>(plan.track_id), *plan.part)
                                         + " has an instrument with no MIDI channel");
            }
            for (const mu::engraving::InstrChannel* channel : instrument->channel()) {
                if (!channel) {
                    throw std::runtime_error("score_audio: " + part_label(static_cast<std::size_t>(plan.track_id), *plan.part)
                                             + " contains a null instrument channel");
                }
                const int mapping_index = channel->channel();
                if (mapping_index < 0 || static_cast<std::size_t>(mapping_index) >= mapping.size()) {
                    throw std::runtime_error("score_audio: " + part_label(static_cast<std::size_t>(plan.track_id), *plan.part)
                                             + " has an instrument channel with invalid MIDI mapping index "
                                             + std::to_string(mapping_index));
                }
                if (!master->playbackChannel(channel)) {
                    throw std::runtime_error("score_audio: " + part_label(static_cast<std::size_t>(plan.track_id), *plan.part)
                                             + " has an instrument channel without a playback mapping");
                }
                const int port = master->midiPort(mapping_index);
                const int midi_channel = master->midiChannel(mapping_index);
                if (port < 0 || port > 127) {
                    throw std::runtime_error("score_audio: " + part_label(static_cast<std::size_t>(plan.track_id), *plan.part)
                                             + " has an invalid MIDI port " + std::to_string(port));
                }
                if (midi_channel < 0 || midi_channel > 15) {
                    throw std::runtime_error("score_audio: " + part_label(static_cast<std::size_t>(plan.track_id), *plan.part)
                                             + " has an invalid MIDI channel " + std::to_string(midi_channel));
                }
            }
        }
    }
}

// ExportMidi puts time signatures and tempo only on track 0, and rehearsal
// markers on track 0 when staff 0 is present. Key signatures are staff-local;
// preserve them with text, channel-prefix, and port-change events in each
// selected staff rather than substituting the first staff's key for all parts.
bool is_conductor_meta(const MidiEvent& event)
{
    if (event.type() != mu::engraving::ME_META) {
        return false;
    }
    switch (event.metaType()) {
    case mu::engraving::META_TEMPO:
    case mu::engraving::META_TIME_SIGNATURE:
    case mu::engraving::META_MARKER:
        return true;
    default:
        return false;
    }
}

// MidiFile's read/write representation is not a perfect round trip:
//  * readEvent() leaves program changes as ME_PROGRAM, but writeEvent() has no
//    ME_PROGRAM case. Convert them back to the exporter's internal form
//    (ME_CONTROLLER + CTRL_PROGRAM) so the program change is not dropped.
//  * For text meta events (types 1..0x14), writeEvent() writes len-1 bytes and
//    assumes the length includes a terminating NUL, while readEvent() reports
//    the length actually stored in the file. Re-add the implicit NUL.
MidiEvent canonical_event(const MidiEvent& source)
{
    if (source.type() == mu::engraving::ME_PROGRAM) {
        MidiEvent program;
        program.setType(mu::engraving::ME_CONTROLLER);
        program.setChannel(source.channel());
        program.setController(mu::engraving::CTRL_PROGRAM);
        program.setValue(source.value());
        return program;
    }

    MidiEvent result = source;
    if (source.type() == mu::engraving::ME_META
        && source.metaType() >= mu::engraving::META_TEXT
        && source.metaType() <= mu::engraving::META_POET) {
        const int length = source.len();
        if (length < 0) {
            throw std::runtime_error("score_audio: exported MIDI contains a negative meta-event length");
        }
        std::vector<unsigned char> data(static_cast<std::size_t>(length) + 1, 0);
        if (length > 0) {
            if (!source.edata()) {
                throw std::runtime_error("score_audio: exported MIDI contains a text meta event without data");
            }
            std::copy_n(source.edata(), length, data.begin());
        }
        result.setLen(length + 1);
        result.setEData(std::move(data));
    }
    return result;
}

void insert_track_name(MidiTrack& track, const std::string& name)
{
    // MidiFile::writeEvent() expects text meta data to include a terminating
    // NUL and writes len-1 bytes, exactly like ExportMidi does for names.
    std::vector<unsigned char> data(name.begin(), name.end());
    data.push_back(0);

    MidiEvent event;
    event.setType(mu::engraving::ME_META);
    event.setMetaType(mu::engraving::META_TRACK_NAME);
    event.setLen(static_cast<int>(data.size()));
    event.setEData(std::move(data));
    track.insert(0, event);
}

bool has_mthd_header(const std::uint8_t* data, std::size_t size)
{
    if (!data || size < 14) {
        return false;
    }
    if (data[0] != 'M' || data[1] != 'T' || data[2] != 'h' || data[3] != 'd') {
        return false;
    }
    const std::uint32_t header_length =
        (static_cast<std::uint32_t>(data[4]) << 24)
        | (static_cast<std::uint32_t>(data[5]) << 16)
        | (static_cast<std::uint32_t>(data[6]) << 8)
        | static_cast<std::uint32_t>(data[7]);
    return header_length >= 6 && header_length <= size - 8;
}

std::string render_part_stem(const MidiFile& source, const PartPlan& plan)
{
    const std::vector<MidiTrack>& source_tracks = source.tracks();
    if (source_tracks.empty()) {
        throw std::runtime_error("score_audio: exported MIDI has no tracks");
    }

    MidiFile output;
    output.setFormat(1);
    output.setDivision(source.division());
    std::vector<MidiTrack>& output_tracks = output.tracks();
    output_tracks.reserve(plan.staff_track_indexes.size() + 1);

    // Track 0: conductor/global events from the exporter's track 0. Only meta
    // events are copied here, so no other part's notes/controllers leak in.
    MidiTrack conductor;
    conductor.setOutChannel(0);
    insert_track_name(conductor, plan.name);
    for (const auto& entry : source_tracks.front().events()) {
        if (is_conductor_meta(entry.second)) {
            conductor.insert(entry.first, canonical_event(entry.second));
        }
    }
    output_tracks.push_back(std::move(conductor));

    // One track per selected staff, in score order. MidiFile::read() sets
    // outChannel to -1 and MidiFile::write() skips tracks whose outChannel is
    // -1; the gating value does not affect the per-event channel numbers.
    for (const std::size_t track_index : plan.staff_track_indexes) {
        if (track_index >= source_tracks.size()) {
            throw std::runtime_error("score_audio: staff track index " + std::to_string(track_index)
                                     + " is outside the exported MIDI track list");
        }

        MidiTrack staff_track;
        staff_track.setOutChannel(0);
        for (const auto& entry : source_tracks[track_index].events()) {
            if (!is_conductor_meta(entry.second)) {
                staff_track.insert(entry.first, canonical_event(entry.second));
            }
        }
        output_tracks.push_back(std::move(staff_track));
    }

    muse::io::Buffer buffer;
    if (!buffer.open(muse::io::IODevice::WriteOnly)) {
        throw std::runtime_error("score_audio: failed to open in-memory MIDI output buffer");
    }

    // Verified against midifile.cpp: MidiFile::write() returns true on error.
    // ExportMidi::write() negates that value and returns true on success.
    if (output.write(&buffer)) {
        throw std::runtime_error("score_audio: failed to serialize stem for part "
                                 + std::to_string(plan.track_id));
    }

    const muse::ByteArray& bytes = buffer.data();
    if (bytes.empty()) {
        throw std::runtime_error("score_audio: serialized stem is empty for part "
                                 + std::to_string(plan.track_id));
    }
    if (!has_mthd_header(bytes.constData(), bytes.size())) {
        throw std::runtime_error("score_audio: serialized stem has an invalid MIDI header for part "
                                 + std::to_string(plan.track_id));
    }

    return std::string(reinterpret_cast<const char*>(bytes.constData()), bytes.size());
}

} // namespace

std::vector<ScoreAudioTrackInfo> score_audio_track_info(Score* score)
{
    const std::vector<PartPlan> plans = build_plans(score);

    std::vector<ScoreAudioTrackInfo> result;
    result.reserve(plans.size());
    for (const PartPlan& plan : plans) {
        result.push_back(ScoreAudioTrackInfo{
            plan.track_id,
            plan.part_id,
            plan.instrument_id,
            plan.name,
        });
    }
    return result;
}

bool json_nesting_ok(std::string_view text, std::size_t max_depth = 64)
{
    std::size_t depth = 0;
    bool in_string = false;
    bool escape = false;
    for (char c : text) {
        if (escape) {
            escape = false;
            continue;
        }
        if (in_string) {
            if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            ++depth;
            if (depth > max_depth) return false;
        } else if (c == '}' || c == ']') {
            if (depth > 0) {
                --depth;
            }
        }
    }
    return true;
}

const muse::ZipReader::FileInfo* find_zip_entry(const std::vector<muse::ZipReader::FileInfo>& entries,
                                                std::string_view name)
{
    for (const auto& entry : entries) {
        if (entry.filePath.toStdString() == name) {
            return &entry;
        }
    }
    return nullptr;
}

ScoreAudioMixDefaults score_audio_mix_defaults(const std::string& mscz_bytes)
{
    ScoreAudioMixDefaults result;
    if (mscz_bytes.size() < 4) return result;
    try {
        muse::ByteArray archive(mscz_bytes.data(), mscz_bytes.size());
        muse::io::Buffer buffer(&archive);
        if (!buffer.open(muse::io::IODevice::ReadOnly)) return result;
        muse::ZipReader zip(&buffer);
        if (zip.hasError()) return result;

        const auto file_infos = zip.fileInfoList();
        if (file_infos.empty() || file_infos.size() > 4096) return result;

        // 1. Root audiosettings.json for the full score. Check uncompressed size BEFORE reading.
        const auto* root_entry = find_zip_entry(file_infos, "audiosettings.json");
        if (root_entry && root_entry->size <= max_audio_settings_bytes) {
            const muse::ByteArray json = zip.fileData("audiosettings.json");
            if (!zip.hasError() && !json.empty() && json.size() <= max_audio_settings_bytes) {
                const std::string_view json_view(reinterpret_cast<const char*>(json.constData()), json.size());
                if (json_nesting_ok(json_view, 64)) {
                    std::string error;
                    const muse::JsonDocument document = muse::JsonDocument::fromJson(json, &error);
                    if (error.empty() && document.isObject()) {
                        const muse::JsonObject root = document.rootObject();

                        const auto master = root.value("master");
                        if (master.isObject()) apply_settings_output(result.master, master.toObject());

                        const auto tracks = root.value("tracks");
                        if (tracks.isArray()) {
                            const muse::JsonArray array = tracks.toArray();
                            if (array.size() <= max_audio_settings_tracks) {
                                for (std::size_t index = 0; index < array.size(); ++index) {
                                    const auto entry = array.at(index);
                                    if (!entry.isObject()) continue;
                                    const muse::JsonObject track = entry.toObject();
                                    const auto part_id = track.value("partId");
                                    const auto instrument_id = track.value("instrumentId");
                                    if (!part_id.isString() || !instrument_id.isString()) continue;

                                    AudioOutputParams output;
                                    const auto settings = track.value("out");
                                    if (settings.isObject()) apply_settings_output(output, settings.toObject());
                                    const auto solo_mute = track.value("soloMuteState");
                                    if (solo_mute.isObject()) {
                                        const muse::JsonObject state = solo_mute.toObject();
                                        if (const auto solo = settings_bool(state, "solo")) output.solo = *solo;
                                        if (const auto muted = settings_bool(state, "mute")) output.muted = *muted;
                                    }
                                    result.tracks.emplace(
                                        std::make_pair(part_id.toStdString(), instrument_id.toStdString()), output);
                                }
                            }
                        }
                    }
                }
            }
        }

        // 2. Discover excerpts and parse their notation solo/mute settings.
        std::vector<std::string> excerpt_names;
        for (const auto& entry : file_infos) {
            const std::string path = entry.filePath.toStdString();
            if (path.starts_with("Excerpts/") && (path.ends_with(".mscx") || path.ends_with(".MSCX"))) {
                const std::size_t slash = path.find('/', 9);
                if (slash != std::string::npos) {
                    std::string name = path.substr(9, slash - 9);
                    if (!name.empty() && std::find(excerpt_names.begin(), excerpt_names.end(), name) == excerpt_names.end()) {
                        excerpt_names.push_back(std::move(name));
                    }
                }
            }
        }
        for (const auto& entry : file_infos) {
            const std::string path = entry.filePath.toStdString();
            if (path.starts_with("Excerpts/") && path.ends_with("/audiosettings.json")) {
                std::string name = path.substr(9, path.size() - 9 - 18);
                if (!name.empty() && std::find(excerpt_names.begin(), excerpt_names.end(), name) == excerpt_names.end()) {
                    excerpt_names.push_back(std::move(name));
                }
            }
        }

        result.excerpt_tracks_by_index.resize(excerpt_names.size());
        for (std::size_t i = 0; i < excerpt_names.size(); ++i) {
            const std::string& name = excerpt_names[i];
            const std::string file_path = "Excerpts/" + name + "/audiosettings.json";
            const auto* excerpt_entry = find_zip_entry(file_infos, file_path);
            if (!excerpt_entry || excerpt_entry->size > max_audio_settings_bytes) continue;

            const muse::ByteArray json = zip.fileData(file_path);
            if (zip.hasError() || json.empty() || json.size() > max_audio_settings_bytes) continue;

            const std::string_view json_view(reinterpret_cast<const char*>(json.constData()), json.size());
            if (!json_nesting_ok(json_view, 64)) continue;

            std::string error;
            const muse::JsonDocument document = muse::JsonDocument::fromJson(json, &error);
            if (!error.empty() || !document.isObject()) continue;

            const muse::JsonObject root = document.rootObject();
            const auto tracks = root.value("tracks");
            if (!tracks.isArray()) continue;

            const muse::JsonArray array = tracks.toArray();
            if (array.size() > max_audio_settings_tracks) continue;

            std::map<std::pair<std::string, std::string>, TrackSoloMute> solo_mute_map;
            for (std::size_t index = 0; index < array.size(); ++index) {
                const auto item = array.at(index);
                if (!item.isObject()) continue;
                const muse::JsonObject track = item.toObject();
                const auto part_id = track.value("partId");
                const auto instrument_id = track.value("instrumentId");
                if (!part_id.isString() || !instrument_id.isString()) continue;

                TrackSoloMute sm;
                const auto solo_mute = track.value("soloMuteState");
                if (solo_mute.isObject()) {
                    const muse::JsonObject state = solo_mute.toObject();
                    if (const auto solo = settings_bool(state, "solo")) sm.solo = *solo;
                    if (const auto muted = settings_bool(state, "mute")) sm.muted = *muted;
                }
                solo_mute_map.emplace(
                    std::make_pair(part_id.toStdString(), instrument_id.toStdString()), sm);
            }
            result.excerpt_tracks_by_index[i] = solo_mute_map;
            result.excerpt_tracks_by_name[name] = std::move(solo_mute_map);
        }
    } catch (...) {
        // A damaged archive or unexpected JSON shape is not a score-open error.
        return {};
    }
    return result;
}

std::vector<AudioTrack> score_audio_stems(Score* score, bool expand_repeats, bool export_rpns)
{
    const std::vector<PartPlan> plans = build_plans(score);
    if (plans.empty()) {
        return {};
    }

    validate_export_channels(score, plans);

    muse::io::Buffer source_buffer;
    if (!source_buffer.open(muse::io::IODevice::WriteOnly)) {
        throw std::runtime_error("score_audio: failed to open in-memory MIDI buffer");
    }

    // This helper has no application-level SynthesizerState, so use the score's
    // state instead of ExportMidi's test-only default-state overload.
    mu::iex::midi::ExportMidi exporter(score);
    bool exported = false;
    try {
        exported = exporter.write(&source_buffer, expand_repeats, export_rpns, score->synthesizerState());
    } catch (const std::exception& error) {
        throw std::runtime_error("score_audio: MIDI export failed: " + std::string(error.what()));
    }
    if (!exported) {
        throw std::runtime_error("score_audio: MIDI export failed");
    }

    const muse::ByteArray& source_bytes = source_buffer.data();
    if (source_bytes.size() > max_total_midi_bytes) {
        throw std::runtime_error("score_audio: source MIDI exceeds the 8 MiB limit");
    }
    if (!has_mthd_header(source_bytes.constData(), source_bytes.size())) {
        throw std::runtime_error("score_audio: MIDI export produced an invalid header");
    }

    muse::io::Buffer input(source_bytes.constData(), source_bytes.size());
    if (!input.open(muse::io::IODevice::ReadOnly)) {
        throw std::runtime_error("score_audio: failed to open exported MIDI for reading");
    }

    // Verified against midifile.cpp: MidiFile::read() returns true on success.
    MidiFile source_midi;
    bool read_ok = false;
    try {
        read_ok = source_midi.read(&input);
    } catch (const std::exception& error) {
        throw std::runtime_error("score_audio: failed to read exported MIDI: " + std::string(error.what()));
    }
    if (!read_ok) {
        throw std::runtime_error("score_audio: failed to read exported MIDI");
    }

    if (source_midi.format() != 1) {
        throw std::runtime_error("score_audio: exported MIDI has unsupported format "
                                 + std::to_string(source_midi.format()));
    }
    if (source_midi.division() <= 0 || source_midi.isDivisionInTps()) {
        throw std::runtime_error("score_audio: exported MIDI has an unsupported time division");
    }
    if (source_midi.tracks().size() != score->nstaves()) {
        throw std::runtime_error("score_audio: exported MIDI track count "
                                 + std::to_string(source_midi.tracks().size())
                                 + " does not match score staff count "
                                 + std::to_string(score->nstaves()));
    }

    std::vector<AudioTrack> result;
    result.reserve(plans.size());
    std::size_t total_bytes = 0;

    for (const PartPlan& plan : plans) {
        std::string midi = render_part_stem(source_midi, plan);
        if (midi.size() > max_total_midi_bytes) {
            throw std::runtime_error("score_audio: stem for part " + std::to_string(plan.track_id)
                                     + " exceeds the 8 MiB MIDI limit");
        }
        total_bytes += midi.size();
        if (total_bytes > max_total_midi_bytes) {
            throw std::runtime_error("score_audio: total stem MIDI exceeds the 8 MiB limit");
        }
        result.push_back(AudioTrack{ plan.track_id, std::move(midi) });
    }

    return result;
}

} // namespace music
