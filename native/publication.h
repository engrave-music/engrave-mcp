#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>

namespace music {

// Pure platform syntax check, before metadata probes or directory creation.
void validate_output_path_syntax(const std::filesystem::path &path);

// Missing paths return not_found; non-directory components are invalid inputs.
// Other filesystem errors propagate as system_error (API code Io).
std::filesystem::file_status output_path_status(const std::filesystem::path &path);

// Create missing directories recursively, after the caller validates syntax and encoding.
// Creation errors are Io; any parents already created may remain.
void require_output_directory(const std::filesystem::path &path);

// Canonical form used for export destinations. Rejects a
// missing, "." or ".." filename. Does not create directories.
// Returns canonical(path) when the file already exists, otherwise
// weakly_canonical(absolute(path)), resolving existing prefixes.
std::filesystem::path normalize_output_path(const std::filesystem::path &path);

[[nodiscard]] std::filesystem::path current_executable_path();

// A directory-sync failure occurs after visibility commits. Callers must
// include that destination in published_artifacts even though durability failed.
struct PublicationError : std::system_error {
    PublicationError(int code, const char *message, bool visible)
        : std::system_error(code, std::generic_category(), message), visible(visible) {}

    bool visible;
};

// POSIX: sync staged contents, rename, then sync destination directory.
void publish_output(const std::filesystem::path &staged, const std::filesystem::path &destination,
                    const std::function<void()> &on_visible = {});

// Creates a fresh directory under parent with an unpredictable name (and
// owner-only permissions on POSIX).
std::filesystem::path create_private_directory(const std::filesystem::path &parent);

} // namespace music
