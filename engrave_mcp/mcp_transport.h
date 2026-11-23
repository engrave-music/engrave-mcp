#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif
#include <asio/awaitable.hpp>

#include "json.h"
#include "publication_gate.h"
#include "types.h"

namespace music {

using AsyncToolCall = UniqueFn<asio::awaitable<Json>(std::string, Json, std::shared_ptr<RequestExecution>)>;

// Asynchronous MCP stdio server. The local io_context is run on the caller's
// SESSION thread; list_tools, tool_exists, call_tool and shutdown are invoked
// there, so a separate Engine owner thread may post completions back to this
// context. tool_exists answers a tools/call name check without materialising
// the full tool schema tree.
//
// A dedicated jthread reads stdin and admits complete lines into a bounded
// queue, applying backpressure when the queue or output budget is exhausted.
// A second dedicated jthread writes ordinary replies using OS-native
// writes (partial writes/EINTR aware) and a bounded byte+message queue, so an
// unread or blocked stdout never blocks the io/session thread, timers, or job
// cancellation. Published export reports write synchronously on the engine
// owner under the same publication mutex as the ordinary writer.
//
// call_tool is co_spawned on the session executor and may await isolated engine
// work. shutdown is invoked exactly once on the session thread on EOF or a
// fatal transport error; it is expected to cancel queued engine work. Running
// operations that already committed are allowed to complete and report their
// result. The server keeps draining the io_context until all engine completion
// handlers have returned; it never calls io_context::stop() from a transport
// failure path.
int serve_stdio_async(UniqueFn<Json()> list_tools, UniqueFn<bool(std::string_view)> tool_exists, AsyncToolCall call_tool,
                      UniqueFn<void()> shutdown);

} // namespace music
