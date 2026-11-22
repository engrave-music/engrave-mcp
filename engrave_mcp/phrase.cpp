#include "phrase.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "api_json_internal.h"
#include "types.h"
#include "utf8_text.h"

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

string_view string(const Json &j, const std::string &path, usize limit = 4096) {
    if (!j.is_string()) reject(path, "a string");
    const auto &value = j.get<std::string>();
    if (utf8_exceeds_length(value, limit)) reject(path, "at most " + std::to_string(limit) + " Unicode characters");
    return value;
}

int number(const Json &j, const std::string &path, int low, int high) {
    if ((!j.holds<u64>() && !j.holds<i64>()) || j.as<double>() < low || j.as<double>() > high) reject(path, "an integer in range");
    return j.as<int>();
}

std::optional<int> decimal(string_view token) {
    if (token.empty() || (token.starts_with('0') && token.size() > 1) || token.starts_with("-0")) return std::nullopt;
    int value{};
    const auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc{} || end != token.data() + token.size()) return std::nullopt;
    return value;
}

std::optional<Fraction> fraction(string_view token) {
    const auto slash = token.find('/');
    if (slash == token.npos) return std::nullopt;
    const auto numerator = decimal(token.substr(0, slash));
    const auto denominator = decimal(token.substr(slash + 1));
    if (!numerator || !denominator || *numerator < 0 || *denominator <= 0) return std::nullopt;
    return Fraction{*numerator, *denominator};
}

int grace_denominator(GraceKind kind) {
    switch (kind) {
        case GraceKind::Grace4: return 4;
        case GraceKind::Grace16:
        case GraceKind::Grace16After: return 16;
        case GraceKind::Grace32:
        case GraceKind::Grace32After: return 32;
        default: return 8;
    }
}

class Dsl {
public:
    Dsl(string_view source, std::string path) : source_(source), path_(std::move(path)) {}

    std::vector<PhraseItem> parse() {
        std::vector<PhraseItem> items;
        for (auto token = peek(); !token.empty(); token = peek()) {
            item_index_ = items.size();
            member_index_.reset();
            if (take('|')) {
                items.emplace_back(BarCheck{});
            } else if (is_tuplet_count(token)) {
                items.emplace_back(tuplet());
            } else {
                items.emplace_back(event());
            }
        }
        return items;
    }

private:
    static constexpr string_view whitespace = " \t\r\n";
    static constexpr string_view punctuation = "[]():~|";

    string_view source_;
    std::string path_;
    usize item_index_ = 0;
    std::optional<usize> member_index_;

