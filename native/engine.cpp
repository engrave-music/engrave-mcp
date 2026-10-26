#include "engine.h"
#include "composition.h"
#include "native_engine.inc"
#include "audio/common/rpc/irpcchannel.h"
#include "audio/engine/isoundfontrepository.h"
#include "async/processevents.h"
#include "audio/audiosynth.h"
#include <QDir>
#include <QSaveFile>
#include <QStandardPaths>
#include <iostream>
#include <map>
#include <stdexcept>

static void initialize_resources() { Q_INIT_RESOURCE(native_resources); }

namespace music {
using Json = nlohmann::json;
namespace {
constexpr qint64 max_input = 8 * 1024 * 1024;
class StderrLog final : public muse::logger::LogDest {
public:
    StderrLog() : LogDest(muse::logger::LogLayout("${type}: ${message}")) {}
    std::string name() const override { return "native-stderr"; }
    void write(const muse::logger::LogMsg& message) override {
        std::cerr << m_layout.output(message) << '\n';
    }
};
void pump() {
    auto rpc = modularity::ioc()->resolve<audio::rpc::IRpcChannel>("");
    for (int i = 0; i < 32; ++i) {
        QCoreApplication::processEvents();
        async::processMessages();
        rpc->process();
    }
}
std::string checked_bytes(const NativeResult& result) {
    if (!result.status.success()) throw std::runtime_error(result.status.toString());
    return result.bytes.toStdString();
}
std::string text_arg(const Json& j, const char* key) {
    if (!j.contains(key) || !j.at(key).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
    auto text = j.at(key).get<std::string>();
    if (text.empty() || text.find('\0') != std::string::npos) throw std::invalid_argument(std::string(key) + " is empty or contains NUL");
    return text;
}
size_t index_arg(const Json& j, const char* key, size_t max) {
    if (!j.contains(key) || !j.at(key).is_number_integer() || j.at(key) < 0 || j.at(key) > max)
        throw std::invalid_argument(std::string(key) + " is out of range");
    return j.at(key).get<size_t>();
}
void only_keys(const Json& j, std::initializer_list<std::string> allowed) {
    if (!j.is_object()) throw std::invalid_argument("Arguments must be an object");
    for (auto it = j.begin(); it != j.end(); ++it)
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
            throw std::invalid_argument("Unknown argument: " + it.key());
}
}

struct Engine::Impl {
    struct Document {
        uintptr_t pointer;
        project::INotationProjectPtr owner;
        Json composition;
    };
    std::map<std::string, Document> documents;
    uint64_t serial = 0;
    bool has_soundfont = false;

