#include <algorithm>
#include <format>
#include <numeric>

#include <ankerl/unordered_dense.h>

#include "phrase.h"
#include "types.h"
#include "utf8_text.h"

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

Fraction add(Fraction a, Fraction b) {
    const auto common = std::gcd(a.den, b.den);
    return {a.num * (b.den / common) + b.num * (a.den / common), a.den * (b.den / common)};
}

Fraction multiply(Fraction a, Fraction b) {
    const auto g1 = std::gcd(a.num, b.den);
    const auto g2 = std::gcd(b.num, a.den);
    return {(a.num / g1) * (b.num / g2), (a.den / g2) * (b.den / g1)};
}

bool same_fraction(Fraction a, Fraction b) { return a.num * b.den == b.num * a.den; }

std::string id_tag(ElementId id, bool ids) { return ids ? "#" + to_string(id) : ""; }

struct Summary {
    void append(std::string_view part) {
        if (truncated) return;
        constexpr std::string_view marker = "\n... [truncated]";
        constexpr usize limit = 32768;
        if (part.size() <= limit - text.size()) {
            text += part;
        } else {
            const auto room = limit - marker.size();
            if (text.size() > room) text.resize(utf8_prefix(text, room));
            text += part.substr(0, utf8_prefix(part, room - text.size()));
            text += marker;
            truncated = true;
        }
    }

    std::string text;
    bool truncated = false;
};

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
        if (note.incoming) token.insert(0, ids ? "~" + id_tag(note.incoming->tie, true) + ":" : "~");
        token += id_tag(note.id, ids);
        if (note.outgoing) token += "~" + id_tag(note.outgoing->tie, ids);
        if (!label.empty()) label += " ";
        label += token;
        json_push(result, std::move(j));
    }
    if (notes.size() > 1) label = "[" + label + "]";
    return result;
}

Json graces(const std::vector<GraceMusic> &group, bool ids, std::string &label) {
    Json result = make_array<Json>();
    for (const auto &g : group) {
        constexpr const char *names[]{"appog", "acciac", "grace4", "after8", "grace16", "after16", "grace32", "after32"};
        std::string chord;
        const auto duration = fraction(g.written_duration);
        const auto *kind = names[static_cast<int>(g.kind)];
        Json j = {{"kind", kind}, {"duration", duration}, {"notes", notes(g.notes, ids, chord)}};
        label += std::string(kind) + "(" + chord + ":" + duration + ")" + id_tag(g.chord, ids) + " ";
        if (ids) j["id"] = to_string(g.chord);
        json_push(result, std::move(j));
    }
    return result;
}
} // namespace

