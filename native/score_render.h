#pragma once
#include <string>
namespace mu::engraving { class Score; }
namespace music { std::string render_score(mu::engraving::Score* score, const std::string& format, int page); }
