// Linux durability ordering and error visibility; link with --wrap=fsync.
#include "publication.h"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>

namespace {

int g_calls = 0;
int g_fail_at = 0;
std::filesystem::path g_root;

} // namespace

extern "C" int __real_fsync(int); // NOLINT(readability-identifier-naming)

extern "C" int __wrap_fsync(int fd) { // NOLINT(readability-identifier-naming)
    struct stat info{};
    if (::fstat(fd, &info)) throw std::runtime_error("fstat");
    ++g_calls;
    if ((g_calls == 1 && !S_ISREG(info.st_mode)) || (g_calls == 2 && !S_ISDIR(info.st_mode))) throw std::runtime_error("sync order");
    if (g_calls == g_fail_at) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

TEST(Publication, FsyncOrderingAndErrorVisibility) {
    ASSERT_FALSE(g_root.empty()) << "pass scratch directory as positional argument";
    std::filesystem::create_directories(g_root);
    const auto staged = g_root / "staged", dest = g_root / "destination";
    for (int failure : {0, 1, 2}) {
        {
            std::ofstream(staged) << "new";
            std::ofstream(dest) << "old";
        }
        g_calls = 0;
        g_fail_at = failure;
        if (failure == 0) {
            EXPECT_NO_THROW(music::publish_output(staged, dest));
        } else {
            try {
                music::publish_output(staged, dest);
                ADD_FAILURE() << "expected PublicationError for failure=" << failure;
            } catch (const music::PublicationError &e) {
                EXPECT_EQ(e.code().value(), EIO);
                EXPECT_EQ(e.visible, failure == 2);
            }
        }
        EXPECT_EQ(g_calls, failure == 1 ? 1 : 2);
        EXPECT_EQ(std::filesystem::exists(staged), failure == 1);
        std::string contents;
        std::ifstream(dest) >> contents;
        EXPECT_EQ(contents, failure == 1 ? "old" : "new");
    }
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    if (argc == 2) g_root = std::filesystem::path(argv[1]);
    return RUN_ALL_TESTS();
}
#else
int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
#endif
