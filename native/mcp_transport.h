#pragma once
#include <functional>
#include <stdexcept>
#include <string>

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio/awaitable.hpp>
#include <nlohmann/json.hpp>

namespace music {
using Json = nlohmann::json;

// Structured, transport-independent failure raised by AsyncSession (or any
// tool implementation) when a call has a stable machine-readable code. The
// transport serialises it as an MCP tool error with a structuredContent.error
// object and a legacy text content item. Details must be JSON-serialisable.
class ToolCallError : public std::runtime_error {
public:
    ToolCallError(std::string code,
                  std::string message,
                  Json details = Json::object());

    std::string code;
    Json details;
};

using AsyncToolCall = std::function<asio::awaitable<Json>(std::string, Json)>;

// Legacy synchronous entry point, kept for the Qt/oldhost API. It wraps the
// synchronous callback in an immediately-ready awaitable and delegates to the
// asynchronous implementation below.
int serve_stdio(const std::function<Json()>& list_tools,
                const std::function<Json(const std::string&, const Json&)>& call_tool);

// Asynchronous MCP stdio server. The local io_context is run on the caller's
// SESSION thread; list_tools, call_tool and shutdown are invoked there, so a
// separate Engine owner thread may post completions back to this context.
//
// A dedicated jthread reads stdin and admits complete lines into a bounded
// queue, applying backpressure when the queue or output budget is exhausted.
// A second dedicated jthread is the only writer to stdout: it uses OS-native
// writes (partial writes/EINTR aware) and a bounded byte+message queue, so an
// unread or blocked stdout never blocks the io/session thread, timers, or job
// cancellation.
//
// call_tool is co_spawned on the session executor and may await isolated engine
// work. shutdown is invoked exactly once on the session thread on EOF or a
// fatal transport error; it is expected to cancel queued engine work. Running
// operations that already committed are allowed to complete and report their
// result. The server keeps draining the io_context until all engine completion
// handlers have returned; it never calls io_context::stop() from a transport
// failure path.
int serve_stdio_async(const std::function<Json()>& list_tools,
                      const AsyncToolCall& call_tool,
                      const std::function<void()>& shutdown);
} // namespace music
