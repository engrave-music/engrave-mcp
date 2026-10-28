#include "engine.h"
#include "music_core.h"
#include "atomic_file.h"
#include "background_job.h"
#include "mcp_transport.h"
#include "worker_protocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
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
using Json = nlohmann::json;

namespace {

constexpr std::size_t max_input = 8 * 1024 * 1024;
// Padded standard base64 expands every three bytes to four characters, so this
// is the longest text that can still decode within max_input.
constexpr std::size_t max_base64 = ((max_input + 2) / 3) * 4;
constexpr std::size_t max_operation_id = 128;
constexpr std::size_t max_pitch_changes = 256;

std::string text_arg(const Json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
    auto value = j.at(key).get<std::string>();
    if (value.empty() || value.find('\0') != std::string::npos) throw std::invalid_argument(std::string(key) + " is empty or contains NUL");
    return value;
}

std::size_t index_arg(const Json& j, const char* key, std::size_t maximum) {
    if (!j.contains(key) || !j.at(key).is_number_integer() || j.at(key) < 0 || j.at(key) > maximum)
        throw std::invalid_argument(std::string(key) + " is out of range");
    return j.at(key).get<std::size_t>();
}

// Strictly non-negative size without an arbitrary upper bound.
std::size_t size_arg(const Json& j, const char* key) {
    if (!j.contains(key)) throw std::invalid_argument(std::string(key) + " is required");
    const auto& value = j.at(key);
    if (!value.is_number_integer() || value.is_boolean())
        throw std::invalid_argument(std::string(key) + " must be a non-negative integer");
    if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            throw std::invalid_argument(std::string(key) + " is out of range");
        return static_cast<std::size_t>(raw);
    }
    const auto raw = value.get<std::int64_t>();
    if (raw < 0) throw std::invalid_argument(std::string(key) + " must be a non-negative integer");
    return static_cast<std::size_t>(raw);
}

std::uint64_t positive_u64_arg(const Json& j, const char* key) {
    if (!j.contains(key)) throw std::invalid_argument(std::string(key) + " is required");
    const auto& value = j.at(key);
    if (!value.is_number_integer() || value.is_boolean())
        throw std::invalid_argument(std::string(key) + " must be a positive integer");
    if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw == 0) throw std::invalid_argument(std::string(key) + " must be a positive integer");
        return raw;
    }
    const auto raw = value.get<std::int64_t>();
    if (raw <= 0) throw std::invalid_argument(std::string(key) + " must be a positive integer");
    return static_cast<std::uint64_t>(raw);
}

std::string operation_id_arg(const Json& j) {
    auto value = text_arg(j, "operationId");
    if (value.size() > max_operation_id)
        throw std::invalid_argument("operationId must be at most 128 bytes");
    return value;
}

void only_keys(const Json& j, std::initializer_list<const char*> allowed,
               std::string_view prefix = "Unknown argument: ") {
    if (!j.is_object()) throw std::invalid_argument("Arguments must be an object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        bool found = false;
        for (const char* key : allowed) {
            if (it.key() == key) {
                found = true;
                break;
            }
        }
        if (!found) throw std::invalid_argument(std::string(prefix) + it.key());
    }
}

std::string local_path_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string read_input(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open score input");
    std::string data(max_input + 1, '\0');
    file.read(data.data(), data.size());
    if (file.bad()) throw std::runtime_error("Cannot read score input");
    data.resize(static_cast<std::size_t>(file.gcount()));
    if (data.size() > max_input) throw std::invalid_argument("Score input exceeds 8 MiB");
    return data;
}

int base64_value(unsigned char value) noexcept {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

// Standard padded base64 (RFC 4648 section 4). Whitespace and the URL-safe
// alphabet are rejected; unused trailing bits are ignored.
std::string decode_base64_input(std::string_view text) {
    if (text.size() % 4 != 0) throw std::invalid_argument("data must be padded standard base64");
    if (text.size() > max_base64) throw std::invalid_argument("Score input exceeds 8 MiB");

    std::size_t padding = 0;
    while (padding < text.size() && text[text.size() - padding - 1] == '=') ++padding;
    if (padding > 2) throw std::invalid_argument("data has invalid base64 padding");
    const std::size_t payload = text.size() - padding;
    if ((padding == 0 && payload % 4 != 0) || (padding == 1 && payload % 4 != 3)
        || (padding == 2 && payload % 4 != 2))
        throw std::invalid_argument("data has invalid base64 padding");

    std::string decoded;
    decoded.reserve(payload * 3 / 4);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (std::size_t index = 0; index < payload; ++index) {
        const int value = base64_value(static_cast<unsigned char>(text[index]));
        if (value < 0) throw std::invalid_argument("data contains invalid base64 characters");
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((accumulator >> bits) & 0xFFu));
        }
    }
    if (decoded.size() > max_input) throw std::invalid_argument("Score input exceeds 8 MiB");
    return decoded;
}

