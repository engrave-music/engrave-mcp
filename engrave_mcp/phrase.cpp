#include "phrase.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>

#include "types.h"

namespace music::api {
namespace {
using namespace engrave;
using std::string_view;

struct InputError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void reject(const std::string &path, string_view expected) {
    throw InputError(path + ": invalid phrase input; expected " + std::string(expected));
}

std::string indexed(const std::string &path, usize i) { return path + "[" + std::to_string(i) + "]"; }

void fields(const Json &j, const std::string &path, std::initializer_list<string_view> keys) {
    if (!j.is_object()) reject(path, "an object");
    for (const auto &[key, value] : j.get_object()) {
        if (std::ranges::find(keys, key) == keys.end()) reject(path + "." + key, "a supported field");
    }
}

const Json &required(const Json &j, const char *key, const std::string &path) {
    if (!j.contains(key)) reject(path + "." + key, "a required field");
    return j.at(key);
}

const Json::array_t &items(const Json &j, const std::string &path) {
    if (!j.is_array()) reject(path, "an array");
    return j.get_array();
}

string_view string(const Json &j, const std::string &path) {
    if (!j.is_string()) reject(path, "a string");
    return j.get<std::string>();
}

int number(const Json &j, const std::string &path, int low, int high) {
    if ((!j.holds<u64>() && !j.holds<i64>()) || j.as<double>() < low || j.as<double>() > high) reject(path, "an integer in range");
    return j.as<int>();
}

int decimal(string_view s, const std::string &path) {
    int n{};
    if (s.empty()) reject(path, "an integer");
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size()) reject(path, "an integer");
    return n;
}

Fraction ratio(string_view s, char sep, const std::string &path) {
    if (s == "0" && sep == '/') return {0, 1};
    const auto pos = s.find(sep);
    if (pos == s.npos) reject(path, sep == '/' ? "a fraction such as 3/8" : "a ratio such as 3:2");
    const int n = decimal(s.substr(0, pos), path), d = decimal(s.substr(pos + 1), path);
    if (n < 0 || d <= 0) reject(path, "a nonnegative fraction with positive denominator");
    return {n, d};
}

// Limiting reduced cursor components to engine tick bounds also leaves room for two cross-products in int64.
Fraction reduced(i64 n, i64 d, const std::string &path) {
    const auto divisor = std::gcd(n, d);
    n /= divisor;
    d /= divisor;
    if (n > INT32_MAX || n < -INT32_MAX || d > INT32_MAX) reject(path, "a duration representable in engine ticks");
    return {n, d};
}

Fraction add(Fraction a, Fraction b, const std::string &path) { return reduced(a.num * b.den + b.num * a.den, a.den * b.den, path); }

SpelledPitch pitch(string_view s, const std::string &path) {
    constexpr string_view names = "CDEFGAB";
    if (s.empty() || names.find(s.front()) == names.npos) reject(path, "a spelled pitch such as Eb4");
    auto step = static_cast<StepName>(names.find(s.front()));
    s.remove_prefix(1);
    int alter = 0;
    if (s.starts_with('#') || s.starts_with('b')) {
        const char accidental = s.front();
        while (s.starts_with(accidental)) {
            alter += accidental == '#' ? 1 : -1;
            s.remove_prefix(1);
        }
    }
    const int octave = decimal(s, path);
    if (alter < -2 || alter > 2 || octave < -2 || octave > 10) reject(path, "a pitch within the supported spelling range");
    return {step, alter, octave};
}

constexpr std::pair<string_view, GraceKind> grace_kinds[]{{"acciac", GraceKind::Acciaccatura},  {"appog", GraceKind::Appoggiatura},
                                                          {"grace4", GraceKind::Grace4},        {"grace16", GraceKind::Grace16},
                                                          {"grace32", GraceKind::Grace32},      {"after8", GraceKind::Grace8After},
                                                          {"after16", GraceKind::Grace16After}, {"after32", GraceKind::Grace32After}};

std::vector<PhraseNote> notes(const Json &j, const std::string &path) {
    const bool drums = j.contains("drums");
    const auto key = drums ? "drums" : "pitches";
    if (drums && j.contains("pitches")) reject(path, "pitches or drums");
    const auto source = path + "." + key;
    const auto &array = items(required(j, key, path), source);
    std::vector<PhraseNote> result;
    for (usize i = 0; i < array.size(); ++i) {
        const auto at = indexed(source, i);
        EntryPitch p = drums ? EntryPitch{DrumKey{number(array[i], at, 0, 127)}} : EntryPitch{pitch(string(array[i], at), at)};
        result.push_back({p, false});
    }
    if (j.contains("tie")) {
        const auto &tie = j.at("tie");
        if (tie.is_boolean()) {
            for (auto &note : result) note.tie_to_next = tie.get<bool>();
        } else {
            const auto &ties = items(tie, path + ".tie");
            for (usize i = 0; i < ties.size(); ++i) {
                auto found = std::find(array.begin(), array.end(), ties[i]);
                if (found == array.end()) reject(indexed(path + ".tie", i), "a member of this chord");
                result[static_cast<usize>(found - array.begin())].tie_to_next = true;
            }
        }
    }
    return result;
}

Fraction duration(const Json &j, const std::string &path) {
    if (!j.contains("duration")) return {1, 4};
    auto f = ratio(string(j.at("duration"), path + ".duration"), '/', path + ".duration");
    if (!f.num) reject(path + ".duration", "a positive written duration");
    return f;
}

PhraseEvent event(const Json &j, const std::string &path) {
    fields(j, path, {"pitches", "drums", "duration", "tie", "grace_before", "grace_after"});
    PhraseEvent result{duration(j, path), notes(j, path), {}, {}};
    for (const auto key : {"grace_before", "grace_after"}) {
        if (!j.contains(key)) continue;
        const bool after = string_view(key) == "grace_after";
        const auto &array = items(j.at(key), path + "." + key);
        for (usize i = 0; i < array.size(); ++i) {
            const auto &g = array[i];
            auto at = indexed(path + "." + key, i);
            fields(g, at, {"kind", "duration", "pitches", "drums"});
            auto kind = string(required(g, "kind", at), at + ".kind");
            auto found = std::ranges::find_if(grace_kinds, [&](auto pair) { return pair.first == kind; });
            if (found == std::end(grace_kinds) || after != kind.starts_with("after")) {
                reject(at + ".kind", "a grace kind on this side of the chord");
            }
            PhraseGrace grace{found->second, duration(g, at), {}};
            for (auto note : notes(g, at)) grace.notes.push_back(note.pitch);
            (after ? result.after : result.before).push_back(std::move(grace));
        }
    }
    return result;
}

// Tokenize the textual arm into the same event vocabulary used by the structured arm.
struct Dsl {
    Dsl(string_view source, std::string path) : source_(source), path_(std::move(path)) {}

