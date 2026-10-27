#include "audio_codecs.h"
#include "audio_session.h"

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace music;

static_assert(audio_chunk_frames == 512, "audio_chunk_frames must remain 512");
static_assert(audio_sample_rate == 44100, "audio_sample_rate must remain 44100");
static_assert(audio_max_tracks == 15, "audio_max_tracks must remain 15");

constexpr std::size_t kDecodeFrames = 4096;
constexpr std::size_t kSeekCompareFrames = 4096;
constexpr std::size_t kMaxBatchSize = 256;
constexpr std::size_t kMidiByteCap = 8u * 1024u * 1024u;
constexpr double kMaxSessionSeconds = 600.0;
constexpr double kSeekNormalizedTolerance = 0.10;
constexpr double kSeekCorrelationFloor = 0.95;
constexpr double kPcm16ComparisonTolerance = 2.0 / 32768.0;
constexpr double kMixRatioTolerance = 0.10;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void expect(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void expect_finite(double value, const std::string& what) {
    expect(std::isfinite(value), what + " is not finite");
}

std::string path_text(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

void append_byte(std::string& out, unsigned int value) {
    out.push_back(static_cast<char>(value & 0xFFu));
}

void append_u16be(std::string& out, std::uint16_t value) {
    append_byte(out, (value >> 8) & 0xFFu);
    append_byte(out, value & 0xFFu);
}

void append_u32be(std::string& out, std::uint32_t value) {
    append_byte(out, (value >> 24) & 0xFFu);
    append_byte(out, (value >> 16) & 0xFFu);
    append_byte(out, (value >> 8) & 0xFFu);
    append_byte(out, value & 0xFFu);
}

void append_vlq(std::string& out, std::uint64_t value) {
    expect(value <= 0x0FFFFFFFu, "MIDI VLQ value is out of range");
    std::array<unsigned int, 10> groups{};
    std::size_t count = 0;
    do {
        groups[count++] = static_cast<unsigned int>(value & 0x7Fu);
        value >>= 7;
    } while (value != 0);
    for (std::size_t i = count; i > 0; --i) {
        unsigned int byte = groups[i - 1];
        if (i > 1) {
            byte |= 0x80u;
        }
        append_byte(out, byte);
    }
}

// Minimal format-0 SMF: 480 PPQ, tempo 500000 us/quarter, program 0 on
// channel 0, one note-on at tick 0 and one note-off at note_off_ticks.
std::string make_smf(int pitch, std::uint64_t note_off_ticks, std::size_t metadata_bytes = 0) {
    expect(pitch >= 0 && pitch <= 127, "test MIDI pitch is out of range");
    expect(note_off_ticks <= 0x0FFFFFFFu, "test MIDI note-off tick is out of range");

    std::string track;
    track.reserve(64 + metadata_bytes);

    append_vlq(track, 0); // tempo: 500000 us per quarter note
    append_byte(track, 0xFF);
    append_byte(track, 0x51);
    append_byte(track, 0x03);
    append_byte(track, 0x07);
    append_byte(track, 0xA1);
    append_byte(track, 0x20);

    if (metadata_bytes > 0) {
        append_vlq(track, 0);
        append_byte(track, 0xFF);
        append_byte(track, 0x01); // text meta event
        append_vlq(track, metadata_bytes);
        track.append(metadata_bytes, 'x');
    }

    append_vlq(track, 0);
    append_byte(track, 0xC0); // program change, channel 0
    append_byte(track, 0x00);

    append_vlq(track, 0);
    append_byte(track, 0x90); // note-on, channel 0
    append_byte(track, static_cast<unsigned int>(pitch));
    append_byte(track, 100);

    append_vlq(track, note_off_ticks);
    append_byte(track, 0x80); // note-off, channel 0
    append_byte(track, static_cast<unsigned int>(pitch));
    append_byte(track, 0x40);

    append_vlq(track, 0);
    append_byte(track, 0xFF);
    append_byte(track, 0x2F); // end of track
    append_byte(track, 0x00);

    expect(track.size() <= std::numeric_limits<std::uint32_t>::max(),
           "test MIDI track is too large");

    std::string smf;
    smf.reserve(14 + 8 + track.size());
    smf.append("MThd", 4);
    append_u32be(smf, 6);
    append_u16be(smf, 0); // format 0
    append_u16be(smf, 1); // one track
    append_u16be(smf, 480); // 480 PPQ
    smf.append("MTrk", 4);
    append_u32be(smf, static_cast<std::uint32_t>(track.size()));
    smf.append(track);
    return smf;
}

std::vector<AudioTrack> default_stems() {
    return {
        AudioTrack{10, make_smf(60, 960)},
        AudioTrack{20, make_smf(67, 480)},
    };
}

std::vector<AudioTrack> many_stems(int count) {
    std::vector<AudioTrack> tracks;
    tracks.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        tracks.push_back(AudioTrack{100 + i, make_smf(60 + i, 480)});
    }
    return tracks;
}

bool has_track(const AudioMix& mix, std::int64_t track_id) {
    for (const auto& track : mix.tracks) {
        if (track.track_id == track_id) {
            return true;
        }
    }
    return false;
}

AudioOutputParams& track_output(AudioMix& mix, std::int64_t track_id) {
    for (auto& track : mix.tracks) {
        if (track.track_id == track_id) {
            return track.output;
        }
    }
    fail("audio mix is missing track " + std::to_string(track_id));
}

struct StreamStats {
    std::vector<float> left;
    std::vector<float> right;
    std::size_t frames = 0;
    std::size_t chunks = 0;
    std::size_t max_chunk_frames = 0;
    std::size_t nonzero_samples = 0;
    double max_abs = 0;
    double sum_squares = 0;
    double sum_squares_left = 0;
    double sum_squares_right = 0;

    double duration_seconds() const {
        return static_cast<double>(frames) / static_cast<double>(audio_sample_rate);
    }

    double mean_square() const {
        if (frames == 0) {
            return 0;
        }
        return sum_squares / (2.0 * static_cast<double>(frames));
    }

    double rms() const {
        return std::sqrt(mean_square());
    }

    double left_rms() const {
        if (frames == 0) {
            return 0;
        }
        return std::sqrt(sum_squares_left / static_cast<double>(frames));
    }

    double right_rms() const {
        if (frames == 0) {
            return 0;
        }
        return std::sqrt(sum_squares_right / static_cast<double>(frames));
    }
};

struct Continuity {
    std::size_t expected_start_frame = 0;
    bool check_expected_start = false;
    bool started = false;
    double previous_end_time = 0;
};

void consume_chunk(const AudioChunk& chunk, Continuity& continuity, StreamStats& stats, bool capture) {
    const std::size_t frames = chunk.left.size();
    expect(frames == chunk.right.size(), "AudioChunk left/right frame count mismatch");
    expect(frames <= audio_chunk_frames, "AudioChunk exceeds audio_chunk_frames");

    if (frames == 0) {
        expect(chunk.done, "empty non-terminal AudioChunk");
        return;
    }

    expect_finite(chunk.start_time, "AudioChunk::start_time");
    expect_finite(chunk.end_time, "AudioChunk::end_time");
    expect(chunk.start_time >= 0, "AudioChunk::start_time is negative");

    const double frame_seconds = 1.0 / static_cast<double>(audio_sample_rate);
    if (!continuity.started) {
        if (continuity.check_expected_start) {
            const double raw_start_frame = std::floor(chunk.start_time / frame_seconds + 1e-9);
            expect(raw_start_frame >= 0, "AudioChunk start frame is negative");
            const auto actual_start_frame = static_cast<std::size_t>(raw_start_frame);
            expect(actual_start_frame == continuity.expected_start_frame,
                   "AudioChunk starts at frame " + std::to_string(actual_start_frame) +
                       ", expected " + std::to_string(continuity.expected_start_frame));
        } else {
            expect(std::abs(chunk.start_time) <= 1e-9,
                   "AudioChunk does not start at time 0");
        }
        continuity.started = true;
    } else {
        expect(std::abs(chunk.start_time - continuity.previous_end_time) <= 1e-9,
               "AudioChunk start_time is not continuous with the previous end_time");
    }

    const double expected_span = static_cast<double>(frames) * frame_seconds;
    expect(std::abs((chunk.end_time - chunk.start_time) - expected_span) <= 1e-9,
           "AudioChunk end_time/start_time span does not match frame count");
    continuity.previous_end_time = chunk.end_time;

    if (capture) {
        stats.left.insert(stats.left.end(), chunk.left.begin(), chunk.left.end());
        stats.right.insert(stats.right.end(), chunk.right.begin(), chunk.right.end());
    }

    for (std::size_t i = 0; i < frames; ++i) {
        const float left = chunk.left[i];
        const float right = chunk.right[i];
        expect(std::isfinite(left) && std::isfinite(right),
               "AudioChunk contains a non-finite sample");
        if (left != 0.0f || right != 0.0f) {
            ++stats.nonzero_samples;
        }
        const double left_abs = std::abs(static_cast<double>(left));
        const double right_abs = std::abs(static_cast<double>(right));
        stats.max_abs = std::max(stats.max_abs, std::max(left_abs, right_abs));
        stats.sum_squares_left += static_cast<double>(left) * static_cast<double>(left);
        stats.sum_squares_right += static_cast<double>(right) * static_cast<double>(right);
    }
    stats.sum_squares = stats.sum_squares_left + stats.sum_squares_right;
    stats.frames += frames;
    ++stats.chunks;
    stats.max_chunk_frames = std::max(stats.max_chunk_frames, frames);
}

StreamStats read_full(AudioSession& session, std::size_t expected_start_frame, bool capture) {
    StreamStats stats;
    Continuity continuity;
    continuity.check_expected_start = true;
    continuity.expected_start_frame = expected_start_frame;

    for (;;) {
        AudioChunk chunk = session.read_chunk();
        if (chunk.left.empty() && chunk.right.empty()) {
            expect(chunk.done, "read_chunk returned an empty non-terminal chunk");
            break;
        }
        consume_chunk(chunk, continuity, stats, capture);
        if (chunk.done) {
            break;
        }
    }

    AudioChunk terminal = session.read_chunk();
    expect(terminal.done && terminal.left.empty() && terminal.right.empty(),
           "read_chunk after terminal did not return empty/done");
    return stats;
}

struct BatchResult {
    StreamStats stats;
    std::size_t sizes_tested = 0;
    bool terminal = false;
};

BatchResult read_batches(AudioSession& session, bool descending) {
    BatchResult result;
    Continuity continuity;
    continuity.check_expected_start = true;
    continuity.expected_start_frame = 0;

    for (std::size_t step = 0; step < kMaxBatchSize; ++step) {
        const std::size_t batch_size = descending ? kMaxBatchSize - step : step + 1;
        std::vector<AudioChunk> chunks = session.read_batch(batch_size);
        expect(chunks.size() <= batch_size,
               "read_batch returned more chunks than the requested batch size");
        if (!result.terminal && chunks.empty()) {
            fail("read_batch returned no chunks before the terminal chunk");
        }
        for (const AudioChunk& chunk : chunks) {
            if (result.terminal) {
                expect(chunk.done && chunk.left.empty() && chunk.right.empty(),
                       "read_batch returned audio after the terminal chunk");
                continue;
            }
            if (chunk.left.empty() && chunk.right.empty()) {
                expect(chunk.done, "read_batch returned an empty non-terminal chunk");
                result.terminal = true;
                continue;
            }
            consume_chunk(chunk, continuity, result.stats, false);
            if (chunk.done) {
                result.terminal = true;
            }
        }
        ++result.sizes_tested;
    }

    expect(result.sizes_tested == kMaxBatchSize, "did not exercise every batch size 1..256");
    expect(result.terminal, "batch reads did not reach the terminal chunk");
    return result;
}

template <typename Function>
void expect_throws(Function&& function, const std::string& what) {
    bool threw = false;
    try {
        function();
    } catch (...) {
        threw = true;
    }
    expect(threw, what);
}

void expect_session_rejected(const std::vector<AudioTrack>& tracks,
                             const std::string& soundfont,
                             const std::string& what) {
    bool rejected = false;
    try {
        AudioSession session(tracks, soundfont);
        (void)session.read_chunk();
    } catch (...) {
        rejected = true;
    }
    expect(rejected, what);
}

StreamStats render_single(const AudioTrack& track, const std::string& soundfont) {
    AudioSession session({track}, soundfont);
    return read_full(session, 0, false);
}

struct Comparison {
    double max_abs_diff = 0;
    double rms_diff = 0;
    double normalized_rms_diff = 0;
    double correlation = 0;
    std::size_t compared_samples = 0;
};

Comparison compare_vectors(const std::vector<float>& reference, const std::vector<float>& candidate) {
    const std::size_t count = std::min(reference.size(), candidate.size());
    expect(count > 0, "cannot compare empty sample vectors");

    double sum_reference_square = 0;
    double sum_candidate_square = 0;
    double sum_cross = 0;
    double sum_difference_square = 0;
    double max_difference = 0;

    for (std::size_t i = 0; i < count; ++i) {
        const double a = static_cast<double>(reference[i]);
        const double b = static_cast<double>(candidate[i]);
        const double difference = a - b;
        sum_reference_square += a * a;
        sum_candidate_square += b * b;
        sum_cross += a * b;
        sum_difference_square += difference * difference;
        max_difference = std::max(max_difference, std::abs(difference));
    }

    const double reference_rms = std::sqrt(sum_reference_square / static_cast<double>(count));
    expect(reference_rms > 0, "reference sample vector is silent");
    const double denominator = std::sqrt(sum_reference_square * sum_candidate_square);

    Comparison result;
    result.max_abs_diff = max_difference;
    result.rms_diff = std::sqrt(sum_difference_square / static_cast<double>(count));
    result.normalized_rms_diff = result.rms_diff / reference_rms;
    result.correlation = denominator > 0 ? sum_cross / denominator : 0;
    result.compared_samples = count;
    return result;
}

std::vector<float> interleave_window(const std::vector<float>& left,
                                     const std::vector<float>& right,
                                     std::size_t offset,
                                     std::size_t count) {
    expect(offset <= left.size() && offset <= right.size(), "sample window offset is out of range");
    expect(count <= left.size() - offset && count <= right.size() - offset,
           "sample window length is out of range");

    std::vector<float> result;
    result.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(left[offset + i]);
        result.push_back(right[offset + i]);
    }
    return result;
}

