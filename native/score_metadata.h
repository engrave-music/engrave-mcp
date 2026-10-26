/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Qt-free native adaptation of the Score* overload in
// .upstream/webmscore/src/converter/internal/compat/notationmeta.cpp
// (mu::converter::NotationMeta). The JSON field schema and the
// title/composer recognition heuristics are preserved; QJson*/QString are
// replaced by nlohmann::json and muse::String/std::string.

#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace mu::engraving {
class Score;
}

namespace music {
// Same fields and values as upstream NotationMeta::metaJson(Score*), except
// that "tracks" is always an empty array here: the audio-track overload is
// not part of the Qt-free host. Returns an empty object when score is null.
nlohmann::json score_metadata(mu::engraving::Score* score);

// Upstream NotationMeta::title(), converted to std::string. Returns an empty
// string when score is null.
std::string score_title(const mu::engraving::Score* score);
}
