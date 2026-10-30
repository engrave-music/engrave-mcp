#include "worker_protocol.h"
#include "runtime/atomic_file.h"
#include "audio/audio_output.h"
#include "background_job.h"
#include "audio/engine_audio.h"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <cmath>

namespace music {
namespace {
using Json = nlohmann::json;
constexpr std::size_t request_limit = 64 * 1024;
constexpr std::size_t snapshot_limit = 8 * 1024 * 1024;
constexpr std::size_t max_mix_tracks = 256;

std::string string_field(const Json& json, const char* name) {
    if (!json.contains(name) || !json.at(name).is_string())
        throw std::invalid_argument(std::string(name) + " must be a string");
    auto value = json.at(name).get<std::string>();
    if (value.empty() || value.find('\0') != std::string::npos)
        throw std::invalid_argument(std::string(name) + " is empty or contains NUL");
    return value;
}
Json parse_bounded_json(const std::filesystem::path& path, std::size_t limit) {
    return Json::parse(read_bounded_file(path, limit), [](int depth, Json::parse_event_t, Json&) {
        if (depth > 64) throw std::invalid_argument("Worker JSON nesting exceeds 64 levels");
        return true;
    });
}
Json output_json(const AudioOutputParams& output) {
    return {{"volumeDb", output.volume_db}, {"balance", output.balance}, {"solo", output.solo},
            {"muted", output.muted}, {"forceMute", output.force_mute}};
}
Json mix_json(const AudioMix& mix) {
    Json tracks = Json::array();
    for (const auto& track : mix.tracks)
        tracks.push_back({{"trackId", track.track_id}, {"output", output_json(track.output)}});
    return {{"master", output_json(mix.master)}, {"tracks", std::move(tracks)}};
}
AudioOutputParams parse_output(const Json& value) {
    return {value.at("volumeDb").get<double>(), value.at("balance").get<double>(),
            value.at("solo").get<bool>(), value.at("muted").get<bool>(), value.at("forceMute").get<bool>()};
}
AudioMix parse_mix(const Json& value) {
    AudioMix mix;
    mix.master = parse_output(value.at("master"));
    if (!value.at("tracks").is_array() || value.at("tracks").size() > max_mix_tracks)
        throw std::invalid_argument("Invalid audio track list");
    for (const auto& track : value.at("tracks"))
        mix.tracks.push_back({track.at("trackId").get<std::int64_t>(), parse_output(track.at("output"))});
    return mix; // Core validates finite ranges, duplicate IDs and complete track identity.
}
void reject_unknown_keys(const Json& object, std::initializer_list<const char*> allowed,
                         const std::string& path) {
    if (!object.is_object()) throw std::invalid_argument(path + " must be an object");
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool found = false;
        for (const char* key : allowed) {
            if (it.key() == key) {
                found = true;
                break;
            }
        }
        if (!found) throw std::invalid_argument("Unknown key in " + path + ": " + it.key());
    }
}
void decode_output_override(const Json& object, AudioOutputOverride& output, const std::string& path) {
    const auto number = [&](const char* key, std::optional<double>& target, double minimum, double maximum) {
        if (!object.contains(key)) return;
        const auto& value = object.at(key);
        if (!value.is_number() || value.is_boolean())
            throw std::invalid_argument(path + "." + key + " must be a number");
        const double result = value.get<double>();
        if (!std::isfinite(result) || result < minimum || result > maximum)
            throw std::invalid_argument(path + "." + key + " is out of range");
        target = result;
    };
    const auto boolean = [&](const char* key, std::optional<bool>& target) {
        if (!object.contains(key)) return;
        const auto& value = object.at(key);
        if (!value.is_boolean()) throw std::invalid_argument(path + "." + key + " must be a boolean");
        target = value.get<bool>();
    };
    number("volumeDb", output.volume_db, -120.0, 24.0);
    number("balance", output.balance, -1.0, 1.0);
    boolean("solo", output.solo);
    boolean("muted", output.muted);
}
std::int64_t track_id_value(const Json& value, const std::string& path) {
    if (!value.is_number_integer() || value.is_boolean())
        throw std::invalid_argument(path + " must be a non-negative integer");
    if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::invalid_argument(path + " is out of range");
        return static_cast<std::int64_t>(raw);
    }
    const auto raw = value.get<std::int64_t>();
    if (raw < 0) throw std::invalid_argument(path + " must be a non-negative integer");
    return raw;
}
std::filesystem::path absolute_field(const Json& json, const char* name) {
    auto path = std::filesystem::u8path(string_field(json, name));
    if (!path.is_absolute()) throw std::invalid_argument(std::string(name) + " must be absolute");
    return path;
}
std::string version_string(const Json& json, const char* name) {
    if (!json.contains(name) || !json.at(name).is_string())
        throw std::invalid_argument(std::string(name) + " must be a string");
    auto value = json.at(name).get<std::string>();
    // An empty version is valid: the source file may predate version tags.
    if (value.size() > 64 || value.find('\0') != std::string::npos)
        throw std::invalid_argument(std::string(name) + " is invalid");
    return value;
}
int version_integer(const Json& json, const char* name, int minimum, int maximum) {
    if (!json.contains(name) || !json.at(name).is_number_integer()
        || json.at(name) < minimum || json.at(name) > maximum)
        throw std::invalid_argument(std::string(name) + " is out of range");
    return json.at(name).get<int>();
}

