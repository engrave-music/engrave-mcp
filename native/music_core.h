#pragma once

#include "audio_session.h"
#include "export_options.h"
#include "score_edit.h"
#include "score_query.h"
#include "score_excerpts.h"
#include "runtime_assets.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace music {

// Stable, non-reused handle. Values start at 1 and are never recycled.
struct DocumentId {
    std::uint64_t value = 0;

    constexpr explicit DocumentId(std::uint64_t value = 0) noexcept : value(value) {}

    friend constexpr bool operator==(const DocumentId& lhs, const DocumentId& rhs) noexcept {
        return lhs.value == rhs.value;
    }
    friend constexpr bool operator!=(const DocumentId& lhs, const DocumentId& rhs) noexcept {
        return !(lhs == rhs);
    }
};

// Monotonic per-document revision. Values start at 1 and only successful
// atomic edits (replacements or pitch changes) advance them.
struct Revision {
    std::uint64_t value = 0;

    constexpr explicit Revision(std::uint64_t value = 0) noexcept : value(value) {}

    friend constexpr bool operator==(const Revision& lhs, const Revision& rhs) noexcept {
        return lhs.value == rhs.value;
    }
    friend constexpr bool operator!=(const Revision& lhs, const Revision& rhs) noexcept {
        return !(lhs == rhs);
    }
};

enum class InputFormat {
    MusicXml,
    Xml,
    Mxl,
    Mscz,
    Mscx,
    Midi,
    Kar,
    Gp,
    Gp3,
    Gp4,
    Gp5,
    Gp6,
    Gp7,
    Gpx,
    Ptb,
};

enum class ExportFormat {
    MusicXml,
    Mxl,
    Mscz,
    Mscx,
    Midi,
    Svg,
    Png,
    Pdf,
    Metadata,
    Wav,
    MeasurePositions,
    SegmentPositions,
    Flac,
    Ogg,
    Mp3,
};

struct RuntimeOptions {
    // Mandatory root containing the trimmed native runtime resources.
    std::string resources_path;
    // Optional SoundFont used by WAV export.
    std::string soundfont;
    // Reserved for follow-up font discovery; initially empty.
    std::vector<std::string> font_paths;
    // Optional cache directory for runtime assets (e.g. soundfont/font cache).
    std::string cache_directory;
};

struct Note {
    std::vector<int> pitches;
    int ticks = 0;
};

struct Measure {
    std::vector<Note> notes;
};

struct Part {
    std::string name;
    int midi_program = 0;
    std::vector<Measure> measures;
};

struct Composition {
    std::string title = "Untitled";
    int tempo = 100;
    std::vector<Part> parts;
};

struct ScoreInfo {
    DocumentId id;
    Revision revision;
    std::string title;
    std::string title_filename_safe;
    std::size_t pages = 0;
    std::size_t parts = 0;
    bool editable = false;
};

struct ScoreView {
    ScoreInfo info;
    std::optional<Composition> composition;
    std::string musicxml;
    std::string metadata_json;
};

// Client-supplied intent for an explicitly rebased, idempotent edit.
struct EditContext {
    Revision base_revision;
    std::string operation_id;
};

struct EditResult {
    ScoreInfo score;
    Revision base_revision;
    std::vector<Revision> rebased_over;
    bool replayed = false;
    std::size_t changed_notes = 0;
};

struct RegionData {
    Revision revision;
    ScoreRegionView region;
};

// Fully owned values; safe to retain and query on another thread. Capture on
// the core owner thread after publication, then publish the shared pointer.
struct ScoreReadSnapshot {
    ScoreInfo info;
    ScoreReadProjection projection;
};

struct ExcerptsData {
    Revision revision;
    std::vector<ExcerptInfo> excerpts;
};

// Owned descriptor for one audio stem of a selected score. The fields mirror
// the private score_audio helper without exposing raw score objects. output
// carries the document's default mix values for this track.
struct AudioTrackInfo {
    std::int64_t track_id = 0;
    std::string part_id;
    std::string instrument_id;
    std::string name;
    AudioOutputParams output;
};

struct AudioTracksData {
    Revision revision;
    std::vector<AudioTrackInfo> tracks;
};

