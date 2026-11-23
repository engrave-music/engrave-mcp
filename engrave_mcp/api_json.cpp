#include "api_json.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>

#include "api_json_internal.h"

namespace music::api {

namespace {
using detail::invalid_error;

Json fraction_json(engrave::Fraction f, bool meter = false) {
    if (!meter) {
        if (!f.num) return Json("0");
        auto d = std::gcd(f.num, f.den);
        f.num /= d;
        f.den /= d;
    }
    return std::to_string(f.num) + "/" + std::to_string(f.den);
}

Json eid_json(const std::optional<engrave::ElementId> &id) { return id ? Json(engrave::to_string(*id)) : Json(nullptr); }

Json score_json(const engrave::ScoreSel &s) { return s.excerpt ? Json{{"excerpt", engrave::to_string(*s.excerpt)}} : make_object<Json>(); }

template <class T, class F>
Json mapped(const std::vector<T> &items, F f) {
    Json out = make_array<Json>();
    for (const auto &x : items) json_push(out, f(x));
    return out;
}

std::vector<std::string> strings(const Json &array) {
    std::vector<std::string> result;
    for (const auto &item : array.get_array()) result.push_back(item.get<std::string>());
    return result;
}

Json strings_json(const std::vector<std::string> &values) {
    Json result = make_array<Json>();
    for (const auto &value : values) json_push(result, value);
    return result;
}

std::vector<i32> ints(const Json &array) {
    std::vector<i32> result;
    for (const auto &item : array.get_array()) result.push_back(item.as<i32>());
    return result;
}

Json ids(const std::vector<engrave::ElementId> &items) {
    return mapped(items, [](const auto &x) { return Json(engrave::to_string(x)); });
}

Json mix(const engrave::AudioMix &m) { return {{"volume_db", m.volume_db}, {"balance", m.balance}, {"mute", m.mute}, {"solo", m.solo}}; }

const char *kind(engrave::ValueKind k) {
    switch (k) {
        case engrave::ValueKind::BOOL: return "bool";
        case engrave::ValueKind::INT: return "int";
        case engrave::ValueKind::REAL: return "real";
        case engrave::ValueKind::STRING: return "string";
        case engrave::ValueKind::POINT: return "point";
        case engrave::ValueKind::SIZE: return "size";
        case engrave::ValueKind::COLOR: return "color";
        case engrave::ValueKind::FRACTION: return "fraction";
        case engrave::ValueKind::ENUM: return "enum";
        case engrave::ValueKind::INT_LIST: return "int_list";
        case engrave::ValueKind::OPAQUE: return "opaque";
    }
    assert(false);
    std::unreachable();
}

const char *unit(engrave::Unit u) {
    switch (u) {
        case engrave::Unit::NONE: return "none";
        case engrave::Unit::SPATIUM: return "spatium";
        case engrave::Unit::ABSOLUTE: return "absolute";
        case engrave::Unit::MILLIMETRE: return "millimetre";
        case engrave::Unit::DEGREE: return "degree";
    }
    assert(false);
    std::unreachable();
}
} // namespace

namespace detail {

std::string_view grace_kind_name(engrave::GraceKind kind) {
    using enum engrave::GraceKind;
    switch (kind) {
        case Appoggiatura: return "appog";
        case Acciaccatura: return "acciac";
        case Grace4: return "grace4";
        case Grace8After: return "after8";
        case Grace16: return "grace16";
        case Grace16After: return "after16";
        case Grace32: return "grace32";
        case Grace32After: return "after32";
    }
    assert(false);
    std::unreachable();
}

std::optional<engrave::GraceKind> grace_kind(std::string_view name) {
    using enum engrave::GraceKind;
    for (const auto kind : {Appoggiatura, Acciaccatura, Grace4, Grace8After, Grace16, Grace16After, Grace32, Grace32After}) {
        if (name == grace_kind_name(kind)) return kind;
    }
    return std::nullopt;
}

Result<void> validate_xml_text(std::string_view value) {
    for (unsigned char c : value) {
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
            return std::unexpected(invalid_error("text contains a character forbidden by XML 1.0"));
        }
    }
    if (value.find("\xef\xbf\xbe") != std::string_view::npos || value.find("\xef\xbf\xbf") != std::string_view::npos) {
        return std::unexpected(invalid_error("text contains a character forbidden by XML 1.0"));
    }
    return {};
}

Result<i64> signed_decimal(std::string_view s) {
    i64 n{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size() || std::to_string(n) != s) {
        return std::unexpected(invalid_error("integer is not a canonical int64 decimal"));
    }
    return n;
}

Result<engrave::ElementId> eid(const Json &j) {
    if (!j.is_string()) return std::unexpected(invalid_error("invalid canonical element id"));
    const auto &s = j.get<std::string>();
    auto id = engrave::element_id_from_string(s);
    if (!id || engrave::to_string(*id) != s) return std::unexpected(invalid_error("invalid canonical element id"));
    return *id;
}

Result<engrave::Fraction> fraction(const Json &j) {
    if (!j.is_string()) return std::unexpected(invalid_error("expected an exact fraction string"));
    const std::string_view s = j.get<std::string>();
    if (s == "0") return engrave::Fraction{0, 1};
    const auto slash = s.find('/');
    if (slash == s.npos) return std::unexpected(invalid_error("expected numerator/denominator"));
    auto n = signed_decimal(s.substr(0, slash));
    if (!n) return std::unexpected(std::move(n.error()));
    auto d = signed_decimal(s.substr(slash + 1));
    if (!d) return std::unexpected(std::move(d.error()));
    if (*d <= 0) return std::unexpected(invalid_error("fraction denominator must be positive"));
    return engrave::Fraction{*n, *d};
}

Result<engrave::ScoreSel> score(const Json &j) {
    if (!j.contains("excerpt")) return engrave::ScoreSel{std::nullopt};
    auto ex = eid(j.at("excerpt"));
    if (!ex) return std::unexpected(std::move(ex.error()));
    return engrave::ScoreSel{*ex};
}

Result<engrave::Address> address(const Json &j, const engrave::Outline *outline) {
    if (j.is_object() && !j.contains("kind")) {
        const auto number = j.at("measure").as<usize>();
        if (!outline || number == 0 || number > outline->measures.size()) {
            return std::unexpected(invalid_error("measure: out of range; expected an existing bar"));
        }
        const auto &m = outline->measures[number - 1];
        engrave::Fraction offset{};
        if (j.contains("offset")) {
            auto parsed = fraction(j.at("offset"));
            if (!parsed) return std::unexpected(std::move(parsed.error()));
            offset = *parsed;
        }
        if (offset.num < 0 || offset >= m.duration || offset.num > INT32_MAX || offset.den > INT32_MAX || m.tick.num > INT32_MAX ||
            m.tick.den > INT32_MAX) {
            return std::unexpected(invalid_error("offset: outside bar; expected a representable measure-local fraction"));
        }
        auto n = m.tick.num * offset.den + offset.num * m.tick.den;
        auto d = m.tick.den * offset.den;
        auto divisor = std::gcd(n, d);
        return engrave::SegmentAddress{
            m.id, json_value<int>(j, "staff", 1) - 1, json_value<int>(j, "voice", 1) - 1, {n / divisor, d / divisor}, "ChordRest"};
    }
    const auto &k = j.at("kind").get<std::string>();
    if (k == "segment") {
        auto measure = eid(j.at("measure"));
        if (!measure) return std::unexpected(std::move(measure.error()));
        auto tick = fraction(j.at("tick"));
        if (!tick) return std::unexpected(std::move(tick.error()));
        return engrave::SegmentAddress{*measure, j.at("staff").as<i32>() - 1, j.at("voice").as<i32>() - 1, *tick,
                                       j.at("segment_kind").get<std::string>()};
    }
    if (k == "page") {
        auto sel = score(j);
        if (!sel) return std::unexpected(std::move(sel.error()));
        return engrave::PageAddress{*sel, j.at("index").as<i32>()};
    }
    if (k == "system") {
        auto sel = score(j);
        if (!sel) return std::unexpected(std::move(sel.error()));
        return engrave::SystemAddress{*sel, j.at("page").as<i32>(), j.at("index").as<i32>()};
    }
    if (k == "spanner_segment") {
        auto spanner = eid(j.at("spanner"));
        if (!spanner) return std::unexpected(std::move(spanner.error()));
        return engrave::SpannerSegmentAddress{*spanner, j.at("index").as<i32>()};
    }
    return std::unexpected(invalid_error("unknown address kind"));
}

Result<engrave::Ref> ref(const Json &j, bool allow_op, usize before) {
    if (j.is_string()) {
        auto id = eid(j);
        if (!id) return std::unexpected(std::move(id.error()));
        return *id;
    }
    if (j.is_object() && j.contains("kind")) {
        auto addr = address(j);
        if (!addr) return std::unexpected(std::move(addr.error()));
        return *addr;
    }
    if (j.is_object() && j.contains("from_op") && allow_op) {
        const auto op = j.at("from_op").as<usize>();
        if (op >= before) return std::unexpected(invalid_error("OpRef must refer to an earlier operation"));
        return engrave::OpRef{op, j.at("item").as<usize>()};
    }
    return std::unexpected(invalid_error("OpRef is only valid in an edit batch"));
}

Result<engrave::Value> value(const Json &j, engrave::ValueKind type, const std::string &path) {
    using namespace engrave;
    auto integral = [](const Json &n) { return n.holds<i64>() || (n.holds<u64>() && n.get<u64>() <= INT64_MAX); };
    auto shape = [&](std::initializer_list<const char *> keys) {
        return j.is_object() && j.size() == keys.size() &&
               std::ranges::all_of(keys, [&](auto key) { return j.contains(key) && j.at(key).is_number(); });
    };
    if (j.is_null()) return Value{std::monostate{}};
    switch (type) {
        case ValueKind::BOOL:
            if (j.is_boolean()) return Value{j.get<bool>()};
            break;
        case ValueKind::INT:
            if (integral(j)) return Value{j.as<i64>()};
            break;
        case ValueKind::REAL:
            if (j.is_number()) return Value{j.as<f64>()};
            break;
        case ValueKind::STRING:
        case ValueKind::ENUM:
            if (j.is_string()) {
                const auto &s = j.get<std::string>();
                if (!validate_xml_text(s)) break;
                return type == ValueKind::ENUM ? Value{Enum{s}} : Value{s};
            }
            break;
        case ValueKind::FRACTION:
            if (auto f = fraction(j)) return Value{*f};
            break;
        case ValueKind::POINT:
            if (shape({"x", "y"})) return Value{Point{j.at("x").as<f64>(), j.at("y").as<f64>()}};
            break;
        case ValueKind::SIZE:
            if (shape({"w", "h"})) return Value{Size{j.at("w").as<f64>(), j.at("h").as<f64>()}};
            break;
        case ValueKind::COLOR:
            if (shape({"r", "g", "b", "a"}) && std::ranges::all_of(j.get_object(), [&](const auto &pair) {
                    return integral(pair.second) && pair.second.template as<i64>() >= 0 && pair.second.template as<i64>() <= 255;
                })) {
                return Value{Color{j.at("r").as<u8>(), j.at("g").as<u8>(), j.at("b").as<u8>(), j.at("a").as<u8>()}};
            }
            break;
        case ValueKind::INT_LIST:
            if (j.is_array() && std::ranges::all_of(j.get_array(), integral)) {
                std::vector<i64> ns;
                for (const auto &n : j.get_array()) ns.push_back(n.as<i64>());
                return Value{std::move(ns)};
            }
            break;
        case ValueKind::OPAQUE: break;
    }
    return std::unexpected(invalid_error(path + ": incompatible value; expected " + kind(type)));
}

Result<std::filesystem::path> path(const Json &j) {
    const auto &s = j.get<std::string>();
    if (s.find('\0') != std::string::npos) return std::unexpected(invalid_error("path contains a NUL byte"));
    return std::filesystem::u8path(s);
}

std::string path_text(const std::filesystem::path &p) {
    auto s = p.generic_u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}

} // namespace detail

