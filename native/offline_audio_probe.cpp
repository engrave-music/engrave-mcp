#include "offline_audio.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("Usage: offline_audio_probe MIDI SOUNDFONT OUTPUT.wav");
        std::ifstream input(std::filesystem::u8path(argv[1]), std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read MIDI");
        std::string midi((std::istreambuf_iterator<char>(input)), {});
        const auto wav = music::render_midi_wav(midi, argv[2]);
        std::ofstream output(std::filesystem::u8path(argv[3]), std::ios::binary);
        output.write(wav.data(), wav.size());
        output.close();
        if (!output) throw std::runtime_error("Cannot write WAV");
        std::cout << "Rendered " << wav.size() << " bytes\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
