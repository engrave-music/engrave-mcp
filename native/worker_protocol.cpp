#include "worker_protocol.h"
#include "atomic_file.h"
#include "background_job.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <cmath>
#include <thread>
#include <chrono>

namespace music {
namespace {
using Json = nlohmann::json;
constexpr std::size_t request_limit = 64 * 1024;
constexpr std::size_t snapshot_limit = 8 * 1024 * 1024;
constexpr std::size_t output_limit = 256 * 1024 * 1024;

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
double seconds_value(const Json& value) {
    if (!value.is_number()) throw std::invalid_argument("Time must be numeric");
    const auto seconds = value.get<double>();
    if (!std::isfinite(seconds) || seconds < 0 || seconds >= 600)
        throw std::invalid_argument("Time must be finite and in [0, 600) seconds");
    return seconds;
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
    if (!value.at("tracks").is_array() || value.at("tracks").size() > audio_max_tracks)
        throw std::invalid_argument("Invalid audio track list");
    for (const auto& track : value.at("tracks"))
        mix.tracks.push_back({track.at("trackId").get<std::int64_t>(), parse_output(track.at("output"))});
    return mix; // Core validates finite ranges, duplicate IDs and complete track identity.
}
void serve_audio_commands(AudioSession& session, const std::filesystem::path& directory) {
    auto idle_since = std::chrono::steady_clock::now();
    std::uint64_t expected_sequence = 1;
    const auto command_path = directory / "command.json", reply_path = directory / "reply.json";
    for (;;) {
        if (!std::filesystem::exists(command_path)) {
            if (std::chrono::steady_clock::now() - idle_since > std::chrono::seconds(150)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (std::filesystem::exists(reply_path)) throw std::runtime_error("Unconsumed audio reply");
        Json reply = {{"version", 1}, {"sequence", expected_sequence}};
        try {
            const auto command = parse_bounded_json(command_path, request_limit);
            std::filesystem::remove(command_path);
            const auto& sequence = command.at("sequence");
            if (!sequence.is_number_integer() || sequence < 1 || sequence != expected_sequence)
                throw std::invalid_argument("Audio command sequence mismatch");
            const auto operation = string_field(command, "operation");
            if (operation == "read") {
                const auto& batch = command.at("batchSize");
                if (!batch.is_number_integer() || batch < 1 || batch > 16)
                    throw std::invalid_argument("Audio batchSize must be between 1 and 16");
                Json chunks = Json::array();
                for (const auto& chunk : session.read_batch(batch.get<std::size_t>())) {
                    if (chunk.left.size() > audio_chunk_frames || chunk.right.size() != chunk.left.size())
                        throw std::runtime_error("Audio chunk exceeded frame bound");
                    chunks.push_back({{"done", chunk.done}, {"startTime", chunk.start_time},
                        {"endTime", chunk.end_time}, {"left", chunk.left}, {"right", chunk.right}});
                }
                reply["chunks"] = std::move(chunks);
            } else if (operation == "seek") {
                const auto seconds = seconds_value(command.at("seconds"));
                session.seek(seconds);
                reply["seconds"] = seconds;
            } else throw std::invalid_argument("Unknown audio operation");
            reply["ok"] = true;
        } catch (const std::exception& error) {
            std::error_code ignored;
            std::filesystem::remove(command_path, ignored);
            reply["ok"] = false;
            reply["error"] = std::string(error.what()).substr(0, 2048);
        }
        ++expected_sequence;
        const auto encoded = reply.dump();
        if (encoded.size() > 1024 * 1024) throw std::runtime_error("Audio reply exceeds byte limit");
        write_atomic(reply_path, encoded);
        idle_since = std::chrono::steady_clock::now();
    }
}
std::filesystem::path absolute_field(const Json& json, const char* name) {
    auto path = std::filesystem::u8path(string_field(json, name));
    if (!path.is_absolute()) throw std::invalid_argument(std::string(name) + " must be absolute");
    return path;
}
}

bool is_audio_format(ExportFormat format) noexcept {
    return format == ExportFormat::Wav || format == ExportFormat::Flac
        || format == ExportFormat::Ogg || format == ExportFormat::Mp3;
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

ExportOptions parse_export_options(const Json& args, ExportFormat format) {
    ExportOptions result;
    const bool image = format == ExportFormat::Png || format == ExportFormat::Svg;
    const bool midi = format == ExportFormat::Midi || is_audio_format(format);
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
    boolean("expandRepeats", result.midi_expand_repeats, midi);
    boolean("exportRPNs", result.midi_export_rpns, midi);
    return result;
}

Json worker_request_json(const ExportWorkerRequest& request) {
    Json options = {{"excerptId", request.options.excerpt_id}};
    if (request.format == ExportFormat::Png || request.format == ExportFormat::Svg) {
        options["page"] = request.options.page;
        options["drawPageBackground"] = request.options.draw_page_background;
    }
    if (request.format == ExportFormat::Png) {
        options["pngDpi"] = request.options.png_dpi;
        options["transparent"] = request.options.transparent;
    }
    if (request.format == ExportFormat::Midi || is_audio_format(request.format)) {
        options["expandRepeats"] = request.options.midi_expand_repeats;
        options["exportRPNs"] = request.options.midi_export_rpns;
    }
    Json result = {{"version", 1}, {"parentPid", current_process_id()}, {"snapshot", path_utf8(request.snapshot_path)},
            {"output", path_utf8(request.output_path)}, {"result", path_utf8(request.result_path)},
            {"resources", request.runtime.resources_path}, {"soundfont", request.runtime.soundfont},
            {"format", format_name(request.format)}, {"options", std::move(options)}, {"fonts", request.runtime.font_paths}, {"stream", request.stream}, {"startTime", request.start_seconds}};
    if (!request.cache_directory.empty()) result["cacheDirectory"] = path_utf8(request.cache_directory);
    if (request.has_mix) result["mix"] = mix_json(request.mix);
    return result;
}

int run_export_worker(const std::filesystem::path& request_path) noexcept {
    std::filesystem::path result_path;
    try {
        const auto request = parse_bounded_json(request_path, request_limit);
        if (!request.is_object() || request.value("version", 0) != 1)
            throw std::invalid_argument("Unsupported worker request version");
        enforce_worker_parent(request.at("parentPid").get<std::uint64_t>());
        const auto requested_result = absolute_field(request, "result");
        // Results belong to the caller-created request directory, never a client-selected export path.
        if (requested_result.parent_path() != request_path.parent_path())
            throw std::invalid_argument("Worker result must be next to its request");
        result_path = requested_result;
        const auto snapshot = absolute_field(request, "snapshot");
        if (snapshot.parent_path() != request_path.parent_path())
            throw std::invalid_argument("Worker snapshot must be next to its request");
        const auto output = absolute_field(request, "output");
        const auto format = export_format_from_name(string_field(request, "format"));
        if (!format) throw std::invalid_argument("Unsupported worker export format");
        const auto options = parse_export_options(request.at("options"), *format);
        const bool stream = request.value("stream", false);
        RuntimeOptions runtime{path_utf8(absolute_field(request, "resources")),
                               (is_audio_format(*format) || stream) ? request.at("soundfont").get<std::string>() : std::string{}};
        runtime.font_paths = request.at("fonts").get<std::vector<std::string>>();
        if (request.contains("cacheDirectory")) {
            const auto cache_dir = absolute_field(request, "cacheDirectory");
            if (cache_dir.parent_path() != request_path.parent_path() && cache_dir != request_path.parent_path())
                throw std::invalid_argument("Worker cache directory must be in its request directory");
            runtime.cache_directory = path_utf8(cache_dir);
        }
        MusicCore core(runtime);
        const auto score = core.open(read_bounded_file(snapshot, snapshot_limit), InputFormat::Mscz);
        if (request.contains("mix")) core.set_audio_mix(score.id, parse_mix(request.at("mix")), options.excerpt_id);
        if (request.value("stream", false)) {
            if (!is_audio_format(*format)) throw std::invalid_argument("Streaming requires audio format");
            {
                auto session = core.create_audio_session(score.id, options, seconds_value(request.at("startTime")));
                write_atomic(result_path, Json{{"version", 1}, {"ok", true}, {"ready", true},
                                              {"sampleRate", 44100}, {"chunkFrames", 512}}.dump());
                serve_audio_commands(*session, request_path.parent_path());
            }
            core.close(score.id);
            return 0;
        }
        const auto data = core.export_bytes(score.id, *format, options);
        if (data.bytes.empty() || data.bytes.size() > output_limit)
            throw std::runtime_error("Worker export exceeds output size limit");
        write_atomic(output, data.bytes);
        core.close(score.id);
        write_atomic(result_path, Json{{"version", 1}, {"ok", true}, {"bytes", data.bytes.size()}}.dump());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Export worker: " << error.what() << '\n';
        if (!result_path.empty()) {
            try { write_atomic(result_path, Json{{"version", 1}, {"ok", false}, {"error", std::string(error.what()).substr(0, 2048)}}.dump()); }
            catch (...) {}
        }
        return 1;
    } catch (...) {
        std::cerr << "Export worker: unknown failure\n";
        return 1;
    }
}
}
