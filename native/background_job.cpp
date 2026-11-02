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
#include <aclapi.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <fcntl.h>
#include <sys/stat.h>
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
struct LocalSecurity {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    ~LocalSecurity() { if (descriptor) LocalFree(descriptor); }
};
// A protected DACL that grants only the current user full control and lets
// children inherit it. Without this, a staging directory created under a
// permissive destination inherits Authenticated Users:Modify on Windows.
struct PrivateDirectorySecurity {
    std::vector<std::byte> token_buffer;
    std::vector<std::byte> acl_buffer;
    SECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{};

    PrivateDirectorySecurity() {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            windows_error("OpenProcessToken failed");
        DWORD needed = 0;
        if (!GetTokenInformation(token, TokenUser, nullptr, 0, &needed) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            const auto error = GetLastError();
            CloseHandle(token);
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "GetTokenInformation failed");
        }
        token_buffer.resize(needed);
        if (!GetTokenInformation(token, TokenUser, token_buffer.data(), needed, &needed)) {
            const auto error = GetLastError();
            CloseHandle(token);
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "GetTokenInformation failed");
        }
        CloseHandle(token);
        const auto* user = reinterpret_cast<const TOKEN_USER*>(token_buffer.data());
        const auto acl_size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) -
                              sizeof(DWORD) + GetLengthSid(user->User.Sid));
        acl_buffer.resize(acl_size);
        auto* acl = reinterpret_cast<ACL*>(acl_buffer.data());
        if (!InitializeAcl(acl, acl_size, ACL_REVISION)) windows_error("InitializeAcl failed");
        if (!AddAccessAllowedAceEx(acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                   GENERIC_ALL, user->User.Sid))
            windows_error("AddAccessAllowedAceEx failed");
        if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION))
            windows_error("InitializeSecurityDescriptor failed");
        if (!SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE))
            windows_error("SetSecurityDescriptorDacl failed");
        if (!SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
            windows_error("SetSecurityDescriptorControl failed");
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = &descriptor;
        attributes.bInheritHandle = FALSE;
    }
};
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
#ifdef _WIN32
    PrivateDirectorySecurity security;
    for (int i = 0; i != 16; ++i) {
        auto path = parent / (".engrave-stage-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (CreateDirectoryW(path.c_str(), &security.attributes)) return path;
        const auto error = GetLastError();
        if (error == ERROR_ALREADY_EXISTS) continue;
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "Cannot create staging directory");
    }
#else
    for (int i = 0; i != 16; ++i) {
        auto path = parent / (".engrave-stage-" + std::to_string(random()) + "-" + std::to_string(random()));
        // Create at the final mode atomically; a create-then-chmod window would
        // let another local process enter the staging directory.
        if (::mkdir(path.c_str(), 0700) == 0) {
            if (::chmod(path.c_str(), 0700) != 0) {
                const auto error = errno;
                ::rmdir(path.c_str());
                throw std::system_error(error, std::generic_category(),
                                        "Cannot restrict staging directory permissions");
            }
            return path;
        }
        if (errno == EEXIST) continue;
        throw std::system_error(errno, std::generic_category(), "Cannot create staging directory");
    }
#endif
    throw std::runtime_error("Cannot allocate unique staging directory");
}

