#include "music_core.h"

#include "composition.h"
#include "engrave/src/audio/engine_audio.h"
#include "engrave/src/runtime/noqt_runtime.h"
#include "engrave/src/audio/score_audio.h"
#include "engrave/src/score/score_metadata.h"

#include "engraving/dom/excerpt.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/repeatlist.h"
#include "engraving/engravingproject.h"

#include <charconv>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <list>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace music {

CoreError::CoreError(Code code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

CoreError::CoreError(Code code, const char* message)
    : std::runtime_error(message), code_(code) {}

DocumentId document_id_from_string(std::string_view text) {
    constexpr std::string_view prefix = "score-";
    if (text.size() <= prefix.size() || text.compare(0, prefix.size(), prefix) != 0)
        throw std::invalid_argument("Unknown or closed documentId");
    const std::string_view digits = text.substr(prefix.size());
    if (digits.empty() || digits.front() == '0')
        throw std::invalid_argument("Unknown or closed documentId");
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc() || end != digits.data() + digits.size())
        throw std::invalid_argument("Unknown or closed documentId");
    return DocumentId{value};
}

std::string document_id_to_string(DocumentId id) {
    return "score-" + std::to_string(id.value);
}

namespace {

using Json = nlohmann::json;

constexpr std::size_t max_documents = 16;
constexpr std::size_t max_history = 256;
constexpr std::size_t max_replay_entries = 128;
constexpr std::size_t max_replay_payload = 1024 * 1024;
constexpr std::size_t max_operation_id = 128;

std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

const char* input_backend_name(InputFormat format) noexcept {
    switch (format) {
    case InputFormat::MusicXml: return "musicxml";
    case InputFormat::Xml: return "xml";
    case InputFormat::Mxl: return "mxl";
    case InputFormat::Mscz: return "mscz";
    case InputFormat::Mscx: return "mscx";
    case InputFormat::Midi: return "midi";
    case InputFormat::Kar: return "kar";
    case InputFormat::Gp: return "gp";
    case InputFormat::Gp3: return "gp3";
    case InputFormat::Gp4: return "gp4";
    case InputFormat::Gp5: return "gp5";
    case InputFormat::Gp6: return "gp6";
    case InputFormat::Gp7: return "gp7";
    case InputFormat::Gpx: return "gpx";
    case InputFormat::Ptb: return "ptb";
    }
    return "";
}

const char* export_backend_name(ExportFormat format) noexcept {
    switch (format) {
    case ExportFormat::MusicXml: return "musicxml";
    case ExportFormat::Mxl: return "mxl";
    case ExportFormat::Mscz: return "mscz";
    case ExportFormat::Mscx: return "mscx";
    case ExportFormat::Midi: return "midi";
    case ExportFormat::Svg: return "svg";
    case ExportFormat::Png: return "png";
    case ExportFormat::Pdf: return "pdf";
    case ExportFormat::Metadata: return "metadata";
    case ExportFormat::Wav: return "wav";
    case ExportFormat::MeasurePositions: return "measurePositions";
    case ExportFormat::SegmentPositions: return "segmentPositions";
    case ExportFormat::Flac: return "flac";
    case ExportFormat::Ogg: return "ogg";
    case ExportFormat::Mp3: return "mp3";
    case ExportFormat::Aac: return "aac";
    }
    return "";
}

// Audio formats are always rendered by the worker's MuseScore engine, never by
// the host's exporter path.
bool is_audio_export_format(ExportFormat format) noexcept {
    switch (format) {
    case ExportFormat::Wav:
    case ExportFormat::Flac:
    case ExportFormat::Ogg:
    case ExportFormat::Mp3:
    case ExportFormat::Aac:
        return true;
    default:
        return false;
    }
}

// Typed -> existing JSON validator/generator. This is the only public-core
// conversion into composition_xml; the algorithms and validation stay in that
// existing component.
Json composition_json(const Composition& composition) {
    Json parts = Json::array();
    for (const Part& part : composition.parts) {
        Json measures = Json::array();
        for (const Measure& measure : part.measures) {
            Json notes = Json::array();
            for (const Note& note : measure.notes) {
                Json note_json = Json::object();
                note_json["pitches"] = note.pitches;
                note_json["ticks"] = note.ticks;
                notes.push_back(std::move(note_json));
            }
            Json measure_json = Json::object();
            measure_json["notes"] = std::move(notes);
            measures.push_back(std::move(measure_json));
        }
        Json part_json = Json::object();
        part_json["name"] = part.name;
        part_json["midiProgram"] = part.midi_program;
        part_json["measures"] = std::move(measures);
        parts.push_back(std::move(part_json));
    }

    Json score = Json::object();
    score["title"] = composition.title;
    score["tempo"] = composition.tempo;
    score["parts"] = std::move(parts);
    return score;
}

// Translate implementation/upstream exceptions at the typed boundary. Core
// errors and bad_alloc pass through unchanged so callers never lose them.
[[noreturn]] void translate_current_exception(CoreError::Code fallback) {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const CoreError&) {
        throw;
    } catch (const std::invalid_argument& error) {
        throw CoreError(CoreError::Code::invalid_argument, error.what());
    } catch (const std::logic_error& error) {
        throw CoreError(CoreError::Code::invalid_argument, error.what());
    } catch (const std::runtime_error& error) {
        throw CoreError(fallback, error.what());
    } catch (const std::exception& error) {
        throw CoreError(fallback, error.what());
    } catch (...) {
        throw;
    }
}

// Length-prefixed canonical payloads make replay fingerprints unambiguous
// without relying on a weak hash. They are the actual request representation.
void append_payload(std::string& output, std::string_view value) {
    output += std::to_string(value.size());
    output.push_back(':');
    output.append(value.data(), value.size());
    output.push_back(';');
}

void append_payload(std::string& output, std::uint64_t value) {
    append_payload(output, std::to_string(value));
}

void append_payload(std::string& output, std::int64_t value) {
    append_payload(output, std::to_string(value));
}

void append_measure_payload(std::string& output, const Measure& measure) {
    append_payload(output, static_cast<std::uint64_t>(measure.notes.size()));
    for (const Note& note : measure.notes) {
        append_payload(output, static_cast<std::int64_t>(note.ticks));
        append_payload(output, static_cast<std::uint64_t>(note.pitches.size()));
        for (const int pitch : note.pitches)
            append_payload(output, static_cast<std::int64_t>(pitch));
    }
}

