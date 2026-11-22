#include <numeric>

#include <ankerl/unordered_dense.h>

#include "phrase.h"

namespace music::api {
namespace {
using namespace engrave;

std::string fraction(Fraction f, bool meter = false) {
    if (!meter) {
        if (!f.num) return "0";
        auto divisor = std::gcd(f.num, f.den);
        f.num /= divisor;
        f.den /= divisor;
    }
    return std::to_string(f.num) + "/" + std::to_string(f.den);
}

std::string spelling(SpelledPitch p) {
    std::string s(1, "CDEFGAB"[static_cast<int>(p.step)]);
    s.append(static_cast<std::size_t>(std::abs(p.alter)), p.alter < 0 ? 'b' : '#');
    return s + std::to_string(p.octave);
}

Json notes(const std::vector<NoteMusic> &notes, bool ids, std::string &label) {
    Json result = make_array<Json>();
    for (const auto &note : notes) {
        Json j = make_object<Json>();
        std::string token;
        if (auto *pitch = std::get_if<PitchedNote>(&note.pitch)) {
            token = spelling(pitch->concert);
            j["concert"] = token;
            j["written"] = spelling(pitch->written);
            j["midi"] = pitch->midi;
        } else {
            const auto &drum = std::get<DrumNote>(note.pitch);
            token = "d" + std::to_string(drum.key);
            j["drum"] = drum.key;
            if (drum.kit_name) j["kit_name"] = *drum.kit_name;
        }
        if (ids) j["id"] = to_string(note.id);
        auto tie = [&](const char *key, const std::optional<TieLink> &link) {
            if (!link) return;
            j[key] = true;
            if (ids) {
                j[key] = Json{{"id", to_string(link->tie)}};
                if (link->other_note) j[key]["other_note"] = to_string(*link->other_note);
            }
        };
        tie("incoming", note.incoming);
        tie("outgoing", note.outgoing);
        if (note.outgoing) token += "~";
        if (!label.empty()) label += " ";
        label += token;
        json_push(result, std::move(j));
    }
    return result;
}

Json graces(const std::vector<GraceMusic> &group, bool ids) {
    Json result = make_array<Json>();
    for (const auto &g : group) {
        constexpr const char *names[]{"appog", "acciac", "grace4", "after8", "grace16", "after16", "grace32", "after32"};
        std::string ignored;
        Json j = {
            {"kind", names[static_cast<int>(g.kind)]}, {"duration", fraction(g.written_duration)}, {"notes", notes(g.notes, ids, ignored)}};
        if (ids) j["id"] = to_string(g.chord);
        json_push(result, std::move(j));
    }
    return result;
}
} // namespace

Json music_result(const engrave::MusicWindow &w, const engrave::Outline &outline, bool ids) {
    Json out = {{"ok", true}, {"revision", std::to_string(w.revision)}};
    for (const auto *key : {"staves", "measures", "tuplets", "spanners", "voltas"}) out[key] = make_array<Json>();
    ankerl::unordered_dense::map<std::string, int> bars, tuplets;
    for (const auto &m : outline.measures) bars[to_string(m.id)] = m.index + 1;
    for (const auto &s : w.staves) {
        json_push(out["staves"], Json{{"staff", s.staff + 1},
                                      {"part_name", s.part_name},
                                      {"instrument_id", s.instrument_id},
                                      {"time_sig", fraction(s.time_sig, true)},
                                      {"key", s.key},
                                      {"uses_drumset", s.uses_drumset},
                                      {"transpose", {{"diatonic", s.transpose.diatonic}, {"chromatic", s.transpose.chromatic}}}});
    }
    for (const auto &t : w.tuplets) {
        int number = static_cast<int>(tuplets.size() + 1);
        tuplets[to_string(t.id)] = number;
        Json j = {{"group", number}, {"actual", t.actual}, {"normal", t.normal}, {"base_duration", fraction(t.base_duration)}};
        if (ids) j["id"] = to_string(t.id);
        json_push(out["tuplets"], std::move(j));
    }
    std::string summary;
    for (const auto &m : w.measures) {
        Json bar = {{"measure", m.measure.index + 1},
                    {"tick", fraction(m.measure.tick)},
                    {"duration", fraction(m.measure.duration)},
                    {"repeat_start", m.repeat_start},
                    {"repeat_end", m.repeat_end},
                    {"repeat_count", m.repeat_count},
                    {"voices", make_array<Json>()},
                    {"signature_changes", make_array<Json>()}};
        if (ids) bar["id"] = to_string(m.measure.id);
        if (!summary.empty()) summary += "\n";
        summary += "Bar " + std::to_string(m.measure.index + 1);
        for (const auto &s : m.signature_changes) {
            Json j = {{"type", s.type}, {"tick", fraction(s.tick)}, {"staff", s.staff + 1}, {"key", s.key}, {"bpm", s.bpm},
                      {"text", s.text}};
            if (s.timesig) j["timesig"] = fraction(*s.timesig, true);
            json_push(bar["signature_changes"], std::move(j));
        }
        for (const auto &v : m.voices) {
            Json voice = {{"staff", v.staff + 1}, {"voice", v.voice + 1}, {"events", make_array<Json>()}};
            summary += "  s" + std::to_string(v.staff + 1) + "v" + std::to_string(v.voice + 1);
            for (const auto &e : v.events) {
                Json j = {
                    {"offset", fraction(e.offset)}, {"duration", fraction(e.written_duration)}, {"elapsed", fraction(e.elapsed_duration)}};
                std::string token;
                if (ids) j["id"] = to_string(e.id);
                if (auto *rest = std::get_if<RestMusic>(&e.body)) {
                    token = "r";
                    j["rest"] = true;
                    j["measure_rest"] = rest->measure_rest;
                    if (rest->measure_repeat_count) j["measure_repeat_count"] = *rest->measure_repeat_count;
                } else {
                    const auto &chord = std::get<ChordMusic>(e.body);
                    j["notes"] = notes(chord.notes, ids, token);
                    if (chord.notes.size() > 1) token = "[" + token + "]";
                    if (!chord.before.empty()) j["grace_before"] = graces(chord.before, ids);
                    if (!chord.after.empty()) j["grace_after"] = graces(chord.after, ids);
                }
                if (!e.tuplet_path.empty()) {
                    j["tuplet_path"] = make_array<Json>();
                    for (auto id : e.tuplet_path) json_push(j["tuplet_path"], tuplets.at(to_string(id)));
                }
                if (summary.size() < 32768) summary += " " + token + ":" + fraction(e.written_duration);
                json_push(voice["events"], std::move(j));
            }
            json_push(bar["voices"], std::move(voice));
        }
        json_push(out["measures"], std::move(bar));
    }
    auto position = [&](const Position &p) {
        Json j = {
            {"measure", bars.at(to_string(p.measure))}, {"offset", fraction(p.offset)}, {"staff", p.staff + 1}, {"voice", p.voice + 1}};
        if (ids) j["measure_id"] = to_string(p.measure);
        return j;
    };
    auto span = [&](const SpanMusic &s) {
        Json j = {{"type", s.type}, {"start", position(s.start)}, {"end", position(s.end)}};
        if (ids) {
            j["id"] = to_string(s.id);
            if (s.start_element) j["start_element"] = to_string(*s.start_element);
            if (s.end_element) j["end_element"] = to_string(*s.end_element);
        }
        return j;
    };
    for (const auto &s : w.spanners) json_push(out["spanners"], span(s));
    for (const auto &v : w.voltas) {
        Json j = span(v.span);
        j["closed"] = v.closed;
        j["endings"] = make_array<Json>();
        for (int ending : v.endings) json_push(j["endings"], ending);
        json_push(out["voltas"], std::move(j));
    }
    out["summary"] = std::move(summary);
    return out;
}
} // namespace music::api
