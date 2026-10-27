#include "engine.h"
#include "mcp_transport.h"
#include "async_session.h"
#include "background_job.h"
#include "worker_protocol.h"
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--worker")
            return music::run_export_worker(std::filesystem::absolute(std::filesystem::u8path(argv[2])));
#ifdef _WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
            throw std::runtime_error("Cannot configure binary stdio");
#endif
        std::string soundfont, resources;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--soundfont" && i + 1 < argc) soundfont = argv[++i];
            else if (std::string(argv[i]) == "--resources" && i + 1 < argc) resources = argv[++i];
            else throw std::invalid_argument("Usage: musescore-mcp [--soundfont FILE.sf2|FILE.sf3] [--resources DIR]");
        }
        music::AsyncSession session(music::RuntimeOptions{resources, soundfont}, music::current_executable_path());
        return music::serve_stdio_async([] {
            auto tools = music::Json::parse(music::Engine::tools_json());
#ifdef MUSIC_ASYNC_TEST_HOOKS
            tools.push_back({{"name", "test_owner_state"}, {"description", "Test-only owner-thread state"},
                             {"inputSchema", {{"type", "object"}, {"additionalProperties", false}}}});
            tools.push_back({{"name", "test_owner_sleep"}, {"description", "Test-only owner-thread stall"},
                             {"inputSchema", {{"type", "object"}, {"additionalProperties", false}}}});
            tools.push_back({{"name", "test_poison_document"}, {"description", "Test-only poison document"},
                             {"inputSchema", {{"type", "object"},
                                              {"properties", {{"documentId", {{"type", "string"}}}}},
                                              {"required", {"documentId"}},
                                              {"additionalProperties", false}}}});
#endif
            return tools;
        },
            [&](std::string name, music::Json args) { return session.call(std::move(name), std::move(args)); },
            [&] { session.shutdown(); });
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
