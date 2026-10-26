#pragma once
#include <string>
#include <nlohmann/json.hpp>
namespace music {
std::string composition_xml(const nlohmann::json& score);
}
