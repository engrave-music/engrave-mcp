#include "worker_v2.h"
#include "audio_budget.h"
#include "api_json_internal.h"
#include "background_job.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#ifdef MUSIC_ASYNC_TEST_HOOKS
#include <chrono>
#include <thread>
#endif

namespace music::v2 {
namespace {
namespace fs = std::filesystem;

struct Failure { HostError error; };
template<class T> T value(engrave::Result<T> result) {
    if (!result) throw Failure{api_error(result.error())};
    return std::move(*result);
}
Json score(const engrave::ScoreSel& s) {
    return s.excerpt ? Json{{"excerpt", engrave::to_string(*s.excerpt)}} : Json::object();
}
const char* format(engrave::Format f) {
    switch (f) {
    case engrave::Format::Mscz: return "mscz"; case engrave::Format::Mscx: return "mscx";
    case engrave::Format::MusicXml: return "musicxml"; case engrave::Format::Mxl: return "mxl";
    case engrave::Format::Midi: return "midi"; default: throw std::logic_error("invalid score format");
    }
}
const char* format(engrave::RenderFormat f) {
    switch (f) {
    case engrave::RenderFormat::Pdf: return "pdf"; case engrave::RenderFormat::Png: return "png";
    case engrave::RenderFormat::Svg: return "svg"; default: throw std::logic_error("invalid render format");
    }
}
const char* format(engrave::AudioFormat f) {
    switch (f) {
    case engrave::AudioFormat::Wav: return "wav"; case engrave::AudioFormat::Flac: return "flac";
    case engrave::AudioFormat::Opus: return "opus"; case engrave::AudioFormat::Mp3: return "mp3";
    case engrave::AudioFormat::Aac: return "aac"; default: throw std::logic_error("invalid audio format");
    }
}
Json audio_options(const engrave::AudioOptions& o) {
    Json tracks = Json::array();
    for (const auto& t : o.tracks) {
        Json track = {{"track", t.track}};
        if (t.volume_db) track["volume_db"] = *t.volume_db;
        if (t.balance) track["balance"] = *t.balance;
        if (t.mute) track["mute"] = *t.mute;
        if (t.solo) track["solo"] = *t.solo;
        if (auto p = std::get_if<std::int32_t>(&t.soundfont)) track["soundfont"] = *p;
        else if (auto p = std::get_if<std::string>(&t.soundfont)) track["soundfont"] = *p;
        if (!t.preset.empty()) track["preset"] = t.preset;
        tracks.push_back(std::move(track));
    }
    Json result = {{"format", format(o.format)}, {"score", score(o.score)}, {"bitrate_kbps", o.bitrate_kbps},
        {"bit_depth", o.bit_depth}, {"use_repeats", o.use_repeats}, {"tracks", std::move(tracks)},
        {"reverb", o.reverb}, {"reverb_send", o.reverb_send}, {"sample_rate", o.sample_rate}};
    if (o.master_volume_db) result["master_volume_db"] = *o.master_volume_db;
    if (o.master_balance) result["master_balance"] = *o.master_balance;
    return result;
}
Json export_request(const ExportSnapshot& snapshot) {
    const auto& r = snapshot.request;
    Json options; const char* kind = nullptr;
    switch (r.kind) {
    case ExportKind::Score:
        kind = "score";
        options = {{"format", format(r.score_options.format)}, {"score", score(r.score_options.score)}};
        if (r.score_options.format == engrave::Format::Midi) {
            options["midi_expand_repeats"] = r.score_options.midi_expand_repeats;
            options["midi_rpns"] = r.score_options.midi_rpns;
        }
        break;
    case ExportKind::Render: {
        kind = "render"; const auto& o = r.render_options;
        options = {{"format", format(o.format)}, {"score", score(o.score)}, {"first_page", o.first_page},
                   {"last_page", o.last_page}, {"background", o.background}};
        if (o.format == engrave::RenderFormat::Png) { options["dpi"] = o.dpi; options["transparent"] = o.transparent; }
        break;
    }
    case ExportKind::Audio: kind = "audio"; options = audio_options(snapshot.effective_audio); break;
    case ExportKind::Positions:
        kind = "positions";
        options = {{"score", score(r.position_score)}, {"position_kind", r.position_kind == engrave::PositionKind::Measure ? "measure" : "segment"}};
        break;
    }
    return {{"document", r.document}, {"action", "start"}, {"kind", kind}, {"options", std::move(options)},
            {"destination", {{"kind", r.destination.directory ? "directory" : "file"}, {"path", detail::path_text(r.destination.path)}}}};
}
Json paths(const std::vector<fs::path>& values) {
    Json result = Json::array(); for (const auto& p : values) result.push_back(detail::path_text(p)); return result;
}
std::vector<fs::path> read_paths(const Json& values) {
    std::vector<fs::path> result;
    for (const auto& v : values) { auto p = detail::path(v); if (!p.is_absolute()) throw std::runtime_error("asset path is not absolute"); result.push_back(std::move(p)); }
    return result;
}
std::string mime(const ExportRequest& r) {
    switch (r.kind) {
    case ExportKind::Positions: return "application/json";
    case ExportKind::Score:
        switch (r.score_options.format) {
        case engrave::Format::Mscz: return "application/vnd.musescore";
        case engrave::Format::Mscx: return "application/vnd.musescore.mscx+xml";
        case engrave::Format::MusicXml: return "application/vnd.recordare.musicxml+xml";
        case engrave::Format::Mxl: return "application/vnd.recordare.musicxml";
        case engrave::Format::Midi: return "audio/midi";
        default: break;
        } break;
    case ExportKind::Render:
        switch (r.render_options.format) {
        case engrave::RenderFormat::Pdf: return "application/pdf";
        case engrave::RenderFormat::Png: return "image/png";
        case engrave::RenderFormat::Svg: return "image/svg+xml";
        } break;
    case ExportKind::Audio:
        switch (r.audio_options.format) {
        case engrave::AudioFormat::Wav: return "audio/wav";
        case engrave::AudioFormat::Flac: return "audio/flac";
        case engrave::AudioFormat::Opus: return "audio/ogg";
        case engrave::AudioFormat::Mp3: return "audio/mpeg";
        case engrave::AudioFormat::Aac: return "audio/aac";
        } break;
    }
    throw std::logic_error("unknown output MIME");
}
void write_bytes(const fs::path& path, std::span<const std::byte> data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw Failure{{"Io", "cannot open staged output"}};
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) throw Failure{{"Io", "cannot write staged output"}};
}
std::string page_name(std::int32_t number, const char* extension) {
    std::ostringstream s; s << "page-" << std::setw(4) << std::setfill('0') << number + 1 << '.' << extension; return s.str();
}
void write_result(const fs::path& path, const Json& result) {
    const auto text = result.dump();
    write_bytes(path, {reinterpret_cast<const std::byte*>(text.data()), text.size()});
}
Json read_request(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot read worker request");
    std::string bytes;
    char block[16384];
    while (file) {
        file.read(block, sizeof(block));
        const auto count = static_cast<std::size_t>(file.gcount());
        if (count > worker_request_limit - bytes.size())
            throw std::runtime_error("worker request exceeds size limit");
        bytes.append(block, count);
    }
    if (file.bad()) throw std::runtime_error("cannot read worker request");
    return Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 64) throw std::runtime_error("worker JSON nesting exceeds 64 levels");
        return true;
    });
}
}

