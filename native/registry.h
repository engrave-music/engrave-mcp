#pragma once
#include "schema_defs.h"
#include <array>
#include <span>
#include <string_view>
namespace music::v2 {
enum class HandlerId { Open, Create, Close, Outline, Query, Describe, Edit, Export, RuntimeInfo, Count };
enum class Scheduling { Owner };
struct ToolSpec {
    std::string_view name;
    HandlerId handler;
    Scheduling scheduling;
    std::string description() const;
    const Json &input_schema() const;
};
std::span<const ToolSpec> registry();
const ToolSpec *find_tool(std::string_view name);
Json tools_json();
// Throws on duplicate/missing handlers, mismatched schema table, or Op coverage.
void validate_registry();
// S4 binds exactly one callable to each id; no string dispatcher is needed.
// Known-tool decode failures return the error envelope with MCP isError=true;
// unknown tools remain JSON-RPC invalid parameters. structuredContent is the
// authoritative envelope; the text representation remains bounded by transport.
using Handler = Json (*)(void *context, const Json &arguments);
using HandlerTable = std::array<Handler, static_cast<std::size_t>(HandlerId::Count)>;
void validate_handlers(const HandlerTable &);
} // namespace music::v2
