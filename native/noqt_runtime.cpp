#include "noqt_runtime.h"
#include "noqt_filesystem.h"
#include "offline_audio.h"
#include "score_metadata.h"
#include "score_render.h"
#include "global/log.h"
#include "global/io/buffer.h"
#include "global/serialization/zipreader.h"
#include "global/serialization/zipwriter.h"
#include "global/serialization/xmlstreamreader.h"
#include "draw/internal/fontprovider.h"
#include "draw/internal/fontsengine.h"
#include "draw/internal/fontsdatabase.h"
#include "engraving/engravingmodule.h"
#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/instrtemplate.h"
#include "engraving/infrastructure/localfileinfoprovider.h"
#include "engraving/rw/inoutdata.h"
#include "mpe/mpemodule.h"
#include "importexport/musicxml/internal/import/importmusicxml.h"
#include "importexport/musicxml/internal/export/exportmusicxml.h"
#include "importexport/musicxml/internal/musicxmlconfiguration.h"
#include "importexport/midi/internal/midiexport/exportmidi.h"
#include <iostream>
#include <stdexcept>

namespace music {
using namespace muse;
using namespace mu;
namespace {
class StderrLog final : public logger::LogDest {
public:
    StderrLog() : LogDest(logger::LogLayout("${type}: ${message}")) {}
    std::string name() const override { return "native-stderr"; }
    void write(const logger::LogMsg& message) override { std::cerr << m_layout.output(message) << '\n'; }
};

// FreeType's upstream text provider assumes 360 DPI; engraving uses 1200.
class EngravingFontProvider final : public draw::FontProvider {
    static draw::Font scaled(draw::Font f) { f.setPointSizeF(f.pointSizeF() * (1200.0 / 360.0)); return f; }
public:
    EngravingFontProvider() : FontProvider(nullptr) {}
    double lineSpacing(const draw::Font& f) const override { return FontProvider::lineSpacing(scaled(f)); }
    double xHeight(const draw::Font& f) const override { return FontProvider::xHeight(scaled(f)); }
    double height(const draw::Font& f) const override { return FontProvider::height(scaled(f)); }
    double capHeight(const draw::Font& f) const override { return FontProvider::capHeight(scaled(f)); }
    double ascent(const draw::Font& f) const override { return FontProvider::ascent(scaled(f)); }
    double descent(const draw::Font& f) const override { return FontProvider::descent(scaled(f)); }
    double horizontalAdvance(const draw::Font& f, const String& s) const override { return FontProvider::horizontalAdvance(scaled(f), s); }
    double horizontalAdvance(const draw::Font& f, char32_t c) const override { return FontProvider::horizontalAdvance(scaled(f), c); }
    RectF boundingRect(const draw::Font& f, const String& s) const override { return FontProvider::boundingRect(scaled(f), s); }
    RectF boundingRect(const draw::Font& f, char32_t c) const override { return FontProvider::boundingRect(scaled(f), c); }
    RectF tightBoundingRect(const draw::Font& f, const String& s) const override { return FontProvider::tightBoundingRect(scaled(f), s); }
};

void check(const Ret& result) { if (!result) throw std::runtime_error(result.toString()); }
std::string bytes(const ByteArray& data) { return {reinterpret_cast<const char*>(data.constData()), data.size()}; }
ByteArray byte_array(const std::string& data) { return ByteArray(data.data(), data.size()); }

void check_zip(ZipReader& zip) {
    uint64_t total = 0;
    const auto entries = zip.fileInfoList();
    if (entries.empty() || entries.size() > 4096) throw std::invalid_argument("Invalid archive entry count");
    for (const auto& entry : entries) {
        if (entry.size > 64 * 1024 * 1024 || total + entry.size > 128 * 1024 * 1024)
            throw std::invalid_argument("Uncompressed archive exceeds size limit");
        total += entry.size;
    }
    if (zip.hasError()) throw std::invalid_argument("Invalid archive");
}
}

void initialize_native_runtime(const std::string& assets) {
    static bool initialized = false;
    if (initialized) throw std::runtime_error("Only one engine instance per process is supported");
    initialized = true;
    auto* logger = logger::Logger::instance();
    logger->clearDests();
    logger->addDest(new StderrLog());
    const auto root = std::filesystem::u8path(assets);
    if (!std::filesystem::is_regular_file(root / "instruments.xml"))
        throw std::invalid_argument("Missing native resources: " + assets);
    modularity::globalIoc()->registerExport<io::IFileSystem>("native", new NativeFileSystem(root));
    modularity::globalIoc()->registerExport<draw::IFontsDatabase>("native", new draw::FontsDatabase());
    auto fonts = std::make_shared<draw::FontsEngine>(modularity::globalCtx());
    modularity::globalIoc()->registerExport<draw::IFontsEngine>("native", fonts);
    modularity::globalIoc()->registerExport<draw::IFontProvider>("native", new EngravingFontProvider());
    fonts->init();
    // Module services live in the process-wide IoC; no desktop application is created.
    engraving::EngravingModule engraving;
    engraving.registerExports();
    engraving.onInit(IApplication::RunMode::ConsoleApp);
    mpe::MpeModule mpe;
    mpe.registerExports();
    auto xml = std::make_shared<iex::musicxml::MusicXmlConfiguration>();
    modularity::globalIoc()->registerExport<iex::musicxml::IMusicXmlConfiguration>("native", xml);
    xml->init();
    engraving::loadInstrumentTemplates(":/instruments.xml");
}

ScoreProject load_native_score(const std::string& input, const std::string& format) {
    if (input.empty() || input.size() > 8 * 1024 * 1024) throw std::invalid_argument("Input must contain 1..8388608 bytes");
    auto project = engraving::EngravingProject::create(modularity::globalCtx());
    project->setFileInfoProvider(std::make_shared<engraving::LocalFileInfoProvider>("Untitled.mscz"));
    ByteArray data = byte_array(input);
    if (format == "mxl" || format == "mscz") {
        io::Buffer buffer(&data);
        buffer.open(io::IODevice::ReadOnly);
        ZipReader zip(&buffer);
        check_zip(zip);
        if (format == "mxl") {
            XmlStreamReader xml(zip.fileData("META-INF/container.xml"));
            std::string root;
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isStartElement() && xml.name() == "rootfile") {
                    root = xml.attribute("full-path").toStdString();
                    break;
                }
            }
            if (root.empty()) throw std::invalid_argument("MXL has no rootfile");
            data = zip.fileData(root);
        }
    }
    if (format == "mscz" || format == "mscx") {
        ByteArray archive;
        if (format == "mscx") {
            io::Buffer out(&archive);
            out.open(io::IODevice::WriteOnly);
            ZipWriter zip(&out);
            zip.addFile("score.mscx", data);
            zip.close();
            data = archive;
        }
        io::Buffer buffer(&data);
        engraving::MscReader::Params params;
        params.device = &buffer;
        params.mode = engraving::MscIoMode::Zip;
        engraving::MscReader reader(params);
        check(reader.open());
        if (!reader.imageFileNames().empty()) throw std::invalid_argument("Embedded score images are not yet supported by the native backend");
        engraving::rw::ReadInOutData inout;
        inout.forcePageMode = true;
        check(project->loadMscz(reader, &inout, false));
    } else if (format == "musicxml" || format == "xml" || format == "mxl") {
        XmlStreamReader scan(data);
        while (!scan.atEnd()) {
            scan.readNext();
            if (scan.isStartElement() && scan.name() == "image")
                throw std::invalid_argument("Embedded score images are not yet supported by the native backend");
        }
        if (iex::musicxml::importMusicXmlfromBuffer(project->masterScore(), u"input.musicxml", data) != engraving::Err::NoError)
            throw std::runtime_error("MusicXML import failed");
    } else throw std::invalid_argument("Unsupported score input format");
    auto* score = project->masterScore();
    score->connectTies();
    check(project->setupMasterScore(true));
    score->setLayoutAll();
    score->update();
    return project;
}

