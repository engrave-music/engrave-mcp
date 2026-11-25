#pragma once

#include <filesystem>
#include <memory>

#include "api_json.h"
#include "runtime_limits.h"
#include "types.h"

namespace music::api {

struct ExportTiming;
enum class ExportKind { SCORE, RENDER, AUDIO, POSITIONS };

struct Destination {
    std::filesystem::path path;
    bool directory = false;
    std::string requested_path;
};

struct ExportRequest {
    std::string document;
    std::optional<std::uint64_t> expected_revision;
    ExportKind kind = ExportKind::SCORE;
    Destination destination;
    engrave::ExportOptions score_options;
    engrave::RenderOptions render_options;
    engrave::AudioOptions audio_options;
    // Score, render and audio selections live in their public options above.
    // Positions use Document::positions(position_kind, position_score).
    engrave::ScoreSel position_score;
    engrave::PositionKind position_kind = engrave::PositionKind::MEASURE;
};

struct Artifact {
    std::string mime;
    std::filesystem::path path;
    std::uint64_t size = 0;
};

struct SoundFontAsset {
    std::filesystem::path source;
    std::filesystem::path copy;
};

struct RuntimeOptions {
    int audio_sample_rate = engrave::EngineOptions{}.audio_sample_rate;
    std::vector<std::filesystem::path> fonts;
    std::vector<SoundFontAsset> soundfonts;
};

// Strict full decode, including option/format and destination compatibility.
Result<ExportRequest> decode_export(const Json &);
Json encode(const Artifact &);

enum class PublicationAdmission { COMMITTED, CANCELLED, DEADLINE_EXCEEDED };
using BeginPublication = FnRef<PublicationAdmission(FnRef<bool()> deadline_expired)>;

// Audio requests run prepare_audio_export and finish_audio_export back to back.
Result<Json> live_export(engrave::Document &, const ExportRequest &, const ExportLimits &, FnRef<bool()> cancelled,
                         std::chrono::steady_clock::time_point submitted, ExportTiming &, const RuntimeOptions &runtime = {},
                         BeginPublication begin_publication = {}, std::vector<std::string> *visible_paths = nullptr);

// An audio export between its owner-thread setup and its render: the staging
// directory, the report inputs and an engrave::PreparedAudio, which holds the
// Engine's single audio render. It refers to no Document, so the owner may
// edit or close the source meanwhile. Destroy it before the Engine.
struct PreparedAudioExport {
    struct State;
    explicit PreparedAudioExport(std::unique_ptr<State>) noexcept;
    PreparedAudioExport(PreparedAudioExport &&) noexcept;
    PreparedAudioExport &operator=(PreparedAudioExport &&) noexcept;
    ~PreparedAudioExport();

    std::unique_ptr<State> state;
};

// Owner thread. Validates the destination and mix, creates the staging
// directory and calls Document::prepare_audio, whose revision becomes the
// report's source_revision.
Result<PreparedAudioExport> prepare_audio_export(engrave::Document &, const ExportRequest &, const ExportLimits &, FnRef<bool()> cancelled,
                                                 std::chrono::steady_clock::time_point submitted, ExportTiming &,
                                                 const RuntimeOptions &runtime = {});

// Any one thread; never touches a Document. Renders straight into staging,
// destroys the engrave::PreparedAudio (freeing the Engine's audio render)
// before begin_publication, then publishes and reports like live_export.
// `cancelled` is polled only on the calling thread.
Result<Json> finish_audio_export(PreparedAudioExport, FnRef<bool()> cancelled, ExportTiming &, BeginPublication begin_publication = {},
                                 std::vector<std::string> *visible_paths = nullptr);

} // namespace music::api