void enforce_fingerprint_limit(const std::string& fingerprint) {
    if (fingerprint.size() > max_replay_payload)
        throw CoreError(CoreError::Code::resource_limit,
                        "Serialized edit request exceeds the 1 MiB replay limit");
}

std::string replace_fingerprint(Revision base, std::size_t part_index, std::size_t start_measure,
                                const std::vector<Measure>& measures) {
    std::string output;
    append_payload(output, "replace_measures");
    append_payload(output, base.value);
    append_payload(output, static_cast<std::uint64_t>(part_index));
    append_payload(output, static_cast<std::uint64_t>(start_measure));
    append_payload(output, static_cast<std::uint64_t>(measures.size()));
    for (const Measure& measure : measures) append_measure_payload(output, measure);
    enforce_fingerprint_limit(output);
    return output;
}

std::string pitch_fingerprint(Revision base, const NativePitchEdit& edit) {
    std::string output;
    append_payload(output, "set_pitches");
    append_payload(output, base.value);
    append_payload(output, static_cast<std::uint64_t>(edit.changes.size()));
    for (const PitchChange& change : edit.changes) {
        append_payload(output, change.note_id);
        append_payload(output, static_cast<std::int64_t>(change.pitch));
    }
    enforce_fingerprint_limit(output);
    return output;
}

bool ranges_overlap(std::size_t first_start, std::size_t first_count,
                    std::size_t second_start, std::size_t second_count) {
    if (first_count == 0 || second_count == 0) return false;
    const auto saturated_end = [](std::size_t start, std::size_t count) {
        return start > std::numeric_limits<std::size_t>::max() - count
            ? std::numeric_limits<std::size_t>::max()
            : start + count;
    };
    const std::size_t first_end = saturated_end(first_start, first_count);
    const std::size_t second_end = saturated_end(second_start, second_count);
    return first_start < second_end && second_start < first_end;
}

bool scope_intersects(const ScoreEditScope& first, const ScoreEditScope& second) {
    if (first.part_index != second.part_index) return false;
    if (first.whole_part || second.whole_part) return true;
    return ranges_overlap(first.first_measure, first.measure_count,
                          second.first_measure, second.measure_count);
}

bool scopes_intersect(const std::vector<ScoreEditScope>& first,
                      const std::vector<ScoreEditScope>& second) {
    for (const ScoreEditScope& a : first)
        for (const ScoreEditScope& b : second)
            if (scope_intersects(a, b)) return true;
    return false;
}

} // namespace

std::optional<InputFormat> input_format_from_name(std::string_view name) noexcept {
    if (name == "musicxml") return InputFormat::MusicXml;
    if (name == "xml") return InputFormat::Xml;
    if (name == "mxl") return InputFormat::Mxl;
    if (name == "mscz") return InputFormat::Mscz;
    if (name == "mscx") return InputFormat::Mscx;
    if (name == "midi") return InputFormat::Midi;
    if (name == "mid") return InputFormat::Midi;
    if (name == "kar") return InputFormat::Kar;
    if (name == "gp") return InputFormat::Gp;
    if (name == "gtp") return InputFormat::Gp;
    if (name == "gp3") return InputFormat::Gp3;
    if (name == "gp4") return InputFormat::Gp4;
    if (name == "gp5") return InputFormat::Gp5;
    if (name == "gp6") return InputFormat::Gp6;
    if (name == "gp7") return InputFormat::Gp7;
    if (name == "gpx") return InputFormat::Gpx;
    if (name == "ptb") return InputFormat::Ptb;
    return std::nullopt;
}

std::optional<ExportFormat> export_format_from_name(std::string_view name) noexcept {
    if (name == "musicxml") return ExportFormat::MusicXml;
    if (name == "mxl") return ExportFormat::Mxl;
    if (name == "mscz") return ExportFormat::Mscz;
    if (name == "mscx") return ExportFormat::Mscx;
    if (name == "midi") return ExportFormat::Midi;
    if (name == "svg") return ExportFormat::Svg;
    if (name == "png") return ExportFormat::Png;
    if (name == "pdf") return ExportFormat::Pdf;
    if (name == "metadata") return ExportFormat::Metadata;
    if (name == "wav") return ExportFormat::Wav;
    if (name == "measurePositions") return ExportFormat::MeasurePositions;
    if (name == "segmentPositions") return ExportFormat::SegmentPositions;
    if (name == "flac") return ExportFormat::Flac;
    if (name == "ogg") return ExportFormat::Ogg;
    if (name == "mp3") return ExportFormat::Mp3;
    if (name == "aac") return ExportFormat::Aac;
    return std::nullopt;
}

const char* format_name(InputFormat format) noexcept {
    return input_backend_name(format);
}

const char* format_name(ExportFormat format) noexcept {
    return export_backend_name(format);
}

struct MusicCore::Impl {
    struct HistoryEntry {
        Revision revision;
        std::vector<ScoreEditScope> read_scopes;
        std::vector<ScoreEditScope> write_scopes;
    };

    struct ReplayEntry {
        std::string operation_id;
        std::string fingerprint;
        EditResult result;
    };

    struct Document {
        ScoreProject project;
        std::optional<Composition> composition;
        Revision revision{1};
        // Revision whose replacement rebuilt the engraving DOM and its EIDs.
        Revision last_dom_reload{1};
        Revision retry_floor{1};
        bool poisoned = false;
        // Immutable default mix from the source MSCZ audiosettings.json (or
        // neutral). Keyed by (partId, instrumentId) so it survives DOM reloads
        // and is mapped to current track IDs on demand.
        ScoreAudioMixDefaults audio_mix_defaults;
        std::list<HistoryEntry> history;
        std::list<ReplayEntry> replays;
        std::size_t replay_payload_bytes = 0;
    };

    std::thread::id owner;
    std::map<std::uint64_t, Document> documents;
    std::uint64_t next_document = 1;
    RuntimeOptions options;
    std::string soundfont;

    void check_owner() const {
        if (std::this_thread::get_id() != owner)
            throw CoreError(CoreError::Code::wrong_thread, "MusicCore used from a non-owner thread");
    }

    void ensure_capacity() const {
        if (documents.size() >= max_documents)
            throw CoreError(CoreError::Code::resource_limit,
                            "Close a document before opening another (limit 16)");
    }

