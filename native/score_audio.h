#pragma once

#include <cstdint>
#include <string>
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