Result<u64> decode_revision(const Json &j) {
    if (!j.is_string()) return std::unexpected(invalid_error("revision must be a decimal string"));
    const auto &s = j.get<std::string>();
    u64 n{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size() || std::to_string(n) != s) {
        return std::unexpected(invalid_error("revision is not a canonical uint64 decimal"));
    }
    return n;
}

Result<engrave::ScoreSel> decode_score(const Json &j) { return detail::score(j); }

Result<engrave::Ref> decode_ref(const Json &j) { return detail::ref(j, false, std::numeric_limits<usize>::max()); }

Result<engrave::Query> decode_query(const Json &j) {
    if (auto ok = detail::validate_tool("score_query", Json{{"document", "doc-A-1"}, {"mode", "elements"}, {"query", j}}); !ok) {
        return std::unexpected(std::move(ok.error()));
    }
    engrave::Query q;
    if (j.contains("excerpt")) {
        auto sel = detail::score(j);
        if (!sel) return std::unexpected(std::move(sel.error()));
        q.score = *sel;
    }
    if (j.contains("types")) q.types = strings(j.at("types"));
    if (j.contains("first_measure")) q.first_measure = j.at("first_measure").as<i32>() - 1;
    if (j.contains("measure_count")) q.measure_count = j.at("measure_count").as<i32>();
    if (j.contains("from_measure")) {
        auto id = detail::eid(j.at("from_measure"));
        if (!id) return std::unexpected(std::move(id.error()));
        q.from_measure = *id;
    }
    if (j.contains("to_measure")) {
        auto id = detail::eid(j.at("to_measure"));
        if (!id) return std::unexpected(std::move(id.error()));
        q.to_measure = *id;
    }
    if (j.contains("staves")) {
        q.staves = ints(j.at("staves"));
        for (auto &n : q.staves) --n;
    }
    if (j.contains("voices")) {
        q.voices = ints(j.at("voices"));
        for (auto &n : q.voices) --n;
    }
    if (j.contains("from_tick")) {
        auto tick = detail::fraction(j.at("from_tick"));
        if (!tick) return std::unexpected(std::move(tick.error()));
        q.from_tick = *tick;
    }
    if (j.contains("to_tick")) {
        auto tick = detail::fraction(j.at("to_tick"));
        if (!tick) return std::unexpected(std::move(tick.error()));
        q.to_tick = *tick;
    }
    if (j.contains("props")) q.props = strings(j.at("props"));
    if (j.contains("layout_boxes")) q.layout_boxes = j.at("layout_boxes").get<bool>();
    if (j.contains("limit")) q.limit = j.at("limit").as<usize>();
    if (j.contains("cursor")) q.cursor = j.at("cursor").as<usize>();
    return q;
}

