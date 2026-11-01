#include "mcp_transport.h"
#include "async_session.h"
#include "background_job.h"
#include "registry.h"
#include "worker_v2.h"
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--worker") {
            const auto request = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
            return music::v2::run_worker(request);
        }
#ifdef _WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
            throw std::runtime_error("Cannot configure binary stdio");
#endif
        std::string soundfont, resources;
        bool live_export = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--soundfont" && i + 1 < argc) soundfont = argv[++i];
            else if (std::string(argv[i]) == "--resources" && i + 1 < argc) resources = argv[++i];
            else if (std::string(argv[i]) == "--export-mode=live") live_export = true;
            else if (std::string(argv[i]) == "--export-mode=isolated") live_export = false;
            else throw std::invalid_argument("Usage: engrave-mcp [--soundfont FILE.sf2|FILE.sf3] [--resources DIR] [--export-mode=live|isolated]");
        }
        engrave::EngineOptions options;
        options.resources = resources.empty() ? music::current_executable_path().parent_path() / "assets"
                                              : std::filesystem::absolute(std::filesystem::u8path(resources));
        if (!soundfont.empty()) options.soundfonts.push_back(std::filesystem::absolute(std::filesystem::u8path(soundfont)));
        music::AsyncSession session(std::move(options), live_export);
        return music::serve_stdio_async([] {
            auto tools = music::v2::tools_json();
#ifdef MUSIC_ASYNC_TEST_HOOKS
            tools.push_back({{"name", "test_owner_state"}, {"description", "Test-only owner-thread state"},
                             {"inputSchema", {{"type", "object"}, {"additionalProperties", false}}}});
            tools.push_back({{"name", "test_owner_sleep"}, {"description", "Test-only owner-thread stall"},
                             {"inputSchema", {{"type", "object"}, {"additionalProperties", false}}}});
            tools.push_back({{"name", "test_capture_count"}, {"description", "Test-only capture counter"},
                             {"inputSchema", {{"type", "object"}, {"additionalProperties", false}}}});
            tools.push_back({{"name", "test_capture_state"}, {"description", "Test-only capture polling state"},
                             {"inputSchema", {{"type", "object"}, {"additionalProperties", false}}}});
#endif
            return tools;
        },
            [&](std::string name, music::Json args, std::shared_ptr<std::atomic<int>> execution) {
                return session.call(std::move(name), std::move(args), std::move(execution));
            },
            [&] {
                session.shutdown();
            }, live_export);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
