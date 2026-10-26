#include "mcp_transport.h"
#include <iostream>
#include <string>
#include <functional>
#include <cctype>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace music {

using Json = nlohmann::json;

namespace {

constexpr size_t MAX_LINE_BYTES = 8 * 1024 * 1024; // 8 MiB strict maximum

enum class ServerState {
    Uninitialized,
    InitializedPending,
    Ready
};

bool is_valid_id(const Json& id) {
    return id.is_string() || id.is_number_integer();
}

bool send_json(const Json& j) {
    try {
        std::string out = j.dump(-1, ' ', false, Json::error_handler_t::replace);
        out.push_back('\n');
        std::cout.write(out.data(), static_cast<std::streamsize>(out.size()));
        std::cout.flush();
        return !std::cout.fail();
    } catch (...) {
        return false;
    }
}

bool send_error(const Json& id, int code, const std::string& message) {
    Json err = {
        {"jsonrpc", "2.0"},
        {"id", is_valid_id(id) ? id : Json(nullptr)},
        {"error", {
            {"code", code},
            {"message", message}
        }}
    };
    return send_json(err);
}

bool send_result(const Json& id, const Json& result) {
    Json res = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
    return send_json(res);
}

} // namespace

int serve_stdio(const std::function<Json()>& list_tools,
                const std::function<Json(const std::string&, const Json&)>& call_tool) {
    ServerState state = ServerState::Uninitialized;

    auto process_line = [&](const std::string& line) -> bool {
        Json req;
        try {
            req = Json::parse(line, [](int depth, Json::parse_event_t, Json&) {
                if (depth > 64) throw std::length_error("JSON nesting exceeds 64 levels");
                return true;
            });
        } catch (const std::length_error&) {
            return send_error(nullptr, -32600, "Invalid Request: JSON nesting exceeds 64 levels");
        } catch (const Json::parse_error&) {
            return send_error(nullptr, -32700, "Parse error");
        } catch (const Json::exception&) {
            return send_error(nullptr, -32700, "Parse error");
        }

        if (req.is_array()) {
            return send_error(nullptr, -32600, "Batch requests are not supported");
        }
        if (!req.is_object()) {
            return send_error(nullptr, -32600, "Invalid Request");
        }

        try {
            if (!req.contains("jsonrpc") || req["jsonrpc"] != "2.0") {
                Json fallback_id = nullptr;
                if (req.contains("id") && is_valid_id(req["id"])) {
                    fallback_id = req["id"];
                }
                return send_error(fallback_id, -32600, "Invalid Request: invalid or missing jsonrpc version");
            }

            if (!req.contains("method") || !req["method"].is_string()) {
                Json fallback_id = nullptr;
                if (req.contains("id") && is_valid_id(req["id"])) {
                    fallback_id = req["id"];
                }
                return send_error(fallback_id, -32600, "Invalid Request: method must be a string");
            }

            std::string method = req["method"].get<std::string>();

            // Check if notification or request
            if (!req.contains("id")) {
                // Notifications have no id
                if (method == "notifications/initialized") {
                    if (state == ServerState::InitializedPending) {
                        state = ServerState::Ready;
                    }
                }
                // Do not execute methods sent as notifications; ignore unknown notifications.
                return true;
            }

            const Json& id = req["id"];
            if (!is_valid_id(id)) {
                // Reject null/object/array/float/bool ids
                return send_error(nullptr, -32600, "Invalid Request: id must be a string or integer");
            }

            Json params = Json::object();
            if (req.contains("params")) {
                if (!req["params"].is_object()) {
                    return send_error(id, -32602, "Invalid params: must be an object");
                }
                params = req["params"];
            }

            if (method == "initialize") {
                if (state != ServerState::Uninitialized) {
                    return send_error(id, -32600, "Server already initialized");
                }
                if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string()) {
                    return send_error(id, -32602, "Invalid params: protocolVersion string required");
                }
                if (!params.contains("capabilities") || !params["capabilities"].is_object()) {
                    return send_error(id, -32602, "Invalid params: capabilities object required");
                }
                if (!params.contains("clientInfo") || !params["clientInfo"].is_object()) {
                    return send_error(id, -32602, "Invalid params: clientInfo object required");
                }
                const auto& client_info = params["clientInfo"];
                if (!client_info.contains("name") || !client_info["name"].is_string() ||
                    !client_info.contains("version") || !client_info["version"].is_string()) {
                    return send_error(id, -32602, "Invalid params: clientInfo name and version strings required");
                }

                Json result = {
                    {"protocolVersion", "2025-11-25"},
                    {"capabilities", {
                        {"tools", Json::object()}
                    }},
                    {"serverInfo", {
                        {"name", "musescore-mcp"},
                        {"version", "0.1.0"}
                    }}
                };
                if (!send_result(id, result)) {
                    return false;
                }
                state = ServerState::InitializedPending;
                return true;
            }

            if (method == "ping") {
                return send_result(id, Json::object());
            }

            if (method == "tools/list") {
                if (state != ServerState::Ready) {
                    return send_error(id, -32600, "Tools available only after initialized notification");
                }

                Json tools_raw;
                try {
                    tools_raw = list_tools();
                } catch (const std::exception& e) {
                    return send_error(id, -32603, std::string("Tool list callback error: ") + e.what());
                } catch (...) {
                    return send_error(id, -32603, "Unknown tool list callback error");
                }

                Json tools_list = Json::array();
                if (tools_raw.is_array()) {
                    tools_list = tools_raw;
                } else if (tools_raw.is_object() && tools_raw.contains("tools") && tools_raw["tools"].is_array()) {
                    tools_list = tools_raw["tools"];
                }

                Json result = {
                    {"tools", tools_list}
                };
                return send_result(id, result);
            }

            if (method == "tools/call") {
                if (state != ServerState::Ready) {
                    return send_error(id, -32600, "Tools available only after initialized notification");
                }
                if (!params.contains("name") || !params["name"].is_string()) {
                    return send_error(id, -32602, "Invalid params: tool name required and must be a string");
                }

                std::string tool_name = params["name"].get<std::string>();
                Json arguments = Json::object();
                if (params.contains("arguments")) {
                    if (!params["arguments"].is_object()) {
                        return send_error(id, -32602, "Invalid params: arguments must be an object");
                    }
                    arguments = params["arguments"];
                }

                Json tools_raw;
                try {
                    tools_raw = list_tools();
                } catch (const std::exception& e) {
                    return send_error(id, -32603, std::string("Tool list callback error: ") + e.what());
                } catch (...) {
                    return send_error(id, -32603, "Unknown tool list callback error");
                }

                Json tools_list = Json::array();
                if (tools_raw.is_array()) {
                    tools_list = tools_raw;
                } else if (tools_raw.is_object() && tools_raw.contains("tools") && tools_raw["tools"].is_array()) {
                    tools_list = tools_raw["tools"];
                }

                bool tool_found = false;
                for (const auto& item : tools_list) {
                    if (item.is_object() && item.contains("name") && item["name"].is_string()) {
                        if (item["name"].get<std::string>() == tool_name) {
                            tool_found = true;
                            break;
                        }
                    }
                }

                if (!tool_found) {
                    return send_error(id, -32602, "Unknown tool: " + tool_name);
                }

                try {
                    Json payload = call_tool(tool_name, arguments);
                    if (!payload.is_object()) throw std::runtime_error("Tool result must be an object");
                    Json result = {
                        {"content", Json::array({
                            {
                                {"type", "text"},
                                {"text", payload.dump(-1, ' ', false, Json::error_handler_t::replace)}
                            }
                        })},
                        {"structuredContent", payload},
                        {"isError", false}
                    };
                    return send_result(id, result);
                } catch (const std::exception& e) {
                    Json result = {
                        {"content", Json::array({
                            {
                                {"type", "text"},
                                {"text", e.what()}
                            }
                        })},
                        {"isError", true}
                    };
                    return send_result(id, result);
                } catch (...) {
                    Json result = {
                        {"content", Json::array({
                            {
                                {"type", "text"},
                                {"text", "Unknown application error"}
                            }
                        })},
                        {"isError", true}
                    };
                    return send_result(id, result);
                }
            }

            return send_error(id, -32601, "Method not found: " + method);
        } catch (const Json::exception& e) {
            Json fallback_id = nullptr;
            if (req.is_object() && req.contains("id") && is_valid_id(req["id"])) {
                fallback_id = req["id"];
            }
            return send_error(fallback_id, -32600, std::string("Malformed request: ") + e.what());
        }
    };

    auto handle_line = [&](const std::string& l) -> bool {
        bool only_ws = true;
        for (unsigned char ch : l) {
            if (!std::isspace(ch)) {
                only_ws = false;
                break;
            }
        }
        if (only_ws) {
            return true;
        }
        return process_line(l);
    };

    std::string current_line;
    current_line.reserve(4096);
    bool oversized = false;
    int c = 0;

    while ((c = std::cin.get()) != EOF) {
        if (c == '\n') {
            if (oversized) {
                if (!send_error(nullptr, -32700, "Parse error: line exceeds 8 MiB limit")) {
                    return 1;
                }
                oversized = false;
                current_line.clear();
                continue;
            }
            if (!current_line.empty() && current_line.back() == '\r') {
                current_line.pop_back();
            }
            if (!handle_line(current_line)) {
                return 1;
            }
            current_line.clear();
        } else {
            if (oversized) {
                continue;
            }
            if (current_line.size() >= MAX_LINE_BYTES) {
                oversized = true;
                current_line.clear();
                continue;
            }
            current_line.push_back(static_cast<char>(c));
        }
    }

    if (oversized) {
        if (!send_error(nullptr, -32700, "Parse error: line exceeds 8 MiB limit")) {
            return 1;
        }
    } else if (!current_line.empty()) {
        if (current_line.back() == '\r') {
            current_line.pop_back();
        }
        if (!handle_line(current_line)) {
            return 1;
        }
    }

    return 0;
}

} // namespace music