Json encode(const engrave::Value &v) {
    return std::visit(
        [](const auto &x) -> Json {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return nullptr;
            } else if constexpr (std::is_same_v<T, bool>) {
                return Json(x);
            } else if constexpr (std::is_same_v<T, i64>) {
                return Json(x);
            } else if constexpr (std::is_same_v<T, f64>) {
                return Json(x);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return Json(x);
            } else if constexpr (std::is_same_v<T, engrave::Fraction>) {
                return fraction_json(x);
            } else if constexpr (std::is_same_v<T, engrave::Point>) {
                return {{"x", x.x}, {"y", x.y}};
            } else if constexpr (std::is_same_v<T, engrave::Size>) {
                return {{"w", x.w}, {"h", x.h}};
            } else if constexpr (std::is_same_v<T, engrave::Color>) {
                return {{"r", x.r}, {"g", x.g}, {"b", x.b}, {"a", x.a}};
            } else if constexpr (std::is_same_v<T, engrave::Enum>) {
                return Json(x.token);
            } else if constexpr (std::is_same_v<T, std::vector<i64>>) {
                return mapped(x, [](auto n) { return Json(n); });
            } else if constexpr (std::is_same_v<T, engrave::Opaque>) {
                return {{"kind", "opaque"}, {"type", x.type}};
            }
        },
        v);
}