Json worker_request(const ExportSnapshot& snapshot, const fs::path& job_directory,
                    const fs::path& staging_directory, std::uint64_t output_limit) {
    const auto& o = snapshot.runtime.options;
    return {{"version", 2}, {"parentPid", current_process_id()}, {"export", export_request(snapshot)},
            {"payload", detail::path_text(job_directory / "payload")},
            {"result", detail::path_text(job_directory / "result.json")},
            {"stage", detail::path_text(staging_directory)}, {"outputLimit", output_limit},
            {"resources", detail::path_text(o.resources)}, {"fonts", paths(o.fonts)},
            {"soundfonts", snapshot.request.kind == ExportKind::Audio ? paths(o.soundfonts) : Json::array()},
            {"sampleRate", o.audio_sample_rate}, {"deterministicIds", o.deterministic_ids}};
}

int run_worker(const fs::path& request_path) noexcept {
    const auto result_path = request_path.parent_path() / "result.json";
    try {
        Json j = read_request(request_path);
        if (j.at("version") != 2) throw std::runtime_error("invalid worker version");
        enforce_worker_parent(j.at("parentPid").get<std::uint64_t>());
#ifdef MUSIC_ASYNC_TEST_HOOKS
        // Deterministic cancellation coverage, absent from the production host.
        if (j.at("export").at("destination").at("path").get<std::string>().find(".test-slow") != std::string::npos)
            std::this_thread::sleep_for(std::chrono::seconds(4));
#endif
        if (detail::path(j.at("result")) != result_path || detail::path(j.at("payload")).parent_path() != request_path.parent_path())
            throw std::runtime_error("worker files are outside the job directory");
        auto decoded = decode_export(j.at("export"));
        if (!decoded) throw Failure{decoded.error()};
        const auto& request = *decoded;
        const auto stage = detail::path(j.at("stage"));
        if (!stage.is_absolute() || !fs::is_directory(stage)) throw std::runtime_error("invalid staging directory");
        const auto limit = j.at("outputLimit").get<std::uint64_t>();
        std::uint64_t total = 0; Json artifacts = Json::array();
        const auto write_artifact = [&](const std::string& name, std::span<const std::byte> data) {
            if (data.empty()) throw Failure{{"EngineFailure", "export produced an empty artifact"}};
            if (data.size() > limit - total) throw Failure{{"ResourceLimit", "export exceeds output byte limit"}};
            write_bytes(stage / name, data); total += data.size();
            artifacts.push_back({{"name", name}, {"mime", mime(request)}, {"size", data.size()}});
        };
        if (request.kind == ExportKind::Score || request.kind == ExportKind::Positions) {
            std::ifstream in(detail::path(j.at("payload")), std::ios::binary);
            if (!in) throw Failure{{"Io", "cannot read captured artifact"}};
            std::vector<std::byte> data; char block[16384];
            while (in) {
                in.read(block, sizeof(block)); const auto n = static_cast<std::size_t>(in.gcount());
                if (n > limit - data.size()) throw Failure{{"ResourceLimit", "export exceeds output byte limit"}};
                const auto* begin = reinterpret_cast<const std::byte*>(block); data.insert(data.end(), begin, begin + n);
            }
            if (in.bad()) throw Failure{{"Io", "cannot read captured artifact"}};
            write_artifact("output", data);
        } else {
            engrave::EngineOptions options;
            options.resources = detail::path(j.at("resources"));
            options.fonts = read_paths(j.at("fonts")); options.soundfonts = read_paths(j.at("soundfonts"));
            options.audio_sample_rate = j.at("sampleRate").get<std::int32_t>();
            options.deterministic_ids = j.at("deterministicIds").get<bool>();
            options.cache_dir = request_path.parent_path() / "cache";
            std::error_code cache_error;
            if (!fs::create_directory(options.cache_dir, cache_error) && cache_error)
                throw Failure{{"Io", "cannot create worker cache: " + cache_error.message()}};
            auto engine = value(engrave::Engine::create(options));
            auto document = value(engine->open(detail::path(j.at("payload")), engrave::OpenOptions{.format = engrave::Format::Mscz}));
            if (request.kind == ExportKind::Render) {
                if (request.render_options.format == engrave::RenderFormat::Pdf) {
                    auto output = value(document->render(request.render_options));
                    write_artifact("output", output.document);
                } else {
                    const auto count = value(document->page_count(request.render_options.score));
                    const auto first = request.render_options.first_page;
                    const auto last = request.render_options.last_page < 0 ? count - 1 : request.render_options.last_page;
                    if (first < 0 || last < first || last >= count)
                        throw Failure{{"InvalidArgument", "render page range is outside the score"}};
                    if (static_cast<std::int64_t>(last) - first + 1 > 4096)
                        throw Failure{{"ResourceLimit", "too many output pages"}};
                    // Bound retained page memory and enforce the output budget
                    // after each page, while keeping all bytes in staging.
                    for (auto page = first; page <= last; ++page) {
                        auto options = request.render_options; options.first_page = options.last_page = page;
                        auto output = value(document->render(options));
                        if (output.pages.size() != 1) throw Failure{{"EngineFailure", "renderer returned an invalid page count"}};
                        write_artifact(page_name(page, options.format == engrave::RenderFormat::Png ? "png" : "svg"), output.pages[0]);
                    }
                }
            } else {
                if (request.audio_options.sample_rate && request.audio_options.sample_rate != options.audio_sample_rate)
                    throw Failure{{"Unsupported", "audio sample rate differs from the engine rate"}};
                const auto selected = value(document->outline(request.audio_options.score));
                std::optional<double> duration;
                if (request.audio_options.use_repeats) {
                    duration = selected.duration;
                } else {
                    // Audio length always follows the master score, including for excerpts.
                    const auto master = request.audio_options.score.excerpt ? value(document->outline()) : selected;
                    try {
                        const auto positions = document->positions(engrave::PositionKind::Measure);
                        if (positions) duration = unexpanded_audio_seconds(master, *positions);
                    } catch (const std::runtime_error&) {
                        // Some layouts/timings cannot be represented by the public
                        // position API. Do not reject using the expanded duration.
                    }
                }
                if (duration && projected_audio_bytes(request.audio_options, *duration, options.audio_sample_rate) > limit)
                    throw Failure{{"ResourceLimit", "projected audio exceeds output byte limit"}};
                std::ofstream out(stage / "output", std::ios::binary | std::ios::trunc);
                if (!out) throw Failure{{"Io", "cannot open staged audio"}};
                auto audio = document->render_audio(request.audio_options, [&](std::span<const std::byte> chunk) {
                    if (chunk.size() > limit - total) throw Failure{{"ResourceLimit", "export exceeds output byte limit"}};
                    out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
                    if (!out) throw Failure{{"Io", "cannot write staged audio"}};
                    total += chunk.size();
                });
                if (!audio) throw Failure{api_error(audio.error())};
                out.close(); if (!out) throw Failure{{"Io", "cannot close staged audio"}};
                if (!total || total != audio->bytes) throw Failure{{"EngineFailure", "invalid audio byte count"}};
                artifacts.push_back({{"name", "output"}, {"mime", mime(request)}, {"size", total}});
            }
        }
        write_result(result_path, {{"version", 2}, {"ok", true}, {"artifacts", std::move(artifacts)}});
        return 0;
    } catch (const Failure& e) {
        try { write_result(result_path, {{"version", 2}, {"ok", false}, {"error", error_envelope(e.error).at("error")}}); } catch (...) {}
    } catch (const detail::DecodeError& e) {
        try { write_result(result_path, {{"version", 2}, {"ok", false}, {"error", error_envelope(e.error).at("error")}}); } catch (...) {}
    } catch (const std::exception& e) {
        try { write_result(result_path, {{"version", 2}, {"ok", false}, {"error", {{"code", "EngineFailure"}, {"message", std::string(e.what()).substr(0, 2048)}}}}); } catch (...) {}
    } catch (...) {
        try { write_result(result_path, {{"version", 2}, {"ok", false}, {"error", {{"code", "EngineFailure"}, {"message", "unknown worker failure"}}}}); } catch (...) {}
    }
    return 1;
}
} // namespace music::v2
