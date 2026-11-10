#include <exception>
#include <iostream>

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
        std::cout << music::dump_json(music::api::tools_json(), 2) << '\n';
        if (!std::cout) return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
