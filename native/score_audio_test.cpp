#include "noqt_runtime.h"
#include "score_audio.h"

#include "engraving/engravingproject.h"
#include "engraving/dom/masterscore.h"
#include "engraving/compat/midi/midicoreevent.h"
#include "global/io/buffer.h"
#include "global/serialization/zipwriter.h"
#include "importexport/midi/internal/midishared/midifile.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace mu::engraving;
using mu::iex::midi::MidiFile;

void expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

MidiFile parse(const std::string& bytes)
{
    muse::ByteArray data(bytes.data(), bytes.size());
    muse::io::Buffer buffer(&data);
    expect(buffer.open(muse::io::IODevice::ReadOnly), "Cannot open MIDI buffer");
    MidiFile midi;
    expect(midi.read(&buffer), "Cannot parse stem MIDI");
    return midi;
}

void verify_stem(const music::AudioTrack& stem, const std::multiset<int>& pitches,
                 int program, int key, std::size_t staff_count, bool rpns)
{
    const auto midi = parse(stem.midi);
    expect(midi.format() == 1 && midi.division() > 0, "Invalid stem timing/header");
    expect(midi.tracks().size() == staff_count + 1, "Staff tracks not grouped under one conductor");
    std::multiset<int> actual;
    bool tempo = false, instrument = false, time_signature = false, key_signature = false;
    std::size_t rpn_count = 0;
    for (const auto& track : midi.tracks()) {
        for (const auto& [tick, event] : track.events()) {
            if (event.type() == ME_NOTEON && event.velo() > 0) actual.insert(event.pitch());
            if (event.type() == ME_PROGRAM && event.value() == program) instrument = true;
            if (event.type() == ME_CONTROLLER && (event.controller() == 100 || event.controller() == 101)) ++rpn_count;
            if (event.type() != ME_META) continue;
            if (event.metaType() == META_TEMPO && event.len() == 3) {
                const auto* p = event.edata();
                const unsigned microseconds = (unsigned(p[0]) << 16) | (unsigned(p[1]) << 8) | unsigned(p[2]);
                if (tick == 0 && microseconds == 1000000) tempo = true;
            }
            if (event.metaType() == META_TIME_SIGNATURE) time_signature = true;
            if (event.metaType() == META_KEY_SIGNATURE) {
                expect(event.len() == 2 && static_cast<signed char>(event.edata()[0]) == key,
                       "Stem copied another part's key signature");
                key_signature = true;
            }
        }
    }
    expect(actual == pitches, "Stem contains missing or foreign note events");
    expect(tempo && time_signature, "Stem lost global tempo/time-signature events");
    expect(instrument, "Stem lost instrument program change");
    expect(key_signature, "Stem lost staff-local key signature");
    expect(rpns ? rpn_count > 0 : rpn_count == 0, "RPN option was not preserved in stems");
}

constexpr const char* fixture = R"(<score-partwise version="4.0">
<part-list>
<score-part id="P1"><part-name>Grand Piano</part-name><score-instrument id="P1-I1"><instrument-name>Piano</instrument-name></score-instrument><midi-instrument id="P1-I1"><midi-channel>1</midi-channel><midi-program>1</midi-program></midi-instrument></score-part>
<score-part id="P2"><part-name>Violin</part-name><score-instrument id="P2-I1"><instrument-name>Violin</instrument-name></score-instrument><midi-instrument id="P2-I1"><midi-channel>2</midi-channel><midi-program>41</midi-program></midi-instrument></score-part>
</part-list>
<part id="P1"><measure number="1"><attributes><divisions>1</divisions><key><fifths>0</fifths></key><time><beats>4</beats><beat-type>4</beat-type></time><staves>2</staves><clef number="1"><sign>G</sign><line>2</line></clef><clef number="2"><sign>F</sign><line>4</line></clef></attributes>
<direction><direction-type><metronome><beat-unit>quarter</beat-unit><per-minute>60</per-minute></metronome></direction-type><sound tempo="60"/></direction>
<note><pitch><step>C</step><octave>4</octave></pitch><duration>4</duration><voice>1</voice><type>whole</type><staff>1</staff></note><backup><duration>4</duration></backup>
<note><pitch><step>C</step><octave>3</octave></pitch><duration>4</duration><voice>2</voice><type>whole</type><staff>2</staff></note></measure></part>
<part id="P2"><measure number="1"><attributes><divisions>1</divisions><key><fifths>2</fifths></key><time><beats>4</beats><beat-type>4</beat-type></time><clef><sign>G</sign><line>2</line></clef></attributes>
<note><pitch><step>G</step><octave>4</octave></pitch><duration>4</duration><type>whole</type></note></measure></part>
</score-partwise>)";
}

