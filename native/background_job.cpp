#include "background_job.h"
#include "publication_gate.h"
#include <cstdlib>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cerrno>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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

std::filesystem::path create_private_directory(const std::filesystem::path& parent) {
    std::random_device random;
    for (int i = 0; i != 16; ++i) {
        auto path = parent / (".engrave-stage-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::error_code error;
        if (std::filesystem::create_directory(path, error)) {
#ifndef _WIN32
            std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
            if (error) {
                std::filesystem::remove(path);
                throw std::system_error(error, "Cannot restrict staging directory permissions");
            }
#endif
            return path;
        }
        if (error && error != std::errc::file_exists)
            throw std::system_error(error, "Cannot create staging directory");
    }
    throw std::runtime_error("Cannot allocate unique staging directory");
}

void validate_output_path_syntax(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto& text = path.native();
    auto separator = [](wchar_t c) { return c == L'/' || c == L'\\'; };
    auto reject = [] { throw std::invalid_argument("invalid Windows destination filename syntax"); };
    if (text.empty()) reject();
    std::size_t start = 0;
    // Only the drive designator and root separators are syntax, not filenames.
    if (text.size() >= 2 && ((text[0] >= L'A' && text[0] <= L'Z') ||
                            (text[0] >= L'a' && text[0] <= L'z')) && text[1] == L':') start = 2;
    if (start < text.size() && separator(text[start])) {
        ++start;
        if (start == 1 && start < text.size() && separator(text[start])) ++start; // UNC
    }
    if (start == text.size()) return; // root directory
    for (;;) {
        const auto end = text.find_first_of(L"/\\", start);
        const auto component = text.substr(start, end == std::wstring::npos ? end : end - start);
        if (component.empty()) reject();
        if (component != L"." && component != L"..") {
            if (component.back() == L'.' || component.back() == L' ') reject();
            for (auto c : component)
                if (c < 32 || std::wstring_view(L"<>:\"|?*").find(c) != std::wstring_view::npos) reject();
            auto base = component.substr(0, component.find(L'.'));
            for (auto& c : base) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
            // Windows also treats the ISO-8859-1 superscript digits as device
            // digits: COM¹/²/³ and LPT¹/²/³ are reserved exactly like COM1–9.
            auto device_digit = [](wchar_t c) {
                return (c >= L'1' && c <= L'9') || c == L'\u00B9' || c == L'\u00B2' || c == L'\u00B3';
            };
            if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
                (base.size() == 4 && (base.substr(0, 3) == L"COM" || base.substr(0, 3) == L"LPT") &&
                 device_digit(base[3]))) reject();
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
#else
    (void)path; // POSIX component names have no additional restrictions.
#endif
}

std::filesystem::file_status output_path_status(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (error == std::errc::not_a_directory)
        throw std::invalid_argument("destination contains a non-directory component");
    if (error == std::errc::no_such_file_or_directory)
        return std::filesystem::file_status(std::filesystem::file_type::not_found);
    if (error) throw std::filesystem::filesystem_error("Cannot inspect export destination", path, error);
    return status;
}

void require_output_directory(const std::filesystem::path& path) {
    const auto status = output_path_status(path);
    if (std::filesystem::exists(status)) {
        if (!std::filesystem::is_directory(status))
            throw std::invalid_argument("destination must be a directory");
        return;
    }
    // Windows may report PATH_NOT_FOUND for a child of a regular file.
    // Inspect the missing chain before creation so wrong types stay inputs,
    // while access and other metadata errors still propagate as Io.
    for (auto parent = path.parent_path(); !parent.empty();) {
        const auto ancestor = output_path_status(parent);
        if (std::filesystem::exists(ancestor)) {
            if (!std::filesystem::is_directory(ancestor))
                throw std::invalid_argument("destination contains a non-directory component");
            break;
        }
        const auto next = parent.parent_path();
        if (next == parent) break;
        parent = next;
    }
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error)
        throw std::filesystem::filesystem_error(
            "Cannot create export directory; created parents may remain", path, error);
}

std::filesystem::path normalize_output_path(const std::filesystem::path& path) {
    if (path.empty()) throw std::invalid_argument("Export path is required");
    const auto filename = path.filename();
    if (filename.empty() || filename == "." || filename == "..")
        throw std::invalid_argument("Export path must name a file");
    if (std::filesystem::exists(output_path_status(path)))
        return std::filesystem::canonical(path);
    return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}

void publish_output(const std::filesystem::path& staged, const std::filesystem::path& destination,
    const std::function<void()>& on_visible) {
#ifdef MUSIC_ASYNC_TEST_HOOKS
    if (std::getenv("ENGRAVE_MCP_TEST_SLOW_SECOND_FLUSH") && staged.filename() == "page-0002.svg") {
        std::fprintf(stderr, "PUBLICATION_SECOND_FLUSH\n"); std::fflush(stderr);
        std::this_thread::sleep_for(std::chrono::seconds(7));
    }
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
    if (on_visible) on_visible();
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
    if (on_visible) on_visible();
    sync(directory.fd, "Cannot sync publication directory", true);
#endif
}
}