DocumentId document_id(const Json& args) {
    return document_id_from_string(text_arg(args, "documentId"));
}

std::int64_t checked_integer(const Json& value, const char* type_message, const char* range_message) {
    if (!value.is_number_integer() || value.is_boolean())
        throw std::invalid_argument(type_message);
    if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument(range_message);
        return static_cast<std::int64_t>(raw);
    }
    return value.get<std::int64_t>();
}

int excerpt_arg(const Json& args) {
    if (!args.contains("excerptId")) return -1;
    const auto value = checked_integer(args.at("excerptId"), "excerptId must be an integer",
        "excerptId is out of range");
    if (value < -1 || value > std::numeric_limits<int>::max())
        throw std::invalid_argument("excerptId is out of range");
    return static_cast<int>(value);
}

Note decode_note(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("Note must be a JSON object");
    only_keys(json, {"pitches", "ticks"}, "Unknown key in note: ");

    if (!json.contains("ticks")) throw std::invalid_argument("Note must contain 'ticks'");
    const auto ticks = checked_integer(json.at("ticks"), "Note ticks must be an integer",
                                       "Note ticks must be one of 120, 240, 480, 960, 1920");
    if (ticks != 120 && ticks != 240 && ticks != 480 && ticks != 960 && ticks != 1920)
        throw std::invalid_argument("Note ticks must be one of 120, 240, 480, 960, 1920");

    if (!json.contains("pitches") || !json.at("pitches").is_array())
        throw std::invalid_argument("Note must contain a 'pitches' array");

    Note note;
    note.ticks = static_cast<int>(ticks);
    for (const auto& pitch : json.at("pitches")) {
        const auto value = checked_integer(pitch, "Pitch must be an integer",
                                           "Pitch must be between 12 and 127 (MusicXML octave 0..9)");
        if (value < 12 || value > 127)
            throw std::invalid_argument("Pitch must be between 12 and 127 (MusicXML octave 0..9)");
        note.pitches.push_back(static_cast<int>(value));
    }
    return note;
}

Measure decode_measure(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("Measure must be a JSON object");
    only_keys(json, {"notes"}, "Unknown key in measure: ");
    if (!json.contains("notes") || !json.at("notes").is_array())
        throw std::invalid_argument("Measure must contain a 'notes' array");

    Measure measure;
    measure.notes.reserve(json.at("notes").size());
    for (const auto& note : json.at("notes"))
        measure.notes.push_back(decode_note(note));
    return measure;
}

Composition decode_composition(const Json& score) {
    if (!score.is_object()) throw std::invalid_argument("Score must be a JSON object");
    only_keys(score, {"title", "tempo", "parts"}, "Unknown key at root level: ");

    Composition composition;
    if (score.contains("title")) {
        if (!score.at("title").is_string()) throw std::invalid_argument("Title must be a string");
        composition.title = score.at("title").get<std::string>();
    }

    if (score.contains("tempo")) {
        const auto tempo = checked_integer(score.at("tempo"), "Tempo must be an integer",
                                           "Tempo must be between 20 and 300");
        if (tempo < 20 || tempo > 300) throw std::invalid_argument("Tempo must be between 20 and 300");
        composition.tempo = static_cast<int>(tempo);
    }

    if (!score.contains("parts") || !score.at("parts").is_array())
        throw std::invalid_argument("Score must contain a 'parts' array");

    for (const auto& part_json : score.at("parts")) {
        if (!part_json.is_object()) throw std::invalid_argument("Part must be a JSON object");
        only_keys(part_json, {"name", "midiProgram", "measures"}, "Unknown key in part: ");

        if (!part_json.contains("name") || !part_json.at("name").is_string())
            throw std::invalid_argument("Part must contain a string 'name'");
        Part part;
        part.name = part_json.at("name").get<std::string>();

        if (!part_json.contains("midiProgram")) throw std::invalid_argument("Part must contain 'midiProgram'");
        const auto program = checked_integer(part_json.at("midiProgram"), "Part midiProgram must be an integer",
                                             "Part midiProgram must be between 1 and 128");
        if (program < 1 || program > 128)
            throw std::invalid_argument("Part midiProgram must be between 1 and 128");
        part.midi_program = static_cast<int>(program);

        if (!part_json.contains("measures") || !part_json.at("measures").is_array())
            throw std::invalid_argument("Part must contain a 'measures' array");
        part.measures.reserve(part_json.at("measures").size());
        for (const auto& measure : part_json.at("measures"))
            part.measures.push_back(decode_measure(measure));
        composition.parts.push_back(std::move(part));
    }

    return composition;
}

