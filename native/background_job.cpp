#include "background_job.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace music {
namespace {
#ifdef _WIN32
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE v) : value(v) {}
    ~Handle() { reset(); }
    void reset() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = nullptr; }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
[[noreturn]] void windows_error(const char* what) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
}
#endif
}
std::filesystem::path current_executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!count) windows_error("GetModuleFileNameW failed");
        if (count < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), count));
        if (buffer.size() >= 65536) throw std::runtime_error("Executable path too long");
        buffer.resize(buffer.size() * 2);
    }
#else
    std::vector<char> buffer(512);
    for (;;) {
        const auto count = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (count < 0) throw std::system_error(errno, std::generic_category(), "readlink failed");
        if (static_cast<std::size_t>(count) < buffer.size()) return std::filesystem::path(std::string(buffer.data(), count));
        if (buffer.size() >= 65536) throw std::runtime_error("Executable path too long");
        buffer.resize(buffer.size() * 2);
    }
#endif
}

bool same_output_path(const std::filesystem::path& first, const std::filesystem::path& second) {
#ifdef _WIN32
    const auto a = first.lexically_normal().wstring(), b = second.lexically_normal().wstring();
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
#else
    return first.lexically_normal() == second.lexically_normal();
#endif
}

std::filesystem::path create_private_directory(const std::filesystem::path& parent) {
    std::random_device random;
    for (int i = 0; i != 16; ++i) {
        auto path = parent / (".musescore-job-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::error_code error;
        if (std::filesystem::create_directory(path, error)) {
#ifndef _WIN32
            std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
            if (error) {
                std::filesystem::remove(path);
                throw std::system_error(error, "Cannot restrict job directory permissions");
            }
#endif
            return path;
        }
        if (error && error != std::errc::file_exists)
            throw std::system_error(error, "Cannot create job directory");
    }
    throw std::runtime_error("Cannot allocate unique job directory");
}

std::filesystem::path normalize_output_path(const std::filesystem::path& path) {
    if (path.empty()) throw std::invalid_argument("Export path is required");
    const auto filename = path.filename();
    if (filename.empty() || filename == "." || filename == "..")
        throw std::invalid_argument("Export path must name a file");
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error)
        return std::filesystem::canonical(path);
    const auto absolute = std::filesystem::absolute(path);
    if (!std::filesystem::is_directory(absolute.parent_path(), error) || error)
        throw std::invalid_argument("Export parent directory must exist");
    return std::filesystem::canonical(absolute.parent_path()) / absolute.filename();
}

void publish_output(const std::filesystem::path& staged, const std::filesystem::path& destination) {
#ifdef MUSIC_ASYNC_TEST_HOOKS
    if (std::getenv("ENGRAVE_MCP_TEST_FAIL_FILE_FLUSH"))
        throw PublicationError(EIO, "Injected staged file flush failure", false);
#endif
#ifdef _WIN32
    // FlushFileBuffers makes staged contents durable before the name is visible
    // (subject to the filesystem/device honoring flushes). WRITE_THROUGH is kept,
    // but its documented copy/delete guarantee does not establish directory-entry
    // power-loss durability for this same-volume rename. No such promise is made.
    Handle file(CreateFileW(staged.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) windows_error("Cannot open staged output for flush");
    if (!FlushFileBuffers(file.value)) windows_error("Cannot flush staged output");
    file.reset(); // Close before rename; a failed flush never replaces destination.
    if (!MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        windows_error("Cannot publish output");
#else
    struct Descriptor {
        int fd;
        ~Descriptor() { if (fd >= 0) ::close(fd); }
    };
    auto parent = destination.parent_path();
    if (parent.empty()) parent = ".";
    Descriptor directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory.fd < 0)
        throw PublicationError(errno, "Cannot open publication directory", false);
    Descriptor file{::open(staged.c_str(), O_RDONLY | O_CLOEXEC)};
    if (file.fd < 0)
        throw PublicationError(errno, "Cannot open staged output", false);
    auto sync = [](int fd, const char* message, bool visible) {
        int result;
        do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
        if (result < 0) throw PublicationError(errno, message, visible);
    };
    sync(file.fd, "Cannot sync staged output", false);
    if (::rename(staged.c_str(), destination.c_str()) != 0)
        throw PublicationError(errno, "Cannot publish output", false);
    sync(directory.fd, "Cannot sync publication directory", true);
#endif
}
}