Comparison compare_window(const StreamStats& reference,
                          std::size_t reference_offset,
                          const StreamStats& candidate,
                          std::size_t candidate_offset,
                          std::size_t count) {
    return compare_vectors(
        interleave_window(reference.left, reference.right, reference_offset, count),
        interleave_window(candidate.left, candidate.right, candidate_offset, count));
}

void write_binary(const fs::path& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    expect(file.good(), "could not open " + path_text(path) + " for writing");
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    expect(file.good(), "could not write " + path_text(path));
    file.close();
    expect(file.good(), "could not close " + path_text(path));
}

std::array<char, 4> read_magic(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(file.good(), "could not open " + path_text(path) + " for reading");
    std::array<char, 4> magic{};
    file.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    expect(file.gcount() == static_cast<std::streamsize>(magic.size()),
           "could not read four header bytes from " + path_text(path));
    return magic;
}

const char* format_name(AudioFormat format) {
    switch (format) {
    case AudioFormat::Wav:
        return "WAV";
    case AudioFormat::Flac:
        return "FLAC";
    case AudioFormat::Ogg:
        return "Ogg";
    case AudioFormat::Mp3:
        return "MP3";
    }
    return "unknown";
}

int expected_major_format(AudioFormat format) {
    switch (format) {
    case AudioFormat::Wav:
        return SF_FORMAT_WAV;
    case AudioFormat::Flac:
        return SF_FORMAT_FLAC;
    case AudioFormat::Ogg:
        return SF_FORMAT_OGG;
    case AudioFormat::Mp3:
        return SF_FORMAT_MPEG;
    }
    return 0;
}

