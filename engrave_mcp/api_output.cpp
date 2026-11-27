#include "api_output.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include <engrave/testing/write470.h>
#include <glaze/json/write.hpp>

#include "api_json_internal.h"
#include "audio_budget.h"
#include "containers.h"
#include "export_timing.h"
#include "publication.h"
#include "publication_gate.h"
#include "terminal_diagnostic.h"
#include "test_hooks.h"
#include "tool_result.h"
#include "utf8_text.h"

namespace music::api {

namespace detail {
// Alphabetical member order matches Json (gtl::btree_map-backed) key order;
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

std::optional<engrave::Format> score_format(std::string_view s) {
    if (s == "mscz") return engrave::Format::MSCZ;
    if (s == "mscx") return engrave::Format::MSCX;
    if (s == "musicxml") return engrave::Format::MUSIC_XML;
    if (s == "mxl") return engrave::Format::MXL;
    if (s == "midi") return engrave::Format::MIDI;
    return std::nullopt;
}

std::optional<engrave::RenderFormat> render_format(std::string_view s) {
    if (s == "pdf") return engrave::RenderFormat::PDF;
    if (s == "png") return engrave::RenderFormat::PNG;
    if (s == "svg") return engrave::RenderFormat::SVG;
    return std::nullopt;
}

std::optional<engrave::AudioFormat> audio_format(std::string_view s) {
    if (s == "wav") return engrave::AudioFormat::WAV;
    if (s == "flac") return engrave::AudioFormat::FLAC;
    if (s == "opus") return engrave::AudioFormat::OPUS;
    if (s == "mp3") return engrave::AudioFormat::MP3;
    return std::nullopt;
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
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec).lexically_normal();
    for (usize i = 0; i < runtime.soundfonts.size(); ++i) {
        const auto &font = runtime.soundfonts[i];
        std::error_code source_ec;
        const auto source_abs = std::filesystem::absolute(font.source, source_ec).lexically_normal();
        if (path == font.source || normalized == font.source.lexically_normal() || (!ec && !source_ec && absolute == source_abs) ||
            path == font.copy) {
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
    HashSet<i32> seen;
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
    auto dest_path = [&]() -> Result<std::filesystem::path> {
        try {
            return detail::path(j.at("path"));
        } catch (const std::exception &e) {
            return std::unexpected(invalid_error(e.what()));
        }
    }();
    if (!dest_path) return destination_error(std::move(dest_path.error()));
    result.destination.path = std::move(*dest_path);
    result.destination.requested_path = j.at("path").get<std::string>();
    if (result.destination.path.empty()) return destination_error(invalid_error("destination path is empty"));
    const auto options = json_value<Json>(j, "options", make_object<Json>());
    const auto render_fmt = render_format(format);
    const auto audio_fmt = audio_format(format);
    for (const auto &[key, value] : options.get_object()) {
        bool valid = false;
        if (key == "midi_expand_repeats" || key == "midi_rpns") {
            valid = format == "midi";
        } else if (key == "native_version") {
            valid = format == "mscx" || format == "mscz";
        } else if (key == "dpi" || key == "transparent") {
            valid = format == "png";
        } else if (key == "first_page" || key == "last_page" || key == "background") {
            valid = render_fmt.has_value();
        } else {
            valid = audio_fmt.has_value();
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
    if (auto score_fmt = score_format(format)) {
        result.kind = ExportKind::SCORE;
        auto &o = result.score_options;
        o.score = selection;
        o.format = *score_fmt;
        assign_if(options, "midi_expand_repeats", o.midi_expand_repeats);
        assign_if(options, "midi_rpns", o.midi_rpns);
        assign_if(options, "native_version", o.native_version);
    } else if (render_fmt) {
        result.kind = ExportKind::RENDER;
        auto &o = result.render_options;
        o.score = selection;
        o.format = *render_fmt;
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
    } else if (audio_fmt) {
        result.kind = ExportKind::AUDIO;
        auto &o = result.audio_options;
        o.score = selection;
        o.format = *audio_fmt;
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
}

Json encode(const Artifact &artifact) {
    return {{"mime", artifact.mime}, {"path", detail::path_text(artifact.path)}, {"size", artifact.size}};
}

namespace {
namespace fs = std::filesystem;

// The PNG renderer deflates a multi-page call's pages in parallel; the batch
// bounds how many encoded pages are held before write() checks the budget.
constexpr i32 k_png_batch_pages = 16;

struct Staged {
    fs::path source;
    Artifact artifact;
};

struct StagingCleanup {
    fs::path stage;
    bool hooks = false;

    explicit StagingCleanup(bool hooks) : hooks(hooks) {}

    StagingCleanup(const StagingCleanup &) = delete;
    StagingCleanup &operator=(const StagingCleanup &) = delete;

    ~StagingCleanup() {
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
};

// One export's destination, staging, cancellation polls and publication. Only
// one thread uses it at a time: the owner, then (for audio) the audio lane.
struct ExportContext {
    ExportContext(const ExportRequest &request, const ExportLimits &limits, ExportTiming::Clock::time_point submitted)
        : request(request), limits(limits), submitted(submitted), hooks(test_hooks_enabled()), cleanup(hooks) {}

    ExportRequest request;
    ExportLimits limits;
    ExportTiming::Clock::time_point submitted;
    bool hooks;
    fs::path destination;
    StagingCleanup cleanup;
    std::vector<Staged> staged;
    std::vector<Artifact> artifacts;
    std::vector<engrave::ExportWarning> warnings;
    u64 total = 0;
    bool test_deadline = false;
    bool conversion_entered = false;
    bool audio_running = false;
    bool audio_held = false;
    i32 cancellation_polls = 0;

    std::unexpected<HostError> fail(HostError error, FnRef<bool()> cancelled) {
        if (hooks && !artifacts.empty() && std::getenv("ENGRAVE_MCP_TEST_THROW_ERROR_REPORT")) {
            throw std::runtime_error("Injected error report construction failure");
        }
        if (error.code == "Io") {
            error = with_destination(std::move(error), request.destination.requested_path);
        } else {
            error.message.resize(utf8_prefix(error.message, k_diagnostic_limit));
        }
        if (error.code == "Cancelled" && !cancelled()) error = HostError{"DeadlineExceeded", "export deadline exceeded"};
        return with_artifacts(std::move(error));
    }

    std::unexpected<HostError> with_artifacts(HostError error) const {
        for (const auto &a : artifacts) error.published_artifacts.push_back(detail::path_text(a.path));
        return std::unexpected(std::move(error));
    }

    std::unexpected<HostError> invalid_destination(std::string_view reason, FnRef<bool()> cancelled) {
        return fail(invalid_error(destination_message(request.destination.requested_path, reason)), cancelled);
    }

    Result<void> check(FnRef<bool()> cancelled) const {
        if (cancelled()) return std::unexpected(HostError{"Cancelled", "live export cancelled"});
        if (test_deadline || ExportTiming::Clock::now() - submitted >= limits.deadline) {
            return std::unexpected(HostError{"DeadlineExceeded", "export deadline exceeded"});
        }
        return {};
    }

    HostError invalid_resolved_error() const {
        // Preserve the well-formed JSON request, including its separators;
        // never convert the unrepresentable resolved path for diagnostics.
        return invalid_error(
            destination_message(request.destination.requested_path,
                                "resolves to a path that is not valid UTF-8; use a destination with a UTF-8 resolved path"));
    }

    Result<std::string> report_path_text(const fs::path &path) const {
        try {
            return detail::path_text(path);
        } catch (const std::exception &) {
            // Windows generic_u8string() throws for unpaired UTF-16
            // surrogates before the JSON UTF-8 validator can run.
            return std::unexpected(invalid_resolved_error());
        }
    }

    Result<void> validate_report_path(const fs::path &path) const {
        auto text = report_path_text(path);
        if (!text) return std::unexpected(std::move(text.error()));
        if (!valid_utf8(*text)) return std::unexpected(invalid_resolved_error());
        return {};
    }

    // Validates the destination and creates the staging directory. Errors are
    // already passed through fail().
    Result<void> stage(FnRef<bool()> cancelled) {
        if (auto ok = check(cancelled); !ok) return fail(std::move(ok.error()), cancelled);
        if (auto ok = validate_output_path_syntax(request.destination.path); !ok) return invalid_destination(ok.error(), cancelled);
        // Resolve existing prefixes and check their encoding before creating directories.
        if (request.destination.directory) {
            const auto status = output_path_status(request.destination.path);
            if (!status) return invalid_destination(status.error(), cancelled);
            if (fs::exists(*status) && !fs::is_directory(*status)) return invalid_destination("destination must be a directory", cancelled);
            destination = fs::weakly_canonical(fs::absolute(request.destination.path));
        } else {
            auto path = normalize_output_path(request.destination.path);
            if (!path) return invalid_destination(path.error(), cancelled);
            destination = std::move(*path);
        }
        if (auto ok = validate_report_path(destination); !ok) return fail(std::move(ok.error()), cancelled);
        if (auto ok = validate_output_path_syntax(destination); !ok) return invalid_destination(ok.error(), cancelled);
        if (!request.destination.directory) {
            const auto status = output_path_status(destination);
            if (!status) return invalid_destination(status.error(), cancelled);
            if (fs::is_directory(*status)) return invalid_destination("file destination is a directory", cancelled);
        }
        const auto directory = request.destination.directory ? destination : destination.parent_path();
        if (auto ok = require_output_directory(directory); !ok) return invalid_destination(ok.error(), cancelled);
        if (hooks && std::getenv("ENGRAVE_MCP_TEST_FAIL_STAGING")) {
            throw std::system_error(EIO, std::generic_category(), "Injected staging directory failure");
        }
        cleanup.stage = create_private_directory(directory);
        return {};
    }

    Result<void> write(const std::string &name, const std::string &mime, std::span<const std::byte> bytes, FnRef<bool()> cancelled,
                       ExportTiming &timing) {
        if (auto ok = check(cancelled); !ok) return std::unexpected(std::move(ok.error()));
        auto start = ExportTiming::Clock::now();
        if (bytes.empty()) return std::unexpected(HostError{"EngineFailure", "export produced an empty artifact"});
        if (bytes.size() > limits.output_bytes - total) {
            return std::unexpected(HostError{"ResourceLimit", "export exceeds output byte limit"});
        }
        const auto target = request.destination.directory ? destination / name : destination;
        auto dest_text = report_path_text(target);
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
        staged.push_back({path, {mime, target, bytes.size()}});
        timing.add("encode_write", start);
        return {};
    }

    // The engine's cancellation callback, including the path-keyed test holds.
    bool poll(FnRef<bool()> cancelled) {
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
                    std::fprintf(stderr, "WRITE470_RETURN units=%llu error=Cancelled\n", static_cast<unsigned long long>(progress.units));
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
            // Holds a running audio render (never its owner-thread setup) until
            // "<destination>.release" exists or the export is cancelled.
            if (audio_running && !audio_held && test_path.find(".test-audio-hold") != std::string::npos) {
                audio_held = true;
                std::fprintf(stderr, "AUDIO_LANE_HOLD %s\n", detail::path_text(destination.filename()).c_str());
                std::fflush(stderr);
                auto release = destination;
                release += ".release";
                std::error_code ec;
                while (!fs::exists(release, ec) && !cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return test_deadline || cancelled() || ExportTiming::Clock::now() - submitted >= limits.deadline;
    }

    Result<Json> publish(u64 source_revision, FnRef<bool()> cancelled, ExportTiming &timing, BeginPublication begin_publication,
                         std::vector<std::string> *visible_paths) {
        // Preflight both complete and worst-case partial reports before any rename.
        Json all = make_array<Json>(), paths = make_array<Json>();
        for (const auto &file : staged) {
            if (auto ok = validate_report_path(file.artifact.path); !ok) return fail(std::move(ok.error()), cancelled);
            json_push(all, encode(file.artifact));
            json_push(paths, detail::path_text(file.artifact.path));
        }
        const auto revision = std::to_string(source_revision);
        Json success{{"ok", true}, {"source_revision", revision}, {"artifacts", std::move(all)}, {"export_warnings", encode(warnings)}};
        // Export exceptions below are bounded to our 16 KiB diagnostic cap,
        // not the schema's larger message ceiling. Control bytes maximize JSON
        // escaping, including the duplicated text form.
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
            return fail(HostError{"ResourceLimit", "export artifact report exceeds transport budget"}, cancelled);
        }
        // All rendering and staging succeeds before any file becomes visible.
        if (auto ok = check(cancelled); !ok) return fail(std::move(ok.error()), cancelled);
        // Atomically exclude watchdog termination before the first visible file.
        // Once committed, even slow publication must finish and report its subset.
        if (hooks) {
            if (const auto *barrier = std::getenv("ENGRAVE_MCP_TEST_ADMISSION_BARRIER")) {
                std::fprintf(stderr, "PUBLICATION_READY\n");
                std::fflush(stderr);
                while (!fs::exists(barrier)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (begin_publication) {
            const auto admitted =
                begin_publication([&] { return test_deadline || ExportTiming::Clock::now() - submitted >= limits.deadline; });
            if (admitted == PublicationAdmission::CANCELLED) {
                // Gate rejection is cancellation/closing/watchdog, never deadline expiry;
                // bypass fail()'s !cancelled() -> DeadlineExceeded rewrite in case
                // cancel_before_publication arrived ahead of cancel->emit().
                return with_artifacts(HostError{"Cancelled", "export cancelled before publication"});
            }
            if (admitted == PublicationAdmission::DEADLINE_EXCEEDED) {
                return with_artifacts(HostError{"DeadlineExceeded", "export deadline exceeded"});
            }
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
                return fail(HostError{"Io", e.what()}, cancelled);
            }
            timing.add("publish", start);
        }
        Json encoded = make_array<Json>();
        for (const auto &a : artifacts) json_push(encoded, encode(a));
        return Json{{"ok", true}, {"source_revision", revision}, {"artifacts", std::move(encoded)}, {"export_warnings", encode(warnings)}};
    }
};

// Standard exceptions become errors that still list every visible artifact.
template <class T, class Fn>
Result<T> guarded(ExportContext &context, Fn &&fn) {
    try {
        return std::forward<Fn>(fn)();
    } catch (const std::system_error &e) {
        return context.with_artifacts(HostError{"Io", destination_message(context.request.destination.requested_path, e.what())});
    } catch (const std::exception &e) {
        std::string message = e.what();
        message.resize(utf8_prefix(message, k_diagnostic_limit));
        return context.with_artifacts(HostError{"EngineFailure", std::move(message)});
    }
}

const char *audio_mime(engrave::AudioFormat format) {
    switch (format) {
        case engrave::AudioFormat::WAV: return "audio/wav";
        case engrave::AudioFormat::FLAC: return "audio/flac";
        case engrave::AudioFormat::OPUS: return "audio/ogg";
        case engrave::AudioFormat::MP3: return "audio/mpeg";
    }
    assert(false);
    std::unreachable();
}
} // namespace

struct PreparedAudioExport::State {
    State(const ExportRequest &request, const ExportLimits &limits, ExportTiming::Clock::time_point submitted)
        : context(request, limits, submitted) {}

    ExportContext context;
    u64 limit = 0;
    u64 source_revision = 0;
    const char *mime = "";
    // Last member: the engine render is gone before staging cleanup runs.
    std::optional<engrave::PreparedAudio> audio;
};

PreparedAudioExport::PreparedAudioExport(std::unique_ptr<State> state) noexcept : state(std::move(state)) {}

PreparedAudioExport::PreparedAudioExport(PreparedAudioExport &&) noexcept = default;
PreparedAudioExport &PreparedAudioExport::operator=(PreparedAudioExport &&) noexcept = default;
PreparedAudioExport::~PreparedAudioExport() = default;

Result<PreparedAudioExport> prepare_audio_export(engrave::Document &document, const ExportRequest &request, const ExportLimits &limits,
                                                 FnRef<bool()> cancelled, std::chrono::steady_clock::time_point submitted,
                                                 ExportTiming &timing, const RuntimeOptions &runtime) {
    if (request.kind != ExportKind::AUDIO) return std::unexpected(HostError{"EngineFailure", "not an audio export"});
    auto state = std::make_unique<PreparedAudioExport::State>(request, limits, submitted);
    auto &context = state->context;
    return guarded<PreparedAudioExport>(context, [&]() -> Result<PreparedAudioExport> {
        if (auto ok = context.stage(cancelled); !ok) return std::unexpected(std::move(ok.error()));
        const auto start = ExportTiming::Clock::now();
        auto options_res = effective_mix(document, context.request, runtime);
        if (!options_res) return context.fail(std::move(options_res.error()), cancelled);
        auto options = std::move(*options_res);
        if (options.sample_rate && options.sample_rate != runtime.audio_sample_rate) {
            return context.fail(HostError{"Unsupported", "audio sample rate differs from the engine rate"}, cancelled);
        }
        // The engine derives playback duration and checks its global projection
        // after the cancellable model build. Do not ask outline() for it:
        // outline also scans lyrics/metadata without cancellation. Per-request
        // byte quotas below are enforced by the encoder as it writes.
        if (auto ok = context.check(cancelled); !ok) return context.fail(std::move(ok.error()), cancelled);
        state->limit = audio_file_budget(limits.output_bytes);
        options.max_bytes = state->limit;
        options.max_write_bytes = audio_write_budget(state->limit);
        // prepare_audio polls this during setup only and keeps no copy.
        options.cancelled = [&] { return context.poll(cancelled); };
        auto prepared = document.prepare_audio(options);
        if (!prepared) return context.fail(api_error(prepared.error()), cancelled);
        state->source_revision = prepared->source_revision();
        state->mime = audio_mime(options.format);
        state->audio.emplace(std::move(*prepared));
        timing.add("engine_call", start);
        return PreparedAudioExport(std::move(state));
    });
}

Result<Json> finish_audio_export(PreparedAudioExport prepared, FnRef<bool()> cancelled, ExportTiming &timing,
                                 BeginPublication begin_publication, std::vector<std::string> *visible_paths) {
    auto state = std::move(prepared.state);
    if (!state || !state->audio) return std::unexpected(HostError{"EngineFailure", "audio export is not prepared"});
    auto &context = state->context;
    return guarded<Json>(context, [&]() -> Result<Json> {
        if (auto ok = context.check(cancelled); !ok) return context.fail(std::move(ok.error()), cancelled);
        const auto start = ExportTiming::Clock::now();
        const auto path = context.cleanup.stage / "output";
        context.audio_running = true;
        auto audio = state->audio->run(path, [&] { return context.poll(cancelled); });
        context.audio_running = false;
        // Frees the Engine's audio render before any publication or report.
        state->audio.reset();
        if (!audio) return context.fail(api_error(audio.error()), cancelled);
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec || !audio->bytes || audio->bytes > state->limit || size != audio->bytes) {
            return context.fail(HostError{"EngineFailure", "invalid audio byte count"}, cancelled);
        }
        timing.add("engine_call", start);
        auto dest_text = context.report_path_text(context.destination);
        if (!dest_text) return context.fail(std::move(dest_text.error()), cancelled);
        if (dest_text->size() > 4096) {
            return context.fail(
                HostError{"ResourceLimit", destination_message(context.request.destination.requested_path, "artifact path exceeds limit")},
                cancelled);
        }
        context.staged.push_back({path, {state->mime, context.destination, audio->bytes}});
        return context.publish(state->source_revision, cancelled, timing, begin_publication, visible_paths);
    });
}

Result<Json> live_export(engrave::Document &document, const ExportRequest &request, const ExportLimits &limits, FnRef<bool()> cancelled,
                         std::chrono::steady_clock::time_point submitted, ExportTiming &timing, const RuntimeOptions &runtime,
                         BeginPublication begin_publication, std::vector<std::string> *visible_paths) {
    if (request.kind == ExportKind::AUDIO) {
        auto prepared = prepare_audio_export(document, request, limits, cancelled, submitted, timing, runtime);
        if (!prepared) return std::unexpected(std::move(prepared.error()));
        return finish_audio_export(std::move(*prepared), cancelled, timing, begin_publication, visible_paths);
    }
    ExportContext context(request, limits, submitted);
    return guarded<Json>(context, [&]() -> Result<Json> {
        if (auto ok = context.stage(cancelled); !ok) return std::unexpected(std::move(ok.error()));
        auto fail = [&](HostError error) { return context.fail(std::move(error), cancelled); };
        auto write = [&](const std::string &name, const std::string &mime, std::span<const std::byte> bytes) {
            return context.write(name, mime, bytes, cancelled, timing);
        };
        engrave::CaptureOptions capture;
        capture.max_bytes = limits.output_bytes;
        capture.cancelled = [&] { return context.poll(cancelled); };
        if (request.kind == ExportKind::SCORE) {
            auto start = ExportTiming::Clock::now();
            auto options = request.score_options;
            options.capture = capture;
            auto output = document.export_score_with_warnings(options);
            if (!output && output.error().code == engrave::ErrorCode::UNSUPPORTED && options.format == engrave::Format::MIDI) {
                if (auto ok = context.check(cancelled); !ok) return fail(std::move(ok.error()));
                options.capture = {};
                output = document.export_score_with_warnings(options);
            }
            if (!output) return fail(api_error(output.error()));
            timing.add("engine_call", start);
            context.warnings = std::move(output->warnings);
            const char *mime = "audio/midi";
            switch (options.format) {
                case engrave::Format::MSCX: mime = "application/vnd.musescore.mscx+xml"; break;
                case engrave::Format::MSCZ: mime = "application/vnd.musescore"; break;
                case engrave::Format::MUSIC_XML: mime = "application/vnd.recordare.musicxml+xml"; break;
                case engrave::Format::MXL: mime = "application/vnd.recordare.musicxml"; break;
                default: break;
            }
            if (auto ok = write("output", mime, output->bytes); !ok) return fail(std::move(ok.error()));
        } else if (request.kind == ExportKind::POSITIONS) {
            auto start = ExportTiming::Clock::now();
            auto positions = document.positions(request.position_kind, request.position_score, capture);
            if (!positions) return fail(api_error(positions.error()));
            timing.add("engine_call", start);
            start = ExportTiming::Clock::now();
            auto bytes = position_json(*positions, capture);
            if (!bytes) return fail(std::move(bytes.error()));
            timing.add("encode_write", start);
            if (auto ok = write("output", "application/json", *bytes); !ok) return fail(std::move(ok.error()));
        } else {
            auto start = ExportTiming::Clock::now();
            auto count_res = document.page_count(request.render_options.score);
            if (!count_res) return fail(api_error(count_res.error()));
            const auto count = *count_res;
            auto options = request.render_options;
            options.cancelled = capture.cancelled;
            const auto last = options.last_page < 0 ? count - 1 : options.last_page;
            if (options.first_page >= count || last >= count) return fail(invalid_error("render page range is outside the score"));
            if (static_cast<i64>(last) - options.first_page + 1 > 4096) return fail(HostError{"ResourceLimit", "too many output pages"});
            timing.add("engine_call", start);
            if (options.format == engrave::RenderFormat::PDF) {
                if (auto ok = context.check(cancelled); !ok) return fail(std::move(ok.error()));
                start = ExportTiming::Clock::now();
                options.max_bytes = limits.output_bytes - context.total;
                auto output = document.render(options);
                if (!output) return fail(api_error(output.error()));
                timing.add("engine_call", start);
                if (auto ok = write("output", "application/pdf", output->document); !ok) return fail(std::move(ok.error()));
            } else {
                const bool png = options.format == engrave::RenderFormat::PNG;
                const i32 batch = png ? k_png_batch_pages : 1;
                for (auto page = options.first_page; page <= last; page += batch) {
                    if (auto ok = context.check(cancelled); !ok) return fail(std::move(ok.error()));
                    start = ExportTiming::Clock::now();
                    options.first_page = page;
                    options.last_page = std::min(last, page + batch - 1);
                    options.max_bytes = limits.output_bytes - context.total;
                    auto output = document.render(options);
                    if (!output) return fail(api_error(output.error()));
                    timing.add("engine_call", start);
                    if (output->pages.size() != static_cast<usize>(options.last_page - page + 1)) {
                        return fail(HostError{"EngineFailure", "renderer returned invalid page count"});
                    }
                    for (usize i = 0; i < output->pages.size(); ++i) {
                        std::ostringstream name;
                        name << "page-" << std::setfill('0') << std::setw(4) << page + static_cast<i32>(i) + 1 << (png ? ".png" : ".svg");
                        if (auto ok = write(name.str(), png ? "image/png" : "image/svg+xml", output->pages[i]); !ok) {
                            return fail(std::move(ok.error()));
                        }
                        std::vector<std::byte>().swap(output->pages[i]);
                    }
                }
            }
        }
        return context.publish(document.revision(), cancelled, timing, begin_publication, visible_paths);
    });
}

} // namespace music::api
