#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "api_json.h"
#include "types.h"

namespace music::api::detail {

// The S1 schemas are the authority for shape and ordinary bounds. The codecs
// additionally check decimal overflow, EID validity and backward-only OpRefs.
inline HostError invalid_error(std::string message) { return HostError{"InvalidArgument", std::move(message)}; }

Result<void> validate_tool(std::string_view name, const Json &value);
Result<void> validate_def(std::string_view tool, std::string_view name, const Json &value);

// Text persisted to MSCX/MusicXML must satisfy XML 1.0 character rules.
Result<void> validate_xml_text(std::string_view value);

Result<i64> signed_decimal(const Json &value);
Result<engrave::ElementId> eid(const Json &value);
Result<engrave::Fraction> fraction(const Json &value);
Result<engrave::Address> address(const Json &value);
Result<engrave::Ref> ref(const Json &value, bool allow_op, usize before);
Result<engrave::ScoreSel> score(const Json &value);
Result<engrave::Value> value(const Json &value);
Result<std::filesystem::path> path(const Json &value);
std::string path_text(const std::filesystem::path &value);

} // namespace music::api::detail
