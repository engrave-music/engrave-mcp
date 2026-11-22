#pragma once

#include "json.h"

namespace music::api {

using Json = music::Json;

inline bool dsl_phrase_syntax = false;

inline constexpr auto phrase_grace_defaults =
    "Before-grace kinds are acciac, appog, grace4, grace16, grace32. After-grace kinds are after8, after16, after32. "
    "Omitted grace durations are 1/8 for acciac, appog and after8, 1/4 for grace4, 1/16 for grace16 and after16, and 1/32 "
    "for grace32 and after32.";
inline constexpr auto json_phrase_grammar =
    "Give each lane an events array. A note, chord or rest has pitches (C through B, up to three # or b accidentals, then octave, "
    "e.g. Eb4 or C###4) or drums (MIDI keys 0 through 127). An empty pitches or drums array enters a rest. "
    "The duration is an exact whole-note fraction such as 1/4 or 3/8 and defaults to 1/4 for main events. "
    "Use tie: true for a whole chord or give an array of its tied pitches or drum keys. "
    "Use {\"tuplet\":\"3:2\",\"events\":[...]} for an actual:normal group of notes, chords or rests, and {\"bar\":true} to check a "
    "measure boundary. Attach grace_before or grace_after arrays to a main chord, with each grace specifying kind, pitches or drums, "
    "and optional duration.";
inline constexpr auto dsl_phrase_grammar =
    "Give each lane a whitespace-separated content string. Use Eb4, F#5 or C###4 for pitches, d36 for a drum key (0 through 127), "
    "r for a rest, and [C4 E4 G4] for a chord. Use [C4~ E4] for selective ties and ~ after a note or chord for a full tie. "
    "Duration suffixes are :w :h :q :e :s :32 :64 (whole through 64th), with up to two dots (:q. or :h..), or exact fractions "
    "such as :3/8. Main events default to :q. Use | to check a measure boundary and 3:2(A4:e G4:e F4:e) for an actual:normal "
    "tuplet. Attach before-graces as acciac(D5) appog(E5) grace4(F5) grace16(G5) grace32(A5) before a main chord and "
    "after-graces as after8(D5) after16(E5) after32(F5) after it. Each grace accepts a single note or bracketed chord and "
    "an optional duration suffix.";

Json declared_tools();

} // namespace music::api