// Validates one job object. Result and snapshot must live next to the request
// file, never at a client-selected path.
ExportWorkerJob parse_job(const Json& job, const std::filesystem::path& request_parent) {
    ExportWorkerJob result;
    result.result_path = absolute_field(job, "result");
    if (result.result_path.parent_path() != request_parent)
        throw std::invalid_argument("Worker result must be next to its request");
    result.snapshot_path = absolute_field(job, "snapshot");
    if (result.snapshot_path.parent_path() != request_parent)
        throw std::invalid_argument("Worker snapshot must be next to its request");
    result.output_path = absolute_field(job, "output");

    const auto format = export_format_from_name(string_field(job, "format"));
    if (!format) throw std::invalid_argument("Unsupported worker export format");
    result.format = *format;
    if (!job.contains("options") || !job.at("options").is_object())
        throw std::invalid_argument("options must be an object");
    result.options = parse_export_options(job.at("options"), result.format);
    if (job.contains("mix")) {
        result.mix = parse_mix(job.at("mix"));
        result.has_mix = true;
    }
    result.versions = ScoreVersions{
        version_string(job, "mscoreVersion"),
        version_integer(job, "mscoreRevision", 0, std::numeric_limits<int>::max()),
        version_integer(job, "mscVersion", 0, 100000),
    };
    return result;
}

Json options_json(const ExportOptions& options, ExportFormat format) {
    Json result = {{"excerptId", options.excerpt_id}};
    if (format == ExportFormat::Png || format == ExportFormat::Svg) {
        result["page"] = options.page;
        result["drawPageBackground"] = options.draw_page_background;
    }
    if (format == ExportFormat::Png) {
        result["pngDpi"] = options.png_dpi;
        result["transparent"] = options.transparent;
    }
    if (format == ExportFormat::Midi || is_audio_format(format)) {
        result["expandRepeats"] = options.midi_expand_repeats;
        // Registered parameter messages only exist in the MIDI exporter; the
        // MuseScore audio engine reads the score's playback data directly.
        if (format == ExportFormat::Midi) result["exportRPNs"] = options.midi_export_rpns;
    }
    return result;
}
}

bool is_audio_format(ExportFormat format) noexcept {
    return format == ExportFormat::Wav || format == ExportFormat::Flac
        || format == ExportFormat::Ogg || format == ExportFormat::Mp3
        || format == ExportFormat::Aac;
}
std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

std::string read_bounded_file(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open worker file");
    std::string result;
    char buffer[16384];
    while (file) {
        file.read(buffer, sizeof(buffer));
        const auto count = static_cast<std::size_t>(file.gcount());
        if (count > limit - result.size()) throw std::runtime_error("Worker file exceeds size limit");
        result.append(buffer, count);
    }
    if (file.bad()) throw std::runtime_error("Cannot read worker file");
    return result;
}