    static std::string escape_token(string_view token) {
        const auto clipped = utf8_abbrev(token, 32);
        std::string out;
        out.reserve(clipped.size());
        for (unsigned char c : clipped) {
            if (c == '\\') {
                out += "\\\\";
            } else if (c == '\'') {
                out += "\\'";
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else if (c == '\t') {
                out += "\\t";
            } else if (c < 0x20 || c == 0x7f) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    [[noreturn]] void fail(string_view token, string_view reason, string_view expected) const {
        const auto where = "item " + std::to_string(item_index_) + (member_index_ ? ", member " + std::to_string(*member_index_) : "") +
                           ", " + (token.empty() ? "end of content" : "token '" + escape_token(token) + "'");
        throw InputError(path_ + " (" + where + "): " + std::string(reason) + "; expected " + std::string(expected));
    }

    string_view peek() {
        source_.remove_prefix(std::min(source_.size(), source_.find_first_not_of(whitespace)));
        if (source_.empty()) return {};
        if (punctuation.find(source_.front()) != string_view::npos) return source_.substr(0, 1);
        return source_.substr(0, source_.find_first_of(" \t\r\n[]():~|"));
    }

    string_view next() {
        const auto token = peek();
        source_.remove_prefix(token.size());
        return token;
    }

    bool take(char c) {
        const auto token = peek();
        if (token.size() != 1 || token.front() != c) return false;
        source_.remove_prefix(1);
        return true;
    }

    void expect(char c) {
        if (!take(c)) fail(peek(), "unexpected input", "'" + std::string(1, c) + "'");
    }

    static bool is_tuplet_count(string_view token) {
        return !token.empty() && (token.front() == '-' || (token.front() >= '0' && token.front() <= '9'));
    }

    int tuplet_count() {
        const auto token = next();
        const auto value = decimal(token);
        if (!value || *value <= 0) fail(token, "invalid tuplet count", "a positive canonical decimal integer");
        return *value;
    }

    EntryPitch pitch(string_view token) const {
        if (token.starts_with('d')) {
            const auto key = decimal(token.substr(1));
            if (!key || *key < 0 || *key > 127) fail(token, "invalid drum key", "a canonical decimal integer from d0 to d127");
            return DrumKey{*key};
        }
        constexpr string_view names = "CDEFGAB";
        const auto step = token.empty() ? names.npos : names.find(token.front());
        if (step == names.npos) fail(token, "invalid pitch", "a spelled pitch such as Eb4, drum d36, or rest r");
        auto spelling = token.substr(1);
        int alter = 0;
        if (spelling.starts_with('#') || spelling.starts_with('b')) {
            const char accidental = spelling.front();
            while (spelling.starts_with(accidental)) {
                alter += accidental == '#' ? 1 : -1;
                spelling.remove_prefix(1);
            }
        }
        const auto octave = decimal(spelling);
        if (!octave) fail(token, "invalid octave", "a canonical decimal integer after the pitch letter and accidentals");
        if (alter < -3 || alter > 3 || *octave < -2 || *octave > 10) {
            fail(token, "invalid pitch", "at most three sharps or flats and an octave from -2 to 10");
        }
        return SpelledPitch{static_cast<StepName>(step), alter, *octave};
    }

    Fraction duration(int default_denominator) {
        if (!take(':')) return {1, default_denominator};
        const auto token = next();
        auto name = token;
        int dots = 0;
        while (name.ends_with('.')) {
            ++dots;
            name.remove_suffix(1);
        }
        if (dots > 2) fail(token, "invalid duration", "at most two duration dots");
        constexpr string_view names[]{"w", "h", "q", "e", "s", "32", "64"};
        const auto it = std::ranges::find(names, name);
        const auto value = it == std::end(names) ? fraction(name) : Fraction{1, 1LL << (it - std::begin(names))};
        if (!value || !value->num) {
            fail(token, "invalid duration", "w, h, q, e, s, 32, 64, or a positive fraction such as 3/8 with canonical decimal terms");
        }
        const auto numerator = value->num * ((2 << dots) - 1);
        const auto denominator = value->den * (1 << dots);
        const auto divisor = std::gcd(numerator, denominator);
        if (numerator / divisor > INT32_MAX || denominator / divisor > INT32_MAX) {
            fail(token, "invalid duration", "a duration representable in engine ticks");
        }
        return {numerator / divisor, denominator / divisor};
    }

    PhraseEvent chord(int default_denominator, bool allow_tie = true) {
        PhraseEvent result;
        const bool bracket = take('[');
        do {
            const auto token = next();
            if (bracket && token.empty()) fail(token, "unclosed chord", "a chord member or ']'");
            if (token == "r") {
                if (bracket) fail(token, "invalid chord member", "a spelled pitch or drum key");
                if (take('~')) fail("~", "invalid tie", "a note or chord; rests cannot be tied");
                break;
            }
            auto member = pitch(token);
            if (!result.notes.empty() && result.notes.front().pitch.index() != member.index()) {
                fail(token, "mixed chord", "all spelled pitches or all drum keys");
            }
            const bool tied = take('~');
            if (tied && !allow_tie) fail("~", "invalid tie", "no tie on a grace note or chord");
            result.notes.push_back({member, tied});
        } while (bracket && !take(']'));

        const bool chord_tie = bracket && take('~');
        if (chord_tie && !allow_tie) fail("~", "invalid tie", "no tie on a grace note or chord");
        result.written_duration = duration(default_denominator);
        const bool trailing_tie = take('~');
        if (trailing_tie) {
            if (!allow_tie) fail("~", "invalid tie", "no tie on a grace note or chord");
            if (result.notes.empty()) fail("~", "invalid tie", "a note or chord; rests cannot be tied");
        }
        if (trailing_tie || chord_tie) {
            for (auto &note : result.notes) note.tie_to_next = true;
        }
        return result;
    }

    std::vector<PhraseGrace> graces(bool after) {
        std::vector<PhraseGrace> result;
        for (;;) {
            const auto token = peek();
            const auto kind = detail::grace_kind(token);
            if (!kind || after != token.starts_with("after")) return result;
            next();
            expect('(');
            const auto notes_token = peek();
            const auto entry = chord(grace_denominator(*kind), false);
            if (entry.notes.empty()) fail(notes_token, "invalid grace", "a note or chord");
            expect(')');
            PhraseGrace grace{*kind, entry.written_duration, {}};
            for (const auto &note : entry.notes) grace.notes.push_back(note.pitch);
            result.push_back(std::move(grace));
        }
    }

    PhraseEvent event() {
        auto before = graces(false);
        auto result = chord(4);
        result.before = std::move(before);
        result.after = graces(true);
        return result;
    }

    TupletGroup tuplet() {
        const auto actual = tuplet_count();
        expect(':');
        const auto normal = tuplet_count();
        expect('(');
        TupletGroup result{actual, normal, {}};
        if (const auto token = peek(); token == ")") fail(token, "empty tuplet", "a note, chord, or rest");
        while (!take(')')) {
            member_index_ = result.members.size();
            const auto token = peek();
            if (token.empty()) fail(token, "unclosed tuplet", "')'");
            if (is_tuplet_count(token) || token == "|") fail(token, "invalid tuplet member", "a note, chord, or rest");
            result.members.push_back(event());
        }
        member_index_.reset();
        return result;
    }
};

PhraseLane lane(const Json &j, const std::string &path, const Outline &outline) {
    fields(j, path, {"measure", "offset", "staff", "voice", "pitch_mode", "content"});
    auto measure = static_cast<usize>(number(required(j, "measure", path), path + ".measure", 1, outline.measure_count) - 1);
    PhraseLane result;
    result.start.measure = outline.measures.at(measure).id;
    if (j.contains("offset")) {
        const auto at = path + ".offset";
        const auto token = string(j.at("offset"), at);
        if (token != "0") {
            const auto value = fraction(token);
            if (!value) reject(at, "a nonnegative fraction such as 3/8 with canonical decimal terms");
            result.start.offset = *value;
        }
    }
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
    const auto content = path + ".content";
    result.items = Dsl(string(required(j, "content", path), content, 65536), content).parse();
    return result;
}
} // namespace

Result<engrave::Phrase> decode_phrase(const Json &lanes, const engrave::Outline &outline, engrave::ScoreSel score) {
    try {
        engrave::Phrase phrase{score, {}};
        const auto &array = items(lanes, "lanes");
        if (array.empty()) reject("lanes", "at least one lane");
        for (usize i = 0; i < array.size(); ++i) phrase.lanes.push_back(lane(array[i], indexed("lanes", i), outline));
        return phrase;
    } catch (const InputError &e) {
        return std::unexpected(HostError{"InvalidArgument", e.what()});
    }
}
} // namespace music::api
