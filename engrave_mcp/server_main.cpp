#include "server_main.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
#include "version.h"

namespace music {

namespace {

constexpr const char *usage_text = "Usage: engrave-mcp [--soundfont FILE.sf2|FILE.sf3] [--resources DIR] [-h|--help] [-V|--version]";

void print_help() {
    std::printf("%s\n\n"
                "Options:\n"
                "  --soundfont FILE   Load SoundFont (.sf2/.sf3); defaults to sibling soundfonts/FluidR3Mono_GM.sf3 if present\n"
                "  --resources DIR    Override runtime assets directory (default: sibling assets/)\n"
                "  -h, --help         Show this help message and exit\n"
                "  -V, --version      Show version and exit\n",
                usage_text);
}

} // namespace

int run_server(int argc, char **argv) {
    try {
        std::string soundfont;
        std::string resources;
        enum class EarlyExit { NONE, HELP, VERSION };
        auto early_exit = EarlyExit::NONE;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            if (arg == "--soundfont" || arg == "--resources") {
                auto &value = arg == "--soundfont" ? soundfont : resources;
                if (!value.empty() || i + 1 >= argc) {
                    throw std::invalid_argument(usage_text);
                }
                const std::string_view next(argv[++i]);
                if (next.empty() || next.starts_with('-')) {
                    throw std::invalid_argument(usage_text);
                }
                value = next;
            } else if (arg == "--help" || arg == "-h") {
                if (early_exit == EarlyExit::NONE) early_exit = EarlyExit::HELP;
            } else if (arg == "--version" || arg == "-V") {
                if (early_exit == EarlyExit::NONE) early_exit = EarlyExit::VERSION;
            } else {
                throw std::invalid_argument(usage_text);
            }
        }
        if (early_exit == EarlyExit::HELP) {
            print_help();
            return 0;
        }
        if (early_exit == EarlyExit::VERSION) {
            std::printf("engrave-mcp %.*s\n", static_cast<int>(host_version.size()), host_version.data());
            return 0;
        }
#ifdef _WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1) {
            throw std::runtime_error("Cannot configure binary stdio");
        }
#endif
        std::filesystem::path exe_dir;
        if (resources.empty()) {
            exe_dir = current_executable_path().parent_path();
        } else if (soundfont.empty()) {
            try {
                exe_dir = current_executable_path().parent_path();
            } catch (const std::runtime_error &) {
                // Explicit resources allow startup without sibling SoundFont discovery.
            }
        }
        engrave::EngineOptions options;
        options.resources = resources.empty() ? exe_dir / "assets" : std::filesystem::absolute(std::filesystem::u8path(resources));
        if (!soundfont.empty()) {
            options.soundfonts.push_back(std::filesystem::absolute(std::filesystem::u8path(soundfont)));
        } else if (!exe_dir.empty()) {
            const auto default_soundfont = exe_dir / "soundfonts" / "FluidR3Mono_GM.sf3";
            std::error_code ec;
            if (std::filesystem::is_regular_file(default_soundfont, ec)) {
                options.soundfonts.push_back(default_soundfont);
            }
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
