#include "api_output.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#include <tuple>

#include <ankerl/unordered_dense.h>
#include <engrave/testing/write470.h>
#include <glaze/json/write.hpp>

#include "api_json_internal.h"
#include "audio_budget.h"
#include "export_timing.h"
#include "publication.h"
#include "publication_gate.h"
#include "terminal_diagnostic.h"
#include "test_hooks.h"
#include "tool_result.h"
#include "utf8_text.h"

namespace music::api {

namespace detail {
// Alphabetical member order matches Json (glz::generic_sorted_u64) key order;
// external linkage is required by Glaze's pure reflection.
struct PositionElementWire {
    i32 id = 0;
    i32 page = 0;
    f64 sx = 0;
    f64 sy = 0;
    f64 x = 0;
    f64 y = 0;
};

struct PositionEventWire {
    i32 elid = 0;
    i32 position = 0;
};

struct PositionPageSizeWire {
    f64 height = 0;
    f64 width = 0;
};
} // namespace detail

namespace {
using detail::invalid_error;
using detail::PositionElementWire;
using detail::PositionEventWire;
using detail::PositionPageSizeWire;

constexpr usize k_diagnostic_limit = 16384;

// Keep the requested destination and the reason in a bounded, UTF-8-safe
// diagnostic. A very long path is abbreviated in the middle rather than cut
// mid-code-point or truncated before the reason.
std::string destination_message(std::string_view requested, std::string_view reason) {
    constexpr std::string_view prefix = "destination '";
    constexpr std::string_view mid = "': ";
    std::string bounded_reason = reason.size() > 8192 ? utf8_abbrev(reason, 8192) : std::string(reason);
    const auto fixed = prefix.size() + mid.size() + bounded_reason.size();
    if (fixed >= k_diagnostic_limit) {
        const auto allowed = k_diagnostic_limit - prefix.size() - mid.size() - 3;
        return std::string(prefix) + "..." + std::string(mid) + utf8_abbrev(bounded_reason, allowed);
    }
    std::string result;
    result.reserve(k_diagnostic_limit);
    result.append(prefix);
    result.append(utf8_abbrev(requested, k_diagnostic_limit - fixed));
    result.append(mid);
    result.append(bounded_reason);
    return result;
}

HostError with_destination(HostError error, std::string_view requested) {
    if (error.message.rfind("destination '", 0) == 0) {
        error.message.resize(utf8_prefix(error.message, k_diagnostic_limit));
    } else {
        error.message = destination_message(requested, error.message);
    }
    return error;
}

template <class T>
void assign_if(const Json &j, const char *key, T &out) {
    if (!j.contains(key)) return;
    if constexpr (std::same_as<T, bool>) {
        out = j.at(key).get<bool>();
    } else if constexpr (std::integral<T> || std::floating_point<T>) {
        out = j.at(key).as<T>();
    } else {
        out = j.at(key).get<T>();
    }
}

// Keep only one position object at a time in the JSON encoder. The engine's
// payload and this byte vector coexist and share the reserved output byte limit.
Result<std::vector<std::byte>> position_json(const engrave::Positions &positions, const engrave::CaptureOptions &capture) {
    const auto limit = capture.max_bytes.value_or(std::numeric_limits<u64>::max());
    const auto payload = engrave::position_payload_bytes(positions);
    if (payload > limit) return std::unexpected(HostError{"ResourceLimit", "position payload exceeds output byte limit"});
    const auto available = limit - payload;
    std::vector<std::byte> result;
    auto append = [&](std::string_view part) -> Result<void> {
        if (capture.cancelled && capture.cancelled()) return std::unexpected(HostError{"Cancelled", "export capture cancelled"});
        if (part.size() > available - result.size()) {
            return std::unexpected(HostError{"ResourceLimit", "position JSON exceeds output byte limit"});
        }
        const auto *data = reinterpret_cast<const std::byte *>(part.data());
        result.insert(result.end(), data, data + part.size());
        return {};
    };

    std::string item;
    if (auto ok = append("{\"elements\":["); !ok) return std::unexpected(std::move(ok.error()));
    bool first = true;
    for (const auto &e : positions.elements) {
        if (!first) {
            if (auto ok = append(","); !ok) return std::unexpected(std::move(ok.error()));
        }
        first = false;
        item.clear();
        const PositionElementWire wire{.id = e.id, .page = e.page, .sx = e.sx, .sy = e.sy, .x = e.x, .y = e.y};
        if (glz::write_json(wire, item)) return std::unexpected(HostError{"EngineFailure", "cannot serialize position element"});
        if (auto ok = append(item); !ok) return std::unexpected(std::move(ok.error()));
    }
    if (auto ok = append("],\"events\":["); !ok) return std::unexpected(std::move(ok.error()));
    first = true;
    for (const auto &e : positions.events) {
        if (!first) {
            if (auto ok = append(","); !ok) return std::unexpected(std::move(ok.error()));
        }
        first = false;
        item.clear();
        const PositionEventWire wire{.elid = e.elid, .position = e.position};
        if (glz::write_json(wire, item)) return std::unexpected(HostError{"EngineFailure", "cannot serialize position event"});
        if (auto ok = append(item); !ok) return std::unexpected(std::move(ok.error()));
    }
    item.clear();
    const PositionPageSizeWire page_size{.height = positions.page_height, .width = positions.page_width};
    if (glz::write_json(page_size, item)) return std::unexpected(HostError{"EngineFailure", "cannot serialize page size"});
    if (auto ok = append("],\"page_size\":"); !ok) return std::unexpected(std::move(ok.error()));
    if (auto ok = append(item); !ok) return std::unexpected(std::move(ok.error()));
    if (auto ok = append("}"); !ok) return std::unexpected(std::move(ok.error()));
    return result;
}

Result<engrave::Format> score_format(const std::string &s) {
    if (s == "mscz") return engrave::Format::MSCZ;
    if (s == "mscx") return engrave::Format::MSCX;
    if (s == "musicxml") return engrave::Format::MUSIC_XML;
    if (s == "mxl") return engrave::Format::MXL;
    if (s == "midi") return engrave::Format::MIDI;
    return std::unexpected(invalid_error("unknown score format"));
}

Result<engrave::RenderFormat> render_format(const std::string &s) {
    if (s == "pdf") return engrave::RenderFormat::PDF;
    if (s == "png") return engrave::RenderFormat::PNG;
    if (s == "svg") return engrave::RenderFormat::SVG;
    return std::unexpected(invalid_error("unknown render format"));
}

Result<engrave::AudioFormat> audio_format(const std::string &s) {
    if (s == "wav") return engrave::AudioFormat::WAV;
    if (s == "flac") return engrave::AudioFormat::FLAC;
    if (s == "opus") return engrave::AudioFormat::OPUS;
    if (s == "mp3") return engrave::AudioFormat::MP3;
    if (s == "aac") return engrave::AudioFormat::AAC;
    return std::unexpected(invalid_error("unknown audio format"));
}

Result<engrave::SoundFontSelection> resolve_soundfont(const engrave::SoundFontSelection &selection, const RuntimeOptions &runtime) {
    if (std::holds_alternative<std::monostate>(selection)) return selection;
    if (const auto *index = std::get_if<i32>(&selection)) {
        if (*index < 0 || static_cast<usize>(*index) >= runtime.soundfonts.size()) {
            return std::unexpected(invalid_error("SoundFont index is out of range"));
        }
        return engrave::SoundFontSelection{*index};
    }
    const auto &name = std::get<std::string>(selection);
    const auto path = std::filesystem::u8path(name);
    const auto normalized = path.lexically_normal();
    for (usize i = 0; i < runtime.soundfonts.size(); ++i) {
        const auto &font = runtime.soundfonts[i];
        if (path == font.source || normalized == font.source.lexically_normal() || path == font.copy) {
            return engrave::SoundFontSelection{static_cast<i32>(i)};
        }
    }
    for (usize i = 0; i < runtime.soundfonts.size(); ++i) {
        const auto &file = runtime.soundfonts[i].copy;
        if (path == file.filename() || path == file.stem()) {
            return engrave::SoundFontSelection{static_cast<i32>(i)};
        }
    }
    return std::unexpected(HostError{"NotFound", "no SoundFont named '" + name + "'"});
}

Result<engrave::AudioOptions> effective_mix(engrave::Document &document, const ExportRequest &request, const RuntimeOptions &runtime) {
    auto result = request.audio_options;
    auto defaults_res = document.audio_tracks(result.score);
    if (!defaults_res) return std::unexpected(api_error(defaults_res.error()));
    const auto &defaults = *defaults_res;
    if (defaults.tracks.size() > 256) return std::unexpected(HostError{"ResourceLimit", "audio track count exceeds 256"});
    result.master_volume_db = result.master_volume_db.value_or(defaults.master.volume_db);
    result.master_balance = result.master_balance.value_or(defaults.master.balance);
    ankerl::unordered_dense::set<i32> seen;
    for (const auto &track : result.tracks) {
        if (!seen.insert(track.track).second) return std::unexpected(invalid_error("duplicate audio track"));
        if (std::none_of(defaults.tracks.begin(), defaults.tracks.end(), [&](const auto &t) { return t.track == track.track; })) {
            return std::unexpected(invalid_error("audio track is out of range"));
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
            auto sf = resolve_soundfont(found->soundfont, runtime);
            if (!sf) return std::unexpected(std::move(sf.error()));
            effective.soundfont = std::move(*sf);
            effective.preset = found->preset;
        }
        result.tracks.push_back(std::move(effective));
    }
    return result;
}
} // namespace

Result<ExportRequest> decode_export(const Json &j) {
    auto destination_error = [&](HostError error) -> Result<ExportRequest> {
        if (j.contains("path") && j.at("path").is_string()) {
            error.message = destination_message(j.at("path").get<std::string>(), error.message);
        }
        return std::unexpected(std::move(error));
    };
    try {
        if (auto ok = detail::validate_tool("score_export", j); !ok) return destination_error(std::move(ok.error()));
        ExportRequest result;
        result.document = j.at("document").get<std::string>();
        if (j.contains("expected_revision")) {
            auto revision = decode_revision(j.at("expected_revision"));
            if (!revision) return std::unexpected(std::move(revision.error()));
            result.expected_revision = *revision;
        }
        const auto &format = j.at("format").get<std::string>();
        result.destination.directory = format == "png" || format == "svg";
        auto dest_path = detail::path(j.at("path"));
        if (!dest_path) return destination_error(std::move(dest_path.error()));
        result.destination.path = std::move(*dest_path);
        result.destination.requested_path = j.at("path").get<std::string>();
        if (result.destination.path.empty()) return destination_error(invalid_error("destination path is empty"));
        const auto options = json_value<Json>(j, "options", make_object<Json>());
        for (const auto &[key, value] : options.get_object()) {
            bool valid = false;
            if (key == "midi_expand_repeats" || key == "midi_rpns") {
                valid = format == "midi";
            } else if (key == "native_version") {
                valid = format == "mscx" || format == "mscz";
            } else if (key == "dpi" || key == "transparent") {
                valid = format == "png";
            } else if (key == "first_page" || key == "last_page" || key == "background") {
                valid = bool(render_format(format));
            } else {
                valid = bool(audio_format(format));
            }
            if (!valid) {
                return destination_error(
                    invalid_error("options." + key + ": unsupported for " + format + "; expected a format-specific option"));
            }
        }
        engrave::ScoreSel selection{};
        if (j.contains("excerpt")) {
            auto sel = detail::score(j);
            if (!sel) return destination_error(std::move(sel.error()));
            selection = *sel;
        }
        if (score_format(format)) {
            result.kind = ExportKind::SCORE;
            auto &o = result.score_options;
            o.score = selection;
            auto fmt = score_format(format);
            if (!fmt) return destination_error(std::move(fmt.error()));
            o.format = *fmt;
            assign_if(options, "midi_expand_repeats", o.midi_expand_repeats);
            assign_if(options, "midi_rpns", o.midi_rpns);
            assign_if(options, "native_version", o.native_version);
        } else if (render_format(format)) {
            result.kind = ExportKind::RENDER;
            auto &o = result.render_options;
            o.score = selection;
            auto fmt = render_format(format);
            if (!fmt) return destination_error(std::move(fmt.error()));
            o.format = *fmt;
            assign_if(options, "first_page", o.first_page);
            assign_if(options, "last_page", o.last_page);
            assign_if(options, "dpi", o.dpi);
            if (o.format == engrave::RenderFormat::PNG) {
                o.background = !json_value<bool>(options, "transparent", false);
                o.transparent = false;
            }
            assign_if(options, "background", o.background);
            assign_if(options, "transparent", o.transparent);
            if (o.last_page != -1 && o.last_page < o.first_page) return destination_error(invalid_error("last_page precedes first_page"));
        } else if (audio_format(format)) {
            result.kind = ExportKind::AUDIO;
            auto &o = result.audio_options;
            o.score = selection;
            auto fmt = audio_format(format);
            if (!fmt) return destination_error(std::move(fmt.error()));
            o.format = *fmt;
            assign_if(options, "bitrate_kbps", o.bitrate_kbps);
            assign_if(options, "bit_depth", o.bit_depth);
            assign_if(options, "use_repeats", o.use_repeats);
            assign_if(options, "reverb", o.reverb);
            assign_if(options, "reverb_send", o.reverb_send);
            assign_if(options, "sample_rate", o.sample_rate);
            if (options.contains("master_volume_db")) o.master_volume_db = options.at("master_volume_db").as<f64>();
            if (options.contains("master_balance")) o.master_balance = options.at("master_balance").as<f64>();
            if (options.contains("tracks")) {
                for (const auto &t : options.at("tracks").get_array()) {
                    engrave::AudioTrackOptions track;
                    track.track = t.at("track").as<i32>();
                    if (t.contains("volume_db")) track.volume_db = t.at("volume_db").as<f64>();
                    if (t.contains("balance")) track.balance = t.at("balance").as<f64>();
                    if (t.contains("mute")) track.mute = t.at("mute").get<bool>();
                    if (t.contains("solo")) track.solo = t.at("solo").get<bool>();
                    if (t.contains("preset")) track.preset = t.at("preset").get<std::string>();
                    if (t.contains("soundfont") && !t.at("soundfont").is_null()) {
                        const auto &sf = t.at("soundfont");
                        track.soundfont =
                            sf.is_string() ? engrave::SoundFontSelection(sf.get<std::string>()) : engrave::SoundFontSelection(sf.as<i32>());
                    }
                    o.tracks.push_back(std::move(track));
                }
            }
        } else {
            result.kind = ExportKind::POSITIONS;
            result.position_score = selection;
            result.position_kind = format == "measure_positions" ? engrave::PositionKind::MEASURE : engrave::PositionKind::SEGMENT;
        }
        return result;
    } catch (const std::exception &e) {
        return destination_error(HostError{"InvalidArgument", e.what()});
    }
}

Json encode(const Artifact &artifact) {
    return {{"mime", artifact.mime}, {"path", detail::path_text(artifact.path)}, {"size", artifact.size}};
}

Result<Json> live_export(engrave::Document &document, const ExportRequest &request, const ExportLimits &limits, FnRef<bool()> cancelled,
                         std::chrono::steady_clock::time_point submitted, ExportTiming &timing, const RuntimeOptions &runtime,
                         FnRef<bool()> begin_publication, std::vector<std::string> *visible_paths) {
    namespace fs = std::filesystem;
    const bool hooks = test_hooks_enabled();

    struct Cleanup {
        fs::path destination, stage;
        bool hooks = false;

        ~Cleanup() {
            std::error_code ec;
            if (hooks && !stage.empty()) {
                if (const auto *barrier = std::getenv("ENGRAVE_MCP_TEST_CLEANUP_BARRIER")) {
                    std::fprintf(stderr, "CLEANUP_BLOCKED\n");
                    std::fflush(stderr);
                    while (!fs::exists(barrier)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (hooks && !stage.empty() && std::getenv("ENGRAVE_MCP_TEST_FAIL_CLEANUP")) {
                ec = std::make_error_code(std::errc::permission_denied);
            } else if (!stage.empty()) {
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
    } cleanup{.destination = {}, .stage = {}, .hooks = hooks};

    std::vector<Artifact> artifacts;
    std::vector<engrave::ExportWarning> warnings;
    bool test_deadline = false;

    auto fail_export = [&](HostError error) -> Result<Json> {
        if (hooks && !artifacts.empty() && std::getenv("ENGRAVE_MCP_TEST_THROW_ERROR_REPORT")) {
            throw std::runtime_error("Injected error report construction failure");
        }
        if (error.code == "Io") {
            error = with_destination(std::move(error), request.destination.requested_path);
        } else {
            error.message.resize(utf8_prefix(error.message, k_diagnostic_limit));
        }
        if (error.code == "Cancelled" && !cancelled()) error = HostError{"DeadlineExceeded", "export deadline exceeded"};
        for (const auto &a : artifacts) error.published_artifacts.push_back(detail::path_text(a.path));
        return std::unexpected(std::move(error));
    };

    auto check = [&]() -> Result<void> {
        if (cancelled()) return std::unexpected(HostError{"Cancelled", "live export cancelled"});
        if (test_deadline || ExportTiming::Clock::now() - submitted >= limits.deadline) {
            return std::unexpected(HostError{"DeadlineExceeded", "export deadline exceeded"});
        }
        return {};
    };

    try {
        if (auto ok = check(); !ok) return fail_export(std::move(ok.error()));
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
            return fail_export(invalid_error(destination_message(request.destination.requested_path, e.what())));
        }
        auto invalid_resolved_error = [&]() -> HostError {
            // Preserve the well-formed JSON request, including its separators;
            // never convert the unrepresentable resolved path for diagnostics.
            return invalid_error(
                destination_message(request.destination.requested_path,
                                    "resolves to a path that is not valid UTF-8; use a destination with a UTF-8 resolved path"));
        };
        auto report_path_text = [&](const fs::path &path) -> Result<std::string> {
            try {
                return detail::path_text(path);
            } catch (const std::exception &) {
                // Windows generic_u8string() throws for unpaired UTF-16
                // surrogates before the JSON UTF-8 validator can run.
                return std::unexpected(invalid_resolved_error());
            }
        };
        auto validate_report_path = [&](const fs::path &path) -> Result<void> {
            auto text = report_path_text(path);
            if (!text) return std::unexpected(std::move(text.error()));
            if (!valid_utf8(*text)) return std::unexpected(invalid_resolved_error());
            return {};
        };
        if (auto ok = validate_report_path(cleanup.destination); !ok) return fail_export(std::move(ok.error()));
        try {
            validate_output_path_syntax(cleanup.destination);
            if (request.destination.directory) {
                require_output_directory(cleanup.destination);
            } else if (fs::is_directory(output_path_status(cleanup.destination))) {
                return fail_export(
                    invalid_error(destination_message(request.destination.requested_path, "file destination is a directory")));
            } else {
                require_output_directory(cleanup.destination.parent_path());
            }
        } catch (const std::invalid_argument &e) {
            return fail_export(invalid_error(destination_message(request.destination.requested_path, e.what())));
        }
        if (hooks && std::getenv("ENGRAVE_MCP_TEST_FAIL_STAGING")) {
            throw std::system_error(EIO, std::generic_category(), "Injected staging directory failure");
        }
        cleanup.stage = create_private_directory(request.destination.directory ? cleanup.destination : cleanup.destination.parent_path());
        u64 total = 0;

        struct Staged {
            fs::path source;
            Artifact artifact;
        };

        std::vector<Staged> staged;
        auto write = [&](const std::string &name, const std::string &mime, std::span<const std::byte> bytes) -> Result<void> {
            if (auto ok = check(); !ok) return std::unexpected(std::move(ok.error()));
            auto start = ExportTiming::Clock::now();
            if (bytes.empty()) return std::unexpected(HostError{"EngineFailure", "export produced an empty artifact"});
            if (bytes.size() > limits.output_bytes - total) {
                return std::unexpected(HostError{"ResourceLimit", "export exceeds output byte limit"});
            }
            const auto destination = request.destination.directory ? cleanup.destination / name : cleanup.destination;
            auto dest_text = report_path_text(destination);
            if (!dest_text) return std::unexpected(std::move(dest_text.error()));
            if (dest_text->size() > 4096) {
                return std::unexpected(
                    HostError{"ResourceLimit", destination_message(request.destination.requested_path, "artifact path exceeds limit")});
            }
            auto path = cleanup.stage / name;
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close();
            if (!out || fs::file_size(path) != bytes.size()) return std::unexpected(HostError{"Io", "cannot write staged output"});
            total += bytes.size();
            staged.push_back({path, {mime, destination, bytes.size()}});
            timing.add("encode_write", start);
            return {};
        };
        engrave::CaptureOptions capture;
        capture.max_bytes = limits.output_bytes;
        i32 cancellation_polls = 0;
        bool conversion_entered = false;
        capture.cancelled = [&] {
            if (hooks) {
                const auto test_path = request.destination.path.string();
                if (test_path.find(".test-470-") != std::string::npos && !conversion_entered) {
                    const bool converting = test_path.find("converting") != std::string::npos;
                    const auto progress = engrave::testing::write470_progress();
                    if (progress.stage == (converting ? engrave::testing::Write470Progress::Stage::CONVERTING :
                                                        engrave::testing::Write470Progress::Stage::SERIALIZING) &&
                        progress.units >= 2) {
                        conversion_entered = true;
                        std::fprintf(stderr, "WRITE470_%s units=%llu\n", converting ? "CONVERTING" : "SERIALIZING",
                                     static_cast<unsigned long long>(progress.units));
                        std::fflush(stderr);
                        test_deadline = test_path.find("deadline") != std::string::npos;
                        while (!test_deadline && !cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        std::fprintf(stderr, "WRITE470_RETURN units=%llu error=Cancelled\n",
                                     static_cast<unsigned long long>(progress.units));
                        std::fflush(stderr);
                    }
                }
                if (test_path.find(".test-live-noncooperative") != std::string::npos) {
                    std::fprintf(stderr, "LIVE_NONCOOPERATIVE\n");
                    std::fflush(stderr);
                    if (std::getenv("ENGRAVE_MCP_TEST_OWNER_UNTIL_WRITE_FAILURE")) {
                        std::unique_lock lock(publication_gate.mutex);
                        publication_gate.changed.wait(lock, [] { return publication_gate.closing.load(); });
                        std::fprintf(stderr, "OWNER_OBSERVED_WRITE_FAILURE\n");
                        std::fflush(stderr);
                    } else if (const auto *barrier = std::getenv("ENGRAVE_MCP_TEST_NONCOOPERATIVE_BARRIER"); barrier && *barrier) {
                        const auto barrier_path = std::filesystem::u8path(barrier);
                        while (!std::filesystem::exists(barrier_path)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    } else if (const auto *delay = std::getenv("ENGRAVE_MCP_TEST_NONCOOPERATIVE_MS")) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoul(delay)));
                    } else {
                        std::this_thread::sleep_for(std::chrono::seconds(30));
                    }
                }
                if (test_path.find(".test-live-cancel") != std::string::npos &&
                    ++cancellation_polls == (request.kind == ExportKind::RENDER ? 2 : 8)) {
                    std::fprintf(stderr, "LIVE_CANCEL_POLL\n");
                    std::fflush(stderr);
                    while (!cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            return test_deadline || cancelled() || ExportTiming::Clock::now() - submitted >= limits.deadline;
        };
        if (request.kind == ExportKind::SCORE) {
            auto start = ExportTiming::Clock::now();
            auto options = request.score_options;
            options.capture = capture;
            auto output = document.export_score_with_warnings(options);
            if (!output && output.error().code == engrave::ErrorCode::UNSUPPORTED && options.format == engrave::Format::MIDI) {
                if (auto ok = check(); !ok) return fail_export(std::move(ok.error()));
                options.capture = {};
                output = document.export_score_with_warnings(options);
            }
            if (!output) return fail_export(api_error(output.error()));
            timing.add("engine_call", start);
            warnings = std::move(output->warnings);
            const char *mime = "audio/midi";
            switch (options.format) {
                case engrave::Format::MSCX: mime = "application/vnd.musescore.mscx+xml"; break;
                case engrave::Format::MSCZ: mime = "application/vnd.musescore"; break;
                case engrave::Format::MUSIC_XML: mime = "application/vnd.recordare.musicxml+xml"; break;
                case engrave::Format::MXL: mime = "application/vnd.recordare.musicxml"; break;
                default: break;
            }
            if (auto ok = write("output", mime, output->bytes); !ok) return fail_export(std::move(ok.error()));
        } else if (request.kind == ExportKind::POSITIONS) {
            auto start = ExportTiming::Clock::now();
            auto positions = document.positions(request.position_kind, request.position_score, capture);
            if (!positions) return fail_export(api_error(positions.error()));
            timing.add("engine_call", start);
            start = ExportTiming::Clock::now();
            auto bytes = position_json(*positions, capture);
            if (!bytes) return fail_export(std::move(bytes.error()));
            timing.add("encode_write", start);
            if (auto ok = write("output", "application/json", *bytes); !ok) return fail_export(std::move(ok.error()));
        } else if (request.kind == ExportKind::RENDER) {
            auto start = ExportTiming::Clock::now();
            auto count_res = document.page_count(request.render_options.score);
            if (!count_res) return fail_export(api_error(count_res.error()));
            const auto count = *count_res;
            auto options = request.render_options;
            options.cancelled = capture.cancelled;
            const auto last = options.last_page < 0 ? count - 1 : options.last_page;
            if (options.first_page >= count || last >= count) return fail_export(invalid_error("render page range is outside the score"));
            if (static_cast<i64>(last) - options.first_page + 1 > 4096) {
                return fail_export(HostError{"ResourceLimit", "too many output pages"});
            }
            timing.add("engine_call", start);
            if (options.format == engrave::RenderFormat::PDF) {
                if (auto ok = check(); !ok) return fail_export(std::move(ok.error()));
                start = ExportTiming::Clock::now();
                auto output = document.render(options);
                if (!output) return fail_export(api_error(output.error()));
                timing.add("engine_call", start);
                if (auto ok = write("output", "application/pdf", output->document); !ok) return fail_export(std::move(ok.error()));
            } else {
                for (auto page = options.first_page; page <= last; ++page) {
                    if (auto ok = check(); !ok) return fail_export(std::move(ok.error()));
                    start = ExportTiming::Clock::now();
                    options.first_page = options.last_page = page;
                    auto output = document.render(options);
                    if (!output) return fail_export(api_error(output.error()));
                    timing.add("engine_call", start);
                    if (output->pages.size() != 1) {
                        return fail_export(HostError{"EngineFailure", "renderer returned invalid page count"});
                    }
                    const bool png = options.format == engrave::RenderFormat::PNG;
                    std::ostringstream name;
                    name << "page-" << std::setfill('0') << std::setw(4) << page + 1 << (png ? ".png" : ".svg");
                    if (auto ok = write(name.str(), png ? "image/png" : "image/svg+xml", output->pages[0]); !ok) {
                        return fail_export(std::move(ok.error()));
                    }
                }
            }
        } else if (request.kind == ExportKind::AUDIO) {
            auto start = ExportTiming::Clock::now();
            auto options_res = effective_mix(document, request, runtime);
            if (!options_res) return fail_export(std::move(options_res.error()));
            auto options = std::move(*options_res);
            std::optional<HostError> sink_error;
            options.cancelled = [&] { return sink_error.has_value() || capture.cancelled(); };
            const auto rate = runtime.audio_sample_rate;
            if (options.sample_rate && options.sample_rate != rate) {
                return fail_export(HostError{"Unsupported", "audio sample rate differs from the engine rate"});
            }
            // The engine derives playback duration and checks its global projection
            // after the cancellable model build. Do not ask outline() for it:
            // outline also scans lyrics/metadata without cancellation. Per-request
            // byte quotas below are enforced by the encoder before any sink call.
            if (auto ok = check(); !ok) return fail_export(std::move(ok.error()));
            const auto limit = audio_file_budget(limits.output_bytes);
            options.max_bytes = limit;
            options.max_write_bytes = audio_write_budget(limit);
            const auto path = cleanup.stage / "output";
            std::ofstream out;
            u64 bytes = 0;
            auto audio = document.render_audio(options, [&](std::span<const std::byte> chunk) {
                if (sink_error) return;
                if (auto ok = check(); !ok) {
                    sink_error = std::move(ok.error());
                    return;
                }
                if (!out.is_open()) out.open(path, std::ios::binary | std::ios::trunc);
                if (chunk.size() > limit - bytes) {
                    sink_error = HostError{"ResourceLimit", "audio exceeds output byte limit"};
                    return;
                }
                out.write(reinterpret_cast<const char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
                if (!out) {
                    sink_error = HostError{"Io", "cannot write staged audio"};
                    return;
                }
                bytes += chunk.size();
            });
            out.close();
            if (sink_error) return fail_export(std::move(*sink_error));
            if (!audio) return fail_export(api_error(audio.error()));
            if (!out) return fail_export(HostError{"Io", "cannot close staged audio"});
            if (!bytes || bytes != audio->bytes || fs::file_size(path) != bytes) {
                return fail_export(HostError{"EngineFailure", "invalid audio byte count"});
            }
            const char *mime = "audio/wav";
            switch (options.format) {
                case engrave::AudioFormat::FLAC: mime = "audio/flac"; break;
                case engrave::AudioFormat::OPUS: mime = "audio/ogg"; break;
                case engrave::AudioFormat::MP3: mime = "audio/mpeg"; break;
                case engrave::AudioFormat::AAC: mime = "audio/aac"; break;
                default: break;
            }
            timing.add("engine_call", start);
            const auto destination = request.destination.directory ? cleanup.destination / "output" : cleanup.destination;
            auto dest_text = report_path_text(destination);
            if (!dest_text) return fail_export(std::move(dest_text.error()));
            if (dest_text->size() > 4096) {
                return fail_export(
                    HostError{"ResourceLimit", destination_message(request.destination.requested_path, "artifact path exceeds limit")});
            }
            staged.push_back({path, {mime, destination, bytes}});
        }
        // Preflight both complete and worst-case partial reports before any rename.
        Json all = make_array<Json>(), paths = make_array<Json>();
        for (const auto &file : staged) {
            if (auto ok = validate_report_path(file.artifact.path); !ok) return fail_export(std::move(ok.error()));
            json_push(all, encode(file.artifact));
            json_push(paths, detail::path_text(file.artifact.path));
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
             {{"code", "EngineFailure"}, {"message", std::string(k_diagnostic_limit, '\1')}, {"published_artifacts", std::move(paths)}}}};
        const auto report_start = ExportTiming::Clock::now();
        const auto report_bytes = std::max(export_report_bound(success), export_report_bound(partial));
        timing.add("report_preflight", report_start);
        usize report_limit = 16u * 1024u * 1024u;
        if (hooks) {
            if (const auto *limit = std::getenv("ENGRAVE_MCP_TEST_REPORT_LIMIT")) report_limit = std::stoull(limit);
            std::fprintf(stderr, "PUBLICATION_REPORT_BOUND %zu\n", report_bytes);
            std::fflush(stderr);
        }
        if (report_bytes > report_limit) {
            return fail_export(HostError{"ResourceLimit", "export artifact report exceeds transport budget"});
        }
        // All rendering and staging succeeds before any file becomes visible.
        if (auto ok = check(); !ok) return fail_export(std::move(ok.error()));
        // Atomically exclude watchdog termination before the first visible file.
        // Once committed, even slow publication must finish and report its subset.
        if (hooks) {
            if (const auto *barrier = std::getenv("ENGRAVE_MCP_TEST_ADMISSION_BARRIER")) {
                std::fprintf(stderr, "PUBLICATION_READY\n");
                std::fflush(stderr);
                while (!fs::exists(barrier)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (begin_publication && !begin_publication()) {
            return fail_export(HostError{"Cancelled", "watchdog claimed export before publication"});
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
                    if (hooks) {
                        if (const auto *after = std::getenv("ENGRAVE_MCP_TEST_THROW_AFTER_RENAME")) {
                            if (artifacts.size() == std::stoul(after)) throw std::runtime_error("Injected post-rename failure");
                        }
                    }
                });
            } catch (const std::exception &e) {
                return fail_export(HostError{"Io", e.what()});
            }
            timing.add("publish", start);
        }
        Json encoded = make_array<Json>();
        for (const auto &a : artifacts) json_push(encoded, encode(a));
        return Json{{"ok", true},
                    {"source_revision", std::to_string(document.revision())},
                    {"artifacts", std::move(encoded)},
                    {"export_warnings", encode(warnings)}};
    } catch (const std::system_error &e) {
        HostError error{"Io", destination_message(request.destination.requested_path, e.what())};
        for (const auto &a : artifacts) error.published_artifacts.push_back(detail::path_text(a.path));
        return std::unexpected(std::move(error));
    } catch (const std::exception &e) {
        std::string message = e.what();
        message.resize(utf8_prefix(message, k_diagnostic_limit));
        HostError error{"EngineFailure", std::move(message)};
        for (const auto &a : artifacts) error.published_artifacts.push_back(detail::path_text(a.path));
        return std::unexpected(std::move(error));
    }
}

} // namespace music::api
