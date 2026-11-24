#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "api_json.h"
#include "types.h"

namespace music::api::detail {

inline HostError invalid_error(std::string message) { return HostError{"InvalidArgument", std::move(message)}; }

// Engine indices are 0-based with -1 for "none"; the wire is 1-based with null for "none". Widened so INT32_MAX + 1 cannot overflow.
inline Json one_based(std::int64_t index) { return index >= 0 ? Json(index + 1) : Json(nullptr); }

Result<void> validate_tool(std::string_view name, const Json &value);
Result<void> validate_op(const Json &value, usize index);

// Text persisted to MSCX/MusicXML must satisfy XML 1.0 character rules.
Result<void> validate_xml_text(std::string_view value);

Result<i64> signed_decimal(std::string_view value);
Result<engrave::ElementId> eid(const Json &value);
Result<engrave::Fraction> fraction(const Json &value);
Result<engrave::Address> address(const Json &value);
// A compact {measure, staff?, voice?, offset?} object; the engine resolves it when its operation runs.
bool is_relative_position(const Json &value);
Result<engrave::RelativePosition> relative_position(const Json &value);
Result<engrave::EditAddress> edit_address(const Json &value);
Result<engrave::Ref> ref(const Json &value, bool allow_op, usize before);
Result<engrave::ScoreSel> score(const Json &value);
std::string_view grace_kind_name(engrave::GraceKind kind);
std::optional<engrave::GraceKind> grace_kind(std::string_view name);
Result<engrave::Value> value(const Json &j, engrave::ValueKind kind, const std::string &path);
Result<std::filesystem::path> path(const Json &value);
std::string path_text(const std::filesystem::path &value);

} // namespace music::api::detail
