#include "api_output.h"
#include "export_timing.h"
#include <fstream>
#include <iostream>
using namespace music::v2;
namespace fs = std::filesystem;
template <class T>
T value(engrave::Result<T> r) {
    if (!r) throw std::runtime_error(r.error().message);
    return std::move(*r);
}
std::vector<std::byte> read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), {});
    return {reinterpret_cast<const std::byte *>(s.data()), reinterpret_cast<const std::byte *>(s.data() + s.size())};
}
int main(int argc, char **argv) {
    try {
        if (argc != 4 || std::string(argv[1]) != "--partial-write")
            throw std::runtime_error("partial: no content comparison; use scripts/test_live_export.py --direct-binary BINARY (file-writing "
                                     "mode: live_export_test --partial-write RESOURCES OUTPUT)");
        engrave::EngineOptions options;
        options.resources = fs::absolute(fs::u8path(argv[2]));
        options.deterministic_ids = true;
        auto engine = value(engrave::Engine::create(options));
        auto root = fs::absolute(fs::u8path(argv[3]));
        fs::create_directories(root);
        std::vector<fs::directory_entry> fixtures;
        for (const auto &entry : fs::directory_iterator("fixtures/golden")) fixtures.push_back(entry);
        fixtures.emplace_back("fixtures/evening.musicxml");
        fixtures.emplace_back("tests/live_export/created.musicxml");
        for (const char *name : {"engine/two-notes.mnx", "project_examples/tupletSimple.mnx", "project_examples/percussionKit.mnx",
                                 "project_examples/graceArps.mnx"})
            fixtures.emplace_back(fs::path("vendor/engrave/src/importexport/mnx/tests/data") / name);
        for (const auto &fixture : fixtures) {
            auto ext = fixture.path().extension().string();
            if (ext != ".mscx" && ext != ".mscz" && ext != ".musicxml" && ext != ".mid" && ext != ".gp" && ext != ".mnx") continue;
            auto doc = value(engine->open(fs::absolute(fixture.path())));
            const auto out = root / (fixture.path() == fs::path("tests/live_export/created.musicxml") ? fs::path("created.json") :
                                                                                                        fixture.path().filename());
            fs::create_directories(out);
            for (const char *fmt : {"svg", "pdf", "png", "mscx", "mscz"}) {
                const bool svg = std::string(fmt) == "svg" || std::string(fmt) == "png", pdf = std::string(fmt) == "pdf";
                auto target = out / fmt;
                if (svg) fs::create_directories(target);
                auto request =
                    decode_export({{"document", "doc-oracle-1"},
                                   {"kind", svg || pdf ? "render" : "score"},
                                   {"options", svg || pdf ? Json{{"format", fmt}, {"first_page", 0}, {"last_page", pdf ? -1 : 0}} :
                                                            Json{{"format", fmt}}},
                                   {"destination", {{"kind", svg ? "directory" : "file"}, {"path", target.generic_string()}}}});
                if (!request) throw std::runtime_error(request.error().message);
                std::vector<std::byte> expected;
                if (svg || pdf) {
                    auto rendered = value(doc->render(request->render_options));
                    expected = pdf ? std::move(rendered.document) : std::move(rendered.pages.at(0));
                } else
                    expected = value(doc->export_score(request->score_options));
                ExportTiming timing("oracle", target);
                auto actual = live_export(*doc, *request, ExportLimits{}, [] { return false; }, ExportTiming::Clock::now(), timing);
                if (!actual) throw std::runtime_error(actual.error().message);
                auto denied_request = *request;
                denied_request.destination.path = svg ? out / "cancel-dir" : out / "cancel-file";
                if (svg) fs::create_directories(denied_request.destination.path);
                auto cancelled = live_export(*doc, denied_request, ExportLimits{}, [] { return true; }, ExportTiming::Clock::now(), timing);
                if (cancelled || cancelled.error().code != "Cancelled") throw std::runtime_error("cancellation failed");
                if (svg ? !fs::is_empty(denied_request.destination.path) : fs::exists(denied_request.destination.path))
                    throw std::runtime_error("cancel published an artifact");
                ExportLimits tiny;
                tiny.output_bytes = 1;
                auto limited = live_export(*doc, denied_request, tiny, [] { return false; }, ExportTiming::Clock::now(), timing);
                if (limited || limited.error().code != "ResourceLimit") throw std::runtime_error("output limit failed");
                const auto path = fs::u8path(actual->at("artifacts").at(0).at("path").get<std::string>());
                if (std::string(fmt) == "mscz" || pdf) {
                    // PDF has volatile Cairo metadata, including compressed streams.
                    // The Python harness compares complete golden.describe_pdf descriptors.
                    std::ofstream direct(out / (pdf ? "direct.pdf" : "direct.mscz"), std::ios::binary);
                    direct.write(reinterpret_cast<const char *>(expected.data()), expected.size());
                } else if (read(path) != expected)
                    throw std::runtime_error("direct byte mismatch: " + fixture.path().string() + "/" + fmt);
            }
        }
        std::cout << "partial: no content comparison for PDF/MSCZ; run scripts/test_live_export.py\n";
        return 3;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