Json encode(const engrave::PropertyRead &p) {
    return {{"name", p.name}, {"value", encode(p.value)}, {"is_default", p.is_default}, {"styled", p.styled}};
}

Json encode(const engrave::Diagnostic &d) {
    return {{"severity", d.severity == engrave::Severity::INFO ? "info" : "warning"}, {"message", d.message}};
}

Json encode(const engrave::Outline &o) {
    auto parts = mapped(o.parts, [](const auto &p) {
        return Json{
            {"id", engrave::to_string(p.id)},
            {"name", p.name},
            {"instrument_id", p.instrument_id},
            {"staves", mapped(p.staves, [](const auto &s) {
                 return Json{
                     {"id", engrave::to_string(s.id)}, {"staff", s.index + 1}, {"part", s.part}, {"group", s.group}, {"lines", s.lines},
                     {"invisible", s.invisible}};
             })}};
    });
    auto measures = mapped(o.measures, [](const auto &m) {
        return Json{{"id", engrave::to_string(m.id)},
                    {"measure", m.index + 1},
                    {"tick", fraction_json(m.tick)},
                    {"duration", fraction_json(m.duration)}};
    });
    auto sigs = mapped(o.signatures, [](const auto &s) {
        return Json{{"type", s.type},       {"tick", fraction_json(s.tick)},
                    {"staff", s.staff + 1}, {"timesig", s.timesig ? fraction_json(*s.timesig, true) : Json(nullptr)},
                    {"key", s.key},         {"bpm", s.bpm},
                    {"text", s.text}};
    });
    auto excerpts = mapped(o.excerpts, [](const auto &e) {
        return Json{{"id", engrave::to_string(e.id)}, {"name", e.name}, {"pages", e.pages ? Json(*e.pages) : Json(nullptr)}};
    });
    return {{"title", o.title},
            {"subtitle", o.subtitle},
            {"composer", o.composer},
            {"poet", o.poet},
            {"timesig", o.timesig},
            {"mscore_version", o.mscore_version},
            {"file_version", o.file_version},
            {"pages", o.pages ? Json(*o.pages) : Json(nullptr)},
            {"measure_count", o.measure_count},
            {"key", o.key},
            {"duration", o.duration},
            {"parts", parts},
            {"measures", measures},
            {"signatures", sigs},
            {"excerpts", excerpts}};
}

