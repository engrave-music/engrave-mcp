#include "audio_codecs.h"

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace music {
namespace {

constexpr int kSampleRate = static_cast<int>(audio_sample_rate);
constexpr int kChannels = 2;
constexpr std::size_t kMaxFramesPerChunk = audio_chunk_frames;
constexpr std::size_t kMaxEncodedBytes = std::size_t{256} * 1024 * 1024;
constexpr std::size_t kInterleavedSamples = audio_chunk_frames * 2;

int format_bits(AudioFormat format) {
    switch (format) {
    case AudioFormat::Wav:
        return SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    case AudioFormat::Flac:
        return SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
    case AudioFormat::Ogg:
        return SF_FORMAT_OGG | SF_FORMAT_VORBIS;
    case AudioFormat::Mp3:
        return SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III;
    }
    throw std::runtime_error("unsupported audio format");
}

const char* format_name(AudioFormat format) noexcept {
    switch (format) {
    case AudioFormat::Wav:
        return "WAV/PCM16";
    case AudioFormat::Flac:
        return "FLAC/PCM16";
    case AudioFormat::Ogg:
        return "Ogg/Vorbis";
    case AudioFormat::Mp3:
        return "MPEG Layer III";
    }
    return "unknown audio format";
}

std::string sndfile_error_text(SNDFILE* file) {
    if (file != nullptr) {
        const int error = sf_error(file);
        if (error != 0) {
            const char* text = sf_strerror(file);
            if (text != nullptr && *text != '\0') {
                return text;
            }
            const char* numbered = sf_error_number(error);
            if (numbered != nullptr && *numbered != '\0') {
                return numbered;
            }
        }
        return "no libsndfile error recorded";
    }

    const char* text = sf_strerror(nullptr);
    if (text != nullptr && *text != '\0') {
        return text;
    }
    return "unknown libsndfile error";
}

std::string sndfile_error_number_text(int error) {
    if (error <= 0) {
        return "unknown libsndfile error";
    }
    const char* text = sf_error_number(error);
    if (text != nullptr && *text != '\0') {
        return text;
    }
    return "unknown libsndfile error";
}

SF_INFO make_sfinfo(AudioFormat format) {
    SF_INFO info{};
    info.samplerate = kSampleRate;
    info.channels = kChannels;
    info.format = format_bits(format);
    info.sections = 1;
    info.seekable = SF_TRUE;
    return info;
}

// libsndfile's SF_VIRTUAL_IO target. It owns the whole encoded artifact in
// memory and exposes only the seekable-buffer operations libsndfile needs,
// including overwrites for header patching at close time.
class MemoryWriter {
public:
    MemoryWriter() = default;
    MemoryWriter(const MemoryWriter&) = delete;
    MemoryWriter& operator=(const MemoryWriter&) = delete;

    sf_count_t get_filelen() const noexcept {
        return static_cast<sf_count_t>(buffer_.size());
    }

    sf_count_t tell() const noexcept {
        return static_cast<sf_count_t>(position_);
    }

    sf_count_t seek(sf_count_t offset, int whence) {
        sf_count_t base = 0;
        switch (whence) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = static_cast<sf_count_t>(position_);
            break;
        case SEEK_END:
            base = static_cast<sf_count_t>(buffer_.size());
            break;
        default:
            throw std::runtime_error("libsndfile requested an unsupported seek mode");
        }

        const sf_count_t limit = static_cast<sf_count_t>(kMaxEncodedBytes);
        if (offset >= 0) {
            if (offset > limit - base) {
                throw std::runtime_error("libsndfile seek exceeds the 256 MiB encoded audio limit");
            }
        } else if (offset < -base) {
            throw std::runtime_error("libsndfile seek precedes the start of the encoded audio buffer");
        }

        const sf_count_t next = base + offset;
        position_ = static_cast<std::size_t>(next);
        return next;
    }

    sf_count_t read(void* ptr, sf_count_t count) {
        if (count < 0) {
            throw std::runtime_error("libsndfile requested a negative-size read");
        }
        if (count == 0) {
            return 0;
        }
        if (ptr == nullptr) {
            throw std::runtime_error("libsndfile passed a null read buffer");
        }

        const sf_count_t limit = static_cast<sf_count_t>(kMaxEncodedBytes);
        const std::size_t requested =
            count > limit ? kMaxEncodedBytes : static_cast<std::size_t>(count);
        const std::size_t available =
            position_ < buffer_.size() ? buffer_.size() - position_ : 0;
        const std::size_t copied = std::min(requested, available);

        if (copied != 0) {
            std::memcpy(ptr, buffer_.data() + position_, copied);
        }
        position_ += copied;
        return static_cast<sf_count_t>(copied);
    }

    sf_count_t write(const void* ptr, sf_count_t count) {
        if (count < 0) {
            throw std::runtime_error("libsndfile requested a negative-size write");
        }
        if (count == 0) {
            return 0;
        }
        if (ptr == nullptr) {
            throw std::runtime_error("libsndfile passed a null write buffer");
        }
        if (count > static_cast<sf_count_t>(kMaxEncodedBytes)) {
            throw std::runtime_error("encoded audio exceeds the 256 MiB limit");
        }

        const std::size_t bytes = static_cast<std::size_t>(count);
        if (position_ > kMaxEncodedBytes - bytes) {
            throw std::runtime_error("encoded audio exceeds the 256 MiB limit");
        }

        const std::size_t end = position_ + bytes;
        if (end > buffer_.size()) {
            buffer_.resize(end);
        }
        std::memcpy(buffer_.data() + position_, ptr, bytes);
        position_ = end;
        return count;
    }

    void latch_current_exception() noexcept {
        if (!error_) {
            error_ = std::current_exception();
        }
    }

    void rethrow_if_error() const {
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

    std::string take_buffer() {
        return std::move(buffer_);
    }

private:
    std::string buffer_;
    std::size_t position_ = 0;
    std::exception_ptr error_;
};

extern "C" sf_count_t memory_vio_get_filelen(void* user_data) noexcept {
    auto* writer = static_cast<MemoryWriter*>(user_data);
    return writer != nullptr ? writer->get_filelen() : 0;
}

extern "C" sf_count_t memory_vio_seek(sf_count_t offset, int whence, void* user_data) noexcept {
    auto* writer = static_cast<MemoryWriter*>(user_data);
    if (writer == nullptr) {
        return -1;
    }
    try {
        return writer->seek(offset, whence);
    } catch (...) {
        writer->latch_current_exception();
        return -1;
    }
}

extern "C" sf_count_t memory_vio_read(void* ptr, sf_count_t count, void* user_data) noexcept {
    auto* writer = static_cast<MemoryWriter*>(user_data);
    if (writer == nullptr) {
        return 0;
    }
    try {
        return writer->read(ptr, count);
    } catch (...) {
        writer->latch_current_exception();
        return 0;
    }
}

extern "C" sf_count_t memory_vio_write(const void* ptr, sf_count_t count, void* user_data) noexcept {
    auto* writer = static_cast<MemoryWriter*>(user_data);
    if (writer == nullptr) {
        return 0;
    }
    try {
        return writer->write(ptr, count);
    } catch (...) {
        writer->latch_current_exception();
        return 0;
    }
}

extern "C" sf_count_t memory_vio_tell(void* user_data) noexcept {
    auto* writer = static_cast<MemoryWriter*>(user_data);
    return writer != nullptr ? writer->tell() : 0;
}

// Closes exactly once. Destructor cleanup is intentionally unable to throw;
// an explicit close() reports the libsndfile result to the caller.
class SoundFileOwner {
public:
    explicit SoundFileOwner(SNDFILE* file) noexcept : file_(file) {}
    SoundFileOwner(const SoundFileOwner&) = delete;
    SoundFileOwner& operator=(const SoundFileOwner&) = delete;

    ~SoundFileOwner() noexcept {
        if (file_ != nullptr) {
            SNDFILE* file = file_;
            file_ = nullptr;
            (void)sf_close(file);
        }
    }

    SNDFILE* get() const noexcept {
        return file_;
    }

    int close() noexcept {
        SNDFILE* file = file_;
        file_ = nullptr;
        return file != nullptr ? sf_close(file) : 0;
    }

private:
    SNDFILE* file_;
};

void throw_if_cancelled(AudioSession& session) {
    if (session.cancelled()) {
        throw AudioEncodingCancelled();
    }
}

void validate_chunk(const AudioChunk& chunk) {
    const std::size_t frames = chunk.left.size();
    if (chunk.right.size() != frames) {
        throw std::runtime_error("audio chunk has mismatched left/right channel lengths");
    }
    if (frames > kMaxFramesPerChunk) {
        throw std::runtime_error("audio chunk exceeds the " +
                                 std::to_string(kMaxFramesPerChunk) + "-frame maximum");
    }
    if (frames == 0 && !chunk.done) {
        throw std::runtime_error("audio session returned an empty non-terminal chunk");
    }
}

void write_chunk(SNDFILE* file, MemoryWriter& writer, AudioSession& session,
                 const AudioChunk& chunk) {
    const std::size_t frame_count = chunk.left.size();
    std::array<float, kInterleavedSamples> interleaved{};
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        interleaved[frame * 2] = chunk.left[frame];
        interleaved[frame * 2 + 1] = chunk.right[frame];
    }

    std::size_t written_frames = 0;
    while (written_frames < frame_count) {
        const sf_count_t remaining = static_cast<sf_count_t>(frame_count - written_frames);
        const sf_count_t written =
            sf_writef_float(file, interleaved.data() + written_frames * 2, remaining);

        writer.rethrow_if_error();
        if (written <= 0 || sf_error(file) != 0) {
            throw std::runtime_error("libsndfile failed to encode audio frames: " +
                                     sndfile_error_text(file));
        }

        const std::size_t written_count = static_cast<std::size_t>(written);
        if (written_count > frame_count - written_frames) {
            throw std::runtime_error("libsndfile reported an invalid encoded frame count");
        }
        written_frames += written_count;

        throw_if_cancelled(session);
    }
}

} // namespace