    Document load(const std::string& xml, const std::string& format, Json composition = nullptr) {
        if (xml.empty() || xml.size() > max_input) throw std::invalid_argument("Input must contain 1..8388608 bytes");
        auto previous = s_globalContext->currentProject();
        const auto old_instances = instances;
        try {
            auto result = _load(format.c_str(), xml.data(), static_cast<uint32_t>(xml.size()), true);
            if (!result.status.success() || !result.score) throw std::runtime_error("Load failed: " + result.status.toString());
            pump();
            Document document{result.score, s_globalContext->currentProject(), std::move(composition)};
            // NotationProject owns EngravingProject; do not retain a second, global
            // ownership path in the web ABI's registry.
            instances.erase(reinterpret_cast<engraving::MasterScore*>(result.score)->project().lock());
            return document;
        } catch (...) {
            auto failed = s_globalContext->currentProject();
            s_globalContext->setCurrentProject(previous);
            pump();
            instances = old_instances;
            failed.reset();
            throw;
        }
    }
    Document& select(const Json& args) {
        auto it = documents.find(text_arg(args, "documentId"));
        if (it == documents.end()) throw std::invalid_argument("Unknown or closed documentId");
        s_globalContext->setCurrentProject(it->second.owner);
        pump();
        return it->second;
    }
    void release(Document& document) {
        if (!document.owner) return;
        auto owner = std::move(document.owner);
        document.pointer = 0;
        if (s_globalContext->currentProject() == owner) s_globalContext->setCurrentProject(nullptr);
        pump();
        owner.reset();
        pump();
    }
    Json describe(const std::string& id, const Document& doc) {
        auto* score = reinterpret_cast<engraving::MasterScore*>(doc.pointer);
        return {{"documentId", id}, {"title", checked_bytes(_title(doc.pointer))},
                {"pages", score->npages()}, {"parts", score->parts().size()},
                {"editable", !doc.composition.is_null()}};
    }
    Json insert(Document document) {
        const auto id = "score-" + std::to_string(++serial);
        try {
            auto result = describe(id, document);
            documents.emplace(id, std::move(document));
            return result;
        } catch (...) {
            release(document);
            throw;
        }
    }
};

Engine::Engine(int& argc, char** argv, const std::string& soundfont, const std::string&) : impl_(std::make_unique<Impl>()) {
    static bool initialized = false;
    if (initialized) throw std::runtime_error("Only one engine instance per process is supported");
    initialized = true;
    try {
        initialize_resources();
        auto* logger = muse::logger::Logger::instance();
        logger->clearDests();
        logger->addDest(new StderrLog());
        QCoreApplication::setOrganizationName("musescore-mcp");
        QCoreApplication::setApplicationName("native-engine");
        QStandardPaths::setTestModeEnabled(true);
        _init(argc, argv, !soundfont.empty());
        pump();
        if (!soundfont.empty()) {
            const auto path = QFileInfo(QString::fromUtf8(soundfont.c_str())).absoluteFilePath();
            if (!QFileInfo(path).isFile()) throw std::invalid_argument("SoundFont does not exist");
            auto repository = modularity::ioc()->resolve<audio::synth::ISoundFontRepository>("");
            const auto uri = audio::synth::SoundFontUri::fromLocalFile(path.toStdString());
            repository->addSoundFont(uri);
            pump();
            if (!repository->soundFonts().contains(uri)) throw std::runtime_error("SoundFont could not be loaded");
            impl_->has_soundfont = true;
        }
    } catch (const QString& error) { throw std::runtime_error(error.toStdString()); }
}
Engine::~Engine() {
    for (auto& [id, doc] : impl_->documents) {
        try { impl_->release(doc); }
        catch (...) { std::cerr << "ERROR: document cleanup failed\n"; }
    }
    try { s_globalContext->setCurrentProject(nullptr); }
    catch (...) { std::cerr << "ERROR: context cleanup failed\n"; }
    instances.clear();
}

std::string Engine::tools_json() {
    return
#include "tool_schema.inc"
    ;
}

std::string Engine::call(const std::string& tool, const std::string& arguments_json) {
    try {
        const auto args = Json::parse(arguments_json);
        Json result;
        if (tool == "score_create" || tool == "score_open") {
            if (impl_->documents.size() >= 16) throw std::runtime_error("Close a document before opening another (limit 16)");
            if (tool == "score_create") {
                only_keys(args, {"score"});
                const auto& score = args.at("score");
                result = impl_->insert(impl_->load(composition_xml(score), "musicxml", score));
            } else {
                only_keys(args, {"path", "musicxml"});
                if (args.contains("path") == args.contains("musicxml")) throw std::invalid_argument("Specify exactly one of path or musicxml");
                std::string data, format = "musicxml";
                if (args.contains("path")) {
                    const auto path = QString::fromUtf8(text_arg(args, "path").c_str());
                    format = QFileInfo(path).suffix().toLower().toStdString();
                    if (format == "xml") format = "musicxml";
                    if (format != "musicxml" && format != "mxl" && format != "mscz" && format != "mscx")
                        throw std::invalid_argument("Supported inputs: musicxml, xml, mxl, mscz, mscx");
                    QFile file(path);
                    if (!file.open(QIODevice::ReadOnly) || file.size() > max_input) throw std::runtime_error("Cannot read input, or it exceeds 8 MiB");
                    data = file.read(max_input + 1).toStdString();
                } else data = text_arg(args, "musicxml");
                result = impl_->insert(impl_->load(data, format));
            }
        } else if (tool == "score_get") {
            only_keys(args, {"documentId"});
            auto& doc = impl_->select(args);
            result = impl_->describe(text_arg(args, "documentId"), doc);
            result["composition"] = doc.composition;
            result["musicxml"] = checked_bytes(_saveXml(doc.pointer, -1));
            result["metadata"] = Json::parse(checked_bytes(_saveMetadata(doc.pointer)));
        } else if (tool == "score_replace_measures") {
            only_keys(args, {"documentId", "partIndex", "startMeasure", "measures"});
            auto& doc = impl_->select(args);
            if (doc.composition.is_null()) throw std::invalid_argument("Structured measure replacement requires a score_create document");
            auto updated = doc.composition;
            auto& parts = updated.at("parts");
            const auto part_index = index_arg(args, "partIndex", parts.size() - 1);
            auto& measures = parts.at(part_index).at("measures");
            const auto start = index_arg(args, "startMeasure", measures.size() - 1);
            const auto& replacement = args.at("measures");
            if (!replacement.is_array() || replacement.empty() || replacement.size() > measures.size() - start)
                throw std::invalid_argument("Replacement must fit within the existing measure range");
            for (size_t i = 0; i < replacement.size(); ++i) measures.at(start + i) = replacement.at(i);
            auto candidate = impl_->load(composition_xml(updated), "musicxml", updated);
            try {
                result = impl_->describe(text_arg(args, "documentId"), candidate);
            } catch (...) {
                impl_->release(candidate);
                s_globalContext->setCurrentProject(doc.owner);
                pump();
                throw;
            }
            impl_->release(doc);
            doc = std::move(candidate);
        } else if (tool == "score_close") {
            only_keys(args, {"documentId"});
            auto node = impl_->documents.extract(text_arg(args, "documentId"));
            if (node.empty()) throw std::invalid_argument("Unknown or closed documentId");
            impl_->release(node.mapped());
            result = {{"closed", true}};
        } else if (tool == "score_export") {
            only_keys(args, {"documentId", "format", "path", "page"});
            auto& doc = impl_->select(args);
            auto format = text_arg(args, "format");
            auto path = QString::fromUtf8(text_arg(args, "path").c_str());
            const auto pages = reinterpret_cast<engraving::MasterScore*>(doc.pointer)->npages();
            if ((format == "svg" || format == "png") && pages == 0) throw std::runtime_error("Score has no pages");
            int page = args.contains("page") ? static_cast<int>(index_arg(args, "page", pages > 0 ? pages - 1 : 0)) : 0;
            if (args.contains("page") && format != "svg" && format != "png") throw std::invalid_argument("page only applies to svg/png");
            NativeResult exported;
            if (format == "musicxml") exported = _saveXml(doc.pointer, -1);
            else if (format == "mxl") exported = _saveMxl(doc.pointer, -1);
            else if (format == "mscz") exported = _saveMsc(doc.pointer, true, -1);
            else if (format == "midi") exported = _saveMidi(doc.pointer, true, true, -1);
            else if (format == "svg") exported = _saveSvg(doc.pointer, page, true, -1);
            else if (format == "png") exported = _savePng(doc.pointer, page, true, false, -1);
            else if (format == "pdf") exported = _savePdf(doc.pointer, -1);
            else if (format == "metadata") exported = _saveMetadata(doc.pointer);
            else if (format == "wav") {
                if (!impl_->has_soundfont) throw std::runtime_error("Restart with --soundfont FILE to enable WAV export");
                if (MainAudio::Synth::getSequenceId() == -1) throw std::runtime_error("Playback sequence unavailable");
                exported = _saveAudio(doc.pointer, "wav", -1);
            } else throw std::invalid_argument("Unsupported export format");
            checked_bytes(exported);
            if (exported.bytes.isEmpty()) throw std::runtime_error("Exporter returned no data");
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(exported.bytes) != exported.bytes.size() || !file.commit())
                throw std::runtime_error("Cannot write export: " + path.toStdString());
            result = {{"path", QFileInfo(path).absoluteFilePath().toStdString()}, {"format", format}, {"bytes", exported.bytes.size()}};
        } else throw std::invalid_argument("Unknown tool");
        pump();
        return result.dump();
    } catch (const QString& error) { throw std::runtime_error(error.toStdString()); }
}
}