    Json parse() { return sequence(false, path_); }

private:
    string_view source_;
    std::string path_;

    void space() { source_.remove_prefix(std::min(source_.size(), source_.find_first_not_of(" \t\r\n"))); }

    bool take(char c) {
        space();
        if (!source_.starts_with(c)) return false;
        source_.remove_prefix(1);
        return true;
    }

    void expect(char c, const std::string &at) {
        if (!take(c)) reject(at, std::string(1, c));
    }

    string_view word(const std::string &at) {
        space();
        auto n = std::min(source_.size(), source_.find_first_of(" \r\t\n[]():~|"));
        if (!n) reject(at, "a token");
        auto token = source_.substr(0, n);
        source_.remove_prefix(n);
        return token;
    }

    Json chord(const std::string &at) {
        Json values = make_array<Json>(), tied = make_array<Json>();
        bool bracket = take('['), drum = false;
        do {
            auto token = word(at);
            bool d = token.starts_with('d');
            if (!values.empty() && drum != d) reject(at, "all pitches or all drum keys");
            drum = d;
            if (token == "r") {
                if (bracket) reject(at, "a chord member");
            } else {
                Json value = d ? Json(decimal(token.substr(1), at)) : Json(std::string(token));
                json_push(values, value);
                if (take('~')) json_push(tied, value);
            }
        } while (bracket && !take(']'));
        Json j = make_object<Json>();
        j[drum ? "drums" : "pitches"] = std::move(values);
        if (bracket && take('~')) {
            j["tie"] = true;
        } else if (!tied.empty()) {
            j["tie"] = std::move(tied);
        }
        if (take(':')) {
            const auto where = at + ".duration";
            auto token = word(where);
            int dots = 0;
            while (token.ends_with('.')) {
                ++dots;
                token.remove_suffix(1);
            }
            if (dots > 2) reject(where, "at most two duration dots");
            constexpr string_view names[]{"w", "h", "q", "e", "s", "32", "64"};
            auto it = std::ranges::find(names, token);
            Fraction f = it == std::end(names) ? ratio(token, '/', where) : Fraction{1, 1LL << (it - std::begin(names))};
            f = reduced(f.num * ((2 << dots) - 1), f.den * (1 << dots), where);
            j["duration"] = std::to_string(f.num) + "/" + std::to_string(f.den);
        }
        if (take('~')) j["tie"] = true;
        return j;
    }

    Json graces(bool after, const std::string &at) {
        Json array = make_array<Json>();
        for (;;) {
            space();
            auto it = std::ranges::find_if(grace_kinds,
                                           [&](auto g) { return after == g.first.starts_with("after") && source_.starts_with(g.first); });
            if (it == std::end(grace_kinds)) return array;
            source_.remove_prefix(it->first.size());
            const auto where = indexed(at + (after ? ".grace_after" : ".grace_before"), array.size());
            expect('(', where);
            Json g = chord(where);
            expect(')', where);
            g["kind"] = std::string(it->first);
            json_push(array, std::move(g));
        }
    }

