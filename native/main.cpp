#include "engine.h"
#include "mcp_transport.h"
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
    try {
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
        music::Engine engine(argc, argv, soundfont, resources);
        return music::serve_stdio([] { return music::Json::parse(music::Engine::tools_json()); },
            [&](const std::string& name, const music::Json& args) {
                return music::Json::parse(engine.call(name, args.dump()));
            });
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
