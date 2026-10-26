#pragma once
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

namespace music {
using Json = nlohmann::json;
int serve_stdio(const std::function<Json()>& list_tools,
                const std::function<Json(const std::string&, const Json&)>& call_tool);
}
