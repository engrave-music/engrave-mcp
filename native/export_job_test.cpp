#include "api_output.h"
#include "audio_budget.h"
#include "host_api.h"
#include "export_job.h"
#include "export_timing.h"
#include "worker_v2.h"
#include "background_job.h"

#include <chrono>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

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
std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot read " + path_text(path));
    std::string data{std::istreambuf_iterator<char>(input), {}};
    const auto* begin = reinterpret_cast<const std::byte*>(data.data());
    return {begin, begin + data.size()};
}
ExportRequest request(const std::string& doc, const char* kind, Json options, bool directory = false) {
    Json j = {{"document", doc}, {"action", "start"}, {"kind", kind}, {"options", std::move(options)},
              {"destination", {{"kind", directory ? "directory" : "file"}, {"path", "output"}}}};
    auto decoded = decode_export(j);
    require(bool(decoded), decoded ? "" : decoded.error().message);
    return std::move(*decoded);
}
std::vector<std::vector<std::byte>> run_snapshot(const ExportSnapshot& snapshot) {
    const auto root = music::create_private_directory(fs::temp_directory_path());
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{root};
    const auto stage = root / "stage"; fs::create_directory(stage);
    const auto& payload = snapshot.request.kind == ExportKind::Score ? snapshot.captured_score_bytes :
                          snapshot.request.kind == ExportKind::Positions ? snapshot.captured_positions_json : snapshot.mscz;
    {
        std::ofstream f(root / "payload", std::ios::binary);
        f.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        require(bool(f), "cannot write worker payload");
    }
    {
        std::ofstream f(root / "request.json", std::ios::binary);
        f << worker_request(snapshot, root, stage, 1024ull * 1024 * 1024).dump();
        require(bool(f), "cannot write worker request");
    }
    music::WorkerProcess process(music::current_executable_path(), root / "request.json", root / "worker.log");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::optional<int> exit;
    while (!(exit = process.poll()) && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(exit.has_value(), "worker timed out");
    std::ifstream result_file(root / "result.json", std::ios::binary);
    require(bool(result_file), "worker result missing");
    Json result = Json::parse(result_file);
    require(*exit == 0 && result.value("ok", false), result.dump());
    std::vector<std::vector<std::byte>> result_bytes;
    for (const auto& artifact : result.at("artifacts")) {
        std::ifstream f(stage / artifact.at("name").get<std::string>(), std::ios::binary);
        require(bool(f), "worker artifact missing");
        std::string data{std::istreambuf_iterator<char>(f), {}};
        require(data.size() == artifact.at("size").get<std::size_t>(), "worker artifact size mismatch");
        const auto* first = reinterpret_cast<const std::byte*>(data.data());
        result_bytes.emplace_back(first, first + data.size());
    }
    return result_bytes;
}
bool done(JobState state) {
    return state == JobState::Succeeded || state == JobState::Failed || state == JobState::Cancelled;
}
JobStatus wait_job(ExportJobs& jobs, const std::string& id, bool running = false) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        auto status = jobs.get(id); require(bool(status), "job disappeared");
        if (done(status->state) || (running && status->state == JobState::Running)) {
            if (done(status->state))
                std::cout << "JOB_CASE " << Json{{"ok", true}, {"status", encode(*status)}}.dump() << '\n';
            return *status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("job did not reach expected state");
}
JobStatus submit(ExportJobs& jobs, ExportSnapshot snapshot, const fs::path& destination) {
    snapshot.request.destination.path = destination;
    auto result = jobs.submit(std::move(snapshot));
    require(bool(result), result ? "" : result.error().message);
    std::cout << "EXPORT_CASE " << Json{{"ok", true}, {"action", "start"}, {"job", result->job},
        {"source_revision", std::to_string(result->source_revision)}}.dump() << '\n';
    return *result;
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--worker") {
            // These hooks exist only in the test executable, not the host.
            const auto path = fs::absolute(fs::u8path(argv[2]));
            std::ifstream f(path, std::ios::binary); const auto j = Json::parse(f);
            const auto target = j.at("export").at("destination").at("path").get<std::string>();
            if (target.find(".test-crash") != std::string::npos) {
                std::ofstream staged(fs::u8path(j.at("stage").get<std::string>()) / "output"); staged << "partial"; staged.close();
#ifdef _WIN32
                _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
                std::abort();
            }
            if (target.find(".test-cancelled") != std::string::npos) {
                std::ofstream result(path.parent_path() / "result.json");
                result << Json{{"version", 2}, {"ok", false}, {"error", {{"code", "Cancelled"}, {"message", "capture cancelled"}}}}.dump();
                return 1;
            }
            if (target.find(".test-slow") != std::string::npos)
                std::this_thread::sleep_for(std::chrono::seconds(2));
            if (target.find(".test-audio-grow") != std::string::npos) {
                const auto cache = path.parent_path() / "cache";
                fs::create_directory(cache);
                std::ofstream file(cache / "engrave-audio-test.wav", std::ios::binary);
                const std::string block(1024, 'x');
                for (int i = 0; i < 256; ++i) {
                    file.write(block.data(), static_cast<std::streamsize>(block.size()));
                    file.flush();
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
                return 1; // The manager must stop this test worker first.
            }
            if (target.find(".test-audio-ignored") != std::string::npos) {
                const auto cache = path.parent_path() / "cache";
                fs::create_directories(cache / "musescore-runtime-fonts-test");
                fs::create_directories(cache / "engrave-audio-directory");
                const std::string large(8192, 'x');
                std::ofstream(cache / "musescore-runtime-fonts-test" / "font", std::ios::binary) << large;
                std::ofstream(cache / "unrelated", std::ios::binary) << large;
                std::error_code ec;
                fs::create_symlink(cache / "unrelated", cache / "engrave-audio-link", ec);
                // Keep the nodes in place for several monitor iterations.
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                std::ofstream(fs::u8path(j.at("stage").get<std::string>()) / "output", std::ios::binary) << "ok";
                std::ofstream(path.parent_path() / "result.json", std::ios::binary) <<
                    Json{{"version", 2}, {"ok", true}, {"artifacts", Json::array({
                        Json{{"name", "output"}, {"mime", "audio/wav"}, {"size", 2}}})}}.dump();
                return 0;
            }
            return run_worker(path);
        }
        {
            engrave::AudioOptions settings;
            settings.format = engrave::AudioFormat::Wav; settings.bit_depth = 16;
            const auto wav16 = projected_audio_bytes(settings, 10, 44100);
            settings.bit_depth = 24;
            const auto wav24 = projected_audio_bytes(settings, 10, 44100);
            require(wav16 == 2385496 && wav24 == 3576196, "WAV projection does not use requested depth/stereo/rate");
            require(projected_audio_bytes(settings, 10, 88200) > wav24, "WAV projection ignores sample rate");
            settings.format = engrave::AudioFormat::Flac; settings.bit_depth = 16;
            const auto flac16 = projected_audio_bytes(settings, 10, 44100);
            settings.bit_depth = 24;
            require(projected_audio_bytes(settings, 10, 44100) > flac16, "FLAC projection ignores bit depth");
            settings.format = engrave::AudioFormat::Mp3; settings.bit_depth = 0; settings.bitrate_kbps = 128;
            const auto mp3_128 = projected_audio_bytes(settings, 10, 44100);
            settings.bitrate_kbps = 512;
            require(projected_audio_bytes(settings, 10, 44100) > mp3_128, "lossy projection ignores bitrate");

            engrave::Outline outline;
            outline.duration = 12;
            outline.measures.resize(2);
            outline.measures[0].tick = {0, 1}; outline.measures[0].duration = {1, 1};
            outline.measures[1].tick = {1, 1}; outline.measures[1].duration = {1, 1};
            engrave::Positions positions;
            positions.elements.resize(2); positions.elements[0].id = 0; positions.elements[1].id = 1;
            positions.events = {{0, 0}, {1, 2100}, {0, 4700}, {1, 6800}, {0, 9400}};
            require(unexpanded_audio_seconds(outline, positions) == 4.7,
                    "unexpanded end did not use the engine's expanded tempo timeline");
            positions.elements.pop_back();
            require(!unexpanded_audio_seconds(outline, positions), "collapsed measures produced a guessed duration");
            positions.elements.resize(2); positions.elements[1].id = 1;
            outline.measures[0].duration = {3, 4}; outline.measures[1].tick = {3, 4};
            outline.measures[1].duration = {1, 1};
            positions.events = {{0, 0}, {0, 1500}, {1, 3000}};
            require(!unexpanded_audio_seconds(outline, positions), "mid-measure end produced a guessed duration");
        }
        require(argc == 4, "usage: export_job_test ASSETS SOUNDFONT OUTPUT_DIR");
        const auto output_dir = fs::absolute(fs::u8path(argv[3])); fs::create_directories(output_dir);
        const auto scratch = music::create_private_directory(output_dir);
        struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{scratch};
        engrave::EngineOptions options;
        options.resources = fs::absolute(fs::u8path(argv[1]));
        options.soundfonts = {fs::absolute(fs::u8path(argv[2]))};
        options.fonts = {options.resources / "fonts/petaluma/PetalumaScript.otf"};
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
                require(captured->snapshot_budget_enforced == (req.score_options.format != engrave::Format::Midi),
                        "capture budget enforcement flag is incorrect");
                require(captured->captured_score_bytes == *direct, std::string(format) + " score capture differs");
                auto result = run_snapshot(*captured);
                require(result.size() == 1 && result[0] == *direct, std::string(format) + " worker score differs");
                return {};
            });
            require(bool(visited), visited ? "" : visited.error().message);
        }
        for (const Json& selection : {Json::object(), excerpt}) {
            auto req = request(doc, "score", {{"format", "midi"}, {"score", selection},
                {"midi_expand_repeats", false}, {"midi_rpns", false}});
            require(!req.score_options.midi_expand_repeats && !req.score_options.midi_rpns, "MIDI options were not decoded");
            auto checked = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
                auto direct = document.export_score(req.score_options);
                if (!direct) return std::unexpected(api_error(direct.error()));
                auto snapshot = capture_export(document, *runtime, req);
                if (!snapshot) return std::unexpected(snapshot.error());
                auto output = run_snapshot(*snapshot);
                require(output.size() == 1 && output[0] == *direct, "MIDI options changed in worker");
                return {};
            });
            require(bool(checked), checked ? "" : checked.error().message);
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
                require(json == encode(*direct).dump(), "positions JSON byte layout changed");
                require(!direct->elements.empty() && !direct->events.empty(), "positions are empty");
                auto result = run_snapshot(*captured);
                require(result.size() == 1 && result[0] == data, "worker positions differ");
                return {};
            });
            require(bool(visited), visited ? "" : visited.error().message);
        }
        {
            // Exercise admission's actual remaining allowance at the engine
            // boundary, including the explicit legacy MIDI fallback.
            JobLimits limits; limits.snapshot_bytes = 32;
            ExportJobs limited(limits);
            for (const char* format : {"mscz", "mscx", "musicxml", "mxl"}) {
                auto req = request(doc, "score", {{"format", format}});
                req.destination.path = scratch / (std::string("capture-limit-") + format);
                auto held = limited.reserve(req); require(bool(held), "capture reservation failed");
                auto checked = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
                    engrave::CaptureOptions cap; cap.max_bytes = held->budget();
                    auto capture = capture_export(document, *runtime, req, cap);
                    require(!capture && capture.error().code == "ResourceLimit", "engine capture exceeded reservation");
                    return {};
                });
                require(bool(checked) && !fs::exists(req.destination.path), "bounded capture published output");
            }
            for (const char* kind : {"render", "audio"}) {
                auto req = request(doc, kind, {{"format", std::string(kind) == "render" ? "pdf" : "wav"}});
                req.destination.path = scratch / (std::string("capture-limit-") + kind);
                auto held = limited.reserve(req); require(bool(held), "snapshot reservation failed");
                auto checked = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
                    engrave::CaptureOptions cap; cap.max_bytes = held->budget();
                    auto capture = capture_export(document, *runtime, req, cap);
                    require(!capture && capture.error().code == "ResourceLimit", "render/audio snapshot exceeded reservation");
                    return {};
                });
                require(bool(checked) && !fs::exists(req.destination.path), "bounded snapshot published output");
            }
            auto req = request(doc, "score", {{"format", "midi"}});
            req.destination.path = scratch / "capture-limit.mid";
            auto held = limited.reserve(req); require(bool(held), "MIDI reservation failed");
            auto checked = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
                engrave::CaptureOptions cap; cap.max_bytes = held->budget();
                auto capture = capture_export(document, *runtime, req, cap);
                require(capture && !capture->snapshot_budget_enforced && capture->captured_score_bytes.size() > held->budget(),
                        "MIDI fallback was not recorded");
                auto submitted = limited.submit(std::move(*held), std::move(*capture));
                require(!submitted && submitted.error().code == "ResourceLimit", "oversize MIDI was not charged after capture");
                return {};
            });
            require(bool(checked) && !fs::exists(req.destination.path), "oversize MIDI published output");
        }
        {
            ExportJobs manager(JobLimits{});
            auto req = request(doc, "score", {{"format", "midi"}});
            req.destination.path = scratch / "budget-fallback.mid";
            auto held = manager.reserve(req); require(bool(held), "MIDI fallback reservation failed");
            auto checked = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
                engrave::CaptureOptions cap; cap.max_bytes = held->budget();
                auto capture = capture_export(document, *runtime, req, cap);
                require(capture && !capture->snapshot_budget_enforced, "MIDI fallback not recorded");
                auto submitted = manager.submit(std::move(*held), std::move(*capture));
                require(submitted && !submitted->snapshot_budget_enforced, "job lost MIDI fallback record");
                auto status = wait_job(manager, submitted->job);
                require(status.state == JobState::Succeeded && !status.snapshot_budget_enforced && fs::exists(req.destination.path),
                        "MIDI fallback job failed");
                return {};
            });
            require(bool(checked), "MIDI fallback test failed");
        }
        {
            auto req = request(doc, "score", {{"format", "musicxml"}});
            req.destination.path = scratch / "cancelled-capture.musicxml";
            ExportJobs manager(JobLimits{});
            auto held = manager.reserve(req); require(bool(held), "cancel capture reservation failed");
            auto checked = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
                int polls = 0;
                engrave::CaptureOptions cap; cap.max_bytes = held->budget(); cap.cancelled = [&] { return ++polls >= 3; };
                auto capture = capture_export(document, *runtime, req, cap);
                require(!capture && capture.error().code == "Cancelled" && polls >= 3, "engine capture cancellation was lost");
                return {};
            });
            require(bool(checked) && !fs::exists(req.destination.path), "cancelled capture published output");
        }
        {
            auto req = request(doc, "positions", {{"position_kind", "segment"}});
            auto checked = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
                auto payload = document.positions(req.position_kind, req.position_score);
                require(bool(payload), "position sizing failed");
                auto baseline = capture_export(document, *runtime, req);
                require(bool(baseline), "position baseline failed");
                const auto needed = engrave::position_payload_bytes(*payload) + baseline->captured_positions_json.size();
                engrave::CaptureOptions cap; cap.max_bytes = needed - 1;
                auto rejected = capture_export(document, *runtime, req, cap);
                require(!rejected && rejected.error().code == "ResourceLimit", "position JSON exceeded shared allowance");
                cap.max_bytes = needed;
                auto accepted = capture_export(document, *runtime, req, cap);
                require(accepted && accepted->captured_positions_json == baseline->captured_positions_json,
                        "position JSON was not stable at its allowance");
                return {};
            });
            require(bool(checked), "position budget test failed");
        }
        {
            auto created = host->create_document({{"template_id", "Treble_Clef"}});
            require(created.at("ok").get<bool>(), "cannot create large position fixture");
            const auto large_doc = created.at("document").get<std::string>();
            auto expanded = host->edit({{"document", large_doc}, {"ops", Json::array({Json{{"op", "insert_measures"}, {"count", 512}}})}});
            require(expanded.at("ok").get<bool>(), expanded.dump());
            const auto rev = std::stoull(expanded.at("revision").get<std::string>());
            auto req = request(large_doc, "positions", {{"position_kind", "measure"}});
            auto checked = host->with_document(large_doc, rev, [&](engrave::Document& document) -> Result<void> {
                auto payload = document.positions(req.position_kind, req.position_score);
                require(payload && payload->elements.size() >= 512, "large position fixture is too small");
                auto baseline = capture_export(document, *runtime, req);
                require(bool(baseline), "large position baseline failed");
                const auto needed = engrave::position_payload_bytes(*payload) + baseline->captured_positions_json.size();
                engrave::CaptureOptions cap; cap.max_bytes = needed - 1;
                auto denied = capture_export(document, *runtime, req, cap);
                require(!denied && denied.error().code == "ResourceLimit", "large position JSON exceeded budget");
                cap.max_bytes = needed;
                auto accepted = capture_export(document, *runtime, req, cap);
                require(accepted && accepted->captured_positions_json == baseline->captured_positions_json,
                        "large position JSON differed at its budget");
                return {};
            });
            require(bool(checked), "large position test failed");
            require(host->close({{"document", large_doc}}).at("ok").get<bool>(), "cannot close large fixture");
        }
        auto repeated = host->open({{"path", path_text(fs::absolute("fixtures/golden/repeat70.mscx"))}});
        require(repeated.at("ok").get<bool>(), repeated.dump());
        const auto repeated_id = repeated.at("document").get<std::string>();
        std::optional<ExportSnapshot> repeat_audio, plain_audio;
        std::uint64_t repeat_projection = 0, plain_projection = 0;
        auto repeat_req = request(repeated_id, "positions", {{"position_kind", "measure"}});
        auto repeat_check = host->with_document(repeated_id, 1, [&](engrave::Document& document) -> Result<void> {
            auto snapshot = capture_export(document, *runtime, repeat_req);
            if (!snapshot) return std::unexpected(snapshot.error());
            const auto& data = snapshot->captured_positions_json;
            const auto result = Json::parse(std::string(reinterpret_cast<const char*>(data.data()), data.size()));
            require(result.at("events") == Json::array({Json{{"elid", 0}, {"position", 0}},
                Json{{"elid", 1}, {"position", 600}}, Json{{"elid", 0}, {"position", 1200}},
                Json{{"elid", 2}, {"position", 1800}}, Json{{"elid", 3}, {"position", 2400}},
                Json{{"elid", 4}, {"position", 3000}}}), "repeat-expanded millisecond events differ");
            require(result.at("page_size").at("width").get<double>() > 9000 &&
                result.at("page_size").at("height").get<double>() > 13000 &&
                result.at("elements").at(0).at("page") == 0, "positions are not page-local 1200-DPI geometry");
            auto output = run_snapshot(*snapshot);
            require(output.size() == 1 && output[0] == data, "repeat positions worker bytes differ");
            return {};
        });
        require(bool(repeat_check), repeat_check ? "" : repeat_check.error().message);
        auto audio_check = host->with_document(repeated_id, 1, [&](engrave::Document& document) -> Result<void> {
            auto outline = document.outline();
            if (!outline) return std::unexpected(api_error(outline.error()));
            auto positions = document.positions(engrave::PositionKind::Measure);
            if (!positions) return std::unexpected(api_error(positions.error()));
            const auto plain_seconds = unexpanded_audio_seconds(*outline, *positions);
            require(plain_seconds.has_value(), "repeat fixture has no unexpanded boundary");
            auto plain = request(repeated_id, "audio", {{"format", "wav"}, {"use_repeats", false}});
            auto expanded = request(repeated_id, "audio", {{"format", "wav"}, {"use_repeats", true}});
            plain_projection = projected_audio_bytes(plain.audio_options, *plain_seconds, options.audio_sample_rate);
            repeat_projection = projected_audio_bytes(expanded.audio_options, outline->duration, options.audio_sample_rate);
            require(plain_projection < repeat_projection, "repeat fixture does not separate the projections");
            auto a = capture_export(document, *runtime, plain);
            if (!a) return std::unexpected(a.error());
            auto b = capture_export(document, *runtime, expanded);
            if (!b) return std::unexpected(b.error());
            plain_audio = std::move(*a); repeat_audio = std::move(*b);
            return {};
        });
        require(bool(audio_check), audio_check ? "" : audio_check.error().message);
        require(host->close({{"document", repeated_id}}).at("ok").get<bool>(), "repeat fixture close failed");
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
        for (const char* format : {"flac", "opus", "mp3", "aac"}) {
            auto req = request(audio_id, "audio", {{"format", format}, {"master_balance", 0.25},
                {"tracks", Json::array({Json{{"track", 0}, {"volume_db", -3.0}, {"soundfont", 0}}})}});
            visited = host->with_document(audio_id, 1, [&](engrave::Document& document) -> Result<void> {
                auto snapshot = capture_export(document, *runtime, req);
                if (!snapshot) return std::unexpected(snapshot.error());
                std::vector<std::byte> direct;
                auto rendered = document.render_audio(req.audio_options, [&](std::span<const std::byte> chunk) {
                    direct.insert(direct.end(), chunk.begin(), chunk.end());
                });
                if (!rendered) return std::unexpected(api_error(rendered.error()));
                auto result = run_snapshot(*snapshot);
                require(result.size() == 1 && result[0] == direct, std::string(format) + " worker audio differs");
                return {};
            });
            require(bool(visited), visited ? "" : visited.error().message);
        }
        std::optional<ExportSnapshot> captured_audio;
        std::vector<std::byte> reference_audio;
        visited = host->with_document(audio_id, 1, [&](engrave::Document& document) -> Result<void> {
            auto captured = capture_export(document, *runtime, audio);
            if (!captured) return std::unexpected(captured.error());
            std::vector<std::byte> direct;
            auto rendered = document.render_audio(audio.audio_options, [&](std::span<const std::byte> chunk) {
                direct.insert(direct.end(), chunk.begin(), chunk.end());
            });
            if (!rendered) return std::unexpected(api_error(rendered.error()));
            auto result = run_snapshot(*captured);
            require(result.size() == 1 && result[0] == direct, "worker audio differs from live default-plus-override mix");
            captured_audio = std::move(*captured); reference_audio = std::move(direct);
            return {};
        });
        require(bool(visited), visited ? "" : visited.error().message);
        auto bad_mix = audio;
        bad_mix.audio_options.tracks.push_back(bad_mix.audio_options.tracks.at(0));
        visited = host->with_document(audio_id, 1, [&](engrave::Document& document) -> Result<void> {
            auto result = capture_export(document, *runtime, bad_mix);
            require(!result && result.error().code == "InvalidArgument", "duplicate mix track accepted");
            bad_mix.audio_options.tracks.pop_back();
            bad_mix.audio_options.tracks[0].soundfont = std::string("no-such-font");
            result = capture_export(document, *runtime, bad_mix);
            require(!result && result.error().code == "NotFound", "unknown SoundFont accepted");
            return {};
        });
        require(bool(visited), visited ? "" : visited.error().message);
        auto parts_doc = host->open({{"path", path_text(fs::absolute("fixtures/audio/mix_parts.mscz"))}});
        require(parts_doc.at("ok").get<bool>(), parts_doc.dump());
        const auto parts_id = parts_doc.at("document").get<std::string>();
        const auto parts_outline = host->outline({{"document", parts_id}});
        const auto& parts_excerpts = parts_outline.at("outline").at("excerpts");
        require(parts_excerpts.size() >= 2, "audio excerpt fixture has too few excerpts");
        for (const auto index : {std::size_t(0), parts_excerpts.size() - 1}) {
            auto req = request(parts_id, "audio", {{"format", "wav"},
                {"score", {{"excerpt", parts_excerpts.at(index).at("id")}}}});
            visited = host->with_document(parts_id, 1, [&](engrave::Document& document) -> Result<void> {
                auto snapshot = capture_export(document, *runtime, req);
                if (!snapshot) return std::unexpected(snapshot.error());
                std::vector<std::byte> direct;
                auto rendered = document.render_audio(req.audio_options, [&](std::span<const std::byte> chunk) {
                    direct.insert(direct.end(), chunk.begin(), chunk.end());
                });
                if (!rendered) return std::unexpected(api_error(rendered.error()));
                auto result = run_snapshot(*snapshot);
                require(result.size() == 1 && result[0] == direct, "excerpt audio mix differs after capture");
                return {};
            });
            require(bool(visited), visited ? "" : visited.error().message);
        }
        require(host->close({{"document", parts_id}}).at("ok").get<bool>(), "audio excerpt close failed");
        auto render = request(doc, "render", {{"format", "png"}, {"first_page", 0}}, true);
        visited = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
            auto captured = capture_export(document, *runtime, render);
            require(captured && !captured->mscz.empty(), "render snapshot failed");
            return {};
        });
        require(bool(visited), visited ? "" : visited.error().message);
        for (const Json& selection : {Json::object(), excerpt}) for (const char* format : {"pdf", "png", "svg"}) {
            auto req = request(doc, "render", {{"format", format}, {"score", selection}, {"last_page", 0}},
                               std::string(format) != "pdf");
            visited = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
                auto direct = document.render(req.render_options);
                if (!direct) return std::unexpected(api_error(direct.error()));
                auto captured = capture_export(document, *runtime, req);
                if (!captured) return std::unexpected(captured.error());
                auto result = run_snapshot(*captured);
                const auto& expected = req.render_options.format == engrave::RenderFormat::Pdf ? direct->document : direct->pages.at(0);
                require(result.size() == 1 && !result[0].empty(), std::string(format) + " worker render is empty");
                const auto save = [](const fs::path& path, const std::vector<std::byte>& data) {
                    std::ofstream out(path, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
                    require(bool(out), "cannot save render comparison");
                };
                const auto prefix = std::string(selection.empty() ? "master-" : "excerpt-");
                save(output_dir / (prefix + "direct." + format), expected);
                auto live_req = req;
                const auto live_dir = scratch / (prefix + "live-" + format);
                fs::create_directory(live_dir);
                live_req.destination = {live_dir / "output", false};
                if (req.render_options.format != engrave::RenderFormat::Pdf) live_req.destination = {live_dir, true};
                ExportTiming timing("live-test", live_dir);
                auto live = live_export(document, live_req, JobLimits{}, [] { return false; },
                    std::chrono::steady_clock::now(), timing);
                require(bool(live), "live render failed");
                auto live_path = fs::u8path(live->at("artifacts").at(0).at("path").get<std::string>());
                std::ifstream live_in(live_path, std::ios::binary);
                std::string live_bytes((std::istreambuf_iterator<char>(live_in)), {});
                require(live_bytes.size() == expected.size() &&
                    std::equal(expected.begin(), expected.end(), reinterpret_cast<const std::byte*>(live_bytes.data())),
                    "live render differs from direct engine bytes");

                save(output_dir / (prefix + "worker." + format), result[0]);
                return {};
            });
            require(bool(visited), visited ? "" : visited.error().message);
        }
        Json invalid = {{"document", doc}, {"action", "start"}, {"kind", "render"},
                        {"options", {{"format", "svg"}, {"dpi", 144}}},
                        {"destination", {{"kind", "directory"}, {"path", "output"}}}};
        require(!decode_export(invalid), "inapplicable option accepted");
        invalid["options"] = {{"format", "png"}}; invalid["destination"]["kind"] = "file";
        require(!decode_export(invalid), "wrong destination kind accepted");
        invalid["destination"]["kind"] = "directory"; invalid["expected_revision"] = "18446744073709551616";
        require(!decode_export(invalid), "overflow revision accepted");

        {
            // The no-repeat worker's positions coexist with the retained MSCZ.
            JobLimits limits; limits.snapshot_bytes = 2 * plain_audio->mscz.size();
            ExportJobs limited(limits);
            auto holding = submit(limited, *plain_audio, scratch / "position-reservation.test-slow.wav");
            require(wait_job(limited, holding.job, true).state == JobState::Running, "position reservation worker did not run");
            auto next = plain_audio->request; next.destination.path = scratch / "position-reservation-next.wav";
            auto denied = limited.reserve(next);
            require(!denied && denied.error().code == "ResourceLimit", "worker position allowance was not retained");
            require(bool(limited.cancel(holding.job)) && wait_job(limited, holding.job).state == JobState::Cancelled,
                    "position reservation cancellation failed");
            require(bool(limited.reserve(next)), "worker position allowance was not released");
        }
        {
            JobLimits limits; limits.output_bytes = 2 * plain_projection;
            ExportJobs bounded(limits);
            const auto plain_path = scratch / "repeat-off-bounded.wav";
            const auto repeated_path = scratch / "repeat-on-rejected.wav";
            auto plain = submit(bounded, *plain_audio, plain_path);
            require(wait_job(bounded, plain.job).state == JobState::Succeeded &&
                    fs::file_size(plain_path) <= plain_projection, "bounded no-repeat job failed");
            auto repeated = submit(bounded, *repeat_audio, repeated_path);
            const auto rejected = wait_job(bounded, repeated.job);
            require(rejected.state == JobState::Failed && rejected.error && rejected.error->code == "ResourceLimit" &&
                    rejected.error->message.find("projected audio") != std::string::npos && !fs::exists(repeated_path),
                    "bounded repeated job did not use the expanded projection");
        }
        {
            JobLimits limits; limits.output_bytes = 2 * repeat_projection;
            ExportJobs bounded(limits);
            const auto path = scratch / "repeat-on-bounded.wav";
            auto repeated = submit(bounded, *repeat_audio, path);
            require(wait_job(bounded, repeated.job).state == JobState::Succeeded &&
                    fs::file_size(path) <= repeat_projection, "expanded job failed at its projected budget");
        }
        plain_audio.reset(); repeat_audio.reset();
        {
            JobLimits limits; limits.output_bytes = 4096;
            ExportJobs limited(limits);
            const auto ignored_path = scratch / "audio.test-audio-ignored.wav";
            auto ignored = submit(limited, *captured_audio, ignored_path);
            require(wait_job(limited, ignored.job).state == JobState::Succeeded &&
                    fs::file_size(ignored_path) == 2, "unrelated cache nodes were charged to audio");
            const auto path = scratch / "audio.test-audio-grow.wav";
            auto growing = submit(limited, *captured_audio, path);
            auto stopped = wait_job(limited, growing.job);
            require(stopped.state == JobState::Failed && stopped.error && stopped.error->code == "ResourceLimit",
                    "growing audio cache was not stopped");
            require(!fs::exists(path), "growing audio cache published an artifact");
        }
        {
            JobLimits limits; limits.output_bytes = 64;
            ExportJobs limited(limits);
            const auto path = scratch / "audio-oversized.wav";
            auto oversized = submit(limited, *captured_audio, path);
            const auto status = wait_job(limited, oversized.job);
            require(status.state == JobState::Failed && status.error && status.error->code == "ResourceLimit" &&
                    !fs::exists(path), "oversized real audio job was not rejected");
        }
        std::optional<ExportSnapshot> captured_score;
        auto score_req = request(doc, "score", {{"format", "mscx"}, {"score", excerpt}});
        visited = host->with_document(doc, revision, [&](engrave::Document& document) -> Result<void> {
            auto result = capture_export(document, *runtime, score_req);
            if (!result) return std::unexpected(result.error());
            captured_score = std::move(*result); return {};
        });
        require(bool(visited) && captured_score.has_value(), "score job capture failed");
        {
            JobLimits limits; limits.snapshot_bytes = 0;
            ExportJobs limited(limits);
            auto req = score_req; req.destination.path = scratch / "no-budget.mscx";
            auto denied = limited.reserve(req);
            require(!denied && denied.error().code == "ResourceLimit", "empty budget admitted capture");
        }
        {
            JobLimits limits; limits.snapshot_bytes = captured_score->captured_score_bytes.size() + 1;
            ExportJobs limited(limits);
            auto req = score_req; req.destination.path = scratch / "reserved.mscx";
            auto held = limited.reserve(req);
            require(bool(held), "capture reservation failed");
            auto other = req; other.destination.path = scratch / "other-reserved.mscx";
            auto denied = limited.reserve(other);
            require(!denied && denied.error().code == "ResourceLimit", "concurrent capture exceeded reserved budget");
            held = std::unexpected(HostError{"Cancelled", "release test reservation"});
            require(bool(limited.reserve(other)), "released reservation retained budget");
            auto invalid = req; invalid.destination.path = scratch / "absent-parent" / "invalid.mscx";
            auto bad = limited.reserve(invalid);
            require(!bad && bad.error().code == "InvalidArgument", "invalid destination admitted capture");
        }
        {
            JobLimits limits; limits.deadline = std::chrono::milliseconds(5);
            ExportJobs limited(limits);
            auto req = score_req; req.destination.path = scratch / "capture-deadline.mscx";
            auto expired = limited.reserve(req, std::chrono::steady_clock::now() - std::chrono::seconds(1));
            require(!expired && expired.error().code == "DeadlineExceeded", "expired request admitted capture");
            auto held = limited.reserve(req);
            require(bool(held), "deadline capture reservation failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            auto snapshot = *captured_score; snapshot.request = req;
            auto denied = limited.submit(std::move(*held), std::move(snapshot));
            require(!denied && denied.error().code == "DeadlineExceeded", "expired capture was submitted");
        }
        {
            ExportJobs limited(JobLimits{});
            constexpr int count = 8;
            std::barrier start(count), finish(count);
            std::atomic<int> accepted{0}, rejected{0};
            std::vector<std::thread> callers;
            for (int i = 0; i < count; ++i) callers.emplace_back([&, i] {
                auto req = score_req; req.destination.path = scratch / ("concurrent-" + std::to_string(i));
                start.arrive_and_wait();
                auto held = limited.reserve(req);
                if (held) ++accepted;
                else if (held.error().code == "ResourceLimit") ++rejected;
                finish.arrive_and_wait(); // Keep the successful reservation while peers try.
            });
            for (auto& caller : callers) caller.join();
            require(accepted == 1 && rejected == count - 1, "concurrent capture reservations exceeded budget");
        }
        {
            ExportJobs limited(JobLimits{});
            auto req = score_req; req.destination.path = scratch / "shutdown-reservation.mscx";
            auto held = limited.reserve(req);
            require(bool(held), "shutdown reservation failed");
            limited.shutdown();
            require(!music::is_output_path_reserved(req.destination.path), "shutdown retained capture destination");
        }
        {
            JobLimits limits; limits.snapshot_bytes = 1;
            ExportJobs jobs(limits);
            auto denied = jobs.submit(*captured_score);
            require(!denied && denied.error().code == "ResourceLimit", "snapshot byte limit not enforced");
        }
        {
            JobLimits limits; limits.output_bytes = 1;
            ExportJobs jobs(limits);
            auto denied = jobs.submit(*captured_score);
            require(!denied && denied.error().code == "ResourceLimit", "output byte limit not enforced");
        }
        {
            JobLimits limits; limits.queue_capacity = 1;
            ExportJobs limited(limits);
            auto occupying = submit(limited, *captured_score, scratch / "capacity.test-slow");
            require(wait_job(limited, occupying.job, true).state == JobState::Running, "capacity worker did not run");
            auto next = *captured_score; next.request.destination.path = scratch / "capacity-other.mscx";
            auto denied = limited.submit(std::move(next));
            require(!denied && denied.error().code == "ResourceLimit", "active queue capacity not enforced");
            limited.shutdown();
            auto stopped = limited.get(occupying.job);
            require(stopped && stopped->state == JobState::Cancelled && !fs::exists(scratch / "capacity.test-slow"),
                    "shutdown did not cancel and reap an active worker");
        }
        {
            JobLimits limits; limits.snapshot_bytes = captured_score->captured_score_bytes.size() + 1;
            ExportJobs limited(limits);
            auto holding = submit(limited, *captured_score, scratch / "budget.test-slow");
            require(wait_job(limited, holding.job, true).state == JobState::Running, "budget worker did not run");
            auto next = *captured_score; next.request.destination.path = scratch / "budget-other.mscx";
            auto denied = limited.submit(next);
            require(!denied && denied.error().code == "ResourceLimit", "aggregate snapshot budget not enforced");
            require(bool(limited.cancel(holding.job)) && wait_job(limited, holding.job).state == JobState::Cancelled,
                    "budget worker cancellation failed");
            auto released = limited.submit(std::move(next));
            require(bool(released) && wait_job(limited, released->job).state == JobState::Succeeded,
                    "cancelled job retained its snapshot budget");
        }
        {
            ExportJobs reentrant(JobLimits{});
            std::atomic<bool> callback_done{false};
            auto snapshot = *captured_score; snapshot.request.destination.path = scratch / "callback-shutdown.mscx";
            auto started = reentrant.submit(std::move(snapshot), [&](Result<JobStatus>) {
                reentrant.shutdown(); callback_done = true;
            });
            require(bool(started), "reentrant callback submission failed");
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!callback_done && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(callback_done, "callback shutdown deadlocked");
        }
        ExportJobs jobs(JobLimits{});
        const auto slow_path = scratch / "first.test-slow";
        auto first = submit(jobs, *captured_score, slow_path);
        require(wait_job(jobs, first.job, true).state == JobState::Running, "slow job did not run");
        auto conflicting = *captured_score; conflicting.request.destination.path = slow_path;
        require(!jobs.submit(std::move(conflicting)), "destination reservation did not conflict");
        auto second = submit(jobs, *captured_score, scratch / "second.test-slow");
        require(wait_job(jobs, second.job, true).state == JobState::Running, "second slow job did not run");
        auto queued = submit(jobs, *captured_score, scratch / "queued.mscx");
        require(jobs.get(queued.job)->state == JobState::Queued, "third job was not queued");
        require(bool(jobs.cancel(queued.job)), "queued cancellation failed");
        require(wait_job(jobs, queued.job).state == JobState::Cancelled, "queued job was not cancelled");
        require(bool(jobs.cancel(first.job)), "running cancellation failed");
        require(wait_job(jobs, first.job).state == JobState::Cancelled && !fs::exists(slow_path), "running cancellation published output");
        require(wait_job(jobs, second.job).state == JobState::Succeeded, "uncancelled worker failed");
        auto crashed = submit(jobs, *captured_score, scratch / "worker.test-crash");
        auto crash_status = wait_job(jobs, crashed.job);
        require(crash_status.state == JobState::Failed && crash_status.error && crash_status.error->code == "WorkerFailed" &&
                !fs::exists(scratch / "worker.test-crash"), "worker crash published output or lost its failure");
        auto cancelled = submit(jobs, *captured_score, scratch / "worker.test-cancelled");
        auto cancelled_status = wait_job(jobs, cancelled.job);
        require(cancelled_status.state == JobState::Cancelled && cancelled_status.error &&
                cancelled_status.error->code == "Cancelled" && !fs::exists(scratch / "worker.test-cancelled"),
                "worker cancellation did not produce a cancelled job");
        {
            JobLimits limits; limits.deadline = std::chrono::milliseconds(500);
            ExportJobs expiring(limits);
            auto expiring_job = submit(expiring, *captured_score, scratch / "deadline.test-slow");
            require(wait_job(expiring, expiring_job.job, true).state == JobState::Running, "deadline worker did not run");
            auto status = wait_job(expiring, expiring_job.job);
            require(status.state == JobState::Failed && status.error && status.error->code == "DeadlineExceeded" &&
                    !fs::exists(scratch / "deadline.test-slow"), "deadline did not terminate worker");
        }
        // Capture before a later edit. The job must publish the captured bytes.
        const auto first_measure = outline.at("outline").at("measures").at(0).at("id");
        edited = host->edit({{"document", doc}, {"ops", Json::array({Json{{"op", "set_layout_break"},
            {"measure", Json{{"kind", "id"}, {"id", first_measure}}}, {"kind", "page"}, {"on", true}}})}});
        require(edited.at("ok").get<bool>(), edited.dump());
        const auto later_revision = std::stoull(edited.at("revision").get<std::string>());
        std::optional<ExportSnapshot> positions_job;
        auto positions_req = request(doc, "positions", {{"position_kind", "measure"}});
        visited = host->with_document(doc, later_revision, [&](engrave::Document& document) -> Result<void> {
            auto result = capture_export(document, *runtime, positions_req);
            if (!result) return std::unexpected(result.error());
            positions_job = std::move(*result); return {};
        });
        require(bool(visited) && positions_job, "position job capture failed");
        auto position = submit(jobs, *positions_job, scratch / "positions.json");
        auto position_status = wait_job(jobs, position.job);
        require(position_status.state == JobState::Succeeded && position_status.artifacts.size() == 1 &&
                position_status.artifacts[0].mime == "application/json", "position job publication failed");
        positions_job.reset();
        auto old_job = submit(jobs, *captured_score, scratch / "old.mscx");
        auto old_status = wait_job(jobs, old_job.job);
        require(old_status.state == JobState::Succeeded && old_status.source_revision == revision, "captured revision changed");
        std::ifstream old_file(scratch / "old.mscx", std::ios::binary);
        std::string old_bytes{std::istreambuf_iterator<char>(old_file), {}};
        require(old_bytes.size() == captured_score->captured_score_bytes.size() &&
                std::equal(old_bytes.begin(), old_bytes.end(), reinterpret_cast<const char*>(captured_score->captured_score_bytes.data())), "later edit changed captured output");

        std::optional<ExportSnapshot> multipage;
        auto pages_req = request(doc, "render", {{"format", "png"}, {"dpi", 36}, {"last_page", 1}}, true);
        visited = host->with_document(doc, later_revision, [&](engrave::Document& document) -> Result<void> {
            auto result = capture_export(document, *runtime, pages_req);
            if (!result) return std::unexpected(result.error());
            multipage = std::move(*result); return {};
        });
        require(bool(visited) && multipage.has_value(), visited ? "multipage capture missing" : visited.error().message);
        {
            const auto output = scratch / "oversized-request"; fs::create_directory(output);
            auto oversized = *multipage;
            oversized.runtime.options.fonts.assign(1100, fs::path(std::string(4096, 'a')));
            auto job = submit(jobs, std::move(oversized), output);
            auto status = wait_job(jobs, job.job);
            require(status.state == JobState::Failed && status.error && status.error->code == "ResourceLimit" &&
                    fs::is_empty(output), "oversized worker request was not rejected cleanly");
        }
        {
            JobLimits limits; limits.output_bytes = 64;
            ExportJobs limited(limits);
            const auto output = scratch / "small-output"; fs::create_directory(output);
            auto too_large = submit(limited, *multipage, output);
            auto status = wait_job(limited, too_large.job);
            require(status.state == JobState::Failed && status.error && status.error->code == "ResourceLimit" &&
                    fs::is_empty(output), "worker output byte limit allowed publication");
        }
        {
            const auto output = scratch / "missing-resources"; fs::create_directory(output);
            auto bad = *multipage; bad.runtime.options.resources = scratch / "nonexistent";
            auto failed = submit(jobs, std::move(bad), output);
            auto status = wait_job(jobs, failed.job);
            require(status.state == JobState::Failed && status.error && status.error->code == "NotFound" &&
                    fs::is_empty(output), "public engine error code was not preserved");
        }
        {
            const auto output = scratch / "reserved.test-slow"; fs::create_directory(output);
            auto reserving = submit(jobs, *multipage, output);
            require(wait_job(jobs, reserving.job, true).state == JobState::Running, "directory reservation worker did not run");
            auto nested = *captured_score; nested.request.destination.path = output / "page-0001.png";
            require(!jobs.submit(std::move(nested)), "directory reservation allowed a nested file");
            require(bool(jobs.cancel(reserving.job)) && wait_job(jobs, reserving.job).state == JobState::Cancelled,
                    "directory reservation job was not cancelled");
        }
        const auto pages_dir = scratch / "pages"; fs::create_directory(pages_dir);
        auto page_job = submit(jobs, *multipage, pages_dir);
        auto page_status = wait_job(jobs, page_job.job);
        require(page_status.state == JobState::Succeeded && page_status.artifacts.size() == 2 &&
                fs::is_regular_file(pages_dir / "page-0001.png") && fs::is_regular_file(pages_dir / "page-0002.png"), "multi-page publication failed");
        std::cout << "EXPORT_CASE " << Json{{"ok", true}, {"action", "run"},
            {"source_revision", std::to_string(page_status.source_revision)}, {"artifacts", encode(page_status).at("artifacts")}}.dump() << '\n';
        const auto partial_dir = scratch / "partial"; fs::create_directory(partial_dir); fs::create_directory(partial_dir / "page-0002.png");
        auto partial = submit(jobs, *multipage, partial_dir);
        auto partial_status = wait_job(jobs, partial.job);
        require(partial_status.state == JobState::Failed && partial_status.error && partial_status.error->code == "Io" &&
                partial_status.error->published_artifacts.size() == 1 && fs::is_regular_file(partial_dir / "page-0001.png"), "partial publication was not reported");

        // Delay worker startup, then replace the live runtime assets. Both jobs
        // must still use the font and SoundFont in their captured leases.
        const auto baseline_pages = run_snapshot(*multipage);
        const auto font_dir = scratch / "fonts.test-slow"; fs::create_directory(font_dir);
        auto font_job = submit(jobs, *multipage, font_dir);
        auto sound_job = submit(jobs, *captured_audio, scratch / "sound.test-slow");
        require(wait_job(jobs, font_job.job, true).state == JobState::Running &&
                wait_job(jobs, sound_job.job, true).state == JobState::Running, "asset jobs did not start");
        auto changed_font = host->runtime_info({{"action", "add_font"}, {"path", path_text(options.resources / "fonts/FreeSerif.ttf")}});
        auto changed_sound = host->runtime_info({{"action", "set_soundfonts"},
            {"files", Json::array({path_text(fs::absolute("tests/engine_audio/sine.sf2"))})}});
        require(changed_font.at("ok").get<bool>() && changed_sound.at("ok").get<bool>(), "runtime asset replacement failed");
        require(wait_job(jobs, font_job.job).state == JobState::Succeeded &&
                wait_job(jobs, sound_job.job).state == JobState::Succeeded, "captured asset jobs failed");
        require(baseline_pages.size() == 2 && read_bytes(font_dir / "page-0001.png") == baseline_pages[0] &&
                read_bytes(font_dir / "page-0002.png") == baseline_pages[1], "later font changed captured rendering");
        require(read_bytes(scratch / "sound.test-slow") == reference_audio, "later SoundFont changed captured audio");
        captured_audio.reset();

        auto closes = submit(jobs, *captured_score, scratch / "close.test-slow");
        require(wait_job(jobs, closes.job, true).state == JobState::Running, "close test worker did not start");
        require(host->close({{"document", doc}}).at("ok").get<bool>(), "document close failed");
        require(wait_job(jobs, closes.job).state == JobState::Succeeded, "closing live document broke captured job");

        // A completion can safely inspect the manager from its callback thread.
        std::atomic<int> callback_count{0};
        auto callback_snapshot = *captured_score; callback_snapshot.request.destination.path = scratch / "callback.mscx";
        auto callback_job = jobs.submit(std::move(callback_snapshot), [&](Result<JobStatus> result) {
            if (result && result->state == JobState::Succeeded && jobs.get(result->job)) ++callback_count;
        });
        require(bool(callback_job), "callback job submission failed");
        require(wait_job(jobs, callback_job->job).state == JobState::Succeeded, "callback job failed");
        const auto callback_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!callback_count && std::chrono::steady_clock::now() < callback_deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(callback_count == 1, "completion callback did not run exactly once");

        // The active job owns the last lease after all live documents, host and
        // other snapshots are gone. It may finish after HostApi is destroyed.
        const auto font_copy = runtime->options.fonts.at(0), sf_copy = runtime->options.soundfonts.at(0);
        auto lease_job = submit(jobs, std::move(*captured_score), scratch / "lease.test-slow");
        captured_score.reset(); multipage.reset();
        require(wait_job(jobs, lease_job.job, true).state == JobState::Running, "asset lease worker did not start");
        require(host->close({{"document", audio_id}}).at("ok").get<bool>(), "audio document close failed");
        *runtime = RuntimeSnapshot{}; host.reset();
        require(fs::is_regular_file(font_copy) && fs::is_regular_file(sf_copy), "captured asset vanished during job");
        require(wait_job(jobs, lease_job.job).state == JobState::Succeeded, "captured asset job failed");
        require(!fs::exists(font_copy) && !fs::exists(sf_copy), "last asset lease did not remove files");
        jobs.shutdown();
        std::cout << "export_job_test capture checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "export_job_test: " << e.what() << '\n';
        return 1;
    }
}
