#include "api_output.h"
#include "api_json_internal.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace music::v2 {
namespace {
using detail::invalid;

template<class T> void assign_if(const Json& j, const char* key, T& out) {
    if (j.contains(key)) out = j.at(key).get<T>();
}

template<class T> T engine_value(engrave::Result<T> result) {
    if (!result) throw detail::DecodeError{api_error(result.error())};
    return std::move(*result);
}

void check_capture_cancelled(const engrave::CaptureOptions& capture) {
    if (capture.cancelled && capture.cancelled())
        throw detail::DecodeError{HostError{"Cancelled", "export capture cancelled"}};
}

// Keep only one position object at a time in the JSON encoder. The engine's
// payload and this byte vector coexist and share the reserved snapshot budget.
std::vector<std::byte> position_json(const engrave::Positions& positions, const engrave::CaptureOptions& capture) {
    const auto limit = capture.max_bytes.value_or(std::numeric_limits<std::uint64_t>::max());
    const auto payload = engrave::position_payload_bytes(positions);
    if (payload > limit) throw detail::DecodeError{HostError{"ResourceLimit", "position payload exceeds snapshot budget"}};
    const auto available = limit - payload;
    std::vector<std::byte> result;
    auto append = [&](const std::string& part) {
        check_capture_cancelled(capture);
        if (part.size() > available - result.size())
            throw detail::DecodeError{HostError{"ResourceLimit", "position JSON exceeds snapshot budget"}};
        const auto* data = reinterpret_cast<const std::byte*>(part.data());
        result.insert(result.end(), data, data + part.size());
    };
    append("{\"page_size\":{\"width\":" + Json(positions.page_width).dump() +
           ",\"height\":" + Json(positions.page_height).dump() + "},\"elements\":[");
    bool first = true;
    for (const auto& e : positions.elements) {
        if (!first) append(","); first = false;
        append(Json{{"id", e.id}, {"x", e.x}, {"y", e.y}, {"sx", e.sx}, {"sy", e.sy}, {"page", e.page}}.dump());
    }
    append("],\"events\":["); first = true;
    for (const auto& e : positions.events) {
        if (!first) append(","); first = false;
        append(Json{{"elid", e.elid}, {"position", e.position}}.dump());
    }
    append("]}");
    return result;
}

engrave::Format score_format(const std::string& s) {
    if (s == "mscz") return engrave::Format::Mscz;
    if (s == "mscx") return engrave::Format::Mscx;
    if (s == "musicxml") return engrave::Format::MusicXml;
    if (s == "mxl") return engrave::Format::Mxl;
    if (s == "midi") return engrave::Format::Midi;
    invalid("unknown score format");
}
engrave::RenderFormat render_format(const std::string& s) {
    if (s == "pdf") return engrave::RenderFormat::Pdf;
    if (s == "png") return engrave::RenderFormat::Png;
    if (s == "svg") return engrave::RenderFormat::Svg;
    invalid("unknown render format");
}
engrave::AudioFormat audio_format(const std::string& s) {
    if (s == "wav") return engrave::AudioFormat::Wav;
    if (s == "flac") return engrave::AudioFormat::Flac;
    if (s == "opus") return engrave::AudioFormat::Opus;
    if (s == "mp3") return engrave::AudioFormat::Mp3;
    if (s == "aac") return engrave::AudioFormat::Aac;
    invalid("unknown audio format");
}

// The public capabilities exposed by HostApi use the host-owned copies. A
// worker's Engine makes its own copies, so resolve names/paths to indices now.
engrave::SoundFontSelection resolve_soundfont(const engrave::SoundFontSelection& selection,
                                              const RuntimeSnapshot& runtime) {
    if (std::holds_alternative<std::monostate>(selection)) return selection;
    if (const auto* index = std::get_if<std::int32_t>(&selection)) {
        if (*index < 0 || static_cast<std::size_t>(*index) >= runtime.options.soundfonts.size())
            invalid("SoundFont index is out of range");
        return *index;
    }
    const auto& name = std::get<std::string>(selection);
    for (std::size_t i = 0; i < runtime.options.soundfonts.size(); ++i) {
        const auto& file = runtime.options.soundfonts[i];
        if (detail::path_text(file) == name || detail::path_text(file.stem()) == name)
            return static_cast<std::int32_t>(i);
    }
    throw detail::DecodeError{HostError{"NotFound", "no captured SoundFont named '" + name + "'"}};
}

engrave::AudioOptions effective_mix(engrave::Document& document, const ExportRequest& request,
                                   const RuntimeSnapshot& runtime) {
    auto result = request.audio_options;
    const auto defaults = engine_value(document.audio_tracks(result.score));
    if (defaults.tracks.size() > 256)
        throw detail::DecodeError{HostError{"ResourceLimit", "audio track count exceeds 256"}};
    result.master_volume_db = result.master_volume_db.value_or(defaults.master.volume_db);
    result.master_balance = result.master_balance.value_or(defaults.master.balance);
    std::set<std::int32_t> seen;
    for (const auto& track : result.tracks) {
        if (!seen.insert(track.track).second) invalid("duplicate audio track");
        if (std::none_of(defaults.tracks.begin(), defaults.tracks.end(),
                         [&](const auto& t) { return t.track == track.track; }))
            invalid("audio track is out of range");
    }
    auto overrides = std::move(result.tracks);
    result.tracks.clear();
    result.tracks.reserve(defaults.tracks.size());
    for (const auto& track : defaults.tracks) {
        engrave::AudioTrackOptions effective;
        effective.track = track.track;
        effective.volume_db = track.mix.volume_db;
        effective.balance = track.mix.balance;
        effective.mute = track.mix.mute;
        effective.solo = track.mix.solo;
        auto found = std::find_if(overrides.begin(), overrides.end(),
                                  [&](const auto& t) { return t.track == track.track; });
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
}

Result<ExportRequest> decode_export(const Json& j) {
    try {
        detail::validate_tool("score_export", j);
        ExportRequest result;
        result.document = j.at("document").get<std::string>();
        if (j.contains("expected_revision")) {
            auto revision = decode_revision(j.at("expected_revision"));
            if (!revision) return std::unexpected(revision.error());
            result.expected_revision = *revision;
        }
        result.action = j.at("action") == "run" ? ExportAction::Run : ExportAction::Start;
        const auto& destination = j.at("destination");
        result.destination.directory = destination.at("kind") == "directory";
        result.destination.path = detail::path(destination.at("path"));
        if (result.destination.path.empty()) invalid("destination path is empty");
        const auto& options = j.at("options");
        const auto selection = options.contains("score") ? detail::score(options.at("score")) : engrave::ScoreSel{};
        const auto kind = j.at("kind").get<std::string>();
        if (kind == "score") {
            result.kind = ExportKind::Score;
            auto& o = result.score_options;
            o.score = selection; o.format = score_format(options.at("format").get<std::string>());
            assign_if(options, "midi_expand_repeats", o.midi_expand_repeats);
            assign_if(options, "midi_rpns", o.midi_rpns);
        } else if (kind == "render") {
            result.kind = ExportKind::Render;
            auto& o = result.render_options;
            o.score = selection; o.format = render_format(options.at("format").get<std::string>());
            assign_if(options, "first_page", o.first_page); assign_if(options, "last_page", o.last_page);
            assign_if(options, "dpi", o.dpi); assign_if(options, "background", o.background);
            assign_if(options, "transparent", o.transparent);
            if (o.last_page != -1 && o.last_page < o.first_page) invalid("last_page precedes first_page");
        } else if (kind == "audio") {
            result.kind = ExportKind::Audio;
            auto& o = result.audio_options;
            o.score = selection; o.format = audio_format(options.at("format").get<std::string>());
            assign_if(options, "bitrate_kbps", o.bitrate_kbps); assign_if(options, "bit_depth", o.bit_depth);
            assign_if(options, "use_repeats", o.use_repeats); assign_if(options, "reverb", o.reverb);
            assign_if(options, "reverb_send", o.reverb_send); assign_if(options, "sample_rate", o.sample_rate);
            if (options.contains("master_volume_db")) o.master_volume_db = options.at("master_volume_db").get<double>();
            if (options.contains("master_balance")) o.master_balance = options.at("master_balance").get<double>();
            if (options.contains("tracks")) for (const auto& t : options.at("tracks")) {
                engrave::AudioTrackOptions track; track.track = t.at("track").get<std::int32_t>();
                if (t.contains("volume_db")) track.volume_db = t.at("volume_db").get<double>();
                if (t.contains("balance")) track.balance = t.at("balance").get<double>();
                if (t.contains("mute")) track.mute = t.at("mute").get<bool>();
                if (t.contains("solo")) track.solo = t.at("solo").get<bool>();
                if (t.contains("preset")) track.preset = t.at("preset").get<std::string>();
                if (t.contains("soundfont") && !t.at("soundfont").is_null()) {
                    const auto& sf = t.at("soundfont");
                    track.soundfont = sf.is_string() ? engrave::SoundFontSelection(sf.get<std::string>())
                                                     : engrave::SoundFontSelection(sf.get<std::int32_t>());
                }
                o.tracks.push_back(std::move(track));
            }
        } else {
            result.kind = ExportKind::Positions;
            result.position_score = selection;
            result.position_kind = options.at("position_kind") == "measure" ? engrave::PositionKind::Measure : engrave::PositionKind::Segment;
        }
        return result;
    } catch (const detail::DecodeError& e) { return std::unexpected(e.error); }
      catch (const std::exception& e) { return std::unexpected(HostError{"InvalidArgument", e.what()}); }
}

Json encode(const Artifact& artifact) {
    return {{"mime", artifact.mime}, {"path", detail::path_text(artifact.path)}, {"size", std::to_string(artifact.size)}};
}

Result<ExportSnapshot> capture_export(engrave::Document& document, RuntimeSnapshot runtime,
                                      const ExportRequest& request, const engrave::CaptureOptions& capture) {
    try {
        ExportSnapshot snapshot;
        snapshot.source_revision = document.revision();
        if (request.expected_revision && *request.expected_revision != snapshot.source_revision)
            return std::unexpected(HostError{"RevisionConflict", "export revision is no longer current", {}, request.document, snapshot.source_revision});
        snapshot.request = request;
        snapshot.runtime = std::move(runtime);
        snapshot.capture_budget = capture.max_bytes.value_or(std::numeric_limits<std::uint64_t>::max());
        check_capture_cancelled(capture);
        if (request.kind == ExportKind::Score) {
            auto options = request.score_options;
            options.capture = capture;
            auto result = document.export_score(options);
            if (!result && result.error().code == engrave::ErrorCode::Unsupported && options.format == engrave::Format::Midi) {
                check_capture_cancelled(capture);
                options.capture = {};
                snapshot.snapshot_budget_enforced = false;
                result = document.export_score(options);
            }
            snapshot.captured_score_bytes = engine_value(std::move(result));
            check_capture_cancelled(capture);
        } else if (request.kind == ExportKind::Positions) {
            auto positions = engine_value(document.positions(request.position_kind, request.position_score, capture));
            snapshot.captured_positions_json = position_json(positions, capture);
        } else {
            if (request.kind == ExportKind::Audio) {
                snapshot.effective_audio = effective_mix(document, request, snapshot.runtime);
                snapshot.duration_seconds = engine_value(document.outline(request.audio_options.score)).duration;
            } else {
                const auto pages = engine_value(document.page_count(request.render_options.score));
                if (request.render_options.first_page >= pages ||
                    (request.render_options.last_page != -1 && request.render_options.last_page >= pages))
                    invalid("render page range is outside the score");
            }
            engrave::ExportOptions options; options.format = engrave::Format::Mscz;
            options.capture = capture;
            snapshot.mscz = engine_value(document.export_score(options));
        }
        check_capture_cancelled(capture);
        return snapshot;
    } catch (const detail::DecodeError& e) { return std::unexpected(e.error); }
}
} // namespace music::v2