Json music_result(const engrave::MusicWindow &w, const engrave::Outline &outline, bool ids) {
    Json out = {{"ok", true},
                {"revision", std::to_string(w.revision)},
                {"score", w.score.excerpt ? "excerpt" : "master"},
                {"excerpt", w.score.excerpt ? Json(to_string(*w.score.excerpt)) : Json(nullptr)}};
    Summary summary;
    if (w.score.excerpt) summary.append("Excerpt " + to_string(*w.score.excerpt) + "\n");
    for (const auto *key : {"staves", "measures", "tuplets", "spanners", "voltas"}) out[key] = make_array<Json>();
    ankerl::unordered_dense::map<std::string, int> bars, tuplets;
    for (const auto &m : outline.measures) bars[to_string(m.id)] = m.index + 1;
    for (const auto &s : w.staves) {
        summary.append(std::format("Staff {} {} time={} key={}\n", s.staff + 1, s.part_name, fraction(s.time_sig, true), s.key));
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
        summary.append(std::format("Bar {}{} len={}", m.measure.index + 1, id_tag(m.measure.id, ids), fraction(m.measure.duration)));
        if (m.repeat_start) summary.append(" |:");
        for (const auto &s : m.signature_changes) {
            Json j = {{"type", s.type}, {"tick", fraction(s.tick)}, {"staff", s.staff + 1}, {"key", s.key}, {"bpm", s.bpm},
                      {"text", s.text}};
            const auto offset = fraction(add(s.tick, {-m.measure.tick.num, m.measure.tick.den}));
            if (s.timesig) {
                j["timesig"] = fraction(*s.timesig, true);
                summary.append(std::format(" time(s{})={}@{}", s.staff + 1, fraction(*s.timesig, true), offset));
            } else if (s.type == "KeySig") {
                summary.append(std::format(" key(s{})={}@{}", s.staff + 1, s.key, offset));
            } else if (s.type == "Tempo") {
                summary.append(std::format(" tempo={}bpm@{}", s.bpm, offset));
            }
            json_push(bar["signature_changes"], std::move(j));
        }
        for (const auto &v : m.voices) {
            Json voice = {{"staff", v.staff + 1}, {"voice", v.voice + 1}, {"events", make_array<Json>()}};
            summary.append(std::format("\n  s{}v{}", v.staff + 1, v.voice + 1));
            Fraction end;
            std::vector<ElementId> open_tuplets;
            for (const auto &e : v.events) {
                Json j = {
                    {"offset", fraction(e.offset)}, {"duration", fraction(e.written_duration)}, {"elapsed", fraction(e.elapsed_duration)}};
                std::string token;
                std::string before, after;
                if (ids) j["id"] = to_string(e.id);
                if (auto *rest = std::get_if<RestMusic>(&e.body)) {
                    token = "r";
                    j["rest"] = true;
                    j["measure_rest"] = rest->measure_rest;
                    if (rest->measure_repeat_count) {
                        j["measure_repeat_count"] = *rest->measure_repeat_count;
                        token = "repeat(" + std::to_string(*rest->measure_repeat_count) + ")";
                    }
                } else {
                    const auto &chord = std::get<ChordMusic>(e.body);
                    j["notes"] = notes(chord.notes, ids, token);
                    if (!chord.before.empty()) j["grace_before"] = graces(chord.before, ids, before);
                    if (!chord.after.empty()) j["grace_after"] = graces(chord.after, ids, after);
                }
                if (!e.tuplet_path.empty()) {
                    j["tuplet_path"] = make_array<Json>();
                    for (auto id : e.tuplet_path) json_push(j["tuplet_path"], tuplets.at(to_string(id)));
                }
                const auto shared = static_cast<usize>(std::ranges::mismatch(open_tuplets, e.tuplet_path).in1 - open_tuplets.begin());
                summary.append(std::string(open_tuplets.size() - shared, ')'));
                summary.append(" ");
                if (e.offset != end) summary.append("@" + fraction(e.offset) + " ");
                for (usize depth = shared; depth < e.tuplet_path.size(); ++depth) {
                    const auto &group = w.tuplets[tuplets.at(to_string(e.tuplet_path[depth])) - 1];
                    summary.append(std::format("{}:{}{}(", group.actual, group.normal, id_tag(group.id, ids)));
                }
                summary.append(before + token + ":" + fraction(e.written_duration) + id_tag(e.id, ids));
                if (!after.empty()) summary.append(" " + after.substr(0, after.size() - 1));
                Fraction expected = e.written_duration;
                for (auto id : e.tuplet_path) {
                    const auto &group = w.tuplets[tuplets.at(to_string(id)) - 1];
                    expected = multiply(expected, {group.normal, group.actual});
                }
                if (!same_fraction(expected, e.elapsed_duration)) {
                    summary.append("(elapsed=" + fraction(e.elapsed_duration) + ")");
                }
                end = add(e.offset, e.elapsed_duration);
                open_tuplets = e.tuplet_path;
                json_push(voice["events"], std::move(j));
            }
            summary.append(std::string(open_tuplets.size(), ')'));
            json_push(bar["voices"], std::move(voice));
        }
        if (m.repeat_end) summary.append(m.repeat_count > 2 ? " :|x" + std::to_string(m.repeat_count) : " :|");
        summary.append("\n");
        json_push(out["measures"], std::move(bar));
    }
    auto position = [&](const Position &p) {
        Json j = {
            {"measure", bars.at(to_string(p.measure))}, {"offset", fraction(p.offset)}, {"staff", p.staff + 1}, {"voice", p.voice + 1}};
        if (ids) j["measure_id"] = to_string(p.measure);
        return j;
    };
    auto position_label = [&](const Position &p) {
        return std::format("{}@{}(s{}v{})", bars.at(to_string(p.measure)), fraction(p.offset), p.staff + 1, p.voice + 1);
    };
    auto span_label = [&](const SpanMusic &s) {
        return std::format(" {}{} {}-{}", s.type, id_tag(s.id, ids), position_label(s.start), position_label(s.end));
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
    if (!w.spanners.empty()) summary.append("Spanners:");
    for (const auto &s : w.spanners) {
        json_push(out["spanners"], span(s));
        summary.append(span_label(s));
    }
    if (!w.spanners.empty()) summary.append("\n");
    if (!w.voltas.empty()) summary.append("Voltas:");
    for (const auto &v : w.voltas) {
        Json j = span(v.span);
        j["closed"] = v.closed;
        j["endings"] = make_array<Json>();
        for (int ending : v.endings) json_push(j["endings"], ending);
        summary.append(span_label(v.span) + " endings=" + dump_json(j["endings"]) + (v.closed ? " closed" : " open"));
        json_push(out["voltas"], std::move(j));
    }
    if (summary.text.ends_with('\n')) summary.text.pop_back();
    out["summary"] = std::move(summary.text);
    return out;
}
} // namespace music::api
