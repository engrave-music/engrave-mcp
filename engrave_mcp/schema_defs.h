#pragma once

#include "json.h"

namespace music::api {

using Json = music::Json;

inline constexpr auto phrase_grace_defaults =
    "Numbered kinds default to and require the reciprocal of their number (1/4, 1/8, 1/16 or 1/32). "
    "The two unnumbered kinds default to 1/8 and accept any standard note value up to 1/4.";
inline constexpr auto phrase_grammar =
    "Give each lane a whitespace-separated content string. Use Eb4, F#5 or C###4 for pitches (up to three # or b), d36 for a drum key (0 "
    "through 127), "
    "r for a rest, and [C4 E4 G4] for a chord. Use [C4~ E4] for selective ties and ~ after a note or chord for a full tie (rests and "
    "graces cannot be tied). "
    "Duration suffixes are :w :h :q :e :s :32 :64 (whole through 64th), with up to two dots (:q. or :h..), or positive fractions "
    "such as :3/8 whose reduced denominator is a power of two up to 1024 (use tuplets for triplets). Main events default to :q. "
    "Use | to check a measure boundary and 3:2(A4:e G4:e F4:e) for an actual:normal "
    "tuplet. Attach before-graces as acciac(D5) appog(E5) grace4(F5) grace16(G5) grace32(A5) before a main chord and "
    "after-graces as after8(D5) after16(E5) after32(F5) after it. Each grace accepts a single note or bracketed chord and "
    "an optional duration suffix.";

Json declared_tools();

} // namespace music::api
