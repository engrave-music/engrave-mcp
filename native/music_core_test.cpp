#include "music_core.h"
#include "draw/fontmetrics.h"
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace music;

void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    expect(bool(stream), "Cannot read test input");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

Composition make_composition() {
    Composition composition;
    composition.title = "Core Test";
    composition.tempo = 120;

    Part part;
    part.name = "Piano";
    part.midi_program = 1;

    Measure first;
    Note chord;
    chord.pitches = {60, 64, 67};
    chord.ticks = 1920;
    first.notes = {chord};

    Measure second;
    Note half_a;
    half_a.pitches = {62};
    half_a.ticks = 960;
    Note half_b;
    half_b.pitches = {65};
    half_b.ticks = 960;
    second.notes = {half_a, half_b};

    part.measures = {first, second};
    composition.parts = {part};
    return composition;
}

Measure replacement_measure() {
    Measure measure;
    Note note;
    note.pitches = {72};
    note.ticks = 1920;
    measure.notes = {note};
    return measure;
}

Measure invalid_measure() {
    Measure measure;
    Note note;
    note.pitches = {72};
    note.ticks = 480; // Measure total must be exactly 1920 ticks.
    measure.notes = {note};
    return measure;
}

Composition make_long_composition() {
    Composition composition = make_composition();
    auto& measures = composition.parts.at(0).measures;
    const Measure tail = measures.back();
    while (measures.size() < 4) measures.push_back(tail);
    return composition;
}

Measure pitch_measure(int pitch) {
    Measure measure;
    Note note;
    note.pitches = {pitch};
    note.ticks = 1920;
    measure.notes = {note};
    return measure;
}

} // namespace

