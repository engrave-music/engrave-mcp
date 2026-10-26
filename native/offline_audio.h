#pragma once

#include <string>

namespace music {

// Renders standard MIDI data to 16-bit 44.1kHz stereo PCM WAV using FluidSynth.
// Returns binary RIFF WAVE audio data.
std::string render_midi_wav(const std::string& midi, const std::string& soundfont);

} // namespace music
