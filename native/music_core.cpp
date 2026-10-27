#include "music_core.h"

#include "audio_codecs.h"
#include "composition.h"
#include "noqt_runtime.h"
#include "score_audio.h"
#include "score_metadata.h"

#include "engraving/dom/masterscore.h"
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
    }
    return "";
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
        // Absence means the all-default mix for the selected score. Entries
        // are full replacement mixes keyed by excerpt id (-1 is the full score).
        std::map<int, AudioMix> audio_mixes;
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

    static std::vector<AudioTrackInfo> describe_audio_tracks(mu::engraving::Score* score) {
        const auto descriptions = score_audio_track_info(score);
        std::vector<AudioTrackInfo> result;
        result.reserve(descriptions.size());
        for (const ScoreAudioTrackInfo& description : descriptions) {
            result.push_back(AudioTrackInfo{description.track_id, description.part_id,
                                            description.instrument_id, description.name});
        }
        return result;
    }

    static std::vector<AudioTrackInfo> describe_audio_tracks(const ScoreProject& project,
                                                             int excerpt_id) {
        return describe_audio_tracks(select_score(project->masterScore(), excerpt_id));
    }

    static std::vector<ScoreMetadataTrack> describe_metadata_tracks(mu::engraving::Score* score,
                                                                   const AudioMix& mix) {
        const auto track_infos = describe_audio_tracks(score);
        std::vector<ScoreMetadataTrack> meta_tracks;
        meta_tracks.reserve(track_infos.size());
        for (const AudioTrackInfo& info : track_infos) {
            ScoreMetadataTrack track;
            track.track_id = info.track_id;
            track.part_id = info.part_id;
            track.instrument_id = info.instrument_id;
            track.name = info.name;
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

    // Metadata must stay available for scores whose parts cannot be planned as
    // audio stems (e.g. multi-instrument parts); their tracks are then empty.
    static std::vector<ScoreMetadataTrack> metadata_tracks_or_empty(const Document& document,
                                                                   mu::engraving::Score* score,
                                                                   int excerpt_id) {
        std::vector<AudioTrackInfo> tracks;
        try {
            tracks = describe_audio_tracks(score);
        } catch (const std::runtime_error&) {
            return {};
        }
        return describe_metadata_tracks(score, current_audio_mix(document, excerpt_id, tracks));
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

    static AudioMix default_audio_mix(const std::vector<AudioTrackInfo>& tracks) {
        AudioMix mix;
        mix.tracks.reserve(tracks.size());
        for (const AudioTrackInfo& track : tracks)
            mix.tracks.push_back(AudioTrackParams{track.track_id, AudioOutputParams{}});
        return mix;
    }

    // Validates the complete replacement and returns it in actual score order.
    static AudioMix canonical_audio_mix(const AudioMix& candidate,
                                        const std::vector<AudioTrackInfo>& tracks) {
        validate_audio_output(candidate.master, "Master mix");
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
            validate_audio_output(track.output, "Track mix");
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
        return canonical;
    }

    static void validate_audio_output_patch(const AudioOutputPatch& patch, std::string_view label) {
        if (patch.volume_db.has_value()) {
            if (!std::isfinite(*patch.volume_db) || *patch.volume_db < -120.0 || *patch.volume_db > 24.0) {
                throw CoreError(CoreError::Code::invalid_argument,
                                std::string(label) + " volumeDb must be finite and in [-120, 24]");
            }
        }
        if (patch.balance.has_value()) {
            if (!std::isfinite(*patch.balance) || *patch.balance < -1.0 || *patch.balance > 1.0) {
                throw CoreError(CoreError::Code::invalid_argument,
                                std::string(label) + " balance must be finite and in [-1, 1]");
            }
        }
    }

    static void apply_output_patch(AudioOutputParams& target, const AudioOutputPatch& patch) {
        if (patch.volume_db.has_value()) target.volume_db = *patch.volume_db;
        if (patch.balance.has_value()) target.balance = *patch.balance;
        if (patch.solo.has_value()) target.solo = *patch.solo;
        if (patch.muted.has_value()) target.muted = *patch.muted;
        if (patch.force_mute.has_value()) target.force_mute = *patch.force_mute;
    }

    static AudioMix canonical_patched_mix(const AudioMix& current,
                                          const AudioMixPatch& patch,
                                          const std::vector<AudioTrackInfo>& tracks) {
        if (patch.master.has_value()) {
            validate_audio_output_patch(*patch.master, "Master mix");
        }

        if (patch.tracks.has_value()) {
            if (patch.tracks->size() > audio_max_tracks) {
                throw CoreError(CoreError::Code::invalid_argument,
                                "Audio mix patch cannot contain more than 15 track entries");
            }

            std::map<std::int64_t, const AudioTrackInfo*> valid_tracks;
            for (const AudioTrackInfo& track : tracks) {
                valid_tracks.emplace(track.track_id, &track);
            }

            std::set<std::int64_t> seen_ids;
            for (const AudioTrackPatch& track_patch : *patch.tracks) {
                if (track_patch.track_id < 0) {
                    throw CoreError(CoreError::Code::invalid_argument,
                                    "Audio mix trackId must be non-negative");
                }
                if (!seen_ids.insert(track_patch.track_id).second) {
                    throw CoreError(CoreError::Code::invalid_argument,
                                    "Audio mix contains duplicate trackId " + std::to_string(track_patch.track_id));
                }
                if (valid_tracks.find(track_patch.track_id) == valid_tracks.end()) {
                    throw CoreError(CoreError::Code::invalid_argument,
                                    "Audio mix contains an unknown trackId");
                }
                validate_audio_output_patch(track_patch.output, "Track mix");
            }
        }

        AudioMix candidate = current;
        if (patch.master.has_value()) {
            apply_output_patch(candidate.master, *patch.master);
        }
        if (patch.tracks.has_value()) {
            for (const AudioTrackPatch& track_patch : *patch.tracks) {
                for (AudioTrackParams& candidate_track : candidate.tracks) {
                    if (candidate_track.track_id == track_patch.track_id) {
                        apply_output_patch(candidate_track.output, track_patch.output);
                        break;
                    }
                }
            }
        }
        return candidate;
    }

    static AudioMix current_audio_mix(const Document& document, int excerpt_id,
                                      const std::vector<AudioTrackInfo>& tracks) {
        const auto it = document.audio_mixes.find(excerpt_id);
        if (it == document.audio_mixes.end()) return default_audio_mix(tracks);
        try {
            return canonical_audio_mix(it->second, tracks);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const CoreError& error) {
            throw CoreError(CoreError::Code::invalid_argument,
                            "Stored audio mix is incompatible with the selected score: "
                                + std::string(error.what()));
        }
    }

    static bool is_default_audio_mix(const AudioMix& mix) {
        if (!(mix.master == AudioOutputParams{})) return false;
        for (const AudioTrackParams& track : mix.tracks)
            if (!(track.output == AudioOutputParams{})) return false;
        return true;
    }

    // Keep stored mixes only while their excerpt still exists and its ordered
    // track identities match. Obsolete excerpt mappings are dropped instead of
    // being applied silently to a different score.
    static std::map<int, AudioMix> preserved_audio_mixes(const std::map<int, AudioMix>& mixes,
                                                         const ScoreProject& project) {
        std::map<int, AudioMix> result;
        for (const auto& [excerpt_id, mix] : mixes) {
            try {
                const auto tracks = describe_audio_tracks(project, excerpt_id);
                result.emplace(excerpt_id, canonical_audio_mix(mix, tracks));
            } catch (const std::bad_alloc&) {
                throw;
            } catch (...) {
                // Invalid excerpt or changed track identities: drop the entry.
            }
        }
        return result;
    }

    std::unique_ptr<AudioSession> make_audio_session(const Document& document,
                                                    const ExportOptions& options,
                                                    double start_seconds) const {
        if (soundfont.empty())
            throw CoreError(CoreError::Code::engine_failure,
                            "Set a SoundFont to enable audio sessions");
        auto* score = select_score(document.project->masterScore(), options.excerpt_id);
        const auto tracks = describe_audio_tracks(score);
        const AudioMix mix = current_audio_mix(document, options.excerpt_id, tracks);
        auto stems = score_audio_stems(score, options.midi_expand_repeats,
                                       options.midi_export_rpns);
        auto session = std::make_unique<AudioSession>(std::move(stems), soundfont,
                                                      start_seconds, retain_runtime_assets());
        session->set_mix(mix);
        return session;
    }

    AudioMixData get_audio_mix(const Document& document, int excerpt_id) const {
        const auto tracks = describe_audio_tracks(document.project, excerpt_id);
        return AudioMixData{document.revision, current_audio_mix(document, excerpt_id, tracks)};
    }

    AudioMixData set_audio_mix(Document& document, const AudioMix& candidate, int excerpt_id) {
        const auto tracks = describe_audio_tracks(document.project, excerpt_id);
        const AudioMix canonical = canonical_audio_mix(candidate, tracks);
        const AudioMix current = current_audio_mix(document, excerpt_id, tracks);
        if (canonical == current) return AudioMixData{document.revision, current};

        if (document.revision.value == std::numeric_limits<std::uint64_t>::max())
            throw CoreError(CoreError::Code::resource_limit, "Document revision limit reached");
        const Revision next{document.revision.value + 1};

        // Copy and modify the map before publication so allocation failure
        // cannot leave a partially updated document. Setting the all-default
        // mix removes the entry and restores the legacy export behavior.
        std::map<int, AudioMix> prepared = document.audio_mixes;
        if (is_default_audio_mix(canonical)) prepared.erase(excerpt_id);
        else prepared.insert_or_assign(excerpt_id, canonical);

        AudioMixData result{next, canonical};
        std::list<HistoryEntry> history;
        {
            HistoryEntry entry;
            entry.revision = next;
            history.push_back(std::move(entry));
        }

        document.audio_mixes.swap(prepared);
        document.revision = next;
        // A mix change does not rebuild the DOM, so last_dom_reload is unchanged.
        publish_history(document, history);
        return result;
    }

    AudioMixData set_audio_mix(Document& document, const AudioMixPatch& patch, int excerpt_id) {
        const auto tracks = describe_audio_tracks(document.project, excerpt_id);
        const AudioMix current = current_audio_mix(document, excerpt_id, tracks);
        const AudioMix canonical = canonical_patched_mix(current, patch, tracks);
        if (canonical == current) return AudioMixData{document.revision, current};

        if (document.revision.value == std::numeric_limits<std::uint64_t>::max())
            throw CoreError(CoreError::Code::resource_limit, "Document revision limit reached");
        const Revision next{document.revision.value + 1};

        std::map<int, AudioMix> prepared = document.audio_mixes;
        if (is_default_audio_mix(canonical)) prepared.erase(excerpt_id);
        else prepared.insert_or_assign(excerpt_id, canonical);

        AudioMixData result{next, canonical};
        std::list<HistoryEntry> history;
        {
            HistoryEntry entry;
            entry.revision = next;
            history.push_back(std::move(entry));
        }

        document.audio_mixes.swap(prepared);
        document.revision = next;
        publish_history(document, history);
        return result;
    }

    static std::optional<AudioFormat> audio_format(ExportFormat format) noexcept {
        switch (format) {
        case ExportFormat::Wav: return AudioFormat::Wav;
        case ExportFormat::Flac: return AudioFormat::Flac;
        case ExportFormat::Ogg: return AudioFormat::Ogg;
        case ExportFormat::Mp3: return AudioFormat::Mp3;
        default: return std::nullopt;
        }
    }

    std::string encode_audio_export(const Document& document, const ExportOptions& options,
                                    AudioFormat format) const {
        auto session = make_audio_session(document, options, 0.0);
        auto data = encode_audio(*session, format);
        if (data.empty())
            throw CoreError(CoreError::Code::engine_failure, "Audio encoder returned no data");
        return data;
    }

    ScoreInfo insert(ScoreProject project, std::optional<Composition> composition) {
        const DocumentId id{next_document++};
        const auto result = info(id, project, Revision{1}, composition.has_value());
        Document document;
        document.project = std::move(project);
        document.composition = std::move(composition);
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
        auto audio_prepared = preserved_audio_mixes(document.audio_mixes, project);

        document.project = std::move(project);
        document.audio_mixes.swap(audio_prepared);
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
            const auto meta_tracks = metadata_tracks_or_empty(document, selected, options.excerpt_id);
            return ExportData{document.revision, score_metadata(selected, meta_tracks).dump()};
        }
        if (const auto codec = audio_format(format)) {
            if (format != ExportFormat::Wav) {
                return ExportData{document.revision,
                                  encode_audio_export(document, options, *codec)};
            }
            // Keep the exact legacy single-track WAV path whenever the selected
            // mix is all-default (including an absent map entry).
            const auto stored = document.audio_mixes.find(options.excerpt_id);
            if (stored != document.audio_mixes.end()
                && !is_default_audio_mix(stored->second)) {
                return ExportData{document.revision,
                                  encode_audio_export(document, options, *codec)};
            }
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
        auto project = load_native_score(bytes, input_backend_name(format));
        return impl_->insert(std::move(project), std::nullopt);
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
        if (!generate_score_excerpts(candidate->masterScore()))
            return {document.revision, {}};
        const Revision next{document.revision.value + 1};
        ExcerptsData result{next, list_score_excerpts(candidate->masterScore())};
        std::list<Impl::HistoryEntry> history{{next, {}, {}}};
        auto audio_prepared = Impl::preserved_audio_mixes(document.audio_mixes, candidate);
        document.project = std::move(candidate);
        document.audio_mixes.swap(audio_prepared);
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
        return AudioTracksData{document.revision,
                                Impl::describe_audio_tracks(document.project, excerpt_id)};
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

AudioMixData MusicCore::audio_mix(DocumentId id, int excerpt_id) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);
        return impl_->get_audio_mix(document, excerpt_id);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

AudioMixData MusicCore::set_audio_mix(DocumentId id, const AudioMix& mix, int excerpt_id) {
    try {
        impl_->check_owner();
        auto& document = impl_->find(id);
        return impl_->set_audio_mix(document, mix, excerpt_id);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

AudioMixData MusicCore::set_audio_mix(DocumentId id, const AudioMixPatch& patch, int excerpt_id) {
    try {
        impl_->check_owner();
        auto& document = impl_->find(id);
        return impl_->set_audio_mix(document, patch, excerpt_id);
    } catch (...) {
        translate_current_exception(CoreError::Code::engine_failure);
    }
}

std::unique_ptr<AudioSession> MusicCore::create_audio_session(DocumentId id,
                                                               const ExportOptions& options,
                                                               double start_seconds) {
    try {
        impl_->check_owner();
        const auto& document = impl_->find(id);
        return impl_->make_audio_session(document, options, start_seconds);
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

ExportData MusicCore::snapshot(DocumentId id) {
    try {
        impl_->check_owner();
        return impl_->export_document(id, ExportFormat::Mscz, {});
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