struct ExportData {
    Revision revision;
    std::string bytes;
};

// Version stamps of a document as it was loaded or created. Writer::writeScore
// rewrites them with the running program's values, so every reload of our own
// writer output restores these fields.
struct ScoreVersions {
    std::string mscore_version;
    int mscore_revision = 0;
    int msc_version = 0;
};

struct SnapshotData {
    Revision revision;
    std::string bytes;
    ScoreVersions versions;
};

class CoreError : public std::runtime_error {
public:
    enum Code {
        invalid_argument,
        not_found,
        resource_limit,
        wrong_thread,
        engine_failure,
        conflict,
        stale_revision,
        poisoned_document,
    };

    CoreError(Code code, const std::string& message);
    CoreError(Code code, const char* message);

    Code code() const noexcept { return code_; }

private:
    Code code_;
};

// Canonical names are shared with the JSON adapter; backend dispatch stays
// private to the implementation.
std::optional<InputFormat> input_format_from_name(std::string_view name) noexcept;
std::optional<ExportFormat> export_format_from_name(std::string_view name) noexcept;
const char* format_name(InputFormat format) noexcept;
const char* format_name(ExportFormat format) noexcept;

// Strict "score-N" document handle conversion shared by all transports.
DocumentId document_id_from_string(std::string_view text);
std::string document_id_to_string(DocumentId id);

// Owner-thread confined, non-copyable, non-movable facade over the native
// score engine. A document owns a private engine project plus an optional typed
// Composition; the public API never exposes borrowed engine objects.
class MusicCore {
public:
    explicit MusicCore(const RuntimeOptions& options);
    ~MusicCore();

    MusicCore(const MusicCore&) = delete;
    MusicCore& operator=(const MusicCore&) = delete;
    MusicCore(MusicCore&&) = delete;
    MusicCore& operator=(MusicCore&&) = delete;

    ScoreInfo create(const Composition& composition);
    ScoreInfo open(const std::string& bytes, InputFormat format);
    ScoreInfo describe(DocumentId id);
    ScoreView get(DocumentId id, int excerpt_id = -1);
    std::shared_ptr<const ScoreReadSnapshot> read_snapshot(DocumentId id);
    ExcerptsData excerpts(DocumentId id);
    ExcerptsData generate_excerpts(DocumentId id);
    // Audio descriptors for the selected score, including the document's
    // default output values (MSCZ audiosettings.json or neutral).
    AudioTracksData audio_tracks(DocumentId id, int excerpt_id = -1);
    // Resolve the default mix plus any export-time overrides. Used by the host
    // to hand an effective mix to a background worker. Read-only: exports and
    // mix resolution never change document state or revision.
    AudioMix audio_export_mix(DocumentId id, const ExportOptions& options = {});
    // Owns freshly rendered stems, the selected SoundFont, and the applied mix.
    // The returned session must be destroyed on this MusicCore's owner thread.
    // Its asset lease keeps the frozen SoundFont alive if it outlives this core.
    std::unique_ptr<AudioSession> create_audio_session(DocumentId id,
                                                       const ExportOptions& options = {},
                                                       double start_seconds = 0);
    RegionData query_region(DocumentId id, const ScoreRegion& region);
    ScoreInfo replace_measures(DocumentId id, std::size_t part_index, std::size_t start_measure,
                               const std::vector<Measure>& measures);
    EditResult replace_measures(DocumentId id, std::size_t part_index, std::size_t start_measure,
                                const std::vector<Measure>& measures, const EditContext& context);
    EditResult set_pitches(DocumentId id, const NativePitchEdit& edit, const EditContext& context);
    ExportData export_bytes(DocumentId id, ExportFormat format, const ExportOptions& options = {});
    SnapshotData snapshot(DocumentId id);
    // Restore the source file's version stamps after reloading our own writer
    // output; an export must not re-author the document.
    void restore_versions(DocumentId id, const ScoreVersions& versions);
    void close(DocumentId id);
    RuntimeOptions runtime_options() const;
    RuntimeSoundFontInfo set_soundfont(const std::string& path);
    RuntimeFontInfo add_font(const std::string& path);
    void poison_document_for_testing(DocumentId id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace music
