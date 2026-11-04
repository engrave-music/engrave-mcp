// Direct regression for the private staging and publication ACL contract.
#include "publication.h"
#include "utf8_text.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

#ifdef _WIN32
static std::set<std::wstring> dacl_sids(const fs::path &path, bool *protected_dacl = nullptr) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result =
        GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) throw std::system_error(static_cast<int>(result), std::system_category(), "GetNamedSecurityInfoW failed");
    struct FreeDescriptor {
        PSECURITY_DESCRIPTOR value;
        ~FreeDescriptor() {
            if (value) LocalFree(value);
        }
    } free_descriptor{descriptor};
    if (!dacl) return {};

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    require(GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE, "GetSecurityDescriptorControl failed");
    if (protected_dacl) *protected_dacl = (control & SE_DACL_PROTECTED) != 0;

    ACL_SIZE_INFORMATION size{};
    require(GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) != FALSE, "GetAclInformation failed");
    std::set<std::wstring> sids;
    for (DWORD i = 0; i < size.AceCount; ++i) {
        LPVOID ace = nullptr;
        require(GetAce(dacl, i, &ace) != FALSE, "GetAce failed");
        const auto *header = static_cast<const ACE_HEADER *>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        auto *allowed = static_cast<ACCESS_ALLOWED_ACE *>(ace);
        LPWSTR raw = nullptr;
        require(ConvertSidToStringSidW(reinterpret_cast<PSID>(&allowed->SidStart), &raw) != FALSE, "ConvertSidToStringSidW failed");
        std::wstring sid(raw);
        LocalFree(raw);
        sids.insert(std::move(sid));
    }
    return sids;
}

static std::wstring dacl_sddl(const fs::path &path) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result =
        GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) throw std::system_error(static_cast<int>(result), std::system_category(), "GetNamedSecurityInfoW failed");
    struct FreeDescriptor {
        PSECURITY_DESCRIPTOR value;
        ~FreeDescriptor() {
            if (value) LocalFree(value);
        }
    } free_descriptor{descriptor};
    LPWSTR text = nullptr;
    require(ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr) !=
                FALSE,
            "ConvertSecurityDescriptorToStringSecurityDescriptorW failed");
    std::wstring sddl(text);
    LocalFree(text);
    return sddl;
}

static void check_private_acl(const fs::path &stage, bool require_protected = true) {
    bool protected_dacl = false;
    const auto sids = dacl_sids(stage, &protected_dacl);
    require(!sids.empty(), "staging directory has no access ACE");
    require((protected_dacl || !require_protected), "staging DACL still inherits parent ACEs");
    require(!sids.contains(L"S-1-1-0"), "staging grants Everyone");
    require(!sids.contains(L"S-1-5-11"), "staging grants Authenticated Users");
    require(!sids.contains(L"S-1-5-32-545"), "staging grants BUILTIN\\Users");
}

static void grant_users_full(const fs::path &path, bool protect = false) {
    PSID users = nullptr;
    require(ConvertStringSidToSidW(L"S-1-5-32-545", &users) != FALSE, "ConvertStringSidToSidW failed");
    struct FreeSid {
        PSID value;
        ~FreeSid() {
            if (value) LocalFree(value);
        }
    } free_sid{users};
    const auto acl_size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(users));
    std::vector<std::byte> buffer(acl_size);
    auto *acl = reinterpret_cast<ACL *>(buffer.data());
    require(InitializeAcl(acl, acl_size, ACL_REVISION) != FALSE, "InitializeAcl failed");
    require(AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, users) != FALSE, "AddAccessAllowedAce failed");
    const SECURITY_INFORMATION information =
        DACL_SECURITY_INFORMATION | (protect ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);
    const auto result =
        SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, information, nullptr, nullptr, acl, nullptr);
    require(result == ERROR_SUCCESS, "SetNamedSecurityInfoW failed");
}

static void check_publication_acl(const fs::path &parent) {
    const auto directory = parent / "publication";
    fs::create_directories(directory);
    const auto reference = directory / "reference.pdf";
    { std::ofstream(reference, std::ios::binary) << "reference"; }
    const auto existing = directory / "existing.pdf";
    { std::ofstream(existing, std::ios::binary) << "old"; }
    grant_users_full(existing);
    require(dacl_sids(existing).contains(L"S-1-5-32-545"), "test ACE was not set");
    const auto ordinary_before = dacl_sddl(existing);

    const auto stage = music::create_private_directory(directory);
    { std::ofstream(stage / "new", std::ios::binary) << "new"; }
    music::publish_output(stage / "new", directory / "new.pdf");
    require(dacl_sddl(directory / "new.pdf") == dacl_sddl(reference), "new publication DACL differs from a normally created file");

    { std::ofstream(stage / "replacement", std::ios::binary) << "replacement"; }
    music::publish_output(stage / "replacement", existing);
    require(dacl_sddl(existing) == ordinary_before, "replacement publication changed the ordinary destination DACL");

    const auto protected_existing = directory / "protected.pdf";
    { std::ofstream(protected_existing, std::ios::binary) << "old protected"; }
    grant_users_full(protected_existing, true);
    const auto protected_before = dacl_sddl(protected_existing);
    { std::ofstream(stage / "protected", std::ios::binary) << "new protected"; }
    music::publish_output(stage / "protected", protected_existing);
    require(dacl_sddl(protected_existing) == protected_before, "protected replacement publication changed the destination DACL");
}

static void check_path_grammar() {
    music::validate_output_path_syntax(fs::path(L"\\\\con.example.com\\share\\out.pdf"));
    music::validate_output_path_syntax(fs::path(L"\\\\server\\share"));
    for (const wchar_t *text : {L"\\\\.\\device\\out.pdf", L"\\\\", L"\\\\server"}) {
        bool rejected = false;
        try {
            music::validate_output_path_syntax(fs::path(text));
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "invalid UNC/device form was accepted");
    }
}
#else
static void check_private_acl(const fs::path &stage, bool = true) {
    const auto permissions = fs::status(stage).permissions();
    require((permissions & fs::perms::owner_all) == fs::perms::owner_all, "staging owner permissions are incomplete");
    require((permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none, "staging is accessible to group or others");
}
#endif

static void check_utf8_prefix() {
    std::string text(1023, 'a');
    text += "\xF0\x9F\x98\x80"; // U+1F600, four bytes
    const auto cut = music::utf8_prefix(text, 1024);
    require(cut == 1023, "UTF-8 prefix split a code point");
    require(text.substr(0, cut).size() == 1023, "UTF-8 prefix length mismatch");
}

int main() {
    try {
        std::random_device random;
        const auto parent =
            fs::temp_directory_path() / ("engrave-staging-test-" + std::to_string(random()) + "-" + std::to_string(random()));
        fs::create_directories(parent);
        try {
#ifndef _WIN32
            const auto previous_umask = ::umask(0);
#endif
            const auto stage = music::create_private_directory(parent);
#ifndef _WIN32
            ::umask(previous_umask);
#endif
            check_private_acl(stage);
            check_utf8_prefix();
            {
                std::ofstream probe(stage / "probe", std::ios::binary);
                probe << "probe";
            }
            require(fs::exists(stage / "probe"), "cannot write inside the staging directory");
#ifdef _WIN32
            check_private_acl(stage / "probe", false);
            check_publication_acl(parent);
            check_path_grammar();
#endif
        } catch (...) {
            fs::remove_all(parent);
            throw;
        }
        fs::remove_all(parent);
        std::cout << "private staging ACL/mode and publication ACL passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