Json encode(const engrave::QueryResult &q) {
    auto elements = mapped(q.elements, [](const auto &e) {
        Json box = nullptr;
        if (e.box) box = Json{{"page", e.box->page}, {"x", e.box->x}, {"y", e.box->y}, {"width", e.box->width}, {"height", e.box->height}};
        return Json{{"id", engrave::to_string(e.id)},
                    {"type", e.type},
                    {"score", score_json(e.score)},
                    {"measure", eid_json(e.measure)},
                    {"measure_number", e.measure_index + 1},
                    {"staff", e.staff + 1},
                    {"voice", e.voice + 1},
                    {"tick", fraction_json(e.tick)},
                    {"duration", fraction_json(e.duration)},
                    {"parent", eid_json(e.parent)},
                    {"links", ids(e.links)},
                    {"box", box},
                    {"props", mapped(e.props, [](const auto &p) { return encode(p); })}};
    });
    return {{"elements", elements}, {"total", q.total}, {"next_cursor", q.next_cursor ? Json(*q.next_cursor) : Json(nullptr)}};
}

Json encode(const engrave::EditResult &r) {
    return {
        {"revision", std::to_string(r.revision)},
        {"applied", r.applied},
        {"ops",
         mapped(r.ops, [](const auto &o) { return Json{{"created", mapped(o.created, [](const auto &id) { return eid_json(id); })}}; })},
        {"created", ids(r.created)},
        {"removed", ids(r.removed)},
        {"replaced", ids(r.replaced)},
        {"diagnostics", mapped(r.diagnostics, [](const auto &d) { return encode(d); })}};
}

Json encode(const engrave::AudioTracks &a) {
    return {{"tracks", mapped(a.tracks,
                              [](const auto &t) {
                                  return Json{{"track", t.track},
                                              {"part_id", t.part_id},
                                              {"instrument_id", t.instrument_id},
                                              {"part_name", t.part_name},
                                              {"instrument_name", t.instrument_name},
                                              {"chord_symbols", t.chord_symbols},
                                              {"mix", mix(t.mix)}};
                              })},
            {"master", mix(a.master)}};
}

Json encode(const engrave::TypeInfo &t) {
    return {{"type", t.type}, {"props", mapped(t.props, [](const auto &p) {
                                   return Json{{"name", p.name},         {"kind", kind(p.kind)},
                                               {"unit", unit(p.unit)},   {"enum_tokens", strings_json(p.enum_tokens)},
                                               {"settable", p.settable}, {"link", p.link},
                                               {"group", p.group}};
                               })}};
}