    Document& find(DocumentId id) {
        const auto it = documents.find(id.value);
        if (it == documents.end())
            throw CoreError(CoreError::Code::not_found, "Unknown or closed documentId");
        if (it->second.poisoned)
            throw CoreError(CoreError::Code::poisoned_document, "Document is unusable after an engine transaction failure; close and reopen it");
        return it->second;
    }

    const Document& find(DocumentId id) const {
        const auto it = documents.find(id.value);
        if (it == documents.end())
            throw CoreError(CoreError::Code::not_found, "Unknown or closed documentId");
        if (it->second.poisoned)
            throw CoreError(CoreError::Code::poisoned_document, "Document is unusable after an engine transaction failure; close and reopen it");
        return it->second;
    }

    static ScoreInfo info(DocumentId id, const ScoreProject& project, Revision revision, bool editable) {
        const auto* score = project->masterScore();
        ScoreInfo result;
        result.id = id;
        result.revision = revision;
        result.title = score_title(score);
        result.title_filename_safe = score_title_filename_safe(score);
        result.pages = static_cast<std::size_t>(score->npages());
        result.parts = score->parts().size();
        result.editable = editable;
        return result;
    }

    static ScoreInfo info(DocumentId id, const Document& document) {
        return info(id, document.project, document.revision, document.composition.has_value());
    }

    static ScoreProject build(const Composition& composition) {
        return load_native_score(composition_xml(composition_json(composition)), "musicxml");
    }

    static void validate_operation(const EditContext& context) {
        const std::string& operation_id = context.operation_id;
        if (operation_id.empty() || operation_id.size() > max_operation_id
            || operation_id.find('\0') != std::string::npos)
            throw CoreError(CoreError::Code::invalid_argument,
                            "operationId must be 1..128 bytes without NUL");
    }

    static void validate_base(const Document& document, Revision base) {
        if (base.value == 0 || base.value > document.revision.value)
            throw CoreError(CoreError::Code::invalid_argument,
                            "baseRevision must be a current or earlier positive revision");
        Revision oldest{1};
        if (!document.history.empty())
            oldest = Revision{document.history.front().revision.value - 1};
        if (base.value < std::max(oldest.value, document.retry_floor.value))
            throw CoreError(CoreError::Code::stale_revision,
                            "baseRevision is older than retained document history");
    }

    static const ReplayEntry* find_replay(const Document& document, std::string_view operation_id) {
        for (const ReplayEntry& entry : document.replays)
            if (entry.operation_id == operation_id) return &entry;
        return nullptr;
    }

    static EditResult replay_result(const ReplayEntry& entry) {
        EditResult result = entry.result;
        result.replayed = true;
        return result;
    }

    // Rebase checks use serialized history: every edit is rebased over all
    // intervening revisions. New reads/writes must not touch intervening
    // writes; new writes need not avoid intervening reads.
    static std::vector<Revision> validate_scopes(
        const Document& document, Revision base,
        const std::vector<ScoreEditScope>& read_scopes,
        const std::vector<ScoreEditScope>& write_scopes) {
        std::vector<Revision> rebased;
        for (const HistoryEntry& entry : document.history) {
            if (entry.revision.value <= base.value) continue;
            rebased.push_back(entry.revision);
            if (scopes_intersect(read_scopes, entry.write_scopes)
                || scopes_intersect(write_scopes, entry.write_scopes)) {
                throw CoreError(CoreError::Code::conflict,
                                "Edit conflicts with intervening revision "
                                    + std::to_string(entry.revision.value));
            }
        }
        if (rebased.size() != document.revision.value - base.value)
            throw CoreError(CoreError::Code::stale_revision,
                            "baseRevision is older than retained document history");
        return rebased;
    }

    static void publish_history(Document& document, std::list<HistoryEntry>& prepared) {
        if (prepared.empty()) return;
        document.history.splice(document.history.end(), prepared);
        while (document.history.size() > max_history) document.history.pop_front();
    }

    static void publish_replay(Document& document, std::list<ReplayEntry>& prepared) {
        if (prepared.empty()) return;
        const std::size_t incoming = prepared.front().fingerprint.size();
        while (!document.replays.empty()
               && (document.replays.size() >= max_replay_entries
                   || document.replay_payload_bytes + incoming > max_replay_payload)) {
            document.retry_floor.value = std::max(document.retry_floor.value,
                document.replays.front().result.base_revision.value + 1);
            document.replay_payload_bytes -= document.replays.front().fingerprint.size();
            document.replays.pop_front();
        }
        document.replay_payload_bytes += incoming;
        document.replays.splice(document.replays.end(), prepared);
    }

    // Eviction invalidates the forgotten request's base, so its exact retry
    // cannot be applied twice. A full cache of same-revision no-ops cannot
    // retire that base until an edit advances it; reject before mutation.
    static void reserve_replay(const Document& document, std::size_t incoming, Revision resulting) {
        std::size_t count = document.replays.size();
        std::size_t bytes = document.replay_payload_bytes;
        for (const auto& old : document.replays) {
            if (count < max_replay_entries && bytes + incoming <= max_replay_payload) break;
            if (old.result.base_revision.value >= resulting.value)
                throw CoreError(CoreError::Code::resource_limit,
                    "Replay cache is full at this revision; perform a changing edit before another no-op");
            --count;
            bytes -= old.fingerprint.size();
        }
    }

    static std::string excerpt_name_for_id(const Document& document, int excerpt_id) {
        if (excerpt_id < 0 || !document.project || !document.project->masterScore()) return {};
        const auto& excerpts = document.project->masterScore()->excerpts();
        if (static_cast<std::size_t>(excerpt_id) < excerpts.size() && excerpts[static_cast<std::size_t>(excerpt_id)]) {
            const auto* exc = excerpts[static_cast<std::size_t>(excerpt_id)];
            if (exc->hasFileName()) return exc->fileName().toStdString();
            return exc->name().toStdString();
        }
        return {};
    }

