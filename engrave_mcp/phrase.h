#pragma once

#include <engrave/music.h>

#include "api_json.h"

namespace music::api {

Json music_result(const engrave::MusicWindow &, const engrave::Outline &, bool ids);

enum class PhraseSyntax { JSON, DSL };

Result<engrave::Phrase> decode_phrase(const Json &lanes, const engrave::Outline &outline, engrave::ScoreSel score, PhraseSyntax syntax);

} // namespace music::api
