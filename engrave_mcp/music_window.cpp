#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <numeric>
#include <optional>

#include "api_json_internal.h"
#include "containers.h"
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

std::string id_tag(ElementId id, bool ids) { return ids ? "#" + to_string(id) : ""; }

std::string id_tag(const std::optional<ElementId> &id, bool ids) { return id ? id_tag(*id, ids) : ""; }

// User-authored text is always quoted and escaped in the summary, so it can never start a line such as a fake "Bar".
std::string quote_text(std::string_view text) { return '"' + escape_text(text, '"') + '"'; }

bool is_token(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

// "s1v2", "s1" or "" for whichever of the staff and voice indices is set.
std::string track(int staff, int voice) {
    std::string out;
    if (staff >= 0) out = std::format("s{}", staff + 1);
    if (voice >= 0) out += std::format("v{}", voice + 1);
    return out;
}

struct Summary {
    void append(std::string_view part) {
        if (truncated) return;
        // Serialization replaces each invalid byte with a 3-byte U+FFFD, so repair before measuring against the cap.
        std::string repaired;
        if (!valid_utf8(part)) part = repaired = replace_invalid_utf8(part);
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

// The engine numbers an endpoint's bar in the selected score even outside the window; -1 means that score lacks the bar.
struct Bars {
    Json position(const Position &p) const {
        Json j = {{"measure", detail::one_based(p.measure_index)},
                  {"offset", fraction(p.offset)},
                  {"staff", detail::one_based(p.staff)},
                  {"voice", detail::one_based(p.voice)}};
        if (ids) j["measure_id"] = to_string(p.measure);
        return j;
    }

    std::string label(const Position &p) const {
        const auto where = track(p.staff, p.voice);
        const auto bar = p.measure_index >= 0 ? std::to_string(static_cast<std::int64_t>(p.measure_index) + 1) : "?";
        return std::format("{}@{}{}", bar, fraction(p.offset), where.empty() ? "" : "(" + where + ")");
    }

    bool ids = false;
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
        std::string chord;
        const auto duration = fraction(g.written_duration);
        const auto kind = detail::grace_kind_name(g.kind);
        Json j = {{"kind", kind}, {"duration", duration}, {"notes", notes(g.notes, ids, chord)}};
        label += std::string(kind) + "(" + chord + ":" + duration + ")" + id_tag(g.chord, ids) + " ";
        if (ids) j["id"] = to_string(g.chord);
        json_push(result, std::move(j));
    }
    return result;
}

void payload(const DynamicMark &mark, Json &j, std::string &label, const Bars &) {
    j["type"] = "dynamic";
    j["value"] = mark.value;
    label = is_token(mark.value) ? mark.value : "dynamic " + quote_text(mark.value);
    if (mark.custom_text) {
        j["custom_text"] = *mark.custom_text;
        label += " " + quote_text(*mark.custom_text);
    }
}

void payload(const FermataMark &mark, Json &j, std::string &label, const Bars &) {
    j["type"] = "fermata";
    j["symbol"] = mark.symbol;
    label = "fermata(" + mark.symbol + ")";
}

void payload(const HarmonyMark &mark, Json &j, std::string &label, const Bars &) {
    j["type"] = "harmony";
    j["text"] = mark.text;
    label = "harmony " + quote_text(mark.text);
}

void payload(const TextMark &mark, Json &j, std::string &label, const Bars &) {
    std::string_view kind = "expression";
    if (mark.kind == TextMark::Kind::STAFF) kind = "staff_text";
    if (mark.kind == TextMark::Kind::SYSTEM) kind = "system_text";
    if (mark.kind == TextMark::Kind::REHEARSAL) kind = "rehearsal_mark";
    j["type"] = "text";
    j["kind"] = kind;
    j["text"] = mark.text;
    label = std::string(kind) + " " + quote_text(mark.text);
}

void payload(const ArticulationMark &mark, Json &j, std::string &label, const Bars &) {
    j["type"] = "articulation";
    j["symbol"] = mark.symbol;
    if (mark.ornament) j["ornament"] = true;
    label = (mark.ornament ? "ornament(" : "artic(") + mark.symbol + ")";
}

void payload(const LyricMark &mark, Json &j, std::string &label, const Bars &) {
    const auto verse = static_cast<std::int64_t>(mark.verse) + 1;
    j["type"] = "lyric";
    j["verse"] = verse;
    j["text"] = mark.text;
    label = std::format("lyric[{}] {}", verse, quote_text(mark.text));
    if (mark.syllabic != "single") {
        j["syllabic"] = mark.syllabic;
        label += " " + mark.syllabic;
    }
    if (mark.melisma_ticks) {
        j["melisma"] = fraction(*mark.melisma_ticks);
        label += " melisma=" + fraction(*mark.melisma_ticks);
    }
}

void payload(const ArpeggioMark &mark, Json &j, std::string &label, const Bars &) {
    j["type"] = "arpeggio";
    j["subtype"] = mark.subtype;
    label = "arpeggio(" + mark.subtype + ")";
}

void payload(const TremoloMark &mark, Json &j, std::string &label, const Bars &bars) {
    j["type"] = "tremolo";
    j["subtype"] = mark.subtype;
    label = "tremolo(" + mark.subtype + ")";
    if (!mark.two_chord) return;
    j["two_chord"] = true;
    if (mark.end) {
        j["end"] = bars.position(*mark.end);
        label += "-" + bars.label(*mark.end);
    }
    if (bars.ids && mark.end_event) j["end_event"] = to_string(*mark.end_event);
}

void payload(const ClefMark &mark, Json &j, std::string &label, const Bars &) {
    j["type"] = "clef";
    j["clef"] = mark.clef;
    label = "clef(" + mark.clef + ")";
}

void payload(const BarlineMark &mark, Json &j, std::string &label, const Bars &) {
    j["type"] = "barline";
    j["subtype"] = mark.subtype;
    label = "barline(" + mark.subtype + ")";
}

// Staff and voice appear only where the scope has them. An instrument-wide marking keeps the staff it is anchored on,
// which may lie outside the selected staves.
Json marking(const MarkMusic &mark, const Bars &bars, std::string &label) {
    const bool ids = bars.ids;
    Json j = {{"offset", fraction(mark.offset)}};
    std::string prefix;
    switch (mark.scope) {
        case MarkScope::SYSTEM: j["scope"] = "system"; break;
        case MarkScope::INSTRUMENT: {
            j["scope"] = "instrument";
            if (mark.staff >= 0) j["staff"] = mark.staff + 1;
            const auto anchor = track(mark.staff, -1);
            prefix = anchor.empty() ? "inst " : "inst(" + anchor + ") ";
            break;
        }
        case MarkScope::STAFF:
            j["scope"] = "staff";
            j["staff"] = mark.staff + 1;
            prefix = track(mark.staff, -1) + " ";
            break;
        case MarkScope::VOICE:
            j["scope"] = "voice";
            j["staff"] = mark.staff + 1;
            j["voice"] = mark.voice + 1;
            prefix = track(mark.staff, mark.voice) + " ";
            break;
    }
    if (ids && mark.id) j["id"] = to_string(*mark.id);
    std::string grace;
    if (mark.attachment) {
        Json attachment = make_object<Json>();
        if (ids && mark.attachment->event) attachment["event"] = to_string(*mark.attachment->event);
        if (const auto &g = mark.attachment->grace) {
            const auto side = g->after ? "after" : "before";
            attachment["grace"] = Json{{"side", side}, {"ordinal", g->ordinal + 1}};
            if (ids && g->chord) attachment["grace"]["id"] = to_string(*g->chord);
            grace = std::format("grace({}:{}) ", side, g->ordinal + 1);
        }
        if (!attachment.get_object().empty()) j["attachment"] = std::move(attachment);
    }
    std::string body;
    std::visit([&](const auto &value) { payload(value, j, body, bars); }, mark.payload);
    label = prefix + "@" + fraction(mark.offset) + " " + grace + body + id_tag(mark.id, ids);
    return j;
}

void span_detail(const SpanDetail &d, Json &j, std::string &label) {
    if (d.direction) {
        j["direction"] = *d.direction;
        label += " " + *d.direction;
    }
    if (d.niente) {
        j["niente"] = true;
        label += " niente";
    }
    for (const auto &[key, text] :
         {std::pair{"text", &d.text}, {"begin_text", &d.begin_text}, {"continue_text", &d.continue_text}, {"end_text", &d.end_text}}) {
        if (!*text || (*text)->empty()) continue;
        j[key] = **text;
        label += std::format(" {}={}", key, quote_text(**text));
    }
    for (const auto &[key, hook] : {std::pair{"begin_hook", &d.begin_hook}, {"end_hook", &d.end_hook}}) {
        if (!*hook || (*hook)->empty()) continue;
        j[key] = **hook;
        label += std::format(" {}={}", key, **hook);
    }
}
} // namespace

Json music_result(const engrave::MusicWindow &w, bool ids) {
    Json out = {{"ok", true},
                {"revision", std::to_string(w.revision)},
                {"score", w.score.excerpt ? "excerpt" : "master"},
                {"excerpt", w.score.excerpt ? Json(to_string(*w.score.excerpt)) : Json(nullptr)}};
    Summary summary;
    if (w.score.excerpt) summary.append("Excerpt " + to_string(*w.score.excerpt) + "\n");
    for (const auto *key : {"staves", "measures", "tuplets", "spanners", "voltas"}) out[key] = make_array<Json>();
    Bars bars{.ids = ids};
    HashMap<std::string, int> tuplets;
    for (const auto &s : w.staves) {
        const auto clef = s.clef.empty() ? std::string() : " clef=" + s.clef;
        summary.append(
            std::format("Staff {} {}{} time={} key={}\n", s.staff + 1, escape_text(s.part_name), clef, fraction(s.time_sig, true), s.key));
        Json staff = {{"staff", s.staff + 1},
                      {"part_name", s.part_name},
                      {"instrument_id", s.instrument_id},
                      {"time_sig", fraction(s.time_sig, true)},
                      {"key", s.key},
                      {"uses_drumset", s.uses_drumset},
                      {"transpose", {{"diatonic", s.transpose.diatonic}, {"chromatic", s.transpose.chromatic}}}};
        if (!s.clef.empty()) staff["clef"] = s.clef;
        json_push(out["staves"], std::move(staff));
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
            Json j = {{"type", s.type}, {"tick", fraction(s.tick)}, {"staff", detail::one_based(s.staff)}, {"key", s.key}, {"bpm", s.bpm},
                      {"text", s.text}};
            if (ids && s.id) j["id"] = to_string(*s.id);
            const auto offset = fraction(add(s.tick, {-m.measure.tick.num, m.measure.tick.den}));
            const auto tag = id_tag(s.id, ids);
            if (s.timesig) {
                j["timesig"] = fraction(*s.timesig, true);
                summary.append(std::format(" time(s{})={}@{}{}", s.staff + 1, fraction(*s.timesig, true), offset, tag));
            } else if (s.type == "KeySig") {
                summary.append(std::format(" key(s{})={}@{}{}", s.staff + 1, s.key, offset, tag));
            } else if (s.type == "Tempo") {
                const auto words = s.text.empty() ? std::string() : quote_text(s.text) + " ";
                summary.append(std::format(" tempo={}{}bpm@{}{}", words, s.bpm, offset, tag));
            }
            json_push(bar["signature_changes"], std::move(j));
        }
        // The repeat end closes the last voice line, or the Bar line when no voice is selected, never the marks line.
        const auto repeat_end = !m.repeat_end ? std::string() : m.repeat_count > 2 ? " :|x" + std::to_string(m.repeat_count) : " :|";
        if (m.voices.empty()) summary.append(repeat_end);
        if (!m.markings.empty()) {
            bar["markings"] = make_array<Json>();
            summary.append("\n  marks:");
            const char *separator = " ";
            for (const auto &mark : m.markings) {
                std::string label;
                json_push(bar["markings"], marking(mark, bars, label));
                summary.append(separator + label);
                separator = " \xc2\xb7 ";
            }
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
                if (expected != e.elapsed_duration) {
                    summary.append("(elapsed=" + fraction(e.elapsed_duration) + ")");
                }
                end = add(e.offset, e.elapsed_duration);
                open_tuplets = e.tuplet_path;
                json_push(voice["events"], std::move(j));
            }
            summary.append(std::string(open_tuplets.size(), ')'));
            json_push(bar["voices"], std::move(voice));
        }
        if (!m.voices.empty()) summary.append(repeat_end);
        summary.append("\n");
        json_push(out["measures"], std::move(bar));
    }
    const auto first_bar = w.measures.empty() ? 0 : w.measures.front().measure.index;
    auto span = [&](const SpanMusic &s, std::string &label) {
        Json j = {{"type", s.type}, {"start", bars.position(s.start)}, {"end", bars.position(s.end)}};
        if (s.subtype) j["subtype"] = *s.subtype;
        if (ids) {
            j["id"] = to_string(s.id);
            if (s.start_element) j["start_element"] = to_string(*s.start_element);
            if (s.end_element) j["end_element"] = to_string(*s.end_element);
        }
        const bool continued = s.start.measure_index >= 0 && s.start.measure_index < first_bar;
        const auto type = s.subtype ? s.type + "(" + *s.subtype + ")" : s.type;
        label =
            std::format("{}{}{} {}-{}", continued ? "\xe2\x86\x90" : "", type, id_tag(s.id, ids), bars.label(s.start), bars.label(s.end));
        if (s.detail) span_detail(*s.detail, j, label);
        return j;
    };
    // Items are joined like marks: so the space-separated details of one spanner cannot read as the next one.
    const char *separator = " ";
    if (!w.spanners.empty()) summary.append("Spanners:");
    for (const auto &s : w.spanners) {
        std::string label;
        json_push(out["spanners"], span(s, label));
        summary.append(separator + label);
        separator = " \xc2\xb7 ";
    }
    if (!w.spanners.empty()) summary.append("\n");
    separator = " ";
    if (!w.voltas.empty()) summary.append("Voltas:");
    for (const auto &v : w.voltas) {
        std::string label;
        Json j = span(v.span, label);
        j["closed"] = v.closed;
        j["endings"] = make_array<Json>();
        for (int ending : v.endings) json_push(j["endings"], ending);
        summary.append(separator + label + " endings=" + dump_json(j["endings"]) + (v.closed ? " closed" : " open"));
        separator = " \xc2\xb7 ";
        json_push(out["voltas"], std::move(j));
    }
    if (summary.text.ends_with('\n')) summary.text.pop_back();
    out["summary"] = std::move(summary.text);
    return out;
}
} // namespace music::api
