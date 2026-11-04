#include "api_output.h"
#include "api_json_internal.h"
#include "publication.h"
#include "export_timing.h"
#include "audio_budget.h"
#include "tool_result.h"
#include "publication_gate.h"
#include "terminal_diagnostic.h"
#include "utf8_text.h"
#include <fstream>
#include <iomanip>
#include <sstream>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <limits>
#include <set>
#include <thread>
#include <cstdio>
#ifdef MUSIC_ASYNC_TEST_HOOKS
#include <engrave/testing/write470.h>
#endif

namespace music::v2 {

namespace {
using detail::invalid;

constexpr std::size_t kDiagnosticLimit = 16384;

// Keep the requested destination and the reason in a bounded, UTF-8-safe
// diagnostic. A very long path is abbreviated in the middle rather than cut
// mid-code-point or truncated before the reason.
std::string destination_message(std::string_view requested, std::string_view reason) {
    constexpr std::string_view prefix = "destination '";
    constexpr std::string_view mid = "': ";
    std::string bounded_reason = reason.size() > 8192 ? utf8_abbrev(reason, 8192) : std::string(reason);
    const auto fixed = prefix.size() + mid.size() + bounded_reason.size();
    if (fixed >= kDiagnosticLimit) {
        const auto allowed = kDiagnosticLimit - prefix.size() - mid.size() - 3;
        return std::string(prefix) + "..." + std::string(mid) + utf8_abbrev(bounded_reason, allowed);
    }
    std::string result;
    result.reserve(kDiagnosticLimit);
    result.append(prefix);
    result.append(utf8_abbrev(requested, kDiagnosticLimit - fixed));
    result.append(mid);
    result.append(bounded_reason);
    return result;
}

HostError with_destination(HostError error, std::string_view requested) {
    if (error.message.rfind("destination '", 0) == 0) {
        error.message.resize(utf8_prefix(error.message, kDiagnosticLimit));
    } else {
        error.message = destination_message(requested, error.message);
    }
    return error;
}

template <class T>
void assign_if(const Json &j, const char *key, T &out) {
    if (j.contains(key)) out = j.at(key).get<T>();
}

template <class T>
T engine_value(engrave::Result<T> result) {
    if (!result) throw detail::DecodeError{api_error(result.error())};
    return std::move(*result);
}

void check_request_cancelled(const engrave::CaptureOptions &capture) {
    if (capture.cancelled && capture.cancelled()) throw detail::DecodeError{HostError{"Cancelled", "export capture cancelled"}};
}

// Keep only one position object at a time in the JSON encoder. The engine's
// payload and this byte vector coexist and share the reserved output byte limit.
std::vector<std::byte> position_json(const engrave::Positions &positions, const engrave::CaptureOptions &capture) {
    const auto limit = capture.max_bytes.value_or(std::numeric_limits<std::uint64_t>::max());
    const auto payload = engrave::position_payload_bytes(positions);
    if (payload > limit) throw detail::DecodeError{HostError{"ResourceLimit", "position payload exceeds output byte limit"}};
    const auto available = limit - payload;
    std::vector<std::byte> result;
    auto append = [&](const std::string &part) {
        check_request_cancelled(capture);
        if (part.size() > available - result.size()) {
            throw detail::DecodeError{HostError{"ResourceLimit", "position JSON exceeds output byte limit"}};
        }
        const auto *data = reinterpret_cast<const std::byte *>(part.data());
        result.insert(result.end(), data, data + part.size());
    };
    // Match the sorted object-key order of encode(positions).dump(), while
    // avoiding construction of its full intermediate JSON object tree.
    append("{\"elements\":[");
    bool first = true;
    for (const auto &e : positions.elements) {
        if (!first) append(",");
        first = false;
        append(Json{{"id", e.id}, {"x", e.x}, {"y", e.y}, {"sx", e.sx}, {"sy", e.sy}, {"page", e.page}}.dump());
    }
    append("],\"events\":[");
    first = true;
    for (const auto &e : positions.events) {
        if (!first) append(",");
        first = false;
        append(Json{{"elid", e.elid}, {"position", e.position}}.dump());
    }
    append("],\"page_size\":{\"height\":" + Json(positions.page_height).dump() + ",\"width\":" + Json(positions.page_width).dump() + "}}");
    return result;
}

engrave::Format score_format(const std::string &s) {
    if (s == "mscz") return engrave::Format::Mscz;
    if (s == "mscx") return engrave::Format::Mscx;
    if (s == "musicxml") return engrave::Format::MusicXml;
    if (s == "mxl") return engrave::Format::Mxl;
    if (s == "midi") return engrave::Format::Midi;
    invalid("unknown score format");
}

engrave::RenderFormat render_format(const std::string &s) {
    if (s == "pdf") return engrave::RenderFormat::Pdf;
    if (s == "png") return engrave::RenderFormat::Png;
    if (s == "svg") return engrave::RenderFormat::Svg;
    invalid("unknown render format");
}

engrave::AudioFormat audio_format(const std::string &s) {
    if (s == "wav") return engrave::AudioFormat::Wav;
    if (s == "flac") return engrave::AudioFormat::Flac;
    if (s == "opus") return engrave::AudioFormat::Opus;
    if (s == "mp3") return engrave::AudioFormat::Mp3;
    if (s == "aac") return engrave::AudioFormat::Aac;
    invalid("unknown audio format");
}

// Resolve names and paths against the engine's current private asset copies.
engrave::SoundFontSelection resolve_soundfont(const engrave::SoundFontSelection &selection, const engrave::EngineOptions &runtime) {
    if (std::holds_alternative<std::monostate>(selection)) return selection;
    if (const auto *index = std::get_if<std::int32_t>(&selection)) {
        if (*index < 0 || static_cast<std::size_t>(*index) >= runtime.soundfonts.size()) invalid("SoundFont index is out of range");
        return *index;
    }
    const auto &name = std::get<std::string>(selection);
    for (std::size_t i = 0; i < runtime.soundfonts.size(); ++i) {
        const auto &file = runtime.soundfonts[i];
        if (detail::path_text(file) == name || detail::path_text(file.stem()) == name) return static_cast<std::int32_t>(i);
    }
    throw detail::DecodeError{HostError{"NotFound", "no SoundFont named '" + name + "'"}};
}

engrave::AudioOptions effective_mix(engrave::Document &document, const ExportRequest &request, const engrave::EngineOptions &runtime) {
    auto result = request.audio_options;
    const auto defaults = engine_value(document.audio_tracks(result.score));
    if (defaults.tracks.size() > 256) throw detail::DecodeError{HostError{"ResourceLimit", "audio track count exceeds 256"}};
    result.master_volume_db = result.master_volume_db.value_or(defaults.master.volume_db);
    result.master_balance = result.master_balance.value_or(defaults.master.balance);
    std::set<std::int32_t> seen;
    for (const auto &track : result.tracks) {
        if (!seen.insert(track.track).second) invalid("duplicate audio track");
        if (std::none_of(defaults.tracks.begin(), defaults.tracks.end(), [&](const auto &t) { return t.track == track.track; })) {
            invalid("audio track is out of range");
        }
    }
    auto overrides = std::move(result.tracks);
    result.tracks.clear();
    result.tracks.reserve(defaults.tracks.size());
    for (const auto &track : defaults.tracks) {
        engrave::AudioTrackOptions effective;
        effective.track = track.track;
        effective.volume_db = track.mix.volume_db;
        effective.balance = track.mix.balance;
        effective.mute = track.mix.mute;
        effective.solo = track.mix.solo;
        auto found = std::find_if(overrides.begin(), overrides.end(), [&](const auto &t) { return t.track == track.track; });
        if (found != overrides.end()) {
            if (found->volume_db) effective.volume_db = found->volume_db;
            if (found->balance) effective.balance = found->balance;
            if (found->mute) effective.mute = found->mute;
            if (found->solo) effective.solo = found->solo;
            effective.soundfont = resolve_soundfont(found->soundfont, runtime);
            effective.preset = found->preset;
        }
        result.tracks.push_back(std::move(effective));
    }
    return result;
}
} // namespace

Result<ExportRequest> decode_export(const Json &j) {
    auto destination_error = [&](HostError error) -> Result<ExportRequest> {
        if (j.contains("destination") && j.at("destination").is_object() && j.at("destination").contains("path") &&
            j.at("destination").at("path").is_string()) {
            error.message = destination_message(j.at("destination").at("path").get<std::string>(), error.message);
        }
        return std::unexpected(std::move(error));
    };
    try {
        detail::validate_tool("score_export", j);
        ExportRequest result;
        result.document = j.at("document").get<std::string>();
        if (j.contains("expected_revision")) {
            auto revision = decode_revision(j.at("expected_revision"));
            if (!revision) return std::unexpected(revision.error());
            result.expected_revision = *revision;
        }
        const auto &destination = j.at("destination");
        result.destination.directory = destination.at("kind") == "directory";
        result.destination.path = detail::path(destination.at("path"));
        result.destination.requested_path = destination.at("path").get<std::string>();
        if (result.destination.path.empty()) invalid("destination path is empty");
        const auto &options = j.at("options");
        const auto selection = options.contains("score") ? detail::score(options.at("score")) : engrave::ScoreSel{};
        const auto kind = j.at("kind").get<std::string>();
        if (kind == "score") {
            result.kind = ExportKind::Score;
            auto &o = result.score_options;
            o.score = selection;
            o.format = score_format(options.at("format").get<std::string>());
            assign_if(options, "midi_expand_repeats", o.midi_expand_repeats);
            assign_if(options, "midi_rpns", o.midi_rpns);
            assign_if(options, "native_version", o.native_version);
        } else if (kind == "render") {
            result.kind = ExportKind::Render;
            auto &o = result.render_options;
            o.score = selection;
            o.format = render_format(options.at("format").get<std::string>());
            assign_if(options, "first_page", o.first_page);
            assign_if(options, "last_page", o.last_page);
            assign_if(options, "dpi", o.dpi);
            assign_if(options, "background", o.background);
            assign_if(options, "transparent", o.transparent);
            if (o.last_page != -1 && o.last_page < o.first_page) invalid("last_page precedes first_page");
        } else if (kind == "audio") {
            result.kind = ExportKind::Audio;
            auto &o = result.audio_options;
            o.score = selection;
            o.format = audio_format(options.at("format").get<std::string>());
            assign_if(options, "bitrate_kbps", o.bitrate_kbps);
            assign_if(options, "bit_depth", o.bit_depth);
            assign_if(options, "use_repeats", o.use_repeats);
            assign_if(options, "reverb", o.reverb);
            assign_if(options, "reverb_send", o.reverb_send);
            assign_if(options, "sample_rate", o.sample_rate);
            if (options.contains("master_volume_db")) o.master_volume_db = options.at("master_volume_db").get<double>();
            if (options.contains("master_balance")) o.master_balance = options.at("master_balance").get<double>();
            if (options.contains("tracks")) {
                for (const auto &t : options.at("tracks")) {
                    engrave::AudioTrackOptions track;
                    track.track = t.at("track").get<std::int32_t>();
                    if (t.contains("volume_db")) track.volume_db = t.at("volume_db").get<double>();
                    if (t.contains("balance")) track.balance = t.at("balance").get<double>();
                    if (t.contains("mute")) track.mute = t.at("mute").get<bool>();
                    if (t.contains("solo")) track.solo = t.at("solo").get<bool>();
                    if (t.contains("preset")) track.preset = t.at("preset").get<std::string>();
                    if (t.contains("soundfont") && !t.at("soundfont").is_null()) {
                        const auto &sf = t.at("soundfont");
                        track.soundfont = sf.is_string() ? engrave::SoundFontSelection(sf.get<std::string>()) :
                                                           engrave::SoundFontSelection(sf.get<std::int32_t>());
                    }
                    o.tracks.push_back(std::move(track));
                }
            }
        } else {
            result.kind = ExportKind::Positions;
            result.position_score = selection;
            result.position_kind =
                options.at("position_kind") == "measure" ? engrave::PositionKind::Measure : engrave::PositionKind::Segment;
        }
        return result;
    } catch (const detail::DecodeError &e) {
        return destination_error(e.error);
    } catch (const std::exception &e) {
        return destination_error(HostError{"InvalidArgument", e.what()});
    }
}

Json encode(const Artifact &artifact) {
    return {{"mime", artifact.mime}, {"path", detail::path_text(artifact.path)}, {"size", std::to_string(artifact.size)}};
}

Result<Json> live_export(engrave::Document &document, const ExportRequest &request, const ExportLimits &limits,
                         const std::function<bool()> &cancelled, std::chrono::steady_clock::time_point submitted, ExportTiming &timing,
                         const engrave::EngineOptions &runtime, const std::function<bool()> &begin_publication,
                         std::vector<std::string> *visible_paths) {
    namespace fs = std::filesystem;

    struct Cleanup {
        fs::path destination, stage;

        ~Cleanup() {
            std::error_code ec;
#ifdef MUSIC_ASYNC_TEST_HOOKS
            if (!stage.empty()) {
                if (const auto *barrier = std::getenv("ENGRAVE_MCP_TEST_CLEANUP_BARRIER")) {
                    std::fprintf(stderr, "CLEANUP_BLOCKED\n");
                    std::fflush(stderr);
                    while (!fs::exists(barrier)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (!stage.empty() && std::getenv("ENGRAVE_MCP_TEST_FAIL_CLEANUP")) {
                ec = std::make_error_code(std::errc::permission_denied);
            } else
#endif
                if (!stage.empty()) {
                fs::remove_all(stage, ec);
            }
            if (ec) {
                {
                    std::lock_guard lock(publication_gate.mutex);
                    ++publication_gate.cleanup_failures;
                }
                try {
                    terminal_diagnostic("staging cleanup failed: " + stage.string() + ": " + ec.message() + "\n");
                } catch (...) {}
            }
        }
    } cleanup;

    std::vector<Artifact> artifacts;
    std::vector<engrave::ExportWarning> warnings;
    bool test_deadline = false;
    auto check = [&] {
        if (cancelled()) throw detail::DecodeError{HostError{"Cancelled", "live export cancelled"}};
        if (test_deadline || ExportTiming::Clock::now() - submitted >= limits.deadline) {
            throw detail::DecodeError{HostError{"DeadlineExceeded", "export deadline exceeded"}};
        }
    };
    try {
        check();
        try {
            validate_output_path_syntax(request.destination.path);
            // Probe without mutation, then resolve existing prefixes even when
            // the leaf or parents are missing. Encoding validation must precede
            // recursive creation (including through symlinks).
            if (request.destination.directory) {
                const auto status = output_path_status(request.destination.path);
                if (fs::exists(status) && !fs::is_directory(status)) throw std::invalid_argument("destination must be a directory");
                cleanup.destination = fs::weakly_canonical(fs::absolute(request.destination.path));
            } else {
                cleanup.destination = normalize_output_path(request.destination.path);
            }
        } catch (const std::invalid_argument &e) {
            invalid(destination_message(request.destination.requested_path, e.what()));
        }
        auto invalid_resolved_path = [&] {
            // Preserve the well-formed JSON request, including its separators;
            // never convert the unrepresentable resolved path for diagnostics.
            invalid(destination_message(request.destination.requested_path,
                                        "resolves to a path that is not valid UTF-8; use a destination with a UTF-8 resolved path"));
        };
        auto report_path_text = [&](const fs::path &path) -> std::string {
            try {
                return detail::path_text(path);
            } catch (const std::exception &) {
                // Windows generic_u8string() throws for unpaired UTF-16
                // surrogates before the JSON UTF-8 validator can run.
                invalid_resolved_path();
                throw; // invalid_resolved_path always throws DecodeError
            }
        };
        auto validate_report_path = [&](const fs::path &path) {
            try {
                // Use the wire encoder's strict UTF-8 rules; replacement would
                // make both success and partial-error paths unusable.
                (void) Json(report_path_text(path)).dump();
            } catch (const Json::type_error &) {
                invalid_resolved_path();
            }
        };
        validate_report_path(cleanup.destination);
        try {
            validate_output_path_syntax(cleanup.destination);
            if (request.destination.directory) {
                require_output_directory(cleanup.destination);
            } else if (fs::is_directory(output_path_status(cleanup.destination))) {
                invalid(destination_message(request.destination.requested_path, "file destination is a directory"));
            } else {
                require_output_directory(cleanup.destination.parent_path());
            }
        } catch (const std::invalid_argument &e) {
            invalid(destination_message(request.destination.requested_path, e.what()));
        }
#ifdef MUSIC_ASYNC_TEST_HOOKS
        if (std::getenv("ENGRAVE_MCP_TEST_FAIL_STAGING")) {
            throw std::system_error(EIO, std::generic_category(), "Injected staging directory failure");
        }
#endif
        cleanup.stage = create_private_directory(request.destination.directory ? cleanup.destination : cleanup.destination.parent_path());
        std::uint64_t total = 0;

        struct Staged {
            fs::path source;
            Artifact artifact;
        };

        std::vector<Staged> staged;
        auto write = [&](const std::string &name, const std::string &mime, std::span<const std::byte> bytes) {
            check();
            auto start = ExportTiming::Clock::now();
            if (bytes.empty()) throw detail::DecodeError{HostError{"EngineFailure", "export produced an empty artifact"}};
            if (bytes.size() > limits.output_bytes - total) {
                throw detail::DecodeError{HostError{"ResourceLimit", "export exceeds output byte limit"}};
            }
            const auto destination = request.destination.directory ? cleanup.destination / name : cleanup.destination;
            if (report_path_text(destination).size() > 4096) {
                throw detail::DecodeError{
                    HostError{"ResourceLimit", destination_message(request.destination.requested_path, "artifact path exceeds limit")}};
            }
            auto path = cleanup.stage / name;
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close();
            if (!out || fs::file_size(path) != bytes.size()) throw detail::DecodeError{HostError{"Io", "cannot write staged output"}};
            total += bytes.size();
            staged.push_back({path, {mime, destination, bytes.size()}});
            timing.add("encode_write", start);
        };
        engrave::CaptureOptions capture;
        capture.max_bytes = limits.output_bytes;
#ifdef MUSIC_ASYNC_TEST_HOOKS
        int cancellation_polls = 0;
        bool conversion_entered = false;
#endif
        capture.cancelled = [&] {
#ifdef MUSIC_ASYNC_TEST_HOOKS
            const auto test_path = request.destination.path.string();
            if (test_path.find(".test-470-") != std::string::npos && !conversion_entered) {
                const bool converting = test_path.find("converting") != std::string::npos;
                const auto progress = engrave::testing::write470_progress();
                if (progress.stage == (converting ? engrave::testing::Write470Progress::Stage::Converting :
                                                    engrave::testing::Write470Progress::Stage::Serializing) &&
                    progress.units >= 2) {
                    conversion_entered = true;
                    std::fprintf(stderr, "WRITE470_%s units=%llu\n", converting ? "CONVERTING" : "SERIALIZING",
                                 static_cast<unsigned long long>(progress.units));
                    std::fflush(stderr);
                    test_deadline = test_path.find("deadline") != std::string::npos;
                    while (!test_deadline && !cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    std::fprintf(stderr, "WRITE470_RETURN units=%llu error=Cancelled\n", static_cast<unsigned long long>(progress.units));
                    std::fflush(stderr);
                }
            }
            if (request.destination.path.string().find(".test-live-noncooperative") != std::string::npos) {
                std::fprintf(stderr, "LIVE_NONCOOPERATIVE\n");
                std::fflush(stderr);
                if (std::getenv("ENGRAVE_MCP_TEST_OWNER_UNTIL_WRITE_FAILURE")) {
                    std::unique_lock lock(publication_gate.mutex);
                    publication_gate.changed.wait(lock, [] { return publication_gate.closing; });
                    std::fprintf(stderr, "OWNER_OBSERVED_WRITE_FAILURE\n");
                    std::fflush(stderr);
                } else if (const auto *delay = std::getenv("ENGRAVE_MCP_TEST_NONCOOPERATIVE_MS")) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::stoul(delay)));
                } else {
                    std::this_thread::sleep_for(std::chrono::seconds(30));
                }
            }
            if (request.destination.path.string().find(".test-live-cancel") != std::string::npos &&
                ++cancellation_polls == (request.kind == ExportKind::Render ? 2 : 8)) {
                std::fprintf(stderr, "LIVE_CANCEL_POLL\n");
                std::fflush(stderr);
                while (!cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
#endif
            return test_deadline || cancelled() || ExportTiming::Clock::now() - submitted >= limits.deadline;
        };
        if (request.kind == ExportKind::Score) {
            auto start = ExportTiming::Clock::now();
            auto options = request.score_options;
            options.capture = capture;
            auto output = document.export_score_with_warnings(options);
            if (!output && output.error().code == engrave::ErrorCode::Unsupported && options.format == engrave::Format::Midi) {
                check();
                options.capture = {};
                output = document.export_score_with_warnings(options);
            }
            auto result = engine_value(std::move(output));
            timing.add("engine_call", start);
            warnings = std::move(result.warnings);
            const char *mime = "audio/midi";
            switch (options.format) {
                case engrave::Format::Mscx: mime = "application/vnd.musescore.mscx+xml"; break;
                case engrave::Format::Mscz: mime = "application/vnd.musescore"; break;
                case engrave::Format::MusicXml: mime = "application/vnd.recordare.musicxml+xml"; break;
                case engrave::Format::Mxl: mime = "application/vnd.recordare.musicxml"; break;
                default: break;
            }
            write("output", mime, result.bytes);
        } else if (request.kind == ExportKind::Positions) {
            auto start = ExportTiming::Clock::now();
            auto positions = engine_value(document.positions(request.position_kind, request.position_score, capture));
            timing.add("engine_call", start);
            start = ExportTiming::Clock::now();
            auto bytes = position_json(positions, capture);
            timing.add("encode_write", start);
            write("output", "application/json", bytes);
        } else if (request.kind == ExportKind::Render) {
            auto start = ExportTiming::Clock::now();
            const auto count = engine_value(document.page_count(request.render_options.score));
            auto options = request.render_options;
            options.cancelled = capture.cancelled;
            const auto last = options.last_page < 0 ? count - 1 : options.last_page;
            if (options.first_page >= count || last >= count) invalid("render page range is outside the score");
            if (static_cast<std::int64_t>(last) - options.first_page + 1 > 4096) {
                throw detail::DecodeError{HostError{"ResourceLimit", "too many output pages"}};
            }
            timing.add("engine_call", start);
            if (options.format == engrave::RenderFormat::Pdf) {
                check();
                start = ExportTiming::Clock::now();
                auto output = engine_value(document.render(options));
                timing.add("engine_call", start);
                write("output", "application/pdf", output.document);
            } else {
                for (auto page = options.first_page; page <= last; ++page) {
                    check();
                    start = ExportTiming::Clock::now();
                    options.first_page = options.last_page = page;
                    auto output = engine_value(document.render(options));
                    timing.add("engine_call", start);
                    if (output.pages.size() != 1) {
                        throw detail::DecodeError{HostError{"EngineFailure", "renderer returned invalid page count"}};
                    }
                    const bool png = options.format == engrave::RenderFormat::Png;
                    std::ostringstream name;
                    name << "page-" << std::setfill('0') << std::setw(4) << page + 1 << (png ? ".png" : ".svg");
                    write(name.str(), png ? "image/png" : "image/svg+xml", output.pages[0]);
                }
            }
        } else if (request.kind == ExportKind::Audio) {
            auto start = ExportTiming::Clock::now();
            auto options = effective_mix(document, request, runtime);
            options.cancelled = capture.cancelled;
            const auto rate = runtime.audio_sample_rate;
            if (options.sample_rate && options.sample_rate != rate) {
                throw detail::DecodeError{HostError{"Unsupported", "audio sample rate differs from the engine rate"}};
            }
            // The engine derives playback duration and checks its global projection
            // after the cancellable model build. Do not ask outline() for it:
            // outline also scans lyrics/metadata without cancellation. Per-request
            // byte quotas below are enforced by the encoder before any sink call.
            check();
            const auto limit = audio_file_budget(limits.output_bytes);
            options.max_bytes = limit;
            options.max_write_bytes = audio_write_budget(limit);
            const auto path = cleanup.stage / "output";
            std::ofstream out;
            std::uint64_t bytes = 0;
            auto audio = engine_value(document.render_audio(options, [&](std::span<const std::byte> chunk) {
                check();
                if (!out.is_open()) out.open(path, std::ios::binary | std::ios::trunc);
                if (chunk.size() > limit - bytes) throw detail::DecodeError{HostError{"ResourceLimit", "audio exceeds output byte limit"}};
                out.write(reinterpret_cast<const char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
                if (!out) throw detail::DecodeError{HostError{"Io", "cannot write staged audio"}};
                bytes += chunk.size();
            }));
            out.close();
            if (!out) throw detail::DecodeError{HostError{"Io", "cannot close staged audio"}};
            if (!bytes || bytes != audio.bytes || fs::file_size(path) != bytes) {
                throw detail::DecodeError{HostError{"EngineFailure", "invalid audio byte count"}};
            }
            const char *mime = "audio/wav";
            switch (options.format) {
                case engrave::AudioFormat::Flac: mime = "audio/flac"; break;
                case engrave::AudioFormat::Opus: mime = "audio/ogg"; break;
                case engrave::AudioFormat::Mp3: mime = "audio/mpeg"; break;
                case engrave::AudioFormat::Aac: mime = "audio/aac"; break;
                default: break;
            }
            timing.add("engine_call", start);
            const auto destination = request.destination.directory ? cleanup.destination / "output" : cleanup.destination;
            if (report_path_text(destination).size() > 4096) {
                throw detail::DecodeError{
                    HostError{"ResourceLimit", destination_message(request.destination.requested_path, "artifact path exceeds limit")}};
            }
            staged.push_back({path, {mime, destination, bytes}});
        }
        // Preflight both complete and worst-case partial reports before any rename.
        Json all = Json::array(), paths = Json::array();
        for (const auto &file : staged) {
            validate_report_path(file.artifact.path);
            all.push_back(encode(file.artifact));
            paths.push_back(detail::path_text(file.artifact.path));
        }
        Json success{{"ok", true},
                     {"source_revision", std::to_string(document.revision())},
                     {"artifacts", std::move(all)},
                     {"export_warnings", encode(warnings)}};
        // Export exceptions below are bounded to our 16 KiB diagnostic cap,
        // not the schema's larger message ceiling. Control bytes maximize JSON
        // escaping, including the duplicated legacy text form.
        Json partial{
            {"ok", false},
            {"error",
             {{"code", "EngineFailure"}, {"message", std::string(kDiagnosticLimit, '\1')}, {"published_artifacts", std::move(paths)}}}};
        const auto report_start = ExportTiming::Clock::now();
        const auto report_bytes = std::max(export_report_bound(success), export_report_bound(partial));
        timing.add("report_preflight", report_start);
        std::size_t report_limit = 16u * 1024u * 1024u;
#ifdef MUSIC_ASYNC_TEST_HOOKS
        if (const auto *limit = std::getenv("ENGRAVE_MCP_TEST_REPORT_LIMIT")) report_limit = std::stoull(limit);
        std::fprintf(stderr, "PUBLICATION_REPORT_BOUND %zu\n", report_bytes);
        std::fflush(stderr);
#endif
        if (report_bytes > report_limit) {
            throw detail::DecodeError{HostError{"ResourceLimit", "export artifact report exceeds transport budget"}};
        }
        // All rendering and staging succeeds before any file becomes visible.
        check();
        // Atomically exclude watchdog termination before the first visible file.
        // Once committed, even slow publication must finish and report its subset.
#ifdef MUSIC_ASYNC_TEST_HOOKS
        if (const auto *barrier = std::getenv("ENGRAVE_MCP_TEST_ADMISSION_BARRIER")) {
            std::fprintf(stderr, "PUBLICATION_READY\n");
            std::fflush(stderr);
            while (!fs::exists(barrier)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
#endif
        if (begin_publication && !begin_publication()) {
            throw detail::DecodeError{HostError{"Cancelled", "watchdog claimed export before publication"}};
        }
        artifacts.reserve(staged.size()); // all storage allocated before visibility
        std::vector<std::string> prepared_paths;
        if (visible_paths) {
            visible_paths->reserve(staged.size());
            for (const auto &file : staged) prepared_paths.push_back(detail::path_text(file.artifact.path));
        }
        for (auto &file : staged) {
            auto start = ExportTiming::Clock::now();
            try {
                publish_output(file.source, file.artifact.path, [&] {
                    // Runs immediately after rename, before even directory fsync.
                    static_assert(std::is_nothrow_move_constructible_v<Artifact>);
                    artifacts.push_back(std::move(file.artifact));
                    if (visible_paths) visible_paths->push_back(std::move(prepared_paths[artifacts.size() - 1]));
#ifdef MUSIC_ASYNC_TEST_HOOKS
                    if (const auto *after = std::getenv("ENGRAVE_MCP_TEST_THROW_AFTER_RENAME")) {
                        if (artifacts.size() == std::stoul(after)) throw std::runtime_error("Injected post-rename failure");
                    }
#endif
                });
            } catch (const std::exception &e) {
                throw detail::DecodeError{HostError{"Io", e.what()}};
            }
            timing.add("publish", start);
        }
        Json encoded = Json::array();
        for (const auto &a : artifacts) encoded.push_back(encode(a));
        return Json{{"ok", true},
                    {"source_revision", std::to_string(document.revision())},
                    {"artifacts", std::move(encoded)},
                    {"export_warnings", encode(warnings)}};
    } catch (const detail::DecodeError &e) {
#ifdef MUSIC_ASYNC_TEST_HOOKS
        if (!artifacts.empty() && std::getenv("ENGRAVE_MCP_TEST_THROW_ERROR_REPORT")) {
            throw std::runtime_error("Injected error report construction failure");
        }
#endif
        auto error = e.error;
        if (error.code == "Io") {
            error = with_destination(std::move(error), request.destination.requested_path);
        } else {
            error.message.resize(utf8_prefix(error.message, kDiagnosticLimit));
        }
        if (error.code == "Cancelled" && !cancelled()) error = HostError{"DeadlineExceeded", "export deadline exceeded"};
        for (const auto &a : artifacts) error.published_artifacts.push_back(detail::path_text(a.path));
        return std::unexpected(std::move(error));
    } catch (const std::system_error &e) {
        HostError error{"Io", destination_message(request.destination.requested_path, e.what())};
        for (const auto &a : artifacts) error.published_artifacts.push_back(detail::path_text(a.path));
        return std::unexpected(std::move(error));
    } catch (const std::exception &e) {
        std::string message = e.what();
        message.resize(utf8_prefix(message, kDiagnosticLimit));
        HostError error{"EngineFailure", std::move(message)};
        for (const auto &a : artifacts) error.published_artifacts.push_back(detail::path_text(a.path));
        return std::unexpected(std::move(error));
    }
}
} // namespace music::v2