using namespace music;

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 4)
            throw std::invalid_argument("Usage: music_core_test ASSETS [SOUNDFONT REPLACEMENT_SOUNDFONT]");

        MusicCore core(RuntimeOptions{argv[1], argc == 4 ? argv[2] : ""});

        const auto created = core.create(make_composition());
        expect(created.id.value == 1, "First document id is not 1");
        expect(created.revision.value == 1, "Initial revision is not 1");
        expect(created.title == "Core Test", "Created title mismatch");
        expect(created.parts == 1, "Created part count mismatch");
        expect(created.pages > 0, "Created score has no pages");
        expect(created.editable, "Created score is not editable");

        const auto created_view = core.get(created.id);
        expect(created_view.composition.has_value(), "Created score has no typed composition");
        expect(!created_view.musicxml.empty(), "Created score has no MusicXML");
        expect(!created_view.metadata_json.empty(), "Created score has no metadata");
        expect(created_view.info.revision == created.revision, "View revision mismatch");

        const auto imported = core.open(created_view.musicxml, InputFormat::MusicXml);
        expect(imported.id != created.id, "Imported document reused the created id");
        expect(imported.revision.value == 1, "Imported document revision is not 1");
        expect(!imported.editable, "Imported document is marked editable");
        const auto imported_view = core.get(imported.id);
        expect(!imported_view.composition.has_value(), "Imported score has a typed composition");
        expect(!imported_view.musicxml.empty(), "Imported score has no MusicXML");
        expect(!imported_view.metadata_json.empty(), "Imported score has no metadata");

        const auto replaced = core.replace_measures(created.id, 0, 0, {replacement_measure()});
        expect(replaced.revision.value == 2, "Revision did not increment on successful replacement");
        const auto before_invalid = core.get(created.id);
        expect(before_invalid.composition.has_value(), "Replacement lost the typed composition");
        expect(before_invalid.composition->parts.at(0).measures.at(0).notes.at(0).pitches
                   == std::vector<int>({72}),
               "Replacement did not update typed pitches");

        bool invalid_rejected = false;
        CoreError::Code invalid_code = CoreError::Code::engine_failure;
        try {
            core.replace_measures(created.id, 0, 0, {invalid_measure()});
        } catch (const CoreError& error) {
            invalid_rejected = true;
            invalid_code = error.code();
        }
        expect(invalid_rejected, "Invalid replacement was accepted");
        expect(invalid_code == CoreError::Code::invalid_argument, "Invalid replacement returned the wrong code");

        const auto after_invalid = core.get(created.id);
        expect(after_invalid.info.revision.value == 2, "Failed replacement advanced the revision");
        expect(after_invalid.musicxml == before_invalid.musicxml, "Failed replacement changed the MusicXML");

        bool wrong_thread = false;
        std::thread worker([&] {
            try {
                core.describe(created.id);
            } catch (const CoreError& error) {
                wrong_thread = error.code() == CoreError::Code::wrong_thread;
            }
        });
        worker.join();
        expect(wrong_thread, "Wrong-thread call was not rejected");

        const auto snapshot = core.snapshot(created.id);
        expect(!snapshot.bytes.empty(), "MSCZ snapshot is empty");
        expect(snapshot.revision == after_invalid.info.revision, "Snapshot revision mismatch");

        const auto exported = core.export_bytes(created.id, ExportFormat::MusicXml);
        expect(!exported.bytes.empty(), "MusicXML export is empty");
        expect(exported.revision == after_invalid.info.revision, "Export revision mismatch");

        core.close(imported.id);
        bool closed_rejected = false;
        try {
            core.describe(imported.id);
        } catch (const CoreError& error) {
            closed_rejected = error.code() == CoreError::Code::not_found;
        }
        expect(closed_rejected, "Closed document remained accessible");

        bool stale_export_rejected = false;
        try {
            core.export_bytes(imported.id, ExportFormat::MusicXml);
        } catch (const CoreError& error) {
            stale_export_rejected = error.code() == CoreError::Code::not_found;
        }
        expect(stale_export_rejected, "Closed document exported through a stale handle");

        const auto third = core.create(make_composition());
        expect(third.id.value == 3, "Closed document id was reused");
        core.close(third.id);

        const auto fourth = core.create(make_composition());
        expect(fourth.id.value == 4, "Document ids are not monotonic and non-reused");
        core.close(fourth.id);
        core.close(created.id);

        // Default mix comes from the MSCZ archive and survives a DOM reload.
        std::ifstream mix_stream("fixtures/audio/mix.mscz", std::ios::binary);
        expect(bool(mix_stream), "Missing fixtures/audio/mix.mscz");
        const std::string mix_bytes{std::istreambuf_iterator<char>(mix_stream),
                                    std::istreambuf_iterator<char>()};
        const auto mixed = core.open(mix_bytes, InputFormat::Mscz);
        auto mixed_tracks = core.audio_tracks(mixed.id);
        expect(mixed_tracks.tracks.size() == 3, "MSCZ default mix did not expose three tracks");
        std::size_t muted_index = 3;
        for (std::size_t index = 0; index < mixed_tracks.tracks.size(); ++index) {
            if (mixed_tracks.tracks[index].output.muted) muted_index = index;
            expect(!mixed_tracks.tracks[index].output.force_mute,
                   "No solo means no force-mute");
        }
        expect(muted_index < 3, "MSCZ default track mute was not reported");
        expect(!mixed_tracks.tracks.at(muted_index).output.force_mute, "No solo means no force-mute");
        const auto mixed_excerpts = core.generate_excerpts(mixed.id);
        expect(!mixed_excerpts.excerpts.empty(), "MSCZ excerpt generation produced no excerpts");
        auto reloaded_tracks = core.audio_tracks(mixed.id);
        expect(reloaded_tracks.tracks.size() == 3
               && reloaded_tracks.tracks.at(muted_index).output.muted,
               "Default mix was lost across excerpt generation");
        expect(core.describe(mixed.id).revision == mixed_excerpts.revision,
               "Default mix lookup changed the document revision");

        ExportOptions mix_override;
        mix_override.mix = AudioMixOverride{};
        AudioTrackMixOverride solo_track;
        solo_track.track_id = 0;
        solo_track.output.solo = true;
        mix_override.mix->tracks.push_back(solo_track);
        const auto resolved = core.audio_export_mix(mixed.id, mix_override);
        expect(resolved.tracks.at(0).output.solo && !resolved.tracks.at(0).output.force_mute,
               "Export override did not solo the requested track");
        expect(resolved.tracks.at(1).output.force_mute && resolved.tracks.at(2).output.force_mute,
               "Export override did not force-mute non-solo tracks");
        expect(core.describe(mixed.id).revision == mixed_excerpts.revision,
               "Mix resolution changed the document revision");

        bool unknown_track_rejected = false;
        try {
            ExportOptions bad = mix_override;
            bad.mix->tracks.front().track_id = 99;
            (void)core.audio_export_mix(mixed.id, bad);
        } catch (const CoreError& error) {
            unknown_track_rejected = error.code() == CoreError::Code::invalid_argument;
        }
        expect(unknown_track_rejected, "Unknown export mix trackId was accepted");
        core.close(mixed.id);

        const auto runtime = core.runtime_options();
        expect(!runtime.resources_path.empty(), "Runtime resources path is empty");
        expect(std::filesystem::u8path(runtime.resources_path).is_absolute(),
               "Runtime resources path is not absolute");
        expect(runtime.font_paths.empty(), "Runtime font paths are not initially empty");
        if (argc == 4) {
            expect(!runtime.soundfont.empty(), "Startup SoundFont was not selected");
            expect(std::filesystem::u8path(runtime.soundfont) != std::filesystem::absolute(std::filesystem::u8path(argv[2])),
                   "Startup SoundFont was not retained independently");
            expect(read_file(std::filesystem::u8path(runtime.soundfont)) == read_file(std::filesystem::u8path(argv[2])),
                   "Retained startup SoundFont differs from the supplied file");
        }
        expect(document_id_from_string("score-42").value == 42, "Document id parser mismatch");
        expect(document_id_to_string(DocumentId{42}) == "score-42", "Document id formatter mismatch");

        // Keep a master and an excerpt open while replacing a missing font face.
        // Measure both cached advances and exported layouts before and after.
        auto font_score = read_file("fixtures/golden/linked_parts.mscx");
        const auto frame = font_score.find("<height>10</height>");
        expect(frame != std::string::npos, "Font layout fixture has no title frame");
        font_score.insert(frame, "<Text><style>title</style><text><font face=\"Petaluma Script\"/>MMMWWWiii012345</text></Text>");
        const std::string old_excerpt = "<text>Flute</text>";
        const auto excerpt_text = font_score.find(old_excerpt);
        expect(excerpt_text != std::string::npos, "Font layout fixture has no excerpt title");
        font_score.replace(excerpt_text, old_excerpt.size(),
                           "<text><font face=\"Petaluma Script\"/>MMMWWWiii012345</text>");
        const auto font_doc = core.open(font_score, InputFormat::Mscx);
        const auto parts = core.excerpts(font_doc.id);
        expect(!parts.excerpts.empty(), "Font layout fixture has no excerpts");
        ExportOptions part_options;
        part_options.excerpt_id = parts.excerpts.front().id;
        const auto master_before = core.export_bytes(font_doc.id, ExportFormat::Svg).bytes;
        const auto part_before = core.export_bytes(font_doc.id, ExportFormat::Svg, part_options).bytes;

        muse::draw::Font face(muse::draw::Font::FontFamily(std::string("Petaluma Script")), muse::draw::Font::Type::Text);
        face.setPointSizeF(12);
        const muse::draw::FontMetrics metrics(face);
        const auto text = muse::String::fromUtf8("MMMWWWiii012345");
        const double advance_before = metrics.horizontalAdvance(text);
        const auto font_file = std::filesystem::u8path(argv[1]) / "fonts/petaluma/PetalumaScript.otf";
        const auto font = core.add_font(utf8(font_file));
        const double advance_after = metrics.horizontalAdvance(text);
        expect(font.family == "Petaluma Script" && !font.content_hash.empty(), "Runtime font metadata mismatch");
        expect(advance_before > 0 && advance_after > 0 && std::abs(advance_after - advance_before) > 0.01,
               "Runtime font did not invalidate the cached fallback advance");
        expect(core.export_bytes(font_doc.id, ExportFormat::Svg).bytes != master_before,
               "Runtime font did not relayout the master score");
        expect(core.export_bytes(font_doc.id, ExportFormat::Svg, part_options).bytes != part_before,
               "Runtime font did not relayout the excerpt");
        expect(core.describe(font_doc.id).revision == font_doc.revision, "Runtime font changed the score revision");
        const auto again = core.add_font(utf8(font_file));
        expect(again.id == font.id && again.path == font.path, "Runtime font did not deduplicate");
        expect(core.runtime_options().font_paths == std::vector<std::string>{font.path},
               "Runtime font selection was not forwarded to workers");

        if (argc == 4) {
            const auto first = core.set_soundfont(argv[3]);
            expect(core.runtime_options().soundfont == first.path && first.path != runtime.soundfont,
                   "Runtime SoundFont did not change selection");
            bool invalid_soundfont_rejected = false;
            try { (void)core.set_soundfont(std::string(argv[3]) + ".missing"); }
            catch (const CoreError&) { invalid_soundfont_rejected = true; }
            expect(invalid_soundfont_rejected && core.runtime_options().soundfont == first.path,
                   "Invalid SoundFont changed the selection");
            const auto restored = core.set_soundfont(argv[2]);
            expect(restored.path == runtime.soundfont && core.runtime_options().soundfont == runtime.soundfont,
                   "SoundFont reselection lost the retained startup copy");
            bool audio_not_initialized = false;
            try { (void)core.export_audio(font_doc.id, ExportFormat::Wav, "unused.wav"); }
            catch (const CoreError& error) { audio_not_initialized = error.code() == CoreError::engine_failure; }
            expect(audio_not_initialized, "SoundFont selection initialized audio in the owner process");
        }
        core.close(font_doc.id);
        std::cout << "runtime font advances and master/excerpt relayout checks passed\n";
        if (argc == 4) std::cout << "runtime SoundFont selection checks passed\n";

        const auto edited = core.create(make_long_composition());
        const EditContext first_context{Revision{1}, "operation-a"};
        const auto first_edit = core.replace_measures(edited.id, 0, 0, {pitch_measure(80)}, first_context);
        expect(first_edit.score.revision.value == 2, "First rebased replacement has the wrong revision");
        expect(first_edit.base_revision.value == 1, "First rebased replacement has the wrong base");
        expect(first_edit.rebased_over.empty(), "First rebased replacement rebased over later work");
        expect(!first_edit.replayed, "First rebased replacement was marked as a replay");
        expect(first_edit.changed_notes == 1, "First rebased replacement changed note count mismatch");

        const EditContext second_context{Revision{1}, "operation-b"};
        const auto second_edit = core.replace_measures(edited.id, 0, 1, {pitch_measure(81)}, second_context);
        expect(second_edit.score.revision.value == 3, "Second disjoint replacement has the wrong revision");
        expect(second_edit.base_revision.value == 1, "Second disjoint replacement has the wrong base");
        expect(second_edit.rebased_over.size() == 1, "Second disjoint replacement omitted its rebase");
        expect(second_edit.rebased_over.at(0).value == 2, "Second disjoint replacement recorded the wrong rebase");
        expect(!second_edit.replayed, "Second disjoint replacement was marked as a replay");

        const auto before_conflict = core.get(edited.id);
        bool overlap_rejected = false;
        try {
            core.replace_measures(edited.id, 0, 1, {pitch_measure(82)},
                                  EditContext{Revision{2}, "operation-c"});
        } catch (const CoreError& error) {
            overlap_rejected = error.code() == CoreError::Code::conflict;
        }
        expect(overlap_rejected, "Overlapping replacement did not report a conflict");
        const auto after_conflict = core.get(edited.id);
        expect(after_conflict.info.revision.value == 3, "Overlapping replacement advanced the revision");
        expect(after_conflict.musicxml == before_conflict.musicxml,
               "Overlapping replacement changed the MusicXML");

        const auto retried = core.replace_measures(edited.id, 0, 0, {pitch_measure(80)},
                                                   EditContext{Revision{1}, "operation-a"});
        expect(retried.replayed, "Exact operation retry was not marked as a replay");
        expect(retried.score.revision.value == 2, "Exact operation retry changed the original revision");
        expect(retried.changed_notes == 1, "Exact operation retry changed the original note count");
        const auto after_retry = core.get(edited.id);
        expect(after_retry.info.revision.value == 3, "Exact operation retry advanced the revision");
        expect(after_retry.musicxml == after_conflict.musicxml, "Exact operation retry changed the MusicXML");

        bool reused_id_rejected = false;
        try {
            core.replace_measures(edited.id, 0, 0, {pitch_measure(90)},
                                  EditContext{Revision{1}, "operation-a"});
        } catch (const CoreError& error) {
            reused_id_rejected = error.code() == CoreError::Code::conflict;
        }
        expect(reused_id_rejected, "Reused operationId with a different payload was accepted");
        const auto after_reuse = core.get(edited.id);
        expect(after_reuse.info.revision.value == 3, "Reused operationId advanced the revision");
        expect(after_reuse.musicxml == after_conflict.musicxml,
               "Reused operationId changed the MusicXML");
        core.close(edited.id);

        const auto replay_score = core.create(make_composition());
        const auto projection = core.read_snapshot(replay_score.id);
        expect(projection->projection.complete, "Small score projection is incomplete");
        expect(!projection->projection.notes.empty(), "Projection contains no notes");
        const auto original_note = projection->projection.notes.front();
        NativePitchEdit no_op{{PitchChange{original_note.id, original_note.pitch}}};
        for (int i = 0; i < 128; ++i) {
            const auto unchanged = core.set_pitches(replay_score.id, no_op,
                {Revision{1}, "noop-" + std::to_string(i)});
            expect(unchanged.score.revision.value == 1 && unchanged.changed_notes == 0,
                "No-op pitch edit advanced revision");
        }
        bool full_rejected = false;
        try {
            core.set_pitches(replay_score.id, no_op, {Revision{1}, "noop-overflow"});
        } catch (const CoreError& error) {
            full_rejected = error.code() == CoreError::resource_limit;
        }
        expect(full_rejected, "No-op eviction made its current revision unusable");
        NativePitchEdit change{{PitchChange{original_note.id, original_note.pitch + 1}}};
        const auto changed_pitch = core.set_pitches(replay_score.id, change, {Revision{1}, "advance"});
        expect(changed_pitch.score.revision.value == 2 && changed_pitch.changed_notes == 1,
            "Changing edit could not retire full no-op replay cache");
        bool forgotten_rejected = false;
        try {
            core.set_pitches(replay_score.id, no_op, {Revision{1}, "noop-0"});
        } catch (const CoreError& error) {
            forgotten_rejected = error.code() == CoreError::stale_revision;
        }
        expect(forgotten_rejected, "Evicted operation retry was not rejected as stale");
        expect(projection->projection.notes.front().pitch == original_note.pitch,
            "Published read projection changed with live score");
        expect(!core.get(replay_score.id).composition, "Native pitch edit left stale composition");
        core.close(replay_score.id);

        const auto with_excerpts = core.create(make_composition());
        const auto generated = core.generate_excerpts(with_excerpts.id);
        expect(generated.revision.value == 2 && !generated.excerpts.empty(),
            "Excerpt generation did not publish a revision");
        expect(core.generate_excerpts(with_excerpts.id).revision.value == 2,
            "Idempotent excerpt generation advanced revision");
        expect(!core.get(with_excerpts.id, generated.excerpts.front().id).musicxml.empty(),
            "Selected excerpt could not export MusicXML");
        core.close(with_excerpts.id);

        // Instrument change / multi-instrument score: every instrument is a
        // separate track, and audio bytes are only ever produced by a worker.
        {
            std::ifstream stream("vendor/musescore/src/engraving/tests/instrumentchange_data/change-ref.mscx", std::ios::binary);
            if (stream) {
                const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                const auto inst_doc = core.open(bytes, InputFormat::Mscx);
                const auto meta_data = core.export_bytes(inst_doc.id, ExportFormat::Metadata);
                const auto meta_json = nlohmann::json::parse(meta_data.bytes);
                expect(meta_json.contains("tracks") && meta_json["tracks"].is_array()
                       && !meta_json["tracks"].empty(),
                       "Multi-instrument score metadata must list its instrument tracks");

                const auto inst_tracks = core.audio_tracks(inst_doc.id);
                expect(inst_tracks.tracks.size() >= 2,
                       "Multi-instrument part must expose one track per instrument");

                bool rejected = false;
                try {
                    (void)core.export_bytes(inst_doc.id, ExportFormat::Wav);
                } catch (const CoreError& error) {
                    rejected = error.code() == CoreError::Code::invalid_argument;
                }
                expect(rejected, "Host audio export must be rejected; audio runs in a worker only");
                core.close(inst_doc.id);
            }
        }

        std::cout << "typed music core checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