    static std::vector<AudioTrackInfo> describe_audio_tracks(mu::engraving::Score* score) {
        try {
            const auto descriptions = score_audio_track_info(score);
            std::vector<AudioTrackInfo> result;
            result.reserve(descriptions.size());
            for (std::size_t index = 0; index < descriptions.size(); ++index) {
                const ScoreAudioTrackInfo& description = descriptions[index];
                AudioTrackInfo info;
                info.track_id = static_cast<std::int64_t>(index);
                info.part_id = description.part_id;
                info.instrument_id = description.instrument_id;
                info.part_name = description.part_name;
                info.instrument_name = description.instrument_name;
                info.chord_symbols = description.chord_symbols;
                result.push_back(std::move(info));
            }
            return result;
        } catch (const std::invalid_argument& error) {
            throw CoreError(CoreError::Code::invalid_argument, error.what());
        } catch (const std::runtime_error& error) {
            throw CoreError(CoreError::Code::invalid_argument, error.what());
        }
    }

    static std::vector<AudioTrackInfo> describe_audio_tracks(const ScoreProject& project,
                                                             int excerpt_id) {
        return describe_audio_tracks(select_score(project->masterScore(), excerpt_id));
    }

    static std::vector<ScoreMetadataTrack> describe_metadata_tracks(
        const std::vector<AudioTrackInfo>& track_infos, const AudioMix& mix) {
        std::vector<ScoreMetadataTrack> meta_tracks;
        meta_tracks.reserve(track_infos.size());
        for (const AudioTrackInfo& info : track_infos) {
            ScoreMetadataTrack track;
            track.track_id = info.track_id;
            track.part_id = info.part_id;
            track.instrument_id = info.instrument_id;
            track.part_name = info.part_name;
            track.name = info.instrument_name;
            track.chord_symbols = info.chord_symbols;
            for (const auto& track_param : mix.tracks) {
                if (track_param.track_id == info.track_id) {
                    track.volume_db = track_param.output.volume_db;
                    track.balance = track_param.output.balance;
                    track.solo = track_param.output.solo;
                    track.muted = track_param.output.muted;
                    track.force_mute = track_param.output.force_mute;
                    break;
                }
            }
            meta_tracks.push_back(std::move(track));
        }
        return meta_tracks;
    }

    // Metadata shares the audio track list; every instrument track (including
    // instrument changes and chord-symbol pseudo tracks) is described.
    static std::vector<ScoreMetadataTrack> metadata_tracks_or_empty(const Document& document,
                                                                   mu::engraving::Score* score,
                                                                   int excerpt_id,
                                                                   const std::optional<AudioMix>& resolved = std::nullopt) {
        const std::vector<AudioTrackInfo> tracks = describe_audio_tracks(score);
        const AudioMix mix = resolved ? canonical_audio_mix(*resolved, tracks)
                                      : default_audio_mix(document, tracks, excerpt_id);
        return describe_metadata_tracks(tracks, mix);
    }

    static void validate_audio_output(const AudioOutputParams& output, std::string_view label) {
        if (!std::isfinite(output.volume_db) || output.volume_db < -120.0
            || output.volume_db > 24.0) {
            throw CoreError(CoreError::Code::invalid_argument,
                            std::string(label) + " volumeDb must be finite and in [-120, 24]");
        }
        if (!std::isfinite(output.balance) || output.balance < -1.0 || output.balance > 1.0) {
            throw CoreError(CoreError::Code::invalid_argument,
                            std::string(label) + " balance must be finite and in [-1, 1]");
        }
    }

    static void validate_audio_override(const AudioOutputOverride& output, std::string_view label) {
        if (output.volume_db
            && (!std::isfinite(*output.volume_db) || *output.volume_db < -120.0
                || *output.volume_db > 24.0)) {
            throw CoreError(CoreError::Code::invalid_argument,
                            std::string(label) + " volumeDb must be finite and in [-120, 24]");
        }
        if (output.balance
            && (!std::isfinite(*output.balance) || *output.balance < -1.0
                || *output.balance > 1.0)) {
            throw CoreError(CoreError::Code::invalid_argument,
                            std::string(label) + " balance must be finite and in [-1, 1]");
        }
    }

    // Derives forceMute and enforces the chord-playback-off rule for the
    // pseudo tracks that score_audio_tracks lists.
    static void finalize_audio_mix(AudioMix& mix, const std::vector<AudioTrackInfo>& tracks) {
        derive_force_mute(mix);
        for (std::size_t index = 0; index < tracks.size() && index < mix.tracks.size(); ++index) {
            if (tracks[index].chord_symbols) {
                mix.tracks[index].output.muted = true;
                mix.tracks[index].output.force_mute = true;
            }
        }
    }

    // Maps the immutable (partId, instrumentId) defaults onto the selected
    // score's current track IDs and derives forceMute.
    // For excerptId >= 0, volume and balance come from root settings while
    // solo and muted come from the excerpt's own audiosettings.json (or neutral).
    static AudioMix default_audio_mix(const Document& document,
                                      const std::vector<AudioTrackInfo>& tracks,
                                      int excerpt_id = -1) {
        const std::string excerpt_name = excerpt_name_for_id(document, excerpt_id);
        AudioMix mix;
        mix.master = document.audio_mix_defaults.master;
        validate_audio_output(mix.master, "Document master mix");
        mix.tracks.reserve(tracks.size());

        const auto* excerpt_solo_mute = (excerpt_id >= 0)
            ? document.audio_mix_defaults.get_excerpt_map(excerpt_id, excerpt_name)
            : nullptr;

        for (const AudioTrackInfo& track : tracks) {
            AudioOutputParams output;
            const auto it = document.audio_mix_defaults.tracks.find(
                std::make_pair(track.part_id, track.instrument_id));
            if (it != document.audio_mix_defaults.tracks.end()) {
                output.volume_db = it->second.volume_db;
                output.balance = it->second.balance;
                if (excerpt_id < 0) {
                    output.solo = it->second.solo;
                    output.muted = it->second.muted;
                }
            }
            if (excerpt_id >= 0) {
                if (excerpt_solo_mute) {
                    const auto sm_it = excerpt_solo_mute->find(
                        std::make_pair(track.part_id, track.instrument_id));
                    if (sm_it != excerpt_solo_mute->end()) {
                        output.solo = sm_it->second.solo;
                        output.muted = sm_it->second.muted;
                    } else {
                        output.solo = false;
                        output.muted = false;
                    }
                } else {
                    output.solo = false;
                    output.muted = false;
                }
            }
            validate_audio_output(output, "Document track mix");
            mix.tracks.push_back(AudioTrackParams{track.track_id, output});
        }
        finalize_audio_mix(mix, tracks);
        return mix;
    }

