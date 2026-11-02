// Direct regression for the private staging directory contract.
#include "background_job.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

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

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#ifdef _WIN32
static void check_private_acl(const fs::path& stage, bool require_protected = true) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto result = GetNamedSecurityInfoW(stage.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS)
        throw std::system_error(static_cast<int>(result), std::system_category(),
                                "GetNamedSecurityInfoW failed");
    struct FreeDescriptor {
        PSECURITY_DESCRIPTOR value;
        ~FreeDescriptor() { if (value) LocalFree(value); }
    } free_descriptor{descriptor};
    require(dacl != nullptr, "staging directory has no DACL");

    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    require(GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE,
            "GetSecurityDescriptorControl failed");
    require((control & SE_DACL_PROTECTED) != 0 || !require_protected,
            "staging DACL still inherits parent ACEs");

    ACL_SIZE_INFORMATION size{};
    require(GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) != FALSE,
            "GetAclInformation failed");
    std::set<std::wstring> sids;
    for (DWORD i = 0; i < size.AceCount; ++i) {
        LPVOID ace = nullptr;
        require(GetAce(dacl, i, &ace) != FALSE, "GetAce failed");
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        LPWSTR raw = nullptr;
        require(ConvertSidToStringSidW(reinterpret_cast<PSID>(&allowed->SidStart), &raw) != FALSE,
                "ConvertSidToStringSidW failed");
        std::wstring sid(raw);
        LocalFree(raw);
        sids.insert(std::move(sid));
    }
    require(!sids.empty(), "staging directory has no explicit access ACE");
    require(!sids.contains(L"S-1-1-0"), "staging grants Everyone");
    require(!sids.contains(L"S-1-5-11"), "staging grants Authenticated Users");
    require(!sids.contains(L"S-1-5-32-545"), "staging grants BUILTIN\\Users");
}
#else
static void check_private_acl(const fs::path& stage, bool = true) {
    const auto permissions = fs::status(stage).permissions();
    require((permissions & fs::perms::owner_all) == fs::perms::owner_all,
            "staging owner permissions are incomplete");
    require((permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
            "staging is accessible to group or others");
}
#endif

int main() {
    try {
        std::random_device random;
        const auto parent = fs::temp_directory_path() /
            ("engrave-staging-test-" + std::to_string(random()) + "-" + std::to_string(random()));
        fs::create_directories(parent);
        try {
            const auto stage = music::create_private_directory(parent);
            check_private_acl(stage);
            { std::ofstream probe(stage / "probe", std::ios::binary); probe << "probe"; }
            require(fs::exists(stage / "probe"), "cannot write inside the staging directory");
#ifdef _WIN32
            check_private_acl(stage / "probe", false);
#endif
        } catch (...) {
            fs::remove_all(parent);
            throw;
        }
        fs::remove_all(parent);
        std::cout << "private staging ACL/mode passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
