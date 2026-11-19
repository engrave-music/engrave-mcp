#include "server_main.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "async_session.h"
#include "mcp_transport.h"
#include "publication.h"
#include "registry.h"
#include "test_hooks.h"

namespace music {

int run_server(int argc, char **argv) {
    try {
#ifdef _WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
            throw std::runtime_error("Cannot configure binary stdio");
        }
#endif
        std::string soundfont;
        std::string resources;
        for (int i = 1; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--soundfont" && i + 1 < argc) {
                soundfont = argv[++i];
            } else if (std::string_view(argv[i]) == "--resources" && i + 1 < argc) {
                resources = argv[++i];
            } else {
                throw std::invalid_argument("Usage: engrave-mcp [--soundfont FILE.sf2|FILE.sf3] [--resources DIR]");
            }
        }
        engrave::EngineOptions options;
        options.resources = resources.empty() ? current_executable_path().parent_path() / "assets" :
                                                std::filesystem::absolute(std::filesystem::u8path(resources));
        if (!soundfont.empty()) {
            options.soundfonts.push_back(std::filesystem::absolute(std::filesystem::u8path(soundfont)));
        }
        AsyncSession session(std::move(options));
        const bool hooks = test_hooks_enabled();
        return serve_stdio_async(make_unique_fn<Json()>([hooks] {
                                     auto tools = api::tools_json();
                                     if (hooks) {
                                         json_push(tools, Json{{"name", "test_owner_state"},
                                                               {"description", "Test-only owner-thread state"},
                                                               {"inputSchema", {{"type", "object"}, {"additionalProperties", false}}}});
                                         json_push(tools, Json{{"name", "test_owner_sleep"},
                                                               {"description", "Test-only owner-thread stall"},
                                                               {"inputSchema",
                                                                {{"type", "object"},
                                                                 {"properties",
                                                                  {{"ms", {{"type", "integer"}, {"minimum", 1}, {"maximum", 60000}}},
                                                                   {"barrier", {{"type", "string"}}}}},
                                                                 {"additionalProperties", false}}}});
                                     }
                                     return tools;
                                 }),
                                 make_unique_fn<bool(std::string_view)>([hooks](std::string_view name) {
                                     if (api::find_tool(name)) return true;
                                     return hooks && (name == "test_owner_state" || name == "test_owner_sleep");
                                 }),
                                 make_unique_fn<asio::awaitable<Json>(std::string, Json, std::shared_ptr<RequestExecution>)>(
                                     [&session](std::string name, Json args, std::shared_ptr<RequestExecution> execution) {
                                         return session.call(std::move(name), std::move(args), std::move(execution));
                                     }),
                                 make_unique_fn<void()>([&session] { session.shutdown(); }));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

} // namespace music
