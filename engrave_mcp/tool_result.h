#pragma once

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

#include "json.h"
#include "runtime_limits.h"
#include "test_hooks.h"
#include "types.h"
#include "version.h"

namespace music {

inline Json server_info() { return {{"name", "engrave-mcp"}, {"version", std::string(host_version)}}; }

inline Json server_info_meta() { return {{"io.modelcontextprotocol/serverInfo", server_info()}}; }

inline void add_modern_result_fields(Json &result) {
    result["resultType"] = "complete";
    if (!result.contains("_meta") || !result.at("_meta").is_object()) result["_meta"] = make_object<Json>();
    result.at("_meta")["io.modelcontextprotocol/serverInfo"] = server_info();
}

// The text cap leaves room for the typed payload within the wire budget.
inline Json make_tool_result(Json payload, bool publication = false) {
    if (test_hooks_enabled() && publication && std::getenv("ENGRAVE_MCP_TEST_THROW_WRAPPER")) {
        throw std::runtime_error("Injected failure inside completed export wrapper");
    }
    std::string text;
    if (payload.contains("summary")) {
        text = payload.at("summary").get<std::string>();
        json_erase(payload, "summary");
    }
    if (text.empty()) text = dump_json(payload);
    if (text.size() > (256u * 1024u)) {
        if (payload.contains("error")) {
            const auto &e = payload.at("error");
            text = e.at("code").get<std::string>() + ": " + e.at("message").get<std::string>();
        } else {
            text = "Result exceeds the text budget. Read structuredContent or request a smaller range.";
            for (const auto &[key, field] : payload.get_object()) {
                if (field.is_array()) text += " " + key + "=" + std::to_string(field.size()) + " items.";
            }
            if (payload.contains("next_cursor")) text += " next_cursor=" + dump_json(payload.at("next_cursor"));
            for (auto key : {"document", "revision", "source_revision"}) {
                if (payload.contains(key)) text += " " + std::string(key) + "=" + dump_json(payload.at(key));
            }
        }
    }
    Json content_item = make_object<Json>();
    content_item["type"] = "text";
    content_item["text"] = std::move(text);

    Json result = make_object<Json>();
    result["content"] = json_array(std::move(content_item));
    result["isError"] = payload.contains("ok") && payload["ok"].is_boolean() && !payload["ok"].get<bool>();
    result["structuredContent"] = std::move(payload);
    return result;
}

inline usize export_report_bound(const Json &payload) {
    // Budget the full payload as text so the bound remains monotonic when a
    // smaller visible subset falls below the text cutoff.
    auto result = make_tool_result(payload);
    result["content"][0]["text"] = dump_json(payload);
    add_modern_result_fields(result);
    // Reserve the request-line budget for a string RPC id and room for the
    // JSON-RPC frame and publication wrapper fallback.
    return dump_json(result).size() + api::transport_request_limit + 1024;
}

} // namespace music
