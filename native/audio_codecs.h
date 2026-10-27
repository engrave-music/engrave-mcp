#pragma once

#include "audio_session.h"

#include <stdexcept>
#include <string>

namespace music {

enum class AudioFormat { Wav, Flac, Ogg, Mp3 };

// Thrown when AudioSession::cancelled() becomes true before or during an
// encode. No partial encoded artifact is returned in that case.
class AudioEncodingCancelled final : public std::runtime_error {
public:
    AudioEncodingCancelled() : std::runtime_error("audio encoding cancelled") {}
};

// Encodes a 44.1 kHz stereo float session into the requested container/codec
// entirely in memory. Throws std::runtime_error on failure and
// AudioEncodingCancelled when the session is cancelled.
std::string encode_audio(AudioSession& session, AudioFormat format);

} // namespace music