AudioMixOverride parse_audio_mix_override(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("mix must be an object");
    reject_unknown_keys(json, {"master", "tracks"}, "mix");

    AudioMixOverride result;
    if (json.contains("master")) {
        reject_unknown_keys(json.at("master"), {"volumeDb", "balance"}, "mix.master");
        decode_output_override(json.at("master"), result.master, "mix.master");
    }
    if (!json.contains("tracks")) return result;

    const auto& tracks = json.at("tracks");
    if (!tracks.is_array()) throw std::invalid_argument("mix.tracks must be an array");
    if (tracks.size() > max_mix_tracks) throw std::invalid_argument("mix.tracks must contain at most 256 entries");
    result.tracks.reserve(tracks.size());
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        const auto& entry = tracks.at(index);
        const std::string path = "mix.tracks[" + std::to_string(index) + "]";
        reject_unknown_keys(entry, {"trackId", "volumeDb", "balance", "solo", "muted"}, path);
        if (!entry.contains("trackId")) throw std::invalid_argument(path + ".trackId is required");
        AudioTrackMixOverride track;
        track.track_id = track_id_value(entry.at("trackId"), path + ".trackId");
        decode_output_override(entry, track.output, path);
        result.tracks.push_back(std::move(track));
    }
    return result;
}

ExportOptions parse_export_options(const Json& args, ExportFormat format) {
    ExportOptions result;
    const bool image = format == ExportFormat::Png || format == ExportFormat::Svg;
    const bool midi = format == ExportFormat::Midi;
    const bool repeats = midi || is_audio_format(format);
    const auto integer = [&](const char* key, int& target, int minimum, int maximum, bool supported) {
        if (!args.contains(key)) return;
        if (!supported) throw std::invalid_argument(std::string(key) + " does not apply to this format");
        const auto& value = args.at(key);
        if (!value.is_number_integer() || value < minimum || value > maximum)
            throw std::invalid_argument(std::string(key) + " is out of range");
        target = value.get<int>();
    };
    const auto boolean = [&](const char* key, bool& target, bool supported) {
        if (!args.contains(key)) return;
        if (!supported) throw std::invalid_argument(std::string(key) + " does not apply to this format");
        if (!args.at(key).is_boolean()) throw std::invalid_argument(std::string(key) + " must be a boolean");
        target = args.at(key).get<bool>();
    };
    integer("excerptId", result.excerpt_id, -1, std::numeric_limits<int>::max(), true);
    integer("page", result.page, 0, std::numeric_limits<int>::max(), image);
    integer("pngDpi", result.png_dpi, 36, 1200, format == ExportFormat::Png);
    boolean("drawPageBackground", result.draw_page_background, image);
    boolean("transparent", result.transparent, format == ExportFormat::Png);
    boolean("expandRepeats", result.midi_expand_repeats, repeats);
    // The audio engine path ignores RPNs entirely, so the option is MIDI-only.
    boolean("exportRPNs", result.midi_export_rpns, midi);
    if (args.contains("mix")) {
        if (!is_audio_format(format))
            throw std::invalid_argument("mix does not apply to this format");
        result.mix = parse_audio_mix_override(args.at("mix"));
    }
    return result;
}

Json worker_request_json(const ExportWorkerRequest& request) {
    Json result = {{"version", 1}, {"parentPid", current_process_id()},
                   {"resources", request.runtime.resources_path},
                   {"soundfont", request.runtime.soundfont},
                   {"fonts", request.runtime.font_paths}};
    if (!request.cache_directory.empty()) result["cacheDirectory"] = path_utf8(request.cache_directory);
    result["snapshot"] = path_utf8(request.snapshot_path);
    result["output"] = path_utf8(request.output_path);
    result["result"] = path_utf8(request.result_path);
    result["format"] = format_name(request.format);
    result["options"] = options_json(request.options, request.format);
    result["mscoreVersion"] = request.versions.mscore_version;
    result["mscoreRevision"] = request.versions.mscore_revision;
    result["mscVersion"] = request.versions.msc_version;
    if (request.has_mix) result["mix"] = mix_json(request.mix);
    return result;
}