std::string export_native_score(const ScoreProject& project, const std::string& format, int page, const std::string& soundfont) {
    auto* score = project->masterScore();
    if (format == "metadata") return score_metadata(score).dump();
    if (format == "svg" || format == "png" || format == "pdf") return render_score(score, format, page);
    ByteArray data;
    io::Buffer buffer(&data);
    buffer.open(io::IODevice::WriteOnly);
    bool success = false;
    if (format == "musicxml") success = iex::musicxml::saveXml(score, &buffer);
    else if (format == "mxl") success = iex::musicxml::saveMxl(score, &buffer);
    else if (format == "mscz") {
        engraving::MscWriter::Params params;
        params.device = &buffer;
        params.mainFileName = u"score.mscx";
        engraving::MscWriter writer(params);
        check(writer.open());
        success = project->writeMscz(writer, false);
        writer.close();
        success = success && !writer.hasError();
    } else if (format == "midi" || format == "wav") {
        iex::midi::ExportMidi writer(score);
        success = writer.write(&buffer, true, true);
    } else throw std::invalid_argument("Unsupported export format");
    buffer.close();
    if (!success || data.empty()) throw std::runtime_error("Score export failed");
    if (format == "wav") {
        if (soundfont.empty()) throw std::runtime_error("Restart with --soundfont FILE to enable WAV export");
        return render_midi_wav(bytes(data), soundfont);
    }
    return bytes(data);
}
}