    // Validates a complete mix (used for worker-supplied explicit mixes) and
    // returns it in actual score order with a recomputed forceMute.
    static AudioMix canonical_audio_mix(const AudioMix& candidate,
                                        const std::vector<AudioTrackInfo>& tracks) {
        validate_audio_output(candidate.master, "Audio mix master");
        if (candidate.tracks.size() != tracks.size()) {
            throw CoreError(CoreError::Code::invalid_argument,
                            "Audio mix must contain exactly one entry for each selected track");
        }

        std::map<std::int64_t, const AudioTrackParams*> by_id;
        for (const AudioTrackParams& track : candidate.tracks) {
            if (track.track_id < 0) {
                throw CoreError(CoreError::Code::invalid_argument,
                                "Audio mix trackId must be non-negative");
            }
            validate_audio_output(track.output, "Audio mix track");
            if (!by_id.emplace(track.track_id, &track).second) {
                throw CoreError(CoreError::Code::invalid_argument,
                                "Audio mix contains duplicate trackId "
                                    + std::to_string(track.track_id));
            }
        }

        AudioMix canonical;
        canonical.master = candidate.master;
        canonical.tracks.reserve(tracks.size());
        for (const AudioTrackInfo& track : tracks) {
            const auto it = by_id.find(track.track_id);
            if (it == by_id.end()) {
                throw CoreError(CoreError::Code::invalid_argument,
                                "Audio mix is missing trackId " + std::to_string(track.track_id));
            }
            canonical.tracks.push_back(*it->second);
        }
        if (by_id.size() != tracks.size()) {
            throw CoreError(CoreError::Code::invalid_argument,
                            "Audio mix contains an unknown trackId");
        }
        finalize_audio_mix(canonical, tracks);
        return canonical;
    }

    // Replaces only the fields the export supplied. forceMute is never accepted
    // as input and is recomputed from the resulting solo flags.
    static AudioMix apply_audio_override(AudioMix mix, const AudioMixOverride& override,
                                         const std::vector<AudioTrackInfo>& tracks) {
        validate_audio_override(override.master, "mix.master");

        std::map<std::int64_t, AudioTrackParams*> by_id;
        for (AudioTrackParams& track : mix.tracks) {
            by_id.emplace(track.track_id, &track);
        }
        std::set<std::int64_t> seen_ids;
        for (const AudioTrackMixOverride& entry : override.tracks) {
            if (entry.track_id < 0) {
                throw CoreError(CoreError::Code::invalid_argument,
                                "mix.tracks trackId must be non-negative");
            }
            if (!seen_ids.insert(entry.track_id).second) {
                throw CoreError(CoreError::Code::invalid_argument,
                                "mix.tracks contains duplicate trackId "
                                    + std::to_string(entry.track_id));
            }
            const auto found = by_id.find(entry.track_id);
            if (found == by_id.end()) {
                throw CoreError(CoreError::Code::invalid_argument,
                                "mix.tracks contains unknown trackId "
                                    + std::to_string(entry.track_id)
                                    + "; use trackIds from score_audio_tracks");
            }
            validate_audio_override(entry.output, "mix.tracks");
            AudioOutputParams& output = found->second->output;
            if (entry.output.volume_db) output.volume_db = *entry.output.volume_db;
            if (entry.output.balance) output.balance = *entry.output.balance;
            if (entry.output.solo) output.solo = *entry.output.solo;
            if (entry.output.muted) output.muted = *entry.output.muted;
        }
        if (override.master.volume_db) mix.master.volume_db = *override.master.volume_db;
        if (override.master.balance) mix.master.balance = *override.master.balance;

        if (by_id.size() != tracks.size()) {
            throw CoreError(CoreError::Code::invalid_argument,
                            "Audio mix track set does not match the selected score");
        }
        finalize_audio_mix(mix, tracks);
        return mix;
    }

    static AudioMix resolved_audio_mix(const Document& document,
                                       const std::vector<AudioTrackInfo>& tracks,
                                       const ExportOptions& options) {
        if (options.explicit_mix) return canonical_audio_mix(*options.explicit_mix, tracks);
        AudioMix mix = default_audio_mix(document, tracks, options.excerpt_id);
        if (options.mix) mix = apply_audio_override(std::move(mix), *options.mix, tracks);
        return mix;
    }

    AudioMix export_mix(const Document& document, const ExportOptions& options) const {
        const auto tracks = describe_audio_tracks(document.project, options.excerpt_id);
        return resolved_audio_mix(document, tracks, options);
    }

    ScoreInfo insert(ScoreProject project, std::optional<Composition> composition,
                     ScoreAudioMixDefaults defaults = {}) {
        const DocumentId id{next_document++};
        const auto result = info(id, project, Revision{1}, composition.has_value());
        Document document;
        document.project = std::move(project);
        document.composition = std::move(composition);
        document.audio_mix_defaults = std::move(defaults);
        document.revision = Revision{1};
        document.last_dom_reload = Revision{1};
        documents.emplace(id.value, std::move(document));
        return result;
    }