void validate_output_path_syntax(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto& text = path.native();
    auto separator = [](wchar_t c) { return c == L'/' || c == L'\\'; };
    auto reject = [] { throw std::invalid_argument("invalid Windows destination filename syntax"); };
    if (text.empty()) reject();
    const bool unc = text.size() >= 2 && separator(text[0]) && separator(text[1]);
    if (unc) {
        // Device namespaces (\\., \\?) are not file destinations.
        if (text.size() >= 4 && (text[2] == L'.' || text[2] == L'?') && separator(text[3])) reject();
        // A UNC destination requires a non-empty server and share.
        const auto rest = text.substr(2);
        const auto first = rest.find_first_of(L"/\\");
        if (first == std::wstring::npos || first + 1 >= rest.size()) reject();
        const auto share_end = rest.find_first_of(L"/\\", first + 1);
        const auto share = rest.substr(first + 1, share_end == std::wstring::npos ? share_end : share_end - first - 1);
        if (share == L"." || share == L"..") reject();
    }
    std::size_t start = 0;
    // Only the drive designator and root separators are syntax, not filenames.
    if (text.size() >= 2 && ((text[0] >= L'A' && text[0] <= L'Z') ||
                            (text[0] >= L'a' && text[0] <= L'z')) && text[1] == L':') start = 2;
    if (start < text.size() && separator(text[start])) {
        ++start;
        if (start == 1 && start < text.size() && separator(text[start])) ++start; // UNC
    }
    if (start == text.size()) return; // root directory
    std::size_t component_index = 0;
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
            // The UNC server is a hostname, not a filename: a DNS name such as
            // con.example.com is valid even though a file named con is reserved.
            const bool unc_server = unc && component_index == 0;
            if (!unc_server &&
                (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
                 (base.size() == 4 && (base.substr(0, 3) == L"COM" || base.substr(0, 3) == L"LPT") &&
                  device_digit(base[3])))) reject();
        }
        ++component_index;
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
    // on_visible moves the Artifact that owns [destination]; keep a stable copy.
    const auto destination_path = destination;
    // FlushFileBuffers makes staged contents durable before the name is visible
    // (subject to the filesystem/device honoring flushes). WRITE_THROUGH is kept,
    // but its documented copy/delete guarantee does not establish directory-entry
    // power-loss durability for this same-volume rename. No such promise is made.
    Handle file(CreateFileW(staged.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) windows_error("Cannot open staged output for flush");
    if (!FlushFileBuffers(file.value)) windows_error("Cannot flush staged output");
    file.reset(); // Close before rename; a failed flush never replaces destination.
    // Capture the DACL a normal destination has: the existing file's DACL, or
    // the ACL a new file inherits from the destination directory. A rename
    // preserves the staged file's private DACL, so restore this one once the
    // name is visible.
    LocalSecurity acl;
    if (GetFileAttributesW(destination_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const auto result = GetNamedSecurityInfoW(destination_path.c_str(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl.dacl, nullptr, &acl.descriptor);
        if (result != ERROR_SUCCESS)
            throw std::system_error(static_cast<int>(result), std::system_category(),
                                    "Cannot inspect publication ACL");
    } else {
        const auto missing = GetLastError();
        if (missing != ERROR_FILE_NOT_FOUND && missing != ERROR_PATH_NOT_FOUND)
            throw std::system_error(static_cast<int>(missing), std::system_category(),
                                    "Cannot inspect publication destination");
        auto parent = destination_path.parent_path();
        if (parent.empty()) parent = ".";
        std::random_device random;
        bool created = false;
        for (int attempt = 0; attempt != 16 && !created; ++attempt) {
            const auto probe = parent / (".engrave-acl-" + std::to_string(random()) +
                                         "-" + std::to_string(random()));
            Handle handle(CreateFileW(probe.c_str(), READ_CONTROL | DELETE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
            if (handle.value == INVALID_HANDLE_VALUE) {
                if (GetLastError() == ERROR_FILE_EXISTS) continue;
                windows_error("Cannot create publication ACL probe");
            }
            const auto result = GetSecurityInfo(handle.value, SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl.dacl, nullptr, &acl.descriptor);
            handle.reset(); // delete-on-close removes the probe
            if (result != ERROR_SUCCESS)
                throw std::system_error(static_cast<int>(result), std::system_category(),
                                        "Cannot inspect publication ACL");
            created = true;
        }
        if (!created) throw std::runtime_error("Cannot allocate publication ACL probe");
    }
    if (!MoveFileExW(staged.c_str(), destination_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        windows_error("Cannot publish output");
    if (on_visible) on_visible();
    Handle published(CreateFileW(destination_path.c_str(), WRITE_DAC | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (published.value == INVALID_HANDLE_VALUE) windows_error("Cannot open published output for ACL");
    const auto result = SetSecurityInfo(published.value, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl.dacl, nullptr);
    if (result != ERROR_SUCCESS)
        throw std::system_error(static_cast<int>(result), std::system_category(),
                                "Cannot apply publication ACL");
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