    Json sequence(bool group, const std::string &base) {
        Json events = make_array<Json>();
        for (;;) {
            space();
            if (group && take(')')) break;
            if (source_.empty()) {
                if (group) reject(base, "closing parenthesis");
                break;
            }
            auto at = indexed(base, events.size());
            Json e = make_object<Json>();
            if (take('|')) {
                e["bar"] = true;
            } else if (source_.front() >= '0' && source_.front() <= '9') {
                if (group) reject(at, "a note, chord or rest in this tuplet");
                auto actual = word(at);
                expect(':', at);
                auto normal = word(at);
                expect('(', at);
                e["tuplet"] = std::string(actual) + ":" + std::string(normal);
                e["events"] = sequence(true, at + ".events");
            } else {
                auto before = graces(false, at);
                e = chord(at);
                auto after = graces(true, at);
                if (!before.empty()) e["grace_before"] = std::move(before);
                if (!after.empty()) e["grace_after"] = std::move(after);
            }
            json_push(events, std::move(e));
        }
        return events;
    }
};

PhraseLane lane(const Json &j, const std::string &path, const Outline &outline, PhraseSyntax syntax) {
    const bool dsl = syntax == PhraseSyntax::DSL;
    fields(j, path, {"measure", "offset", "staff", "voice", "pitch_mode", dsl ? "content" : "events"});
    auto measure = static_cast<usize>(number(required(j, "measure", path), path + ".measure", 1, outline.measure_count) - 1);
    PhraseLane result;
    result.start.measure = outline.measures.at(measure).id;
    if (j.contains("offset")) result.start.offset = ratio(string(j.at("offset"), path + ".offset"), '/', path + ".offset");
    if (j.contains("staff")) result.start.staff = number(j.at("staff"), path + ".staff", 1, INT32_MAX) - 1;
    if (j.contains("voice")) result.start.voice = number(j.at("voice"), path + ".voice", 1, 4) - 1;
    if (j.contains("pitch_mode")) {
        auto mode = string(j.at("pitch_mode"), path + ".pitch_mode");
        if (mode == "written") {
            result.domain = PitchDomain::Written;
        } else if (mode != "concert") {
            reject(path + ".pitch_mode", "concert or written");
        }
    }
    const auto base = path + ".events";
    Json parsed;
    if (dsl) parsed = Dsl(string(required(j, "content", path), path + ".content"), base).parse();
    const auto &events = items(dsl ? parsed : required(j, "events", path), base);
    auto cursor = result.start.offset;
    if (cursor >= outline.measures[measure].duration) reject(path + ".offset", "an offset inside the starting bar");
    for (usize i = 0; i < events.size(); ++i) {
        auto at = indexed(base, i);
        const auto &e = events[i];
        if (e.contains("bar")) {
            fields(e, at, {"bar"});
            if (!e.at("bar").is_boolean() || !e.at("bar").get<bool>() || cursor.num) {
                reject(at, "a checked barline at an exact measure boundary");
            }
            continue;
        }
        auto advance = [&](Fraction duration, Fraction scale) {
            auto delta = reduced(duration.num * scale.num, duration.den * scale.den, at);
            cursor = add(cursor, delta, at);
            while (measure < outline.measures.size() && cursor >= outline.measures[measure].duration) {
                auto bar = outline.measures[measure++].duration;
                cursor = add(cursor, {-bar.num, bar.den}, at);
            }
        };
        if (e.contains("tuplet")) {
            fields(e, at, {"tuplet", "events"});
            auto r = ratio(string(e.at("tuplet"), at + ".tuplet"), ':', at + ".tuplet");
            if (!r.num) reject(at + ".tuplet", "positive tuplet counts");
            TupletGroup g{static_cast<int>(r.num), static_cast<int>(r.den), {}};
            const auto &members = items(required(e, "events", at), at + ".events");
            for (usize k = 0; k < members.size(); ++k) {
                g.members.push_back(event(members[k], indexed(at + ".events", k)));
                advance(g.members.back().written_duration, {r.den, r.num});
            }
            result.items.emplace_back(std::move(g));
        } else {
            auto member = event(e, at);
            advance(member.written_duration, {1, 1});
            result.items.emplace_back(std::move(member));
        }
    }
    return result;
}
} // namespace

Result<engrave::Phrase> decode_phrase(const Json &lanes, const engrave::Outline &outline, engrave::ScoreSel score, PhraseSyntax syntax) {
    try {
        engrave::Phrase phrase{score, {}};
        const auto &array = items(lanes, "lanes");
        if (array.empty()) reject("lanes", "at least one lane");
        for (usize i = 0; i < array.size(); ++i) phrase.lanes.push_back(lane(array[i], indexed("lanes", i), outline, syntax));
        return phrase;
    } catch (const InputError &e) {
        return std::unexpected(HostError{"InvalidArgument", e.what()});
    }
}
} // namespace music::api
