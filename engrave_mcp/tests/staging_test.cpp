// Direct regression for the private staging and publication ACL contract.
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <sys/stat.h>
#endif

#include "publication.h"
#include "utf8_text.h"

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
ankerl::unordered_dense::set<std::wstring> dacl_sids(const fs::path &path, bool *protected_dacl = nullptr) {
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
    EXPECT_NE(GetSecurityDescriptorControl(descriptor, &control, &revision), FALSE);
    if (protected_dacl) *protected_dacl = (control & SE_DACL_PROTECTED) != 0;

    ACL_SIZE_INFORMATION size{};
    EXPECT_NE(GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation), FALSE);
    ankerl::unordered_dense::set<std::wstring> sids;
    for (DWORD i = 0; i < size.AceCount; ++i) {
        LPVOID ace = nullptr;
        EXPECT_NE(GetAce(dacl, i, &ace), FALSE);
        const auto *header = static_cast<const ACE_HEADER *>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        auto *allowed = static_cast<ACCESS_ALLOWED_ACE *>(ace);
        LPWSTR raw = nullptr;
        EXPECT_NE(ConvertSidToStringSidW(reinterpret_cast<PSID>(&allowed->SidStart), &raw), FALSE);
        std::wstring sid(raw);
        LocalFree(raw);
        sids.insert(std::move(sid));
    }
    return sids;
}

std::wstring dacl_sddl(const fs::path &path) {
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

    // SE_DACL_AUTO_INHERITED ("AI") only records that inherited ACEs were propagated automatically. SetSecurityInfo
    // always sets it, while a normally created file gets it only when its parent has it (the GitHub Windows runner's
    // %TEMP% does not). It grants nothing, so compare ACEs and protection without it.
    EXPECT_NE(SetSecurityDescriptorControl(descriptor, SE_DACL_AUTO_INHERITED, 0), FALSE);

    LPWSTR text = nullptr;
    EXPECT_NE(ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr),
              FALSE);
    std::wstring sddl(text);
    LocalFree(text);
    return sddl;
}

void check_private_acl(const fs::path &stage, bool require_protected = true) {
    bool protected_dacl = false;
    const auto sids = dacl_sids(stage, &protected_dacl);
    EXPECT_FALSE(sids.empty()) << "staging directory has no access ACE";
    EXPECT_TRUE(protected_dacl || !require_protected) << "staging DACL still inherits parent ACEs";
    EXPECT_FALSE(sids.contains(L"S-1-1-0")) << "staging grants Everyone";
    EXPECT_FALSE(sids.contains(L"S-1-5-11")) << "staging grants Authenticated Users";
    EXPECT_FALSE(sids.contains(L"S-1-5-32-545")) << "staging grants BUILTIN\\Users";
}

void grant_users_full(const fs::path &path, bool protect = false) {
    PSID users = nullptr;
    ASSERT_NE(ConvertStringSidToSidW(L"S-1-5-32-545", &users), FALSE);

    struct FreeSid {
        PSID value;

        ~FreeSid() {
            if (value) LocalFree(value);
        }
    } free_sid{users};

    const auto acl_size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(users));
    std::vector<std::byte> buffer(acl_size);
    auto *acl = reinterpret_cast<ACL *>(buffer.data());
    ASSERT_NE(InitializeAcl(acl, acl_size, ACL_REVISION), FALSE);
    ASSERT_NE(AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, users), FALSE);
    const SECURITY_INFORMATION information =
        DACL_SECURITY_INFORMATION | (protect ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);
    const auto result =
        SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, information, nullptr, nullptr, acl, nullptr);
    ASSERT_EQ(result, ERROR_SUCCESS);
}

void check_publication_acl(const fs::path &parent) {
    const auto directory = parent / "publication";
    fs::create_directories(directory);
    const auto reference = directory / "reference.pdf";
    { std::ofstream(reference, std::ios::binary) << "reference"; }
    const auto existing = directory / "existing.pdf";
    { std::ofstream(existing, std::ios::binary) << "old"; }
    grant_users_full(existing);
    ASSERT_TRUE(dacl_sids(existing).contains(L"S-1-5-32-545"));
    const auto ordinary_before = dacl_sddl(existing);

    const auto stage = music::create_private_directory(directory);
    { std::ofstream(stage / "new", std::ios::binary) << "new"; }
    music::publish_output(stage / "new", directory / "new.pdf");
    EXPECT_EQ(dacl_sddl(directory / "new.pdf"), dacl_sddl(reference));

    { std::ofstream(stage / "replacement", std::ios::binary) << "replacement"; }
    music::publish_output(stage / "replacement", existing);
    EXPECT_EQ(dacl_sddl(existing), ordinary_before);

    const auto protected_existing = directory / "protected.pdf";
    { std::ofstream(protected_existing, std::ios::binary) << "old protected"; }
    grant_users_full(protected_existing, true);
    const auto protected_before = dacl_sddl(protected_existing);
    { std::ofstream(stage / "protected", std::ios::binary) << "new protected"; }
    music::publish_output(stage / "protected", protected_existing);
    EXPECT_EQ(dacl_sddl(protected_existing), protected_before);
}
#else
void check_private_acl(const fs::path &stage, bool = true) {
    const auto permissions = fs::status(stage).permissions();
    EXPECT_EQ(permissions & fs::perms::owner_all, fs::perms::owner_all);
    EXPECT_EQ(permissions & (fs::perms::group_all | fs::perms::others_all), fs::perms::none);
}
#endif

TEST(Staging, Utf8PrefixNeverSplitsCodePoint) {
    std::string text(1023, 'a');
    text += "\xF0\x9F\x98\x80"; // U+1F600, four bytes
    const auto cut = music::utf8_prefix(text, 1024);
    EXPECT_EQ(cut, 1023u);
}

#ifdef _WIN32
TEST(Staging, WindowsPathGrammar) {
    EXPECT_TRUE(music::validate_output_path_syntax(fs::path(L"\\\\con.example.com\\share\\out.pdf")));
    EXPECT_TRUE(music::validate_output_path_syntax(fs::path(L"\\\\server\\share")));
    for (const wchar_t *text : {L"\\\\.\\device\\out.pdf", L"\\\\", L"\\\\server"}) {
        const auto result = music::validate_output_path_syntax(fs::path(text));
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), "invalid Windows destination filename syntax");
    }
}
#endif

TEST(Staging, PrivateDirectoryAndPublicationAcl) {
    std::random_device random;
    const auto parent = fs::temp_directory_path() / ("engrave-staging-test-" + std::to_string(random()) + "-" + std::to_string(random()));
    fs::create_directories(parent);

    struct Cleanup {
        fs::path path;

        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{parent};

#ifndef _WIN32
    const auto previous_umask = ::umask(0);
#endif
    const auto stage = music::create_private_directory(parent);
#ifndef _WIN32
    ::umask(previous_umask);
#endif
    check_private_acl(stage);
    {
        std::ofstream probe(stage / "probe", std::ios::binary);
        probe << "probe";
    }
    ASSERT_TRUE(fs::exists(stage / "probe"));
#ifdef _WIN32
    check_private_acl(stage / "probe", false);
    check_publication_acl(parent);
#endif
}

} // namespace

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
