#include <cstdio>
#include <exception>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "registry.h"

int main() {
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 1;
#endif
    try {
        music::api::validate_registry();
        const std::string text = music::dump_json(music::api::tools_json(), 2) + '\n';
        if (std::fwrite(text.data(), 1, text.size(), stdout) != text.size()) return 1;
        if (std::fflush(stdout) != 0) return 1;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "Unexpected error\n");
        return 1;
    }
}
