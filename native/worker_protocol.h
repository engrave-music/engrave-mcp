#pragma once
#include "export_options.h"
#include "music_core.h"
#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

namespace music {
struct ExportWorkerRequest {
    std::filesystem::path snapshot_path;
    std::filesystem::path output_path;
    std::filesystem::path result_path;
    RuntimeOptions runtime;
    ExportFormat format;
    ExportOptions options;
    // Source document stamps; the snapshot reload would otherwise report the
    // writing program instead of the file's own version.
    ScoreVersions versions;
    std::filesystem::path cache_directory;
    AudioMix mix;
    bool has_mix = false;
};
bool is_audio_format(ExportFormat format) noexcept;
std::string path_utf8(const std::filesystem::path& path);
ExportOptions parse_export_options(const nlohmann::json& args, ExportFormat format);
// Strict decoder for the score_export/score_export_start "mix" option.
AudioMixOverride parse_audio_mix_override(const nlohmann::json& json);
nlohmann::json worker_request_json(const ExportWorkerRequest& request);
int run_export_worker(const std::filesystem::path& request_path) noexcept;
// Throws if bytes exceed the supplied bound; never allocates from untrusted file size.
std::string read_bounded_file(const std::filesystem::path& path, std::size_t limit);
}
