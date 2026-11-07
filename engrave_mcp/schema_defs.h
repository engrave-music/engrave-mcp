#pragma once

#include <nlohmann/json.hpp>

namespace music::api {

using Json = nlohmann::json;

Json declared_tools();

} // namespace music::api
