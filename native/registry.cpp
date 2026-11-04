#include "registry.h"
#include "api_json.h"
#include "host_api.h"
#include <set>
#include <stdexcept>

namespace music::v2 {
namespace {
constexpr std::array specs = {
    ToolSpec{"score_open", HandlerId::Open, Scheduling::Owner},         ToolSpec{"score_create", HandlerId::Create, Scheduling::Owner},
    ToolSpec{"score_close", HandlerId::Close, Scheduling::Owner},       ToolSpec{"score_outline", HandlerId::Outline, Scheduling::Owner},
    ToolSpec{"score_query", HandlerId::Query, Scheduling::Owner},       ToolSpec{"score_describe", HandlerId::Describe, Scheduling::Owner},
    ToolSpec{"score_edit", HandlerId::Edit, Scheduling::Owner},         ToolSpec{"score_export", HandlerId::Export, Scheduling::Owner},
    ToolSpec{"runtime_info", HandlerId::RuntimeInfo, Scheduling::Owner}};

consteval bool complete() {
    std::array<int, static_cast<std::size_t>(HandlerId::Count)> counts{};
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
    for (auto &tool : schemas()) {
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
    Json tools = Json::array();
    for (auto &spec : specs) tools.push_back(definition(spec.name));
    return tools;
}

void validate_handlers(const HandlerTable &handlers) {
    for (auto handler : handlers) {
        if (!handler) throw std::logic_error("unbound v2 handler");
    }
}

void validate_registry() {
    auto tools = tools_json();
    if (schemas().size() != specs.size()) throw std::logic_error("tool count mismatch");
    std::set<std::string> names;
    for (const auto &tool : schemas()) {
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
    for (auto &op : tools[6]["inputSchema"]["$defs"]["op"]["oneOf"]) {
        if (!declared.insert(op["properties"]["op"]["const"].get<std::string>()).second) throw std::logic_error("duplicate op");
    }
    if (declared.size() != operation_names.size()) throw std::logic_error("op count mismatch");
    for (auto name : operation_names) {
        if (!declared.contains(std::string(name))) throw std::logic_error("missing op schema");
    }
}
} // namespace music::v2