Json encode(const engrave::StyleInfo &s) {
    return {{"name", s.name},
            {"kind", kind(s.kind)},
            {"unit", unit(s.unit)},
            {"enum_tokens", strings_json(s.enum_tokens)},
            {"default_value", encode(s.default_value)}};
}

Json encode(const engrave::TemplateInfo &t) {
    return {{"id", t.id}, {"title", t.title}, {"category", t.category}, {"path", detail::path_text(t.path)}};
}

Json encode(const engrave::Capabilities &c) {
    auto format = [](engrave::Format f) -> const char * {
        switch (f) {
            case engrave::Format::MSCZ: return "mscz";
            case engrave::Format::MSCX: return "mscx";
            case engrave::Format::MUSIC_XML: return "musicxml";
            case engrave::Format::MXL: return "mxl";
            case engrave::Format::MIDI: return "midi";
            case engrave::Format::GUITAR_PRO: return "guitarpro";
            case engrave::Format::MNX: return "mnx";
        }
        assert(false);
        std::unreachable();
    };
    auto render = [](engrave::RenderFormat f) -> const char * {
        switch (f) {
            case engrave::RenderFormat::PDF: return "pdf";
            case engrave::RenderFormat::PNG: return "png";
            case engrave::RenderFormat::SVG: return "svg";
        }
        assert(false);
        std::unreachable();
    };
    auto audio = [](engrave::AudioFormat f) -> const char * {
        switch (f) {
            case engrave::AudioFormat::WAV: return "wav";
            case engrave::AudioFormat::FLAC: return "flac";
            case engrave::AudioFormat::OPUS: return "opus";
            case engrave::AudioFormat::MP3: return "mp3";
        }
        assert(false);
        std::unreachable();
    };
    return {{"engine_version", c.engine_version},
            {"import_formats", mapped(c.import_formats, [&](auto f) { return Json(format(f)); })},
            {"export_formats", mapped(c.export_formats, [&](auto f) { return Json(format(f)); })},
            {"render_formats", mapped(c.render_formats, [&](auto f) { return Json(render(f)); })},
            {"audio_formats", mapped(c.audio_formats, [&](auto f) { return Json(audio(f)); })},
            {"soundfonts", mapped(c.soundfonts, [](const auto &p) { return Json(detail::path_text(p)); })},
            {"audio_sample_rate", c.audio_sample_rate}};
}

Json encode(const std::vector<engrave::ExportWarning> &warnings) {
    Json result = make_array<Json>();
    for (const auto &warning : warnings) {
        Json item = {{"code", warning.code}, {"message", warning.message}};
        if (warning.object) item["object"] = Json{{"kind", warning.object->kind}, {"id", warning.object->id}};
        json_push(result, std::move(item));
    }
    return result;
}

HostError api_error(const engrave::Error &e) {
    std::string message = e.message;
    if (e.poisoned && e.code != engrave::ErrorCode::POISONED) {
        if (!message.empty()) message += "; ";
        message += "document is poisoned: restore_checkpoint or reopen the file";
    }
    HostError result{std::string(engrave::to_string(e.code)), std::move(message), e.op_index};
    result.export_warnings = e.export_warnings;
    if (e.poisoned) result.poisoned = true;
    return result;
}

Json error_envelope(const HostError &e) {
    Json err = {{"code", e.code}, {"message", e.message}};
    if (e.op_index) err["op_index"] = *e.op_index;
    if (e.applied) err["applied"] = static_cast<u64>(*e.applied);
    if (e.poisoned) err["poisoned"] = *e.poisoned;
    if (e.document) err["document"] = *e.document;
    if (e.current_revision) err["current_revision"] = std::to_string(*e.current_revision);
    if (e.revision) err["revision"] = std::to_string(*e.revision);
    if (e.created) err["created"] = ids(*e.created);
    if (e.removed) err["removed"] = ids(*e.removed);
    if (e.replaced) err["replaced"] = ids(*e.replaced);
    if (!e.diagnostics.empty()) err["diagnostics"] = mapped(e.diagnostics, [](const auto &d) { return encode(d); });
    if (!e.published_artifacts.empty()) err["published_artifacts"] = e.published_artifacts;
    if (!e.export_warnings.empty()) err["export_warnings"] = encode(e.export_warnings);
    return {{"ok", false}, {"error", err}};
}

} // namespace music::api
