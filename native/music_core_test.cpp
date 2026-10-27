#include "music_core.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using namespace music;

void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
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
        if (argc != 2) throw std::invalid_argument("Usage: music_core_test ASSETS");

        MusicCore core(RuntimeOptions{argv[1], ""});

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

        const auto runtime = core.runtime_options();
        expect(!runtime.resources_path.empty(), "Runtime resources path is empty");
        expect(std::filesystem::u8path(runtime.resources_path).is_absolute(),
               "Runtime resources path is not absolute");
        expect(runtime.font_paths.empty(), "Runtime font paths are not initially empty");
        expect(document_id_from_string("score-42").value == 42, "Document id parser mismatch");
        expect(document_id_to_string(DocumentId{42}) == "score-42", "Document id formatter mismatch");

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

        std::cout << "typed music core checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