    EditResult replace(DocumentId id, std::size_t part_index, std::size_t start_measure,
                       const std::vector<Measure>& replacement, const EditContext* context) {
        auto& document = find(id);

        std::string fingerprint;
        if (context) {
            validate_operation(*context);
            fingerprint = replace_fingerprint(context->base_revision, part_index, start_measure,
                                              replacement);
            if (const ReplayEntry* replay = find_replay(document, context->operation_id)) {
                if (replay->fingerprint != fingerprint)
                    throw CoreError(CoreError::Code::conflict,
                                    "operationId was reused with a different request");
                return replay_result(*replay);
            }
            validate_base(document, context->base_revision);
        }

        if (!document.composition)
            throw CoreError(CoreError::Code::invalid_argument,
                            "Structured measure replacement requires a score_create document; "
                            "native pitch edits make the typed composition stale");
        const Composition& current = *document.composition;
        if (part_index >= current.parts.size())
            throw CoreError(CoreError::Code::invalid_argument, "partIndex is out of range");
        const auto& measures = current.parts[part_index].measures;
        if (start_measure >= measures.size())
            throw CoreError(CoreError::Code::invalid_argument, "startMeasure is out of range");
        if (replacement.empty() || replacement.size() > measures.size() - start_measure)
            throw CoreError(CoreError::Code::invalid_argument,
                            "Replacement must fit within the existing measure range");

        const ScoreEditScope scope{part_index, start_measure, replacement.size(), false};
        const Revision base = context ? context->base_revision : document.revision;
        std::vector<Revision> rebased;
        if (context) {
            validate_base(document, base);
            rebased = validate_scopes(document, base, {scope}, {scope});
        }

        if (document.revision.value == std::numeric_limits<std::uint64_t>::max())
            throw CoreError(CoreError::Code::resource_limit, "Document revision limit reached");
        const Revision revision{document.revision.value + 1};
        if (context) reserve_replay(document, fingerprint.size(), revision);

        Composition candidate = current;
        for (std::size_t i = 0; i < replacement.size(); ++i)
            candidate.parts[part_index].measures[start_measure + i] = replacement[i];

        // Build and describe the candidate completely before touching the live
        // document. All replay/history/result allocations also happen first.
        auto project = build(candidate);
        if (!list_score_excerpts(document.project->masterScore()).empty())
            generate_score_excerpts(project->masterScore());
        EditResult result;
        result.score = info(id, project, revision, true);
        result.base_revision = base;
        result.rebased_over = std::move(rebased);
        for (const Measure& measure : replacement)
            result.changed_notes += measure.notes.size();

        std::list<ReplayEntry> replay_prepared;
        if (context) {
            ReplayEntry entry;
            entry.operation_id = context->operation_id;
            entry.fingerprint = fingerprint;
            entry.result = result;
            replay_prepared.push_back(std::move(entry));
        }
        std::list<HistoryEntry> history_prepared;
        {
            HistoryEntry entry;
            entry.revision = revision;
            entry.read_scopes = {scope};
            entry.write_scopes = {scope};
            history_prepared.push_back(std::move(entry));
        }

        document.project = std::move(project);
        *document.composition = std::move(candidate);
        document.revision = revision;
        document.last_dom_reload = revision;
        publish_history(document, history_prepared);
        publish_replay(document, replay_prepared);
        return result;
    }

    EditResult set_pitches(DocumentId id, const NativePitchEdit& edit, const EditContext& context) {
        auto& document = find(id);
        validate_operation(context);
        const std::string fingerprint = pitch_fingerprint(context.base_revision, edit);
        if (const ReplayEntry* replay = find_replay(document, context.operation_id)) {
            if (replay->fingerprint != fingerprint)
                throw CoreError(CoreError::Code::conflict,
                                "operationId was reused with a different request");
            return replay_result(*replay);
        }

        validate_base(document, context.base_revision);
        if (context.base_revision.value < document.last_dom_reload.value)
            throw CoreError(CoreError::Code::conflict,
                            "Native pitch edit base predates a score replacement; note ids may have changed");

        const NativePitchResult inspected =
            inspect_score_pitch_edit(document.project->masterScore(), edit);
        std::vector<Revision> rebased = validate_scopes(
            document, context.base_revision, inspected.read_scopes, inspected.write_scopes);

        if (inspected.changed_notes && document.revision.value == std::numeric_limits<std::uint64_t>::max())
            throw CoreError(CoreError::Code::resource_limit, "Document revision limit reached");
        const Revision revision{document.revision.value + (inspected.changed_notes ? 1 : 0)};
        reserve_replay(document, fingerprint.size(), revision);

        EditResult result;
        result.score = info(id, document);
        result.score.revision = revision;
        result.score.editable = false;
        result.base_revision = context.base_revision;
        result.rebased_over = std::move(rebased);
        result.changed_notes = 0;

        // Prepare all allocations before apply() mutates the shared score.
        ReplayEntry replay_entry;
        replay_entry.operation_id = context.operation_id;
        replay_entry.fingerprint = fingerprint;
        replay_entry.result = result;
        std::list<ReplayEntry> replay_prepared;
        replay_prepared.push_back(std::move(replay_entry));

        std::list<HistoryEntry> history_prepared;
        {
            HistoryEntry entry;
            entry.revision = revision;
            entry.read_scopes = inspected.read_scopes;
            entry.write_scopes = inspected.write_scopes;
            history_prepared.push_back(std::move(entry));
        }

        NativePitchResult applied;
        try {
            applied = apply_score_pitch_edit(document.project->masterScore(), edit);
        } catch (const NativeEditFailure& error) {
            if (error.poisoned()) {
                document.poisoned = true;
                throw CoreError(CoreError::Code::poisoned_document, error.what());
            }
            throw;
        }
        const std::size_t changed_notes = applied.changed_notes;
        const bool changed = changed_notes > 0;

        EditResult& cached = replay_prepared.front().result;
        if (changed) {
            document.revision = revision;
            document.composition.reset();
            result.score.revision = revision;
            result.score.editable = false;
        } else {
            result.score.revision = document.revision;
            result.score.editable = document.composition.has_value();
        }
        result.changed_notes = changed_notes;
        cached.changed_notes = changed_notes;
        cached.score.revision = result.score.revision;
        cached.score.editable = result.score.editable;

        // Pages may change without affecting the prepared string/vector
        // allocations; update those non-allocating fields in place.
        const auto* master = document.project->masterScore();
        result.score.pages = static_cast<std::size_t>(master->npages());
        cached.score.pages = result.score.pages;
        result.score.parts = master->parts().size();
        cached.score.parts = result.score.parts;

        publish_replay(document, replay_prepared);
        if (changed) publish_history(document, history_prepared);
        if (changed) (void)cap_native_edit_history(document.project->masterScore());
        return result;
    }

    ExportData export_document(DocumentId id, ExportFormat format, const ExportOptions& options) {
        const auto& document = find(id);
        if (format == ExportFormat::Metadata) {
            auto* selected = select_score(document.project->masterScore(), options.excerpt_id);
            const auto meta_tracks = metadata_tracks_or_empty(document, selected, options.excerpt_id,
                                                              options.explicit_mix);
            return ExportData{document.revision, score_metadata(selected, meta_tracks).dump()};
        }
        if (is_audio_export_format(format)) {
            // Audio never renders in the host process; score_export and
            // score_export_start route it through a worker (export_audio).
            throw CoreError(CoreError::Code::invalid_argument,
                            "Audio exports are rendered in a worker process");
        }
        auto data = export_native_score(document.project, export_backend_name(format), options, soundfont);
        if (data.empty()) throw CoreError(CoreError::Code::engine_failure, "Exporter returned no data");
        return ExportData{document.revision, std::move(data)};
    }
};

