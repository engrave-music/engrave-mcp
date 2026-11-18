#include "server_main.h"
#include "test_hooks.h"

int main(int argc, char **argv) {
    music::enable_test_hooks();
    return music::run_server(argc, argv);
}
