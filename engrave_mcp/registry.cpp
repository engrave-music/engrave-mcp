#include "registry.h"

#include <set>
#include <stdexcept>

#include "api_json.h"
#include "host_api.h"

namespace music::api {

namespace {
constexpr std::array specs = {
    ToolSpec{"score_open", HandlerId::OPEN, Scheduling::OWNER},          ToolSpec{"score_create", HandlerId::CREATE, Scheduling::OWNER},
    ToolSpec{"score_close", HandlerId::CLOSE, Scheduling::OWNER},        ToolSpec{"score_outline", HandlerId::OUTLINE, Scheduling::OWNER},
    ToolSpec{"score_query", HandlerId::QUERY, Scheduling::OWNER},        ToolSpec{"score_describe", HandlerId::DESCRIBE, Scheduling::OWNER},
    ToolSpec{"score_edit", HandlerId::EDIT, Scheduling::OWNER},          ToolSpec{"score_export", HandlerId::EXPORT, Scheduling::OWNER},
    ToolSpec{"runtime_info", HandlerId::RUNTIME_INFO, Scheduling::OWNER}};

consteval bool complete() {
    std::array<int, static_cast<std::size_t>(HandlerId::COUNT)> counts{};
    for (auto &s : specs) ++counts[static_cast<std::size_t>(s.handler)];
    for (auto count : counts) {
        if (count != 1) return false;
    }
    return true;
}

static_assert(complete());

const Json &schemas() {
    static const Json tools = declared_tools();
    return tools;
}

const Json &definition(std::string_view name) {
    for (const auto &tool : schemas().get_array()) {
        if (tool.at("name").get<std::string>() == name) return tool;
    }
    throw std::logic_error("missing tool definition");
}
} // namespace

std::string ToolSpec::description() const { return definition(name).at("description").get<std::string>(); }

const Json &ToolSpec::input_schema() const { return definition(name).at("inputSchema"); }

std::span<const ToolSpec> registry() { return specs; }

const ToolSpec *find_tool(std::string_view name) {
    for (auto &s : specs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

Json tools_json() {
    Json tools = make_array<Json>();
    for (auto &spec : specs) json_push(tools, definition(spec.name));
    return tools;
}

void validate_handlers(const HandlerTable &handlers) {
    for (auto handler : handlers) {
        if (!handler) throw std::logic_error("unbound tool handler");
    }
}

void validate_registry() {
    auto tools = tools_json();
    if (schemas().size() != specs.size()) throw std::logic_error("tool count mismatch");
    std::set<std::string> names;
    for (const auto &tool : schemas().get_array()) {
        const auto name = tool.at("name").get<std::string>();
        if (!names.insert(name).second || !find_tool(name)) throw std::logic_error("duplicate or unknown schema definition");
    }
    names.clear();
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (tools[i].at("name").get<std::string>() != specs[i].name || !names.insert(std::string(specs[i].name)).second) {
            throw std::logic_error("tool table mismatch");
        }
    }
    std::set<std::string> declared;
    for (auto &op : tools[6]["inputSchema"]["$defs"]["op"]["oneOf"].get_array()) {
        if (!declared.insert(op["properties"]["op"]["const"].get<std::string>()).second) throw std::logic_error("duplicate op");
    }
    if (declared.size() != operation_names.size()) throw std::logic_error("op count mismatch");
    for (auto name : operation_names) {
        if (!declared.contains(std::string(name))) throw std::logic_error("missing op schema");
    }
}
} // namespace music::api
