#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "api_json.h"
#include "types.h"

namespace music::api::detail {

inline HostError invalid_error(std::string message) { return HostError{"InvalidArgument", std::move(message)}; }

Result<void> validate_tool(std::string_view name, const Json &value);
Result<void> validate_op(const Json &value, usize index);

// Text persisted to MSCX/MusicXML must satisfy XML 1.0 character rules.
Result<void> validate_xml_text(std::string_view value);

Result<i64> signed_decimal(const Json &value);
Result<engrave::ElementId> eid(const Json &value);
Result<engrave::Fraction> fraction(const Json &value);
Result<engrave::Address> address(const Json &value, const engrave::Outline *outline = nullptr);
Result<engrave::Ref> ref(const Json &value, bool allow_op, usize before);
Result<engrave::ScoreSel> score(const Json &value);
Result<engrave::Value> value(const Json &j, engrave::ValueKind kind, const std::string &path);
Result<std::filesystem::path> path(const Json &value);
std::string path_text(const std::filesystem::path &value);

} // namespace music::api::detail
