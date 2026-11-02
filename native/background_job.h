#pragma once

#include <filesystem>
#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>

namespace music {
// Canonical form used for export destinations. Rejects a
// missing, "." or ".." filename and requires an existing parent directory.
// Returns canonical(path) when the file already exists, otherwise
// canonical(parent) / filename.
std::filesystem::path normalize_output_path(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path current_executable_path();
// A directory-sync failure occurs after visibility commits. Callers must
// include that destination in published_artifacts even though durability failed.
class PublicationError : public std::system_error {
public:
    PublicationError(int code, const char* message, bool visible)
        : std::system_error(code, std::generic_category(), message), visible(visible) {}
    bool visible;
};
// POSIX: sync staged contents, rename, then sync destination directory.
void publish_output(const std::filesystem::path& staged, const std::filesystem::path& destination,
    const std::function<void()>& on_visible = {});
// Creates a fresh directory under parent with an unpredictable name (and
// owner-only permissions on POSIX).
std::filesystem::path create_private_directory(const std::filesystem::path& parent);

} // namespace music
