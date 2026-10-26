#include "engine.h"
#include "composition.h"
#include "noqt_runtime.h"
#include "score_metadata.h"
#include "atomic_file.h"
#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

namespace music {
using Json = nlohmann::json;
namespace {
std::string text_arg(const Json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
    auto value = j.at(key).get<std::string>();
    if (value.empty() || value.find('\0') != std::string::npos) throw std::invalid_argument(std::string(key) + " is empty or contains NUL");
    return value;
}
size_t index_arg(const Json& j, const char* key, size_t maximum) {
    if (!j.contains(key) || !j.at(key).is_number_integer() || j.at(key) < 0 || j.at(key) > maximum)
        throw std::invalid_argument(std::string(key) + " is out of range");
    return j.at(key).get<size_t>();
}
void only_keys(const Json& j, std::initializer_list<std::string> allowed) {
    if (!j.is_object()) throw std::invalid_argument("Arguments must be an object");
    for (auto it = j.begin(); it != j.end(); ++it)
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
            throw std::invalid_argument("Unknown argument: " + it.key());
}
std::string path_utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
std::string read_input(const std::filesystem::path& path) {
    constexpr size_t limit = 8 * 1024 * 1024;
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open score input");
    std::string data(limit + 1, '\0');
    file.read(data.data(), data.size());
    if (file.bad()) throw std::runtime_error("Cannot read score input");
    data.resize(static_cast<size_t>(file.gcount()));
    if (data.size() > limit) throw std::invalid_argument("Score input exceeds 8 MiB");
    return data;
}
}

struct Engine::Impl {
    struct Document { ScoreProject project; Json composition; };
    std::map<std::string, Document> documents;
    uint64_t serial = 0;
    std::string soundfont;
    std::filesystem::path assets;
    Document& select(const Json& args) {
        const auto it = documents.find(text_arg(args, "documentId"));
        if (it == documents.end()) throw std::invalid_argument("Unknown or closed documentId");
        return it->second;
    }
    Json describe(const std::string& id, const Document& document) const {
        const auto* score = document.project->masterScore();
        return {{"documentId", id}, {"title", score_title(score)}, {"pages", score->npages()},
                {"parts", score->parts().size()}, {"editable", !document.composition.is_null()}};
    }
    Json insert(Document document) {
        const auto id = "score-" + std::to_string(++serial);
        auto result = describe(id, document);
        documents.emplace(id, std::move(document));
        return result;
    }
};

Engine::Engine(int&, char** argv, const std::string& soundfont, const std::string& resources) : impl_(std::make_unique<Impl>()) {
    if (resources.empty() && (!argv || !argv[0])) throw std::invalid_argument("Missing executable path");
    impl_->assets = std::filesystem::absolute(resources.empty() ? std::filesystem::u8path(argv[0]).parent_path() / "assets"
                                                             : std::filesystem::u8path(resources));
    if (!soundfont.empty()) {
        const auto path = std::filesystem::absolute(std::filesystem::u8path(soundfont));
        if (!std::filesystem::is_regular_file(path)) throw std::invalid_argument("SoundFont does not exist");
        impl_->soundfont = path_utf8(path);
    }
    initialize_native_runtime(path_utf8(impl_->assets));
}
Engine::~Engine() = default;
std::string Engine::tools_json() {
    return
#include "tool_schema.inc"
    ;
}

std::string Engine::call(const std::string& tool, const std::string& arguments_json) {
    const auto args = Json::parse(arguments_json);
    Json result;
    if (tool == "score_create" || tool == "score_open") {
        if (impl_->documents.size() >= 16) throw std::runtime_error("Close a document before opening another (limit 16)");
        if (tool == "score_create") {
            only_keys(args, {"score"});
            const auto& model = args.at("score");
            result = impl_->insert({load_native_score(composition_xml(model), "musicxml"), model});
        } else {
            only_keys(args, {"path", "musicxml"});
            if (args.contains("path") == args.contains("musicxml")) throw std::invalid_argument("Specify exactly one of path or musicxml");
            std::string data, format = "musicxml";
            if (args.contains("path")) {
                const auto path = std::filesystem::u8path(text_arg(args, "path"));
                format = path_utf8(path.extension());
                if (!format.empty()) format.erase(0, 1);
                std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (format != "musicxml" && format != "xml" && format != "mxl" && format != "mscz" && format != "mscx")
                    throw std::invalid_argument("Supported inputs: musicxml, xml, mxl, mscz, mscx");
                data = read_input(path);
            } else data = text_arg(args, "musicxml");
            result = impl_->insert({load_native_score(data, format), nullptr});
        }
    } else if (tool == "score_get") {
        only_keys(args, {"documentId"});
        const auto& doc = impl_->select(args);
        result = impl_->describe(text_arg(args, "documentId"), doc);
        result["composition"] = doc.composition;
        result["musicxml"] = export_native_score(doc.project, "musicxml", 0, "");
        result["metadata"] = score_metadata(doc.project->masterScore());
    } else if (tool == "score_replace_measures") {
        only_keys(args, {"documentId", "partIndex", "startMeasure", "measures"});
        auto& doc = impl_->select(args);
        if (doc.composition.is_null()) throw std::invalid_argument("Structured measure replacement requires a score_create document");
        auto updated = doc.composition;
        auto& parts = updated.at("parts");
        auto& bars = parts.at(index_arg(args, "partIndex", parts.size() - 1)).at("measures");
        const auto start = index_arg(args, "startMeasure", bars.size() - 1);
        const auto& replacement = args.at("measures");
        if (!replacement.is_array() || replacement.empty() || replacement.size() > bars.size() - start)
            throw std::invalid_argument("Replacement must fit within the existing measure range");
        for (size_t i = 0; i < replacement.size(); ++i) bars.at(start + i) = replacement.at(i);
        Impl::Document candidate{load_native_score(composition_xml(updated), "musicxml"), std::move(updated)};
        result = impl_->describe(text_arg(args, "documentId"), candidate);
        doc = std::move(candidate);
    } else if (tool == "score_close") {
        only_keys(args, {"documentId"});
        if (!impl_->documents.erase(text_arg(args, "documentId"))) throw std::invalid_argument("Unknown or closed documentId");
        result = {{"closed", true}};
    } else if (tool == "score_export") {
        only_keys(args, {"documentId", "format", "path", "page"});
        const auto& doc = impl_->select(args);
        const auto format = text_arg(args, "format");
        const auto path = std::filesystem::absolute(std::filesystem::u8path(text_arg(args, "path")));
        const auto pages = doc.project->masterScore()->npages();
        if ((format == "svg" || format == "png" || format == "pdf") && !pages) throw std::runtime_error("Score has no pages");
        if (args.contains("page") && format != "svg" && format != "png") throw std::invalid_argument("page only applies to svg/png");
        const int page = args.contains("page") ? static_cast<int>(index_arg(args, "page", pages ? pages - 1 : 0)) : 0;
        const auto data = export_native_score(doc.project, format, page, impl_->soundfont);
        if (data.empty()) throw std::runtime_error("Exporter returned no data");
        write_atomic(path, data);
        result = {{"path", path_utf8(path)}, {"format", format}, {"bytes", data.size()}};
    } else throw std::invalid_argument("Unknown tool");
    return result.dump();
}
}
