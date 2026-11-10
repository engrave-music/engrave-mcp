#pragma once

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

#include "json.h"
#include "runtime_limits.h"

namespace music {

// Successful results carry the payload both as legacy text and as
// structuredContent. Duplicating a large query payload
// would exceed the 16 MiB wire cap even though the structured payload itself
// fits. Above this threshold the legacy text becomes a short pointer to
// structuredContent, keeping the authoritative structured result intact.
inline Json make_tool_result(Json payload, bool publication = false) {
#ifdef MUSIC_ASYNC_TEST_HOOKS
    if (publication && std::getenv("ENGRAVE_MCP_TEST_THROW_WRAPPER"))
        throw std::runtime_error("Injected failure inside completed export wrapper");
#endif
    std::string text = dump_json(payload);
    if (text.size() > (4u * 1024u * 1024u)) {
        text = "Tool result is available in structuredContent; its "
               "serialized form exceeds the legacy text budget.";
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

// Reserve the entire request-line budget for a string RPC id, plus envelope.
inline std::size_t export_report_bound(const Json &payload) {
    // Always budget legacy duplication, even above its cutoff. This keeps the
    // bound monotonic when a smaller visible subset falls below that cutoff.
    auto result = make_tool_result(payload);
    result["content"][0]["text"] = dump_json(payload);
    return dump_json(result).size() + api::transport_request_limit + 1024;
}

} // namespace music