int run_export_worker(const std::filesystem::path& request_path) noexcept {
    std::vector<std::filesystem::path> result_paths;
    try {
        const auto request = parse_bounded_json(request_path, request_limit);
        if (!request.is_object() || request.value("version", 0) != 1)
            throw std::invalid_argument("Unsupported worker request version");
        enforce_worker_parent(request.at("parentPid").get<std::uint64_t>());
        const auto request_parent = request_path.parent_path();

        RuntimeOptions runtime{path_utf8(absolute_field(request, "resources")), std::string{}};
        const std::string soundfont = request.value("soundfont", std::string{});
        runtime.font_paths = request.at("fonts").get<std::vector<std::string>>();
        if (request.contains("cacheDirectory")) {
            const auto cache_dir = absolute_field(request, "cacheDirectory");
            for (const auto& element : cache_dir) {
                if (element == "..")
                    throw std::invalid_argument("Worker cache directory must not contain '..'");
            }
            const auto norm_cache = cache_dir.lexically_normal();
            const auto norm_req = request_parent.lexically_normal();
            if (norm_cache.parent_path() != norm_req && norm_cache != norm_req)
                throw std::invalid_argument("Worker cache directory must be in its request directory");
            runtime.cache_directory = path_utf8(norm_cache);
        }

        // Exactly one job per worker process. The audio engine inherits Muse
        // Reverb state from a previous render, so a second audio render in the
        // same process is not byte-identical to a fresh one.
        const ExportWorkerJob job = parse_job(request, request_parent);
        result_paths.push_back(job.result_path);
        if (request.contains("jobs"))
            throw std::invalid_argument("Worker requests carry exactly one job");

        MusicCore core(runtime);
        try {
            const auto score = core.open(read_bounded_file(job.snapshot_path, snapshot_limit),
                                         InputFormat::Mscz);
            const auto close_score = [&core, &score]() {
                try { core.close(score.id); } catch (...) {}
            };
            try {
                core.restore_versions(score.id, job.versions);
                ExportOptions effective = job.options;
                if (job.has_mix) effective.explicit_mix = job.mix;

                std::uintmax_t bytes = 0;
                if (is_audio_format(job.format)) {
                    std::string engine_error;
                    if (!initialize_engine_audio(soundfont, &engine_error)) {
                        throw std::runtime_error(engine_error.empty()
                                                     ? "Audio engine initialisation failed"
                                                     : engine_error);
                    }
                    const auto rendered = core.export_audio(score.id, job.format,
                                                            path_utf8(job.output_path), effective);
                    if (!rendered.ok) {
                        throw std::runtime_error(rendered.error.empty()
                                                     ? "Audio export failed"
                                                     : rendered.error);
                    }
                    bytes = rendered.bytes;
                    if (bytes == 0 || bytes > kAudioOutputLimit)
                        throw std::runtime_error("Worker audio export is empty or exceeds the 1 GiB limit");
                } else {
                    const auto data = core.export_bytes(score.id, job.format, effective);
                    if (data.bytes.empty() || data.bytes.size() > kNonAudioOutputLimit)
                        throw std::runtime_error("Worker export exceeds the 256 MiB limit");
                    write_atomic(job.output_path, data.bytes);
                    bytes = data.bytes.size();
                }
                close_score();
                write_atomic(job.result_path,
                             Json{{"version", 1}, {"ok", true}, {"bytes", bytes}}.dump());
            } catch (...) {
                close_score();
                throw;
            }
        } catch (const std::exception& error) {
            std::cerr << "Export worker: " << error.what() << '\n';
            try {
                write_atomic(job.result_path,
                             Json{{"version", 1}, {"ok", false},
                                  {"error", std::string(error.what()).substr(0, 2048)}}.dump());
            } catch (...) {}
            return 1;
        } catch (...) {
            std::cerr << "Export worker: unknown failure\n";
            try {
                write_atomic(job.result_path,
                             Json{{"version", 1}, {"ok", false}, {"error", "unknown failure"}}.dump());
            } catch (...) {}
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Export worker: " << error.what() << '\n';
        // A request-level failure before job validation cannot name a result
        // file; a failure after validation reports the pending job.
        for (const auto& result_path : result_paths) {
            try {
                write_atomic(result_path,
                             Json{{"version", 1}, {"ok", false},
                                  {"error", std::string(error.what()).substr(0, 2048)}}.dump());
            } catch (...) {}
        }
        return 1;
    } catch (...) {
        std::cerr << "Export worker: unknown failure\n";
        return 1;
    }
}
}