Json info_json(const ScoreInfo& info) {
    return {
        {"documentId", document_id_to_string(info.id)},
        {"revision", info.revision.value},
        {"title", info.title},
        {"titleFilenameSafe", info.title_filename_safe},
        {"pages", info.pages},
        {"parts", info.parts},
        {"editable", info.editable},
        {"capabilities", {{"replaceMeasures", info.editable}, {"queryRegion", true},
                          {"setPitches", "bounded-native-pitch-edits"}}},
    };
}

Json edit_result_json(const EditResult& result) {
    Json json = info_json(result.score);
    json["baseRevision"] = result.base_revision.value;
    Json rebased = Json::array();
    for (const Revision revision : result.rebased_over) rebased.push_back(revision.value);
    json["rebasedOver"] = std::move(rebased);
    json["replayed"] = result.replayed;
    json["changedNotes"] = result.changed_notes;
    return json;
}

Json region_json(const RegionData& data) {
    Json notes = Json::array();
    for (const NoteView& note : data.region.notes) {
        Json note_json = Json::object();
        note_json["id"] = note.id;
        note_json["measureId"] = note.measure_id;
        note_json["partIndex"] = note.part_index;
        note_json["staffIndex"] = note.staff_index;
        note_json["measureIndex"] = note.measure_index;
        note_json["voice"] = note.voice;
        note_json["tick"] = note.tick;
        note_json["durationTicks"] = note.duration_ticks;
        note_json["pitch"] = note.pitch;
        note_json["tied"] = note.tied;
        note_json["spanned"] = note.spanned;
        notes.push_back(std::move(note_json));
    }

    Json measures = Json::array();
    for (const MeasureView& measure : data.region.measures) {
        Json measure_json = Json::object();
        measure_json["id"] = measure.id;
        measure_json["index"] = measure.index;
        measure_json["tick"] = measure.tick;
        measure_json["durationTicks"] = measure.duration_ticks;
        measures.push_back(std::move(measure_json));
    }

    Json result = Json::object();
    result["revision"] = data.revision.value;
    result["notes"] = std::move(notes);
    result["measures"] = std::move(measures);
    result["hasCrossRegionSpanners"] = data.region.has_cross_region_spanners;
    return result;
}

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

// Sync audio exports still run in a child: the host process never initialises
// the audio engine. This mirrors the background-job capture and publication,
// but waits for the worker before returning the tool result.
Json export_audio_via_worker(MusicCore& core, DocumentId id, ExportFormat format,
                             const std::filesystem::path& destination,
                             const ExportOptions& options) {
    const auto snapshot = core.snapshot(id);
    const auto directory = create_private_directory(
        std::filesystem::absolute(std::filesystem::temp_directory_path()));
    const auto staging_directory = create_private_directory(destination.parent_path());
    struct Cleanup {
        std::filesystem::path directory, staging_directory;
        ~Cleanup() {
            std::error_code ignored;
            if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
            if (!staging_directory.empty()) std::filesystem::remove_all(staging_directory, ignored);
        }
    } cleanup{ directory, staging_directory };

    write_atomic(directory / "snapshot.mscz", snapshot.bytes);
    ExportWorkerRequest request;
    request.snapshot_path = directory / "snapshot.mscz";
    request.output_path = staging_directory / "output";
    request.result_path = directory / "result.json";
    request.runtime = core.runtime_options();
    request.format = format;
    request.options = options;
    request.versions = snapshot.versions;
    request.cache_directory = directory / "runtime-cache";
    // The snapshot has no audiosettings.json; send the effective mix.
    request.mix = core.audio_export_mix(id, options);
    request.has_mix = true;
    write_atomic(directory / "request.json", worker_request_json(request).dump());

    reserve_output_path(destination);
    struct Release {
        std::filesystem::path path;
        ~Release() { release_output_path(path); }
    } release{ destination };

    WorkerProcess process(current_executable_path(), directory / "request.json",
                          directory / "worker.log");
    std::optional<int> status;
    while (!(status = process.poll())) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const auto worker_result = Json::parse(read_bounded_file(directory / "result.json", 64 * 1024));
    if (*status != 0 || !worker_result.value("ok", false))
        throw std::runtime_error(worker_result.value("error", "Audio export worker failed"));

    const auto staged = staging_directory / "output";
    const auto bytes = std::filesystem::file_size(staged);
    if (!bytes || bytes > 256 * 1024 * 1024
        || bytes != worker_result.at("bytes").get<std::uintmax_t>()) {
        throw std::runtime_error("Worker output size does not match result");
    }
    publish_job_output(staged, destination);
    return Json{{"path", local_path_utf8(destination)}, {"format", format_name(format)},
                {"bytes", bytes}, {"revision", snapshot.revision.value}};
}

} // namespace