int main(int argc, char** argv)
{
    try {
        if (argc != 3) throw std::invalid_argument("Usage: score_audio_test ASSETS UPSTREAM_ROOT");
        music::initialize_native_runtime(argv[1]);

        // Default mix: a document opened from MSCZ carries the archive's
        // audiosettings.json values, matched by (partId, instrumentId).
        expect(music::score_audio_mix_defaults("").is_neutral(), "Empty input must give neutral defaults");
        expect(music::score_audio_mix_defaults("not a zip archive").is_neutral(),
               "Invalid archive must give neutral defaults");
        std::ifstream mix_stream("fixtures/audio/mix.mscz", std::ios::binary);
        expect(bool(mix_stream), "Missing fixtures/audio/mix.mscz");
        const std::string mix_bytes{std::istreambuf_iterator<char>(mix_stream),
                                    std::istreambuf_iterator<char>()};
        const auto defaults = music::score_audio_mix_defaults(mix_bytes);
        expect(!defaults.is_neutral(), "MSCZ audiosettings.json was not read");
        expect(defaults.master.volume_db == 0.0 && defaults.master.balance == 0.0,
               "Unexpected fixture master defaults");
        auto mix_project = music::load_native_score(mix_bytes, "mscz");
        const auto mix_tracks = music::score_audio_track_info(mix_project->masterScore());
        expect(mix_tracks.size() == 3, "Mix fixture must expose three tracks");
        std::size_t muted_tracks = 0;
        for (const auto& track : mix_tracks) {
            const auto found = defaults.tracks.find({ track.part_id, track.instrument_id });
            expect(found != defaults.tracks.end(), "audiosettings entry did not match a score track");
            if (found->second.muted) ++muted_tracks;
        }
        expect(muted_tracks == 1, "MSCZ soloMuteState.mute was not applied");

        music::AudioMix forced;
        forced.tracks.push_back({0, music::AudioOutputParams{}});
        forced.tracks.push_back({1, music::AudioOutputParams{}});
        forced.tracks[0].output.solo = true;
        music::derive_force_mute(forced);
        expect(!forced.tracks[0].output.force_mute && forced.tracks[1].output.force_mute,
               "forceMute must silence non-solo tracks");
        expect(!music::is_neutral_mix(forced), "Soloed mix must not be neutral");

        // Hostile deeply nested JSON (>64 nesting) and oversized entry (>1 MiB)
        {
            // 1) Hostile nested JSON: [[[[... 100 deep
            std::string nested = "";
            for (int i = 0; i < 100; ++i) nested += "[";
            for (int i = 0; i < 100; ++i) nested += "]";
            muse::ByteArray zip_bytes;
            {
                muse::io::Buffer buf(&zip_bytes);
                expect(buf.open(muse::io::IODevice::WriteOnly), "Cannot open zip buffer");
                muse::ZipWriter writer(&buf);
                writer.addFile("audiosettings.json", muse::ByteArray(nested.data(), nested.size()));
                writer.close();
            }
            std::string nested_archive(reinterpret_cast<const char*>(zip_bytes.constData()), zip_bytes.size());
            const auto nested_defaults = music::score_audio_mix_defaults(nested_archive);
            expect(nested_defaults.is_neutral(), "Deeply nested hostile document must yield neutral defaults");

            // 2) Oversized entry: > 1 MiB uncompressed size
            {
                std::string huge(1024 * 1024 + 16, ' ');
                muse::ByteArray huge_zip;
                muse::io::Buffer huge_buf(&huge_zip);
                expect(huge_buf.open(muse::io::IODevice::WriteOnly), "Cannot open huge zip buffer");
                muse::ZipWriter huge_writer(&huge_buf);
                huge_writer.addFile("audiosettings.json", muse::ByteArray(huge.data(), huge.size()));
                huge_writer.close();
                std::string huge_archive(reinterpret_cast<const char*>(huge_zip.constData()), huge_zip.size());
                const auto huge_defaults = music::score_audio_mix_defaults(huge_archive);
                expect(huge_defaults.is_neutral(), "Oversized audiosettings entry must yield neutral defaults");
            }
        }

        // Excerpts audiosettings.json fixture (mix_parts.mscz)
        {
            std::ifstream parts_stream("fixtures/audio/mix_parts.mscz", std::ios::binary);
            expect(bool(parts_stream), "Missing fixtures/audio/mix_parts.mscz");
            const std::string parts_bytes{std::istreambuf_iterator<char>(parts_stream),
                                          std::istreambuf_iterator<char>()};
            const auto parts_defaults = music::score_audio_mix_defaults(parts_bytes);
            // Root defaults: Violin is muted
            auto violin_it = parts_defaults.tracks.find({"1", "violin"});
            expect(violin_it != parts_defaults.tracks.end() && violin_it->second.muted,
                   "mix_parts root must report Violin muted");

            // Excerpt 0 (Violin): Violin is NOT muted in excerpt (ignores root mute)
            const auto* ex0_map = parts_defaults.get_excerpt_map(0, "0_Violin");
            expect(ex0_map != nullptr, "mix_parts must find excerpt 0 audiosettings");
            auto ex0_violin = ex0_map->find({"1", "violin"});
            expect(ex0_violin != ex0_map->end() && !ex0_violin->second.muted,
                   "Excerpt 0 must have Violin unmuted");

            // Excerpt 2 (Cello): Cello IS muted in excerpt
            const auto* ex2_map = parts_defaults.get_excerpt_map(2, "2_Cello");
            expect(ex2_map != nullptr, "mix_parts must find excerpt 2 audiosettings");
            auto ex2_cello = ex2_map->find({"3", "baryton"});
            expect(ex2_cello != ex2_map->end() && ex2_cello->second.muted,
                   "Excerpt 2 must have Cello muted");
        }

        auto project = music::load_native_score(fixture, "musicxml");
        auto* score = project->masterScore();
        const auto info = music::score_audio_track_info(score);
        expect(info.size() == 2, "Two-staff piano must count as one logical audio track");
        expect(info[0].track_id == 0 && info[1].track_id == 1, "Unexpected track identity");
        expect(info[0].part_id != info[1].part_id, "Distinct parts share descriptor identity");
        expect(!info[0].instrument_id.empty() && !info[1].instrument_id.empty(), "Missing instrument identity");
        for (bool rpns : {false, true}) {
            const auto stems = music::score_audio_stems(score, true, rpns);
            expect(stems.size() == info.size(), "Descriptor and stem counts differ");
            expect(stems[0].track_id == info[0].track_id && stems[1].track_id == info[1].track_id,
                   "Descriptor and stem identities differ");
            verify_stem(stems[0], {48, 60}, 0, 0, 2, rpns);
            verify_stem(stems[1], {67}, 40, 2, 1, rpns);
        }
        const std::string path = std::string(argv[2]) + "/src/engraving/tests/instrumentchange_data/change-ref.mscx";
        std::ifstream stream(path, std::ios::binary);
        expect(bool(stream), "Missing pinned instrument-change fixture");
        const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        auto changed = music::load_native_score(bytes, "mscx");
        for (bool stems : {false, true}) {
            bool rejected = false;
            try {
                if (stems) (void)music::score_audio_stems(changed->masterScore(), true, false);
                else (void)music::score_audio_track_info(changed->masterScore());
            } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("multi-instrument") != std::string::npos;
            }
            expect(rejected, "Ambiguous instrument changes were silently mixed");
        }
        std::cout << "{\"status\":\"passed\",\"logicalTracks\":2,\"staffTracks\":3,\"tempoPreserved\":true,\"programsPreserved\":true,\"staffKeysPreserved\":true,\"rpnOptions\":true,\"instrumentChange\":\"explicitly_rejected\",\"msczMixDefaults\":{\"tracks\":3,\"mutedTracks\":"
                  << muted_tracks << "},\"forceMuteDerived\":true}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
