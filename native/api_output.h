#pragma once
#include "api_json.h"
#include "runtime_limits.h"
#include <filesystem>
namespace music::v2 {
struct ExportTiming;
enum class ExportKind { Score, Render, Audio, Positions };
struct Destination {
    std::filesystem::path path;
    bool directory = false;
    std::string requested_path;
};
struct ExportRequest {
    std::string document;
    std::optional<std::uint64_t> expected_revision;
    ExportKind kind = ExportKind::Score;
    Destination destination;
    engrave::ExportOptions score_options;
    engrave::RenderOptions render_options;
    engrave::AudioOptions audio_options;
    // Score, render and audio selections live in their public options above.
    // Positions use Document::positions(position_kind, position_score).
    engrave::ScoreSel position_score;
    engrave::PositionKind position_kind = engrave::PositionKind::Measure;
};
struct Artifact {
    std::string mime;
    std::filesystem::path path;
    std::uint64_t size = 0;
};
// Strict full decode, including option/format and destination compatibility.
Result<ExportRequest> decode_export(const Json &);
Json encode(const Artifact &);
Result<Json> live_export(engrave::Document &, const ExportRequest &, const ExportLimits &, const std::function<bool()> &cancelled,
                         std::chrono::steady_clock::time_point submitted, ExportTiming &, const engrave::EngineOptions &runtime = {},
                         const std::function<bool()> &begin_publication = {}, std::vector<std::string> *visible_paths = nullptr);
} // namespace music::v2