MusicCore::MusicCore(const RuntimeOptions& options) : impl_(std::make_unique<Impl>()) {
    impl_->owner = std::this_thread::get_id();
    try {
        if (options.resources_path.empty())
            throw std::invalid_argument("Missing native resources path");

        RuntimeOptions normalized;
        const auto resources = std::filesystem::absolute(std::filesystem::u8path(options.resources_path));
        normalized.resources_path = path_utf8(resources);

        if (!options.soundfont.empty()) {
            try {
                const auto soundfont = std::filesystem::absolute(std::filesystem::u8path(options.soundfont));
                if (!std::filesystem::is_regular_file(soundfont))
                    throw std::invalid_argument("SoundFont does not exist");
                normalized.soundfont = path_utf8(soundfont);
            } catch (const std::filesystem::filesystem_error&) {
                throw std::invalid_argument("SoundFont does not exist");
            }
        }
        normalized.font_paths.clear();
        if (!options.cache_directory.empty()) {
            normalized.cache_directory = path_utf8(std::filesystem::absolute(std::filesystem::u8path(options.cache_directory)));
            set_runtime_cache_root(normalized.cache_directory);
        }

        impl_->options = std::move(normalized);
        impl_->soundfont = impl_->options.soundfont;
        initialize_native_runtime(impl_->options.resources_path);
        if (!impl_->soundfont.empty()) {
            const auto frozen = freeze_runtime_soundfont(impl_->soundfont);
            impl_->soundfont = frozen.path;
            impl_->options.soundfont = frozen.path;
        }
        for (const auto& path : options.font_paths) add_runtime_font(path);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

MusicCore::~MusicCore() {
    if (impl_ && std::this_thread::get_id() != impl_->owner)
        std::terminate();
    if (impl_) {
        impl_->documents.clear();
        try {
            shutdown_runtime_assets();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Runtime asset cleanup failed: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "Runtime asset cleanup failed\n");
        }
    }
}

ScoreInfo MusicCore::create(const Composition& composition) {
    try {
        impl_->check_owner();
        impl_->ensure_capacity();
        auto project = Impl::build(composition);
        return impl_->insert(std::move(project), composition);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

ScoreInfo MusicCore::open(const std::string& bytes, InputFormat format) {
    try {
        impl_->check_owner();
        impl_->ensure_capacity();
        // MSCZ can carry the project's default mixer settings. Read them before
        // the DOM exists and keep the identity-keyed values with the document.
        ScoreAudioMixDefaults defaults;
        if (format == InputFormat::Mscz) defaults = score_audio_mix_defaults(bytes);
        auto project = load_native_score(bytes, input_backend_name(format));
        return impl_->insert(std::move(project), std::nullopt, std::move(defaults));
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

ScoreInfo MusicCore::describe(DocumentId id) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);
        return Impl::info(id, document);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

ScoreView MusicCore::get(DocumentId id, int excerpt_id) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);

        ScoreView view;
        view.info = Impl::info(id, document);
        auto* selected = select_score(document.project->masterScore(), excerpt_id);
        if (excerpt_id == -1) view.composition = document.composition;
        view.info.title = score_title(selected);
        view.info.title_filename_safe = score_title_filename_safe(selected);
        view.info.pages = static_cast<std::size_t>(selected->npages());
        view.info.parts = selected->parts().size();
        view.info.editable = excerpt_id == -1 && document.composition.has_value();
        ExportOptions options;
        options.excerpt_id = excerpt_id;
        view.musicxml = export_native_score(document.project, "musicxml", options, impl_->soundfont);
        const auto meta_tracks = Impl::metadata_tracks_or_empty(document, selected, excerpt_id);
        view.metadata_json = score_metadata(selected, meta_tracks).dump();
        return view;
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

std::shared_ptr<const ScoreReadSnapshot> MusicCore::read_snapshot(DocumentId id) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);
        auto snapshot = std::make_shared<ScoreReadSnapshot>();
        snapshot->info = Impl::info(id, document);
        snapshot->projection = snapshot_score_projection(document.project->masterScore());
        return snapshot;
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

ExcerptsData MusicCore::excerpts(DocumentId id) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);
        return {document.revision, list_score_excerpts(document.project->masterScore())};
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

ExcerptsData MusicCore::generate_excerpts(DocumentId id) {
    try {
        impl_->check_owner();
        auto& document = impl_->find(id);
        auto existing = list_score_excerpts(document.project->masterScore());
        if (!existing.empty()) return {document.revision, std::move(existing)};
        if (document.revision.value == std::numeric_limits<std::uint64_t>::max())
            throw CoreError(CoreError::resource_limit, "Document revision limit reached");
        // Excerpt creation changes a candidate project only. A failed layout or
        // allocation cannot leave a partially generated live score behind.
        const auto bytes = export_native_score(document.project, "mscz", 0, impl_->soundfont);
        auto candidate = load_native_score(bytes, "mscz");
        // The reload reads the version our writer stamped; keep the source file's.
        copy_score_versions(document.project->masterScore(), candidate->masterScore());
        if (!generate_score_excerpts(candidate->masterScore()))
            return {document.revision, {}};
        const Revision next{document.revision.value + 1};
        ExcerptsData result{next, list_score_excerpts(candidate->masterScore())};
        std::list<Impl::HistoryEntry> history{{next, {}, {}}};
        // audio_mix_defaults is identity-keyed, so it survives this reload.
        document.project = std::move(candidate);
        document.revision = next;
        document.last_dom_reload = next;
        Impl::publish_history(document, history);
        return result;
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

AudioTracksData MusicCore::audio_tracks(DocumentId id, int excerpt_id) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);
        auto tracks = Impl::describe_audio_tracks(document.project, excerpt_id);
        const AudioMix mix = Impl::default_audio_mix(document, tracks, excerpt_id);
        for (std::size_t index = 0; index < tracks.size() && index < mix.tracks.size(); ++index) {
            tracks[index].output = mix.tracks[index].output;
        }
        return AudioTracksData{document.revision, std::move(tracks)};
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

AudioMix MusicCore::audio_export_mix(DocumentId id, const ExportOptions& options) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);
        return impl_->export_mix(document, options);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

RegionData MusicCore::query_region(DocumentId id, const ScoreRegion& region) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);
        RegionData data;
        data.revision = document.revision;
        data.region = query_score_region(document.project->masterScore(), region);
        return data;
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