struct Engine::Impl {
    std::unique_ptr<MusicCore> core;
};

Engine::Engine(int&, char** argv, const std::string& soundfont, const std::string& resources)
    : impl_(std::make_unique<Impl>()) {
    // Path discovery stays in the engine; MusicCore only receives concrete
    // resource/SoundFont paths.
    if (resources.empty() && (!argv || !argv[0])) throw std::invalid_argument("Missing executable path");
    std::filesystem::path assets;
    if (resources.empty()) {
        auto executable = std::filesystem::u8path(argv[0]);
#ifdef __linux__
        std::error_code error;
        const auto resolved = std::filesystem::canonical("/proc/self/exe", error);
        if (!error) executable = resolved;
#endif
        assets = std::filesystem::absolute(executable.parent_path() / "assets");
    } else {
        assets = std::filesystem::absolute(std::filesystem::u8path(resources));
    }

    impl_->core = std::make_unique<MusicCore>(RuntimeOptions{local_path_utf8(assets), soundfont});
}

Engine::~Engine() = default;

MusicCore& Engine::core() {
    return *impl_->core;
}

std::string Engine::tools_json() {
    return
#include "tool_schema.inc"
    ;
}

std::string Engine::call(const std::string& tool, const std::string& arguments_json) {
    const auto args = Json::parse(arguments_json);
    Json result;
    if (tool == "score_create") {
        only_keys(args, {"score"});
        result = info_json(impl_->core->create(decode_composition(args.at("score"))));
    } else if (tool == "score_open") {
        only_keys(args, {"path", "musicxml", "data", "format"});
        const bool has_path = args.contains("path");
        const bool has_musicxml = args.contains("musicxml");
        const bool has_data = args.contains("data");
        if (static_cast<int>(has_path) + static_cast<int>(has_musicxml) + static_cast<int>(has_data) != 1)
            throw std::invalid_argument("Specify exactly one of path, musicxml or data");
        if (has_data != args.contains("format"))
            throw std::invalid_argument("format is required with data and rejected otherwise");

        std::string data;
        auto format = InputFormat::MusicXml;
        if (has_path) {
            const auto path = std::filesystem::u8path(text_arg(args, "path"));
            auto extension = local_path_utf8(path.extension());
            if (!extension.empty()) extension.erase(0, 1);
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto parsed = input_format_from_name(extension);
            if (!parsed) throw std::invalid_argument("Unsupported input format");
            format = *parsed;
            data = read_input(path);
        } else if (has_musicxml) {
            data = text_arg(args, "musicxml");
        } else {
            const auto parsed = input_format_from_name(text_arg(args, "format"));
            if (!parsed) throw std::invalid_argument("Unsupported input format");
            format = *parsed;
            data = decode_base64_input(text_arg(args, "data"));
        }
        result = info_json(impl_->core->open(data, format));
    } else if (tool == "score_get") {
        only_keys(args, {"documentId", "excerptId"});
        const auto view = impl_->core->get(document_id(args), excerpt_arg(args));
        result = info_json(view.info);
        if (view.composition) result["composition"] = composition_json(*view.composition);
        else result["composition"] = nullptr;
        result["musicxml"] = view.musicxml;
        result["metadata"] = Json::parse(view.metadata_json);
    } else if (tool == "score_generate_excerpts" || tool == "score_list_excerpts") {
        only_keys(args, {"documentId"});
        const auto data = tool == "score_generate_excerpts"
            ? impl_->core->generate_excerpts(document_id(args)) : impl_->core->excerpts(document_id(args));
        Json excerpts = Json::array();
        for (const auto& excerpt : data.excerpts)
            excerpts.push_back({{"id", excerpt.id}, {"title", excerpt.title},
                {"partIds", excerpt.part_ids}, {"pages", excerpt.pages}});
        result = {{"revision", data.revision.value}, {"excerpts", std::move(excerpts)}};
    } else if (tool == "score_audio_tracks") {
        only_keys(args, {"documentId", "excerptId"});
        const auto data = impl_->core->audio_tracks(document_id(args), excerpt_arg(args));
        Json tracks = Json::array();
        for (const AudioTrackInfo& track : data.tracks) {
            tracks.push_back({{"trackId", track.track_id}, {"partId", track.part_id},
                {"instrumentId", track.instrument_id}, {"partName", track.part_name},
                {"name", track.instrument_name}, {"chordSymbols", track.chord_symbols},
                {"volumeDb", track.output.volume_db}, {"balance", track.output.balance},
                {"solo", track.output.solo}, {"muted", track.output.muted},
                {"forceMute", track.output.force_mute}});
        }
        result = {{"revision", data.revision.value}, {"tracks", std::move(tracks)}};
    } else if (tool == "runtime_set_soundfont") {
        only_keys(args, {"path"});
        const auto asset = impl_->core->set_soundfont(text_arg(args, "path"));
        result = {{"path", asset.path}, {"contentHash", asset.content_hash}};
    } else if (tool == "runtime_add_font") {
        only_keys(args, {"path"});
        const auto asset = impl_->core->add_font(text_arg(args, "path"));
        result = {{"id", asset.id}, {"family", asset.family}, {"bold", asset.bold},
            {"italic", asset.italic}, {"path", asset.path}, {"contentHash", asset.content_hash}};
    } else if (tool == "score_query_region") {
        only_keys(args, {"documentId", "partIndex", "firstMeasure", "measureCount", "voice"});
        ScoreRegion region;
        region.part_index = size_arg(args, "partIndex");
        region.first_measure = size_arg(args, "firstMeasure");
        region.measure_count = size_arg(args, "measureCount");
        if (region.measure_count < 1 || region.measure_count > 128)
            throw std::invalid_argument("measureCount must be between 1 and 128");
        if (args.contains("voice")) {
            const auto voice = checked_integer(args.at("voice"), "voice must be an integer",
                                               "voice must be between 0 and 3");
            if (voice < 0 || voice > 3) throw std::invalid_argument("voice must be between 0 and 3");
            region.voice = static_cast<unsigned>(voice);
        }
        result = region_json(impl_->core->query_region(document_id(args), region));
    } else if (tool == "score_replace_measures") {
        only_keys(args, {"documentId", "partIndex", "startMeasure", "measures",
                         "baseRevision", "operationId"});
        const auto id = document_id(args);
        if (args.contains("baseRevision") != args.contains("operationId"))
            throw std::invalid_argument("baseRevision and operationId must be provided together");
        const auto part_index = index_arg(args, "partIndex", 14);
        const auto start_measure = index_arg(args, "startMeasure", 127);
        const auto& replacement_json = args.at("measures");
        if (!replacement_json.is_array() || replacement_json.empty())
            throw std::invalid_argument("Replacement must fit within the existing measure range");

        std::vector<Measure> replacement;
        replacement.reserve(replacement_json.size());
        for (const auto& measure : replacement_json)
            replacement.push_back(decode_measure(measure));

        if (args.contains("baseRevision")) {
            const auto base = positive_u64_arg(args, "baseRevision");
            const auto operation_id = operation_id_arg(args);
            result = edit_result_json(impl_->core->replace_measures(
                id, part_index, start_measure, replacement,
                EditContext{Revision{base}, operation_id}));
        } else {
            result = info_json(impl_->core->replace_measures(id, part_index, start_measure, replacement));
        }
    } else if (tool == "score_set_pitches") {
        only_keys(args, {"documentId", "baseRevision", "operationId", "changes"});
        const auto id = document_id(args);
        const auto base = positive_u64_arg(args, "baseRevision");
        const auto operation_id = operation_id_arg(args);

        const auto& changes_json = args.at("changes");
        if (!changes_json.is_array() || changes_json.empty()
            || changes_json.size() > max_pitch_changes)
            throw std::invalid_argument("changes must contain between 1 and 256 entries");

        NativePitchEdit edit;
        edit.changes.reserve(changes_json.size());
        for (const auto& change_json : changes_json) {
            if (!change_json.is_object())
                throw std::invalid_argument("Each pitch change must be an object");
            only_keys(change_json, {"noteId", "pitch"}, "Unknown key in pitch change: ");
            PitchChange change;
            change.note_id = text_arg(change_json, "noteId");
            const auto pitch = checked_integer(change_json.at("pitch"),
                                               "pitch must be an integer",
                                               "pitch must be between 0 and 127");
            if (pitch < 0 || pitch > 127)
                throw std::invalid_argument("pitch must be between 0 and 127");
            change.pitch = static_cast<int>(pitch);
            edit.changes.push_back(std::move(change));
        }

        result = edit_result_json(impl_->core->set_pitches(
            id, edit, EditContext{Revision{base}, operation_id}));
    } else if (tool == "score_close") {
        only_keys(args, {"documentId"});
        impl_->core->close(document_id(args));
        result = {{"closed", true}};
    } else if (tool == "score_export") {
        only_keys(args, {"documentId", "format", "path", "page", "pngDpi", "drawPageBackground",
                         "transparent", "expandRepeats", "exportRPNs", "excerptId", "mix"});
        const auto id = document_id(args);
        const auto format_text = text_arg(args, "format");
        const auto parsed_format = export_format_from_name(format_text);
        if (!parsed_format) throw std::invalid_argument("Unsupported export format");
        const auto format = *parsed_format;
        const auto path = normalize_output_path(std::filesystem::u8path(text_arg(args, "path")));

        if (args.contains("page") && format != ExportFormat::Svg && format != ExportFormat::Png)
            throw std::invalid_argument("page only applies to svg/png");

        ExportOptions options;
        options.excerpt_id = excerpt_arg(args);
        options.page = args.contains("page")
            ? static_cast<int>(index_arg(args, "page", std::numeric_limits<int>::max()))
            : 0;
        if (args.contains("pngDpi")) {
            if (format != ExportFormat::Png) throw std::invalid_argument("pngDpi only applies to png");
            options.png_dpi = static_cast<int>(index_arg(args, "pngDpi", 1200));
            if (options.png_dpi < 36) throw std::invalid_argument("pngDpi must be between 36 and 1200");
        }
        const auto boolean_option = [&](const char* key, bool& value, bool supported) {
            if (!args.contains(key)) return;
            if (!supported) throw std::invalid_argument(std::string(key) + " does not apply to this format");
            if (!args.at(key).is_boolean()) throw std::invalid_argument(std::string(key) + " must be a boolean");
            value = args.at(key).get<bool>();
        };
        boolean_option("drawPageBackground", options.draw_page_background,
                       format == ExportFormat::Svg || format == ExportFormat::Png);
        boolean_option("transparent", options.transparent, format == ExportFormat::Png);
        boolean_option("expandRepeats", options.midi_expand_repeats,
                       format == ExportFormat::Midi || is_audio_format(format));
        // RPNs only exist in the MIDI exporter; the engine path ignores them.
        boolean_option("exportRPNs", options.midi_export_rpns, format == ExportFormat::Midi);
        if (args.contains("mix")) {
            if (!is_audio_format(format))
                throw std::invalid_argument("mix does not apply to this format");
            options.mix = parse_audio_mix_override(args.at("mix"));
        }

        if (is_output_path_reserved(path))
            throw ToolCallError("busy", "Another job owns this export destination");

        if (is_audio_format(format)) {
            result = export_audio_via_worker(*impl_->core, id, format, path, options);
        } else {
            const auto data = impl_->core->export_bytes(id, format, options);
            if (data.bytes.empty()) throw std::runtime_error("Exporter returned no data");
            write_atomic(path, data.bytes);
            result = {{"path", local_path_utf8(path)}, {"format", format_text}, {"bytes", data.bytes.size()},
                {"revision", data.revision.value}};
        }
    } else {
        throw std::invalid_argument("Unknown tool");
    }
    return result.dump();
}

} // namespace music
