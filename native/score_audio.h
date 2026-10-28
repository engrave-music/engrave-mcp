#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "audio_session.h"

namespace mu::engraving {
class Score;
}

namespace music {

// One logical audio stem: a score part containing a single instrument.
//
// track_id is the part's zero-based index in Score::parts(), i.e. score order.
// It is stable for a given score ordering and identical in
// score_audio_track_info() and score_audio_stems(). Multi-instrument parts are
// rejected by both functions instead of being split or mixed incorrectly.
struct ScoreAudioTrackInfo {
    std::int64_t track_id = 0;
    std::string part_id;
    std::string instrument_id;
    std::string name;
};

// Describes the stems score_audio_stems() would produce, without rendering.
// An empty score (no parts and no staves) produces an empty result.
// Throws std::invalid_argument for a null score and std::runtime_error for a
// structurally inconsistent score or an unsupported multi-instrument part.
std::vector<ScoreAudioTrackInfo> score_audio_track_info(mu::engraving::Score* score);

struct TrackSoloMute {
    bool solo = false;
    bool muted = false;
    bool operator==(const TrackSoloMute&) const = default;
};

// Per-document default mix read from an MSCZ archive's audiosettings.json.
// Tracks are keyed by the same (partId, instrumentId) identities that
// score_audio_track_info() reports. Volume and balance are project-wide (from
// root audiosettings.json). For the full score (excerptId == -1), solo and
// muted come from the root file. For an excerpt (excerptId >= 0), solo and
// muted come from Excerpts/<name>/audiosettings.json when present, else neutral.
// Unknown entries and fields are ignored; missing, malformed or non-MSCZ input
// yields neutral defaults.
struct ScoreAudioMixDefaults {
    AudioOutputParams master;
    std::map<std::pair<std::string, std::string>, AudioOutputParams> tracks;
    std::map<std::string, std::map<std::pair<std::string, std::string>, TrackSoloMute>> excerpt_tracks_by_name;
    std::vector<std::map<std::pair<std::string, std::string>, TrackSoloMute>> excerpt_tracks_by_index;

    const std::map<std::pair<std::string, std::string>, TrackSoloMute>* get_excerpt_map(
        int excerpt_id, const std::string& excerpt_name = "") const noexcept
    {
        if (!excerpt_name.empty()) {
            auto it = excerpt_tracks_by_name.find(excerpt_name);
            if (it != excerpt_tracks_by_name.end()) return &it->second;
        }
        if (excerpt_id >= 0 && static_cast<std::size_t>(excerpt_id) < excerpt_tracks_by_index.size()) {
            return &excerpt_tracks_by_index[static_cast<std::size_t>(excerpt_id)];
        }
        return nullptr;
    }

    bool is_neutral(int excerpt_id = -1, const std::string& excerpt_name = "") const noexcept
    {
        if (!(master == AudioOutputParams{})) return false;
        if (excerpt_id < 0) {
            for (const auto& [identity, output] : tracks) {
                (void)identity;
                if (!(output == AudioOutputParams{})) return false;
            }
            return true;
        }
        const auto* excerpt_map = get_excerpt_map(excerpt_id, excerpt_name);
        for (const auto& [identity, output] : tracks) {
            if (output.volume_db != 0.0 || output.balance != 0.0) return false;
            if (excerpt_map) {
                auto it = excerpt_map->find(identity);
                if (it != excerpt_map->end()) {
                    if (it->second.solo || it->second.muted) return false;
                }
            }
        }
        return true;
    }
};

// Reads audiosettings.json from MSCZ bytes. Never throws: invalid archives,
// absent files and malformed JSON all produce neutral defaults. Values outside
// the supported range are ignored field by field.
ScoreAudioMixDefaults score_audio_mix_defaults(const std::string& mscz_bytes);

// Renders the score once with the upstream MIDI exporter, then builds one
// format-1 MIDI file per part by grouping that part's staff tracks. Global
// conductor events (tempo, time signatures, markers) are copied from the
// exporter's track 0 into every stem; other parts' channel events are never
// copied. Each selected staff keeps its own channel events, program changes,
// controllers/RPNs, and port-change meta events. Text meta events are preserved.
//
// Enforces the audio_session limits: at most audio_max_tracks stems and at most
// 8 MiB of MIDI bytes in total. expand_repeats and export_rpns are passed
// through unchanged to the upstream ExportMidi algorithm, together with the
// score's own SynthesizerState.
// Throws std::invalid_argument for a null score and std::runtime_error when
// export, MIDI serialization, validation, or the size limits fail.
std::vector<AudioTrack> score_audio_stems(mu::engraving::Score* score, bool expand_repeats, bool export_rpns);

} // namespace music