std::string sndfile_global_error() {
    const char* text = sf_strerror(nullptr);
    return text != nullptr && *text != '\0' ? text : "unknown libsndfile error";
}

std::string sndfile_error_number_text(int error) {
    if (error == 0) {
        return "no error";
    }
    const char* text = sf_error_number(error);
    return text != nullptr && *text != '\0' ? text : "unknown libsndfile error";
}

struct SndFileCloser {
    void operator()(SNDFILE* file) const noexcept {
        if (file != nullptr) {
            sf_close(file);
        }
    }
};

struct DecodedAudio {
    std::string name;
    std::size_t frames = 0;
    std::size_t nonzero_samples = 0;
    double duration_seconds = 0;
    double rms = 0;
    double max_abs = 0;
    std::vector<float> samples;
};

DecodedAudio decode_file(const fs::path& path, AudioFormat format) {
    DecodedAudio result;
    result.name = format_name(format);

    const std::array<char, 4> magic = read_magic(path);
    bool header_ok = true;
    switch (format) {
    case AudioFormat::Wav:
        header_ok = magic == std::array<char, 4>{'R', 'I', 'F', 'F'};
        break;
    case AudioFormat::Flac:
        header_ok = magic == std::array<char, 4>{'f', 'L', 'a', 'C'};
        break;
    case AudioFormat::Ogg:
        header_ok = magic == std::array<char, 4>{'O', 'g', 'g', 'S'};
        break;
    case AudioFormat::Mp3:
        // libsndfile's MP3 decoder is the independent check for MP3.
        header_ok = true;
        break;
    }
    expect(header_ok, std::string("missing ") + format_name(format) + " header in " + path_text(path));

    const std::string path_string = path_text(path);
    SF_INFO info{};
    std::unique_ptr<SNDFILE, SndFileCloser> file(sf_open(path_string.c_str(), SFM_READ, &info));
    expect(file != nullptr, "sf_open failed for " + path_string + ": " + sndfile_global_error());
    expect(info.samplerate == static_cast<int>(audio_sample_rate),
           "decoded sample rate is not 44100 for " + path_string);
    expect(info.channels == 2, "decoded channel count is not 2 for " + path_string);

    const int major = info.format & SF_FORMAT_TYPEMASK;
    expect(major == expected_major_format(format),
           "decoded container does not match " + std::string(format_name(format)) + " for " + path_string);
    if (format == AudioFormat::Wav || format == AudioFormat::Flac) {
        expect((info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_16,
               std::string("decoded ") + format_name(format) + " is not PCM16 for " + path_string);
    }

    std::vector<float> buffer(static_cast<std::size_t>(kDecodeFrames) * 2);
    double sum_squares = 0;
    for (;;) {
        const sf_count_t read_frames =
            sf_readf_float(file.get(), buffer.data(), static_cast<sf_count_t>(kDecodeFrames));
        expect(read_frames >= 0, "sf_readf_float failed for " + path_string);
        expect(read_frames <= static_cast<sf_count_t>(kDecodeFrames),
               "sf_readf_float returned more frames than requested for " + path_string);
        if (read_frames == 0) {
            break;
        }
        const std::size_t sample_count = static_cast<std::size_t>(read_frames) * 2;
        for (std::size_t i = 0; i < sample_count; ++i) {
            const float sample = buffer[i];
            expect(std::isfinite(sample), "decoded audio contains a non-finite sample in " + path_string);
            if (sample != 0.0f) {
                ++result.nonzero_samples;
            }
            result.max_abs = std::max(result.max_abs, std::abs(static_cast<double>(sample)));
            sum_squares += static_cast<double>(sample) * static_cast<double>(sample);
        }
        result.samples.insert(result.samples.end(), buffer.begin(),
                              buffer.begin() + static_cast<std::ptrdiff_t>(sample_count));
        result.frames += static_cast<std::size_t>(read_frames);
    }

    const int close_error = sf_close(file.release());
    expect(close_error == 0, "sf_close failed for " + path_string + ": " +
                                 sndfile_error_number_text(close_error));

    expect(result.frames > 0, "decoded audio has no frames in " + path_string);
    expect(result.nonzero_samples > 0, "decoded audio is silent in " + path_string);
    result.duration_seconds =
        static_cast<double>(result.frames) / static_cast<double>(audio_sample_rate);
    result.rms = std::sqrt(sum_squares / static_cast<double>(result.samples.size()));
    return result;
}

std::string json_bool(bool value) {
    return value ? "true" : "false";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::invalid_argument("Usage: audio_test SOUNDFONT OUTPUT_DIRECTORY");
        }

        const std::string soundfont = argv[1];
        const fs::path output_directory = fs::u8path(argv[2]);
        fs::create_directories(output_directory);
        expect(fs::is_directory(output_directory), "OUTPUT_DIRECTORY is not a directory");

        validate_soundfont(soundfont);
        const std::vector<AudioTrack> stems = default_stems();

        // Session limits and malformed inputs. Boundary 600-second cases are
        // expected to reject before any synthesis starts.
        expect_session_rejected({AudioTrack{10, ""}}, soundfont,
                                "AudioSession accepted empty MIDI");
        expect_session_rejected({AudioTrack{10, "this is not a MIDI file"}}, soundfont,
                                "AudioSession accepted malformed MIDI");
        expect_session_rejected({AudioTrack{10, make_smf(60, 480)},
                                 AudioTrack{10, make_smf(67, 480)}},
                                soundfont,
                                "AudioSession accepted duplicate track IDs");
        expect_throws([&] {
            AudioSession session(many_stems(static_cast<int>(audio_max_tracks) + 1), soundfont);
            (void)session;
        }, "AudioSession accepted more than audio_max_tracks stems");
        expect_throws([&] {
            AudioSession session({AudioTrack{10, make_smf(60, 960, kMidiByteCap)}}, soundfont);
            (void)session;
        }, "AudioSession accepted more than 8 MiB of MIDI");
        expect_throws([&] {
            AudioSession session(stems, soundfont, kMaxSessionSeconds);
            (void)session;
        }, "AudioSession accepted start_seconds at the 600-second limit");


        const fs::path missing_soundfont =
            output_directory / "audio_test_missing_soundfont_dir" / "missing.sf2";
        expect(!fs::exists(missing_soundfont),
               "reserved missing-soundfont path unexpectedly exists");
        expect_throws([&] { validate_soundfont(path_text(missing_soundfont)); },
                      "validate_soundfont accepted a missing SoundFont");
        expect_session_rejected(stems, path_text(missing_soundfont),
                                "AudioSession accepted a missing SoundFont");

        const fs::path invalid_soundfont =
            output_directory / "audio_test_invalid_soundfont.sf2";
        write_binary(invalid_soundfont, "this is not a SoundFont");
        expect_throws([&] { validate_soundfont(path_text(invalid_soundfont)); },
                      "validate_soundfont accepted an existing but invalid SoundFont");
        expect_session_rejected(stems, path_text(invalid_soundfont),
                                "AudioSession accepted an existing but invalid SoundFont");

        // A core may release its asset registry before this session ends. The
        // session's opaque lease must cover backward seek and synth destruction.
        auto retained_asset = std::make_shared<int>(42);
        const std::weak_ptr<int> asset_lifetime = retained_asset;
        {
            AudioSession retained_session(stems, soundfont, 0, retained_asset);
            retained_asset.reset();
            expect(!asset_lifetime.expired(), "Audio session released its asset lease early");
            (void)retained_session.read_chunk();
            retained_session.seek(0);
            expect(!asset_lifetime.expired(), "Backward seek lost the asset lease");
        }
        expect(asset_lifetime.expired(), "Audio session leaked its asset lease");

        AudioSession baseline_session(stems, soundfont);
        const AudioMix default_mix = baseline_session.mix();
        expect(default_mix.tracks.size() == 2, "default mix does not contain two stems");
        expect(has_track(default_mix, 10) && has_track(default_mix, 20),
               "default mix does not contain stable track IDs 10 and 20");
        expect(default_mix.master == AudioOutputParams{}, "default master output is not neutral");
        for (const auto& track : default_mix.tracks) {
            expect(track.output == AudioOutputParams{},
                   "default track output is not neutral for track " + std::to_string(track.track_id));
        }

        const StreamStats baseline = read_full(baseline_session, 0, true);
        expect(baseline.frames > 0, "baseline render produced no frames");
        expect(baseline.max_chunk_frames <= audio_chunk_frames,
               "baseline chunk exceeded audio_chunk_frames");
        expect(baseline.nonzero_samples > 0, "baseline render is silent");
        expect(baseline.rms() > 1e-4, "baseline RMS is too low to be a meaningful test signal");
        expect(baseline.duration_seconds() < 30.0,
               "baseline render duration is unexpectedly long; refusing slow test");

        AudioSession batch_session(stems, soundfont);
        expect_throws([&] { (void)batch_session.read_batch(0); },
                      "read_batch(0) was accepted");
        expect_throws([&] { (void)batch_session.read_batch(kMaxBatchSize + 1); },
                      "read_batch(257) was accepted");
        const BatchResult batch = read_batches(batch_session, false);
        expect(batch.stats.frames > 0, "batch rendering produced no frames");
        expect(batch.stats.frames == baseline.frames,
               "ascending read_batch frame count differs from the baseline stream");
        expect(batch.stats.max_chunk_frames <= audio_chunk_frames,
               "batch chunk exceeded audio_chunk_frames");
        expect(batch.stats.nonzero_samples > 0, "batch rendering is silent");

        AudioSession large_batch_session(stems, soundfont);
        const BatchResult large_batch = read_batches(large_batch_session, true);
        expect(large_batch.sizes_tested == kMaxBatchSize,
               "descending read_batch did not exercise every batch size");
        expect(large_batch.terminal, "descending read_batch did not reach the terminal chunk");
        expect(large_batch.stats.frames == baseline.frames,
               "descending read_batch frame count differs from the baseline stream");
        expect(large_batch.stats.max_chunk_frames <= audio_chunk_frames,
               "large-batch chunk exceeded audio_chunk_frames");
        expect(large_batch.stats.nonzero_samples > 0, "large-batch rendering is silent");

        const StreamStats only_track_10 = render_single(stems[0], soundfont);
        const StreamStats only_track_20 = render_single(stems[1], soundfont);
        expect(only_track_10.nonzero_samples > 0, "track 10 alone is silent");
        expect(only_track_20.nonzero_samples > 0, "track 20 alone is silent");
        expect(only_track_10.mean_square() > 0.01 * baseline.mean_square(),
               "track 10 energy is too small to detect solo/mute crosstalk");
        expect(only_track_20.mean_square() > 0.01 * baseline.mean_square(),
               "track 20 energy is too small to detect solo/mute crosstalk");

        // Backend rendering stays on the time-zero block grid. Fractional seek
        // must retain the exact terminal frame and tightly matching float PCM.
        const auto frame_floor = [](double seconds) {
            return static_cast<std::size_t>(
                std::floor(seconds * static_cast<double>(audio_sample_rate) + 1e-9));
        };
        const std::size_t seek_frame = frame_floor(0.123);
        const std::size_t forward_seek_frame = frame_floor(0.246);
        expect(seek_frame == 5424, "floor(0.123 * 44100) is not 5424");
        expect(forward_seek_frame == 10848, "floor(0.246 * 44100) is not 10848");
        expect(baseline.frames > forward_seek_frame + kSeekCompareFrames,
               "baseline is too short for the forward-seek comparison");

        const auto expect_seek_extent = [&](const StreamStats& stats,
                                            std::size_t target_frame,
                                            const std::string& label) {
            expect(baseline.frames > target_frame, label + ": baseline is too short for the target");
            const std::size_t expected_remaining = baseline.frames - target_frame;
            expect(stats.frames == expected_remaining,
                   label + ": seek changed the exact rendered terminal frame");
        };

        AudioSession seek_session(stems, soundfont);
        seek_session.seek(0.123);
        const StreamStats seek_nonblock = read_full(seek_session, seek_frame, true);
        expect_seek_extent(seek_nonblock, seek_frame, "seek(0.123)");
        const auto comparison_frame_count = [&](const StreamStats& stats,
                                                std::size_t target_frame) {
            return std::min({kSeekCompareFrames, stats.frames, baseline.frames - target_frame});
        };
        const std::size_t seek_compare_frames = comparison_frame_count(seek_nonblock, seek_frame);
        expect(seek_compare_frames > 0, "no frames available for seek comparison");
        const Comparison seek_nonblock_comparison =
            compare_window(baseline, seek_frame, seek_nonblock, 0, seek_compare_frames);
        expect(seek_nonblock_comparison.normalized_rms_diff <= kSeekNormalizedTolerance,
               "seek(0.123) waveform differs from the independently rendered baseline");
        expect(seek_nonblock_comparison.correlation >= kSeekCorrelationFloor,
               "seek(0.123) waveform correlation is too low");

        seek_session.seek(0.6);
        seek_session.seek(0.123);
        const StreamStats seek_backward = read_full(seek_session, seek_frame, true);
        expect_seek_extent(seek_backward, seek_frame, "backward seek(0.123)");
        const std::size_t backward_compare_frames =
            comparison_frame_count(seek_backward, seek_frame);
        expect(backward_compare_frames > 0, "no frames available for backward seek comparison");
        const Comparison seek_backward_comparison =
            compare_window(baseline, seek_frame, seek_backward, 0, backward_compare_frames);
        expect(seek_backward_comparison.normalized_rms_diff <= kSeekNormalizedTolerance,
               "backward seek(0.123) waveform differs from the independently rendered baseline");
        expect(seek_backward_comparison.correlation >= kSeekCorrelationFloor,
               "backward seek(0.123) waveform correlation is too low");

        seek_session.seek(0.123);
        seek_session.seek(0.246);
        const StreamStats seek_forward = read_full(seek_session, forward_seek_frame, true);
        expect_seek_extent(seek_forward, forward_seek_frame, "forward seek(0.246)");
        const std::size_t forward_compare_frames =
            std::min({kSeekCompareFrames, seek_forward.frames,
                      baseline.frames - forward_seek_frame});
        expect(forward_compare_frames > 0, "no frames available for forward seek comparison");
        const Comparison seek_forward_comparison =
            compare_window(baseline, forward_seek_frame, seek_forward, 0, forward_compare_frames);
        expect(seek_forward_comparison.normalized_rms_diff <= kSeekNormalizedTolerance,
               "forward seek(0.246) waveform differs from the independently rendered baseline");
        expect(seek_forward_comparison.correlation >= kSeekCorrelationFloor,
               "forward seek(0.246) waveform correlation is too low");

        AudioSession thread_session(stems, soundfont);
        bool wrong_thread_threw = false;
        std::string wrong_thread_message;
        std::thread wrong_thread([&] {
            try {
                (void)thread_session.read_chunk();
            } catch (const std::logic_error& error) {
                wrong_thread_threw = true;
                wrong_thread_message = error.what();
            } catch (const std::exception& error) {
                wrong_thread_message = std::string("wrong exception type: ") + error.what();
            } catch (...) {
                wrong_thread_message = "wrong non-standard exception type";
            }
        });
        wrong_thread.join();
        expect(wrong_thread_threw,
               "read_chunk from a non-owner thread did not throw std::logic_error: " +
                   wrong_thread_message);

        AudioSession cancel_session(stems, soundfont);
        expect(!cancel_session.cancelled(), "a new session is already cancelled");
        std::thread canceller([&] { cancel_session.cancel(); });
        canceller.join();
        expect(cancel_session.cancelled(), "cancel() from std::thread did not set cancelled()");
        const AudioChunk cancelled_terminal = cancel_session.read_chunk();
        expect(cancelled_terminal.done && cancelled_terminal.left.empty() &&
                   cancelled_terminal.right.empty(),
               "read_chunk after cancel did not return the empty terminal chunk");

        AudioSession cancelled_encoder(stems, soundfont);
        cancelled_encoder.cancel();
        bool cancellation_threw = false;
        try {
            const std::string partial = encode_audio(cancelled_encoder, AudioFormat::Wav);
            (void)partial;
        } catch (const AudioEncodingCancelled&) {
            cancellation_threw = true;
        }
        expect(cancellation_threw,
               "encode_audio on a cancelled session did not throw AudioEncodingCancelled");

        bool mid_encode_cancelled = false;
        {
            AudioSession long_session({AudioTrack{10, make_smf(60, 115200)}}, soundfont);
            std::jthread stop_encoding([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                long_session.cancel();
            });
            try { (void)encode_audio(long_session, AudioFormat::Wav); }
            catch (const AudioEncodingCancelled&) { mid_encode_cancelled = true; }
        }
        expect(mid_encode_cancelled, "Mid-encode cancellation returned a complete or partial artifact");

        AudioSession invalid_seek_session(stems, soundfont);
        expect_throws([&] {
            invalid_seek_session.seek(std::numeric_limits<double>::quiet_NaN());
        }, "seek(NaN) was accepted");
        expect_throws([&] { invalid_seek_session.seek(-0.001); },
                      "seek(negative) was accepted");
        expect_throws([&] { invalid_seek_session.seek(kMaxSessionSeconds); },
                      "seek(600) was accepted");
        expect_throws([&] { invalid_seek_session.seek(kMaxSessionSeconds + 0.5); },
                      "seek(600.5) was accepted");

        const AudioMix pre_invalid_mix = baseline_session.mix();
        AudioMix nan_mix = pre_invalid_mix;
        nan_mix.master.volume_db = std::numeric_limits<double>::quiet_NaN();
        expect_throws([&] { baseline_session.set_mix(nan_mix); },
                      "set_mix accepted a NaN volume");
        expect(baseline_session.mix() == pre_invalid_mix,
               "failed NaN set_mix changed the saved mix");

        AudioMix unknown_track_mix = pre_invalid_mix;
        unknown_track_mix.tracks[0].track_id = 999;
        expect_throws([&] { baseline_session.set_mix(unknown_track_mix); },
                      "set_mix accepted an unknown track ID");
        expect(baseline_session.mix() == pre_invalid_mix,
               "failed unknown-track set_mix changed the saved mix");

        AudioMix duplicate_track_mix = pre_invalid_mix;
        duplicate_track_mix.tracks[1].track_id = duplicate_track_mix.tracks[0].track_id;
        expect_throws([&] { baseline_session.set_mix(duplicate_track_mix); },
                      "set_mix accepted a duplicate track ID");
        expect(baseline_session.mix() == pre_invalid_mix,
               "failed duplicate-track set_mix changed the saved mix");

        AudioMix changed_mix = pre_invalid_mix;
        changed_mix.master.volume_db = -6.0;
        track_output(changed_mix, 10).solo = true;
        baseline_session.set_mix(changed_mix);
        expect(baseline_session.mix() == changed_mix,
               "successful set_mix did not round-trip the snapshot");
        baseline_session.set_mix(default_mix);

        const auto render_mix = [&](const AudioMix& mix) {
            baseline_session.set_mix(mix);
            baseline_session.seek(0);
            return read_full(baseline_session, 0, false);
        };

        AudioMix volume_mix = default_mix;
        volume_mix.master.volume_db = -6.0;
        const StreamStats volume_result = render_mix(volume_mix);
        expect(baseline.mean_square() > 0, "baseline mean square is zero");
        const double minus_six_energy_ratio = volume_result.mean_square() / baseline.mean_square();
        const double minus_six_amplitude_ratio = std::sqrt(minus_six_energy_ratio);
        const double expected_gain = std::pow(10.0, -6.0 / 20.0);
        expect_finite(minus_six_energy_ratio, "master -6 dB energy ratio");
        expect_finite(minus_six_amplitude_ratio, "master -6 dB amplitude ratio");
        expect(std::abs(minus_six_amplitude_ratio - expected_gain) <= 0.05,
               "master -6 dB amplitude ratio is not 10^(-6/20)");
        expect(std::abs(minus_six_energy_ratio - expected_gain * expected_gain) <= 0.05,
               "master -6 dB energy ratio is not 10^(-6/10)");

        AudioMix pan_mix = default_mix;
        pan_mix.master.balance = -1.0;
        const StreamStats pan_result = render_mix(pan_mix);
        expect(baseline.left_rms() > 0, "baseline left channel is silent");
        expect(baseline.right_rms() > 0, "baseline right channel is silent");
        expect(pan_result.left_rms() > 0, "hard-left result left channel is silent");
        const double hard_left_ratio = pan_result.left_rms() / baseline.left_rms();
        expect_finite(hard_left_ratio, "hard-left left-channel ratio");
        expect(std::abs(hard_left_ratio - 2.0) <= 0.10,
               "hard-left balance did not double the left channel (upstream linear balance left2/right0)");
        expect(pan_result.right_rms() <= 1e-6,
               "hard-left balance leaked signal into the right channel");

        AudioMix master_mute_mix = default_mix;
        master_mute_mix.master.muted = true;
        const StreamStats master_mute_result = render_mix(master_mute_mix);
        expect(master_mute_result.max_abs <= 1e-12, "master mute did not produce silence");

        AudioMix solo_10_mix = default_mix;
        track_output(solo_10_mix, 10).solo = true;
        const StreamStats solo_10_result = render_mix(solo_10_mix);
        const double solo_10_energy_ratio =
            solo_10_result.sum_squares / only_track_10.sum_squares;
        expect_finite(solo_10_energy_ratio, "track 10 solo energy ratio");
        expect(std::abs(solo_10_energy_ratio - 1.0) <= kMixRatioTolerance,
               "solo track 10 did not match the individual track 10 energy");

        AudioMix solo_20_mix = default_mix;
        track_output(solo_20_mix, 20).solo = true;
        const StreamStats solo_20_result = render_mix(solo_20_mix);
        expect(solo_20_result.sum_squares > 0, "solo track 20 produced silence");

        AudioMix mute_10_mix = default_mix;
        track_output(mute_10_mix, 10).muted = true;
        const StreamStats mute_10_result = render_mix(mute_10_mix);
        const double mute_10_energy_ratio =
            mute_10_result.sum_squares / solo_20_result.sum_squares;
        expect_finite(mute_10_energy_ratio, "track 10 mute energy ratio");
        expect(std::abs(mute_10_energy_ratio - 1.0) <= kMixRatioTolerance,
               "muting track 10 did not match the soloed track 20 energy");

        AudioMix force_mute_10_mix = default_mix;
        track_output(force_mute_10_mix, 10).force_mute = true;
        const StreamStats force_mute_10_result = render_mix(force_mute_10_mix);
        const double force_mute_10_energy_ratio =
            force_mute_10_result.sum_squares / solo_20_result.sum_squares;
        expect_finite(force_mute_10_energy_ratio, "track 10 force-mute energy ratio");
        expect(std::abs(force_mute_10_energy_ratio - 1.0) <= kMixRatioTolerance,
               "force_mute on track 10 did not match the soloed track 20 energy");

        AudioMix both_muted_mix = default_mix;
        track_output(both_muted_mix, 10).muted = true;
        track_output(both_muted_mix, 20).muted = true;
        const StreamStats both_muted_result = render_mix(both_muted_mix);
        expect(both_muted_result.max_abs <= 1e-12,
               "muting both tracks did not produce silence");

        // Encoders. Every container is written to OUTPUT_DIRECTORY and read
        // back through libsndfile, not through the encoder's own APIs.
        baseline_session.set_mix(default_mix);
        const double expected_duration =
            static_cast<double>(baseline.frames) / static_cast<double>(audio_sample_rate);

        const fs::path wav_path = output_directory / "audio_test.wav";
        baseline_session.seek(0);
        std::string encoded = encode_audio(baseline_session, AudioFormat::Wav);
        expect(!encoded.empty(), "WAV encoder returned empty audio");
        write_binary(wav_path, encoded);
        const DecodedAudio wav_first = decode_file(wav_path, AudioFormat::Wav);

        baseline_session.seek(0);
        encoded = encode_audio(baseline_session, AudioFormat::Wav);
        expect(!encoded.empty(), "WAV encoder returned empty audio on the repeat render");
        write_binary(wav_path, encoded);
        const DecodedAudio wav = decode_file(wav_path, AudioFormat::Wav);

        const fs::path flac_path = output_directory / "audio_test.flac";
        baseline_session.seek(0);
        encoded = encode_audio(baseline_session, AudioFormat::Flac);
        expect(!encoded.empty(), "FLAC encoder returned empty audio");
        write_binary(flac_path, encoded);
        const DecodedAudio flac = decode_file(flac_path, AudioFormat::Flac);

        const fs::path ogg_path = output_directory / "audio_test.ogg";
        baseline_session.seek(0);
        encoded = encode_audio(baseline_session, AudioFormat::Ogg);
        expect(!encoded.empty(), "Ogg encoder returned empty audio");
        write_binary(ogg_path, encoded);
        const DecodedAudio ogg = decode_file(ogg_path, AudioFormat::Ogg);

        const fs::path mp3_path = output_directory / "audio_test.mp3";
        baseline_session.seek(0);
        encoded = encode_audio(baseline_session, AudioFormat::Mp3);
        expect(!encoded.empty(), "MP3 encoder returned empty audio");
        write_binary(mp3_path, encoded);
        const DecodedAudio mp3 = decode_file(mp3_path, AudioFormat::Mp3);

        const Comparison wav_repeat_comparison =
            compare_vectors(wav_first.samples, wav.samples);
        const bool deterministic =
            wav_first.frames == wav.frames &&
            wav_repeat_comparison.max_abs_diff <= kPcm16ComparisonTolerance;
        const std::size_t wav_repeat_frame_difference =
            wav_first.frames > wav.frames ? wav_first.frames - wav.frames
                                          : wav.frames - wav_first.frames;
        const std::size_t wav_flac_frame_difference =
            wav.frames > flac.frames ? wav.frames - flac.frames
                                     : flac.frames - wav.frames;
        expect(deterministic, "Float synth/PCM16 re-render is not deterministic");
        const double lossless_duration_tolerance = 1.0 / audio_sample_rate;

        expect(std::abs(wav.duration_seconds - expected_duration) <= lossless_duration_tolerance,
               "WAV duration differs from the rendered session duration");
        expect(std::abs(flac.duration_seconds - expected_duration) <= lossless_duration_tolerance,
               "FLAC duration differs from the rendered session duration");
        expect(std::abs(ogg.duration_seconds - expected_duration) <= 0.10,
               "Ogg duration differs from the rendered session duration");
        expect(std::abs(mp3.duration_seconds - expected_duration) <= 0.10,
               "MP3 duration differs from the rendered session duration by more than 0.1 s");
        expect(wav.rms > 0, "WAV RMS is zero");
        expect(ogg.rms >= 0.5 * wav.rms && ogg.rms <= 2.0 * wav.rms,
               "Ogg decoded energy is implausibly different from WAV");
        expect(mp3.rms >= 0.5 * wav.rms && mp3.rms <= 2.0 * wav.rms,
               "MP3 decoded energy is implausibly different from WAV");
        double wav_flac_difference = 0;
        if (deterministic) {
            expect(wav.frames == flac.frames,
                   "deterministic WAV/FLAC frame counts differ");
            const Comparison wav_flac_comparison = compare_vectors(wav.samples, flac.samples);
            wav_flac_difference = wav_flac_comparison.max_abs_diff;
            expect(wav_flac_difference <= kPcm16ComparisonTolerance + 1e-12,
                   "WAV and FLAC do not match PCM16 quantization of the same synth stream");
        }

        const std::size_t seek_chunk_slack =
            seek_nonblock.frames - (baseline.frames - seek_frame);
        const std::size_t forward_chunk_slack =
            seek_forward.frames - (baseline.frames - forward_seek_frame);

        std::ostringstream json;
        json << std::setprecision(10);
        json << "{"
             << "\"status\":\"ok\""
             << ",\"sample_rate\":" << audio_sample_rate
             << ",\"channels\":2"
             << ",\"planar_chunks\":true"
             << ",\"tracks\":[10,20]"
             << ",\"baseline_frames\":" << baseline.frames
             << ",\"baseline_duration_seconds\":" << baseline.duration_seconds()
             << ",\"baseline_rms\":" << baseline.rms()
             << ",\"baseline_chunks\":" << baseline.chunks
             << ",\"baseline_max_chunk_frames\":" << baseline.max_chunk_frames
             << ",\"terminal_empty_done\":true"
             << ",\"mid_encode_cancelled\":" << json_bool(mid_encode_cancelled)
             << ",\"asset_lease_lifetime_verified\":true"
             << ",\"batch_sizes_tested\":" << batch.sizes_tested
             << ",\"batch_frames\":" << batch.stats.frames
             << ",\"large_batch_sizes_tested\":" << large_batch.sizes_tested
             << ",\"large_batch_frames\":" << large_batch.stats.frames
             << ",\"seek\":{"
             << "\"target_seconds\":0.123"
             << ",\"target_frame\":" << seek_frame
             << ",\"forward_target_seconds\":0.246"
             << ",\"forward_target_frame\":" << forward_seek_frame
             << ",\"remaining_frames\":" << seek_nonblock.frames
             << ",\"expected_remaining_frames\":" << (baseline.frames - seek_frame)
             << ",\"remaining_chunk_slack_frames\":" << seek_chunk_slack
             << ",\"forward_remaining_frames\":" << seek_forward.frames
             << ",\"forward_expected_remaining_frames\":"
             << (baseline.frames - forward_seek_frame)
             << ",\"forward_remaining_chunk_slack_frames\":" << forward_chunk_slack
             << ",\"compared_frames\":" << seek_compare_frames
             << ",\"nonblock_normalized_rms_diff\":"
             << seek_nonblock_comparison.normalized_rms_diff
             << ",\"backward_normalized_rms_diff\":"
             << seek_backward_comparison.normalized_rms_diff
             << ",\"forward_normalized_rms_diff\":"
             << seek_forward_comparison.normalized_rms_diff
             << ",\"nonblock_correlation\":" << seek_nonblock_comparison.correlation
             << ",\"backward_correlation\":" << seek_backward_comparison.correlation
             << ",\"forward_correlation\":" << seek_forward_comparison.correlation
             << ",\"nonblock_max_abs_diff\":" << seek_nonblock_comparison.max_abs_diff
             << ",\"backward_max_abs_diff\":" << seek_backward_comparison.max_abs_diff
             << ",\"forward_max_abs_diff\":" << seek_forward_comparison.max_abs_diff
             << "}"
             << ",\"mix\":{"
             << "\"minus6_db_amplitude_ratio\":" << minus_six_amplitude_ratio
             << ",\"minus6_db_energy_ratio\":" << minus_six_energy_ratio
             << ",\"hard_left_left_rms_ratio\":" << hard_left_ratio
             << ",\"hard_left_right_rms\":" << pan_result.right_rms()
             << ",\"master_mute_max_abs\":" << master_mute_result.max_abs
             << ",\"solo10_energy_ratio\":" << solo_10_energy_ratio
             << ",\"mute10_energy_ratio\":" << mute_10_energy_ratio
             << ",\"force_mute10_energy_ratio\":" << force_mute_10_energy_ratio
             << ",\"both_muted_max_abs\":" << both_muted_result.max_abs
             << "}"
             << ",\"encoders\":{";

        const std::array<const char*, 4> encoder_keys = {"wav", "flac", "ogg", "mp3"};
        const std::array<const DecodedAudio*, 4> encoder_values = {&wav, &flac, &ogg, &mp3};
        for (std::size_t i = 0; i < encoder_keys.size(); ++i) {
            if (i != 0) {
                json << ',';
            }
            const DecodedAudio& decoded = *encoder_values[i];
            json << '\"' << encoder_keys[i] << "\":{"
                 << "\"frames\":" << decoded.frames
                 << ",\"duration_seconds\":" << decoded.duration_seconds
                 << ",\"nonzero_samples\":" << decoded.nonzero_samples
                 << ",\"rms\":" << decoded.rms
                 << ",\"max_abs\":" << decoded.max_abs
                 << ",\"header_ok\":true"
                 << ",\"sf_close_ok\":true"
                 << "}";
        }

        json << "}"
             << ",\"lossless_tolerance_verified\":" << json_bool(deterministic)
             << ",\"lossless_pcm16_tolerance\":" << kPcm16ComparisonTolerance
             << ",\"wav_repeat_max_abs_difference\":"
             << wav_repeat_comparison.max_abs_diff
             << ",\"wav_repeat_frame_difference\":" << wav_repeat_frame_difference
             << ",\"wav_flac_max_abs_difference\":" << wav_flac_difference
             << ",\"wav_flac_frame_difference\":" << wav_flac_frame_difference;
        json << ",\"limits\":{"
             << "\"max_tracks\":" << audio_max_tracks
             << ",\"max_midi_bytes\":" << kMidiByteCap
             << ",\"max_duration_seconds\":" << kMaxSessionSeconds
             << "}"
             << ",\"outputs\":[\"audio_test.wav\",\"audio_test.flac\","
                "\"audio_test.ogg\",\"audio_test.mp3\"]"
             << "}";

        std::cout << json.str() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio_test failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "audio_test failed: unknown exception\n";
        return 1;
    }
}