ScoreInfo MusicCore::replace_measures(DocumentId id, std::size_t part_index, std::size_t start_measure,
                                      const std::vector<Measure>& measures) {
    try {
        impl_->check_owner();
        return impl_->replace(id, part_index, start_measure, measures, nullptr).score;
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

EditResult MusicCore::replace_measures(DocumentId id, std::size_t part_index, std::size_t start_measure,
                                       const std::vector<Measure>& measures,
                                       const EditContext& context) {
    try {
        impl_->check_owner();
        return impl_->replace(id, part_index, start_measure, measures, &context);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

EditResult MusicCore::set_pitches(DocumentId id, const NativePitchEdit& edit,
                                  const EditContext& context) {
    try {
        impl_->check_owner();
        return impl_->set_pitches(id, edit, context);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

ExportData MusicCore::export_bytes(DocumentId id, ExportFormat format, const ExportOptions& options) {
    try {
        impl_->check_owner();
        return impl_->export_document(id, format, options);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

double MusicCore::audio_export_duration(DocumentId id, bool use_repeats) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);
        const auto* master = document.project->masterScore();
        if (!master)
            throw CoreError(CoreError::Code::engine_failure, "Score has no master score");
        // Same duration handoff as export_score_audio() and
        // notationplayback.cpp:235-252: repeat-expanded play time plus 3 s.
        const int last_utick = master->repeatList(use_repeats).ticks();
        return master->utick2utime(last_utick) + 3.0;
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

AudioExportResult MusicCore::export_audio(DocumentId id, ExportFormat format, const std::string& output_path,
                                          const ExportOptions& options) {
    try {
        impl_->check_owner();
        EngineAudioFormat engine_format;
        switch (format) {
        case ExportFormat::Wav: engine_format = EngineAudioFormat::Wav; break;
        case ExportFormat::Flac: engine_format = EngineAudioFormat::Flac; break;
        case ExportFormat::Ogg: engine_format = EngineAudioFormat::Ogg; break;
        case ExportFormat::Mp3: engine_format = EngineAudioFormat::Mp3; break;
        case ExportFormat::Aac: engine_format = EngineAudioFormat::Aac; break;
        default:
            throw CoreError(CoreError::Code::invalid_argument,
                            "Audio export requires an audio format");
        }
        if (!engine_audio_initialized()) {
            throw CoreError(CoreError::Code::engine_failure,
                            "Audio engine is not initialized in this worker process");
        }

        const auto& document = impl_->find(id);
        auto* master = document.project->masterScore();
        auto* selected = select_score(master, options.excerpt_id);
        const auto tracks = Impl::describe_audio_tracks(selected);
        const AudioMix mix = Impl::resolved_audio_mix(document, tracks, options);

        AudioRenderRequest request;
        request.format = engine_format;
        request.output_path = output_path;
        request.excerpt_id = options.excerpt_id;
        request.use_repeats = options.midi_expand_repeats;
        request.master_volume_db = mix.master.volume_db;
        request.master_balance = mix.master.balance;
        request.tracks.reserve(mix.tracks.size());
        for (const AudioTrackParams& track : mix.tracks) {
            request.tracks.push_back(EngineAudioTrackParams{
                track.output.volume_db, track.output.balance, track.output.solo, track.output.muted });
        }

        const AudioRenderResult rendered = export_score_audio(master, request);
        AudioExportResult result;
        result.ok = rendered.ok;
        result.error = rendered.error;
        result.frames = rendered.frames;
        result.bytes = rendered.bytes;
        result.play_time_secs = rendered.play_time_secs;
        result.duration_secs = rendered.duration_secs;
        return result;
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

SnapshotData MusicCore::snapshot(DocumentId id) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);
        const auto* master = document.project->masterScore();
        SnapshotData result;
        result.revision = document.revision;
        result.versions = {master->mscoreVersion().toStdString(), master->mscoreRevision(),
                           master->mscVersion()};
        // Read the stamps first: the export restores them, and the worker must
        // apply the source file's values to its own reload.
        result.bytes = impl_->export_document(id, ExportFormat::Mscz, {}).bytes;
        return result;
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

void MusicCore::restore_versions(DocumentId id, const ScoreVersions& versions) {
    try {
        impl_->check_owner();
        auto& document = impl_->find(id);
        apply_score_versions(document.project->masterScore(), versions.mscore_version,
                             versions.mscore_revision, versions.msc_version);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

void MusicCore::close(DocumentId id) {
    try {
        impl_->check_owner();
        if (impl_->documents.erase(id.value) == 0)
            throw CoreError(CoreError::Code::not_found, "Unknown or closed documentId");
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

RuntimeOptions MusicCore::runtime_options() const {
    impl_->check_owner();
    auto result = impl_->options;
    result.font_paths.clear();
    for (const auto& font : runtime_fonts()) result.font_paths.push_back(font.path);
    return result;
}

RuntimeSoundFontInfo MusicCore::set_soundfont(const std::string& path) {
    try {
        impl_->check_owner();
        auto frozen = freeze_runtime_soundfont(path);
        // Allocate both copies before publishing the new selection.
        auto selected = frozen.path;
        auto option = frozen.path;
        impl_->soundfont.swap(selected);
        impl_->options.soundfont.swap(option);
        return frozen;
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

RuntimeFontInfo MusicCore::add_font(const std::string& path) {
    try {
        impl_->check_owner();
        auto font = add_runtime_font(path);
        bool failed = false;
        for (auto& [id, document] : impl_->documents) {
            if (document.poisoned) continue;
            try {
                document.project->masterScore()->doLayout();
                for (const auto& excerpt : list_score_excerpts(document.project->masterScore()))
                    select_score(document.project->masterScore(), excerpt.id)->doLayout();
            } catch (...) {
                document.poisoned = true;
                failed = true;
            }
        }
        if (failed)
            throw CoreError(CoreError::poisoned_document,
                "Font was installed, but some documents failed relayout and must be closed/reopened");
        return font;
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

void MusicCore::poison_document_for_testing(DocumentId id) {
    impl_->check_owner();
    auto& doc = impl_->find(id);
    doc.poisoned = true;
}

} // namespace music
