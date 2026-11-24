#include "phrase.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <functional>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "api_json_internal.h"
#include "types.h"
#include "utf8_text.h"

namespace music::api {
namespace {
using namespace engrave;
using std::string_view;

std::unexpected<HostError> reject(const std::string &path, string_view expected) {
    auto message = path + ": invalid phrase input; expected " + std::string(expected);
    // Unknown field names may contain NUL. Preserve the original diagnostic prefix.
    message.erase(std::ranges::find(message, '\0'), message.end());
    return std::unexpected(detail::invalid_error(std::move(message)));
}

std::string indexed(const std::string &path, usize i) { return path + "[" + std::to_string(i) + "]"; }

Result<void> fields(const Json &j, const std::string &path, std::initializer_list<string_view> keys) {
    if (!j.is_object()) return reject(path, "an object");
    for (const auto &[key, value] : j.get_object()) {
        if (std::ranges::find(keys, key) == keys.end()) return reject(path + "." + key, "a supported field");
    }
    return {};
}

Result<std::reference_wrapper<const Json>> required(const Json &j, const char *key, const std::string &path) {
    if (!j.contains(key)) return reject(path + "." + key, "a required field");
    return std::cref(j.at(key));
}

Result<std::span<const Json>> items(const Json &j, const std::string &path) {
    if (!j.is_array()) return reject(path, "an array");
    return std::span<const Json>{j.get_array()};
}

Result<string_view> string(const Json &j, const std::string &path, usize limit = 4096) {
    if (!j.is_string()) return reject(path, "a string");
    const auto &value = j.get<std::string>();
    if (utf8_exceeds_length(value, limit)) return reject(path, "at most " + std::to_string(limit) + " Unicode characters");
    return value;
}

Result<int> number(const Json &j, const std::string &path, int low, int high) {
    if ((!j.holds<u64>() && !j.holds<i64>()) || j.as<double>() < low || j.as<double>() > high) return reject(path, "an integer in range");
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

struct Dsl {
    Dsl(string_view input, std::string content_path) : source(input), path(std::move(content_path)) {}

    Result<std::vector<PhraseItem>> parse() {
        std::vector<PhraseItem> items;
        for (auto token = peek(); !token.empty(); token = peek()) {
            item_index = items.size();
            member_index.reset();
            if (take('|')) {
                items.emplace_back(BarCheck{});
            } else if (is_tuplet_count(token)) {
                auto group = tuplet();
                if (!group) return std::unexpected(std::move(group.error()));
                items.emplace_back(std::move(*group));
            } else {
                auto entry = event();
                if (!entry) return std::unexpected(std::move(entry.error()));
                items.emplace_back(std::move(*entry));
            }
        }
        return items;
    }

private:
    static constexpr string_view whitespace = " \t\r\n";
    static constexpr string_view punctuation = "[]():~|";

    string_view source;
    std::string path;
    usize item_index = 0;
    std::optional<usize> member_index;

    static std::string escape_token(string_view token) { return escape_text(utf8_abbrev(token, 32), '\''); }

    std::unexpected<HostError> fail(string_view token, string_view reason, string_view expected) const {
        const auto where = "item " + std::to_string(item_index) + (member_index ? ", member " + std::to_string(*member_index) : "") + ", " +
                           (token.empty() ? "end of content" : "token '" + escape_token(token) + "'");
        return std::unexpected(
            detail::invalid_error(path + " (" + where + "): " + std::string(reason) + "; expected " + std::string(expected)));
    }

    string_view peek() {
        source.remove_prefix(std::min(source.size(), source.find_first_not_of(whitespace)));
        if (source.empty()) return {};
        if (punctuation.find(source.front()) != string_view::npos) return source.substr(0, 1);
        return source.substr(0, source.find_first_of(" \t\r\n[]():~|"));
    }

    string_view next() {
        const auto token = peek();
        source.remove_prefix(token.size());
        return token;
    }

    bool take(char c) {
        const auto token = peek();
        if (token.size() != 1 || token.front() != c) return false;
        source.remove_prefix(1);
        return true;
    }

    Result<void> expect(char c) {
        if (!take(c)) return fail(peek(), "unexpected input", "'" + std::string(1, c) + "'");
        return {};
    }

    static bool is_tuplet_count(string_view token) {
        return !token.empty() && (token.front() == '-' || (token.front() >= '0' && token.front() <= '9'));
    }

    Result<int> tuplet_count() {
        const auto token = next();
        const auto value = decimal(token);
        if (!value || *value <= 0) return fail(token, "invalid tuplet count", "a positive canonical decimal integer");
        return *value;
    }

    Result<EntryPitch> pitch(string_view token) const {
        if (token.starts_with('d')) {
            const auto key = decimal(token.substr(1));
            if (!key || *key < 0 || *key > 127) return fail(token, "invalid drum key", "a canonical decimal integer from d0 to d127");
            return DrumKey{*key};
        }
        constexpr string_view names = "CDEFGAB";
        const auto step = token.empty() ? names.npos : names.find(token.front());
        if (step == names.npos) return fail(token, "invalid pitch", "a spelled pitch such as Eb4, drum d36, or rest r");
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
        if (!octave) return fail(token, "invalid octave", "a canonical decimal integer after the pitch letter and accidentals");
        if (alter < -3 || alter > 3 || *octave < -2 || *octave > 10) {
            return fail(token, "invalid pitch", "at most three sharps or flats and an octave from -2 to 10");
        }
        return SpelledPitch{static_cast<StepName>(step), alter, *octave};
    }

    Result<Fraction> duration(int default_denominator) {
        if (!take(':')) return Fraction{1, default_denominator};
        const auto token = next();
        auto name = token;
        int dots = 0;
        while (name.ends_with('.')) {
            ++dots;
            name.remove_suffix(1);
        }
        if (dots > 2) return fail(token, "invalid duration", "at most two duration dots");
        constexpr string_view names[]{"w", "h", "q", "e", "s", "32", "64"};
        const auto it = std::ranges::find(names, name);
        const auto value = it == std::end(names) ? fraction(name) : Fraction{1, 1LL << (it - std::begin(names))};
        if (!value || !value->num) {
            return fail(token, "invalid duration",
                        "w, h, q, e, s, 32, 64, or a positive fraction such as 3/8 with canonical decimal terms");
        }
        const auto numerator = value->num * ((2 << dots) - 1);
        const auto denominator = value->den * (1 << dots);
        const auto divisor = std::gcd(numerator, denominator);
        if (numerator / divisor > INT32_MAX || denominator / divisor > INT32_MAX) {
            return fail(token, "invalid duration", "a duration representable in engine ticks");
        }
        return Fraction{numerator / divisor, denominator / divisor};
    }

    Result<PhraseEvent> chord(int default_denominator, bool allow_tie = true) {
        PhraseEvent result;
        const bool bracket = take('[');
        do {
            const auto token = next();
            if (bracket && token.empty()) return fail(token, "unclosed chord", "a chord member or ']'");
            if (token == "r") {
                if (bracket) return fail(token, "invalid chord member", "a spelled pitch or drum key");
                if (take('~')) return fail("~", "invalid tie", "a note or chord; rests cannot be tied");
                break;
            }
            auto member = pitch(token);
            if (!member) return std::unexpected(std::move(member.error()));
            if (!result.notes.empty() && result.notes.front().pitch.index() != member->index()) {
                return fail(token, "mixed chord", "all spelled pitches or all drum keys");
            }
            const bool tied = take('~');
            if (tied && !allow_tie) return fail("~", "invalid tie", "no tie on a grace note or chord");
            result.notes.push_back({*member, tied});
        } while (bracket && !take(']'));

        const bool chord_tie = bracket && take('~');
        if (chord_tie && !allow_tie) return fail("~", "invalid tie", "no tie on a grace note or chord");
        auto written_duration = duration(default_denominator);
        if (!written_duration) return std::unexpected(std::move(written_duration.error()));
        result.written_duration = *written_duration;
        const bool trailing_tie = take('~');
        if (trailing_tie) {
            if (!allow_tie) return fail("~", "invalid tie", "no tie on a grace note or chord");
            if (result.notes.empty()) return fail("~", "invalid tie", "a note or chord; rests cannot be tied");
        }
        if (trailing_tie || chord_tie) {
            for (auto &note : result.notes) note.tie_to_next = true;
        }
        return result;
    }

    Result<std::vector<PhraseGrace>> graces(bool after) {
        std::vector<PhraseGrace> result;
        for (;;) {
            const auto token = peek();
            const auto kind = detail::grace_kind(token);
            if (!kind || after != token.starts_with("after")) return result;
            next();
            if (auto open = expect('('); !open) return std::unexpected(std::move(open.error()));
            const auto notes_token = peek();
            auto entry = chord(grace_denominator(*kind), false);
            if (!entry) return std::unexpected(std::move(entry.error()));
            if (entry->notes.empty()) return fail(notes_token, "invalid grace", "a note or chord");
            if (auto close = expect(')'); !close) return std::unexpected(std::move(close.error()));
            PhraseGrace grace{*kind, entry->written_duration, {}};
            for (const auto &note : entry->notes) grace.notes.push_back(note.pitch);
            result.push_back(std::move(grace));
        }
    }

    Result<PhraseEvent> event() {
        auto before = graces(false);
        if (!before) return std::unexpected(std::move(before.error()));
        auto result = chord(4);
        if (!result) return result;
        result->before = std::move(*before);
        auto after = graces(true);
        if (!after) return std::unexpected(std::move(after.error()));
        result->after = std::move(*after);
        return result;
    }

    Result<TupletGroup> tuplet() {
        auto actual = tuplet_count();
        if (!actual) return std::unexpected(std::move(actual.error()));
        if (auto separator = expect(':'); !separator) return std::unexpected(std::move(separator.error()));
        auto normal = tuplet_count();
        if (!normal) return std::unexpected(std::move(normal.error()));
        if (auto open = expect('('); !open) return std::unexpected(std::move(open.error()));
        TupletGroup result{*actual, *normal, {}};
        if (const auto token = peek(); token == ")") return fail(token, "empty tuplet", "a note, chord, or rest");
        while (!take(')')) {
            member_index = result.members.size();
            const auto token = peek();
            if (token.empty()) return fail(token, "unclosed tuplet", "')'");
            if (is_tuplet_count(token) || token == "|") return fail(token, "invalid tuplet member", "a note, chord, or rest");
            auto entry = event();
            if (!entry) return std::unexpected(std::move(entry.error()));
            result.members.push_back(std::move(*entry));
        }
        member_index.reset();
        return result;
    }
};

Result<PhraseLane> lane(const Json &j, const std::string &path, const Outline &outline) {
    if (auto checked = fields(j, path, {"measure", "offset", "staff", "voice", "pitch_mode", "content"}); !checked) {
        return std::unexpected(std::move(checked.error()));
    }
    auto measure_field = required(j, "measure", path);
    if (!measure_field) return std::unexpected(std::move(measure_field.error()));
    auto measure = number(measure_field->get(), path + ".measure", 1, outline.measure_count);
    if (!measure) return std::unexpected(std::move(measure.error()));
    PhraseLane result;
    result.start.measure = outline.measures.at(static_cast<usize>(*measure - 1)).id;
    if (j.contains("offset")) {
        const auto at = path + ".offset";
        auto token = string(j.at("offset"), at);
        if (!token) return std::unexpected(std::move(token.error()));
        if (*token != "0") {
            const auto value = fraction(*token);
            if (!value) return reject(at, "a nonnegative fraction such as 3/8 with canonical decimal terms");
            result.start.offset = *value;
        }
    }
    if (j.contains("staff")) {
        auto staff = number(j.at("staff"), path + ".staff", 1, INT32_MAX);
        if (!staff) return std::unexpected(std::move(staff.error()));
        result.start.staff = *staff - 1;
    }
    if (j.contains("voice")) {
        auto voice = number(j.at("voice"), path + ".voice", 1, 4);
        if (!voice) return std::unexpected(std::move(voice.error()));
        result.start.voice = *voice - 1;
    }
    if (j.contains("pitch_mode")) {
        auto mode = string(j.at("pitch_mode"), path + ".pitch_mode");
        if (!mode) return std::unexpected(std::move(mode.error()));
        if (*mode == "written") {
            result.domain = PitchDomain::Written;
        } else if (*mode != "concert") {
            return reject(path + ".pitch_mode", "concert or written");
        }
    }
    auto content_field = required(j, "content", path);
    if (!content_field) return std::unexpected(std::move(content_field.error()));
    auto content_path = path + ".content";
    auto content = string(content_field->get(), content_path, 65536);
    if (!content) return std::unexpected(std::move(content.error()));
    auto parsed = Dsl(*content, std::move(content_path)).parse();
    if (!parsed) return std::unexpected(std::move(parsed.error()));
    result.items = std::move(*parsed);
    return result;
}
} // namespace

Result<engrave::Phrase> decode_phrase(const Json &lanes, const engrave::Outline &outline, engrave::ScoreSel score) {
    engrave::Phrase phrase{score, {}};
    auto array = items(lanes, "lanes");
    if (!array) return std::unexpected(std::move(array.error()));
    if (array->empty()) return reject("lanes", "at least one lane");
    for (usize i = 0; i < array->size(); ++i) {
        auto parsed = lane((*array)[i], indexed("lanes", i), outline);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        phrase.lanes.push_back(std::move(*parsed));
    }
    return phrase;
}
} // namespace music::api
