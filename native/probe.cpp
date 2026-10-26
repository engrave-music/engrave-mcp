#include "engine.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: musescore-native-probe INPUT.musicxml OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        const std::string input = argv[1];
        const auto output = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
        std::filesystem::create_directories(output);
        music::Engine engine(argc, argv, "");
        using Json = nlohmann::json;
        const auto opened = Json::parse(engine.call("score_open", Json{{"path", input}}.dump()));
        const auto id = opened.at("documentId");
        for (const auto& [format, filename] : std::initializer_list<std::pair<std::string, std::string>>{
            {"musicxml", "score.musicxml"}, {"midi", "score.mid"}, {"svg", "score.svg"},
            {"pdf", "score.pdf"}, {"metadata", "metadata.json"}}) {
            auto bytes = (output / filename).u8string();
            engine.call("score_export", Json{{"documentId", id}, {"format", format},
                {"path", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())}}.dump());
        }
        engine.call("score_close", Json{{"documentId", id}}.dump());
        auto output_bytes = output.u8string();
        std::cout << Json{{"native", true}, {"pointerBits", sizeof(uintptr_t) * 8},
            {"title", opened.at("title")},
            {"output", std::string(reinterpret_cast<const char*>(output_bytes.data()), output_bytes.size())}}.dump() << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
