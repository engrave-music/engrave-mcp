#pragma once

#include "api_json.h"

namespace music::v2::detail {

// The S1 schemas are the authority for shape and ordinary bounds. The codecs
// additionally check decimal overflow, EID validity and backward-only OpRefs.
struct DecodeError {
    HostError error;
};

[[noreturn]] void invalid(std::string message);
void validate_tool(std::string_view name, const Json &value);
void validate_def(std::string_view tool, std::string_view name, const Json &value);

// Text persisted to MSCX/MusicXML must satisfy XML 1.0 character rules.
void validate_xml_text(std::string_view value);

std::int64_t signed_decimal(const Json &value);
engrave::ElementId eid(const Json &value);
engrave::Fraction fraction(const Json &value);
engrave::Address address(const Json &value);
engrave::Ref ref(const Json &value, bool allow_op, std::size_t before);
engrave::ScoreSel score(const Json &value);
engrave::Value value(const Json &value);
std::filesystem::path path(const Json &value);
std::string path_text(const std::filesystem::path &value);

} // namespace music::v2::detail