std::string encode_audio(AudioSession& session, AudioFormat format) {
    throw_if_cancelled(session);

    SF_INFO info = make_sfinfo(format);
    if (sf_format_check(&info) == 0) {
        throw std::runtime_error(std::string("libsndfile does not support the requested format: ") +
                                 format_name(format));
    }

    MemoryWriter writer;
    SF_VIRTUAL_IO vio{};
    vio.get_filelen = &memory_vio_get_filelen;
    vio.seek = &memory_vio_seek;
    vio.read = &memory_vio_read;
    vio.write = &memory_vio_write;
    vio.tell = &memory_vio_tell;

    SNDFILE* raw_file = sf_open_virtual(&vio, SFM_WRITE, &info, &writer);
    if (raw_file == nullptr) {
        writer.rethrow_if_error();
        throw std::runtime_error(std::string("libsndfile could not open the requested format: ") +
                                 format_name(format) + ": " + sndfile_error_text(nullptr));
    }
    SoundFileOwner file(raw_file);
    writer.rethrow_if_error();
    throw_if_cancelled(session);

    if (format == AudioFormat::Wav || format == AudioFormat::Flac) {
        if (sf_command(file.get(), SFC_SET_CLIPPING, nullptr, SF_TRUE) != SF_TRUE) {
            throw std::runtime_error(std::string("libsndfile could not enable PCM clipping for ") +
                                     format_name(format) + ": " + sndfile_error_text(file.get()));
        }
    }

    for (;;) {
        throw_if_cancelled(session);
        AudioChunk chunk = session.read_chunk();
        throw_if_cancelled(session);

        validate_chunk(chunk);
        if (chunk.left.empty()) {
            // validate_chunk() guarantees an empty chunk is terminal.
            break;
        }

        write_chunk(file.get(), writer, session, chunk);
        if (chunk.done) {
            break;
        }
    }

    const int close_result = file.close();
    throw_if_cancelled(session);
    writer.rethrow_if_error();
    if (close_result != 0) {
        throw std::runtime_error(std::string("libsndfile failed to finalize the encoded audio: ") +
                                 sndfile_error_number_text(close_result));
    }

    return writer.take_buffer();
}

} // namespace music
