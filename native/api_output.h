#pragma once
#include "api_json.h"
#include "runtime_snapshot.h"
#include <filesystem>
namespace music::v2 {
enum class ExportKind { Score, Render, Audio, Positions };
enum class ExportAction { Run, Start };
struct Destination { std::filesystem::path path; bool directory=false; };
struct ExportRequest {
    std::string document;
    std::optional<std::uint64_t> expected_revision;
    ExportAction action=ExportAction::Run;
    ExportKind kind=ExportKind::Score;
    Destination destination;
    engrave::ExportOptions score_options;
    engrave::RenderOptions render_options;
    engrave::AudioOptions audio_options;
    // Score, render and audio selections live in their public options above.
    // Positions use Document::positions(position_kind, position_score).
    engrave::ScoreSel position_score;
    engrave::PositionKind position_kind=engrave::PositionKind::Measure;
};
struct Artifact { std::string mime; std::filesystem::path path; std::uint64_t size=0; };
// Strict full decode, including option/format and destination compatibility.
Result<ExportRequest> decode_export(const Json&);
Json encode(const Artifact&);
// Captured on owner thread, immutable thereafter. No live Document or Engine.
// Audio mix contains defaults overlaid exactly once before snapshot capture.
struct ExportSnapshot {
    std::uint64_t source_revision=0;
    ExportRequest request;
    RuntimeSnapshot runtime;
    std::vector<std::byte> mscz;
    std::vector<std::byte> captured_score_bytes;
    std::vector<std::byte> captured_positions_json;
    engrave::AudioOptions effective_audio;
    double duration_seconds=0;
};
Result<ExportSnapshot> capture_export(engrave::Document&, RuntimeSnapshot, const ExportRequest&);
}
