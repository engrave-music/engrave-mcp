#include "api_output.h"
#include "host_api.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace music::v2;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string path_text(const fs::path& path) {
    auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
ExportRequest request(const std::string& doc, const char* kind, Json options, bool directory = false) {
    Json j = {{"document", doc}, {"action", "start"}, {"kind", kind}, {"options", std::move(options)},
              {"destination", {{"kind", directory ? "directory" : "file"}, {"path", "output"}}}};
    auto decoded = decode_export(j);
    require(bool(decoded), decoded ? "" : decoded.error().message);
    return std::move(*decoded);
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: export_job_test ASSETS SOUNDFONT");
        engrave::EngineOptions options;
        options.resources = fs::absolute(fs::u8path(argv[1]));
        options.soundfonts = {fs::absolute(fs::u8path(argv[2]))};
        auto made = HostApi::create(options);
        require(bool(made), made ? "" : made.error().message);
        auto host = std::move(*made);
        auto opened = host->open({{"path", path_text(fs::absolute("fixtures/golden/linked_parts.mscx"))}});
        require(opened.at("ok").get<bool>(), opened.dump());
        const auto doc = opened.at("document").get<std::string>();
        auto runtime = host->capture_runtime(); require(bool(runtime), "cannot capture runtime");
        auto outline = host->outline({{"document", doc}});
        require(outline.at("ok").get<bool>() && !outline.at("outline").at("parts").empty(), outline.dump());
        const auto part = outline.at("outline").at("parts").at(0).at("id");
        auto edited = host->edit({{"document", doc}, {"ops", Json::array({Json{{"op", "generate_excerpts"},
            {"parts", Json::array({Json{{"kind", "id"}, {"id", part}}})}}})}});
        require(edited.at("ok").get<bool>(), edited.dump());
        const auto revision = std::stoull(edited.at("revision").get<std::string>());
        outline = host->outline({{"document", doc}});
        require(outline.at("ok").get<bool>() && !outline.at("outline").at("excerpts").empty(), "excerpt generation failed");
        const Json excerpt = {{"excerpt", outline.at("outline").at("excerpts").at(0).at("id")}};

        for (const Json& selection : {Json::object(), excerpt}) for (const char* format : {"mscz", "mscx", "musicxml", "mxl", "midi"}) {
            auto req = request(doc, "score", {{"format", format}, {"score", selection}});
            auto visited = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
                auto direct = document.export_score(req.score_options);
                if (!direct) return std::unexpected(api_error(direct.error()));
                auto captured = capture_export(document, *runtime, req);
                if (!captured) return std::unexpected(captured.error());
                require(captured->source_revision == revision && !captured->captured_score_bytes.empty(), "score capture is empty");
                require(captured->captured_score_bytes == *direct, std::string(format) + " score capture differs");
                return {};
            });
            require(bool(visited), visited ? "" : visited.error().message);
        }
        for (const Json& selection : {Json::object(), excerpt}) for (const char* kind : {"measure", "segment"}) {
            auto req = request(doc, "positions", {{"position_kind", kind}, {"score", selection}});
            auto visited = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
                auto direct = document.positions(req.position_kind, req.position_score);
                if (!direct) return std::unexpected(api_error(direct.error()));
                auto captured = capture_export(document, *runtime, req);
                if (!captured) return std::unexpected(captured.error());
                const auto& data = captured->captured_positions_json;
                const std::string json(reinterpret_cast<const char*>(data.data()), data.size());
                require(Json::parse(json) == encode(*direct), "positions capture differs");
                require(!direct->elements.empty() && !direct->events.empty(), "positions are empty");
                return {};
            });
            require(bool(visited), visited ? "" : visited.error().message);
        }
        auto audio_doc = host->open({{"path", path_text(fs::absolute("fixtures/audio/mix.mscz"))}});
        require(audio_doc.at("ok").get<bool>(), audio_doc.dump());
        const auto audio_id = audio_doc.at("document").get<std::string>();
        auto audio = request(audio_id, "audio", {{"format", "wav"}, {"master_balance", 0.25},
            {"tracks", Json::array({Json{{"track", 0}, {"volume_db", -3.0}, {"soundfont", path_text(runtime->options.soundfonts[0])}}})}});
        auto visited = host->with_document(audio_id, 1, [&](engrave::Document& document) -> Result<void> {
            auto defaults = document.audio_tracks();
            if (!defaults) return std::unexpected(api_error(defaults.error()));
            auto captured = capture_export(document, *runtime, audio);
            if (!captured) return std::unexpected(captured.error());
            require(captured->effective_audio.tracks.size() == defaults->tracks.size(), "mix track count differs");
            require(captured->effective_audio.master_volume_db == defaults->master.volume_db, "master default lost");
            require(captured->effective_audio.master_balance == 0.25, "master override lost");
            require(captured->effective_audio.tracks.at(0).volume_db == -3.0, "track override lost");
            require(std::get<std::int32_t>(captured->effective_audio.tracks.at(0).soundfont) == 0, "SoundFont path not resolved");
            require(!captured->mscz.empty(), "audio snapshot is empty");
            return {};
        });
        require(bool(visited), visited ? "" : visited.error().message);
        auto render = request(doc, "render", {{"format", "png"}, {"first_page", 0}}, true);
        visited = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
            auto captured = capture_export(document, *runtime, render);
            require(captured && !captured->mscz.empty(), "render snapshot failed");
            return {};
        });
        require(bool(visited), visited ? "" : visited.error().message);
        Json invalid = {{"document", doc}, {"action", "start"}, {"kind", "render"},
                        {"options", {{"format", "svg"}, {"dpi", 144}}},
                        {"destination", {{"kind", "directory"}, {"path", "output"}}}};
        require(!decode_export(invalid), "inapplicable option accepted");
        invalid["options"] = {{"format", "png"}}; invalid["destination"]["kind"] = "file";
        require(!decode_export(invalid), "wrong destination kind accepted");
        invalid["destination"]["kind"] = "directory"; invalid["expected_revision"] = "18446744073709551616";
        require(!decode_export(invalid), "overflow revision accepted");
        std::cout << "export_job_test capture checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "export_job_test: " << e.what() << '\n';
        return 1;
    }
}
