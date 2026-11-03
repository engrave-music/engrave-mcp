// Linux durability ordering and error visibility; link with --wrap=fsync.
#include "publication.h"
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

static int calls, fail_at;
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    struct stat info{};
    if (::fstat(fd, &info)) throw std::runtime_error("fstat");
    ++calls;
    if ((calls == 1 && !S_ISREG(info.st_mode)) || (calls == 2 && !S_ISDIR(info.st_mode)))
        throw std::runtime_error("sync order");
    if (calls == fail_at) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
static void require(bool ok) { if (!ok) throw std::runtime_error("publication assertion"); }
int main(int argc, char** argv) {
    require(argc == 2);
    const auto root = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(root);
    const auto staged = root/"staged", dest = root/"destination";
    for (int failure : {0, 1, 2}) {
        { std::ofstream(staged) << "new"; std::ofstream(dest) << "old"; }
        calls = 0; fail_at = failure;
        try { music::publish_output(staged, dest); require(failure == 0); }
        catch (const music::PublicationError& e) {
            require(failure != 0 && e.code().value() == EIO && e.visible == (failure == 2));
        }
        require(calls == (failure == 1 ? 1 : 2));
        require(std::filesystem::exists(staged) == (failure == 1));
        std::string contents; std::ifstream(dest) >> contents;
        require(contents == (failure == 1 ? "old" : "new"));
    }
    std::cout << "publication sync ordering, pre-rename and post-rename failures passed\n";
}
