#pragma once
#include <memory>
#include <string>

namespace mu::engraving { class EngravingProject; }
namespace music {
using ScoreProject = std::shared_ptr<mu::engraving::EngravingProject>;
void initialize_native_runtime(const std::string& assets);
ScoreProject load_native_score(const std::string& bytes, const std::string& format);
std::string export_native_score(const ScoreProject& project, const std::string& format, int page,
                                const std::string& soundfont);
}
