// SPDX-License-Identifier: GPL-3.0-only
// Adapted from partitioncloud/webmscore web/importexport/positionjsonwriter.cpp.
// Upstream revision is recorded in upstream.lock.json.
#pragma once

#include <nlohmann/json.hpp>

namespace mu::engraving {
class Score;
}

namespace music {
// Layout coordinates use the upstream 1200-DPI units; event times are milliseconds.
// Requires a laid-out score and execution on its owner thread. IDs are local to this result.
nlohmann::json score_positions(mu::engraving::Score* score, bool segments);
} // namespace music
