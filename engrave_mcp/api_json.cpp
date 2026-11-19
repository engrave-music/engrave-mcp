#include "api_json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <regex>
#include <type_traits>

#include <ankerl/unordered_dense.h>

#include "api_json_internal.h"
#include "registry.h"

namespace music::api {

namespace {
using detail::invalid_error;

usize unicode_length(const std::string &s) {
    usize n = 0;
    for (unsigned char c : s) {
        if ((c & 0xc0) != 0x80) ++n;
    }
    return n;
}

bool base64_shape(std::string_view s) {
    if (s.size() % 4) return false;
    usize padding = 0;
    if (!s.empty() && s.back() == '=') ++padding;
    if (s.size() > 1 && s[s.size() - 2] == '=') ++padding;
    for (usize i = 0; i < s.size() - padding; ++i) {
        const unsigned char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')) return false;
    }
    return true;
}

// Evaluates the keywords used by S1's generated input schemas. Keeping this
// tied to the declarations avoids a second, drifting set of object shapes.
// Input-schema patterns are fixed, so compile each one once per thread rather
// than rebuilding a std::regex for every string field of every request.
const std::regex &compiled_pattern(const std::string &pattern) {
    static thread_local ankerl::unordered_dense::map<std::string, std::regex> cache;
    auto it = cache.find(pattern);
    if (it == cache.end()) {
        // Construct before inserting so a throwing pattern leaves the cache
        // unchanged rather than storing an empty regex.
        it = cache.emplace(pattern, std::regex(pattern, std::regex::ECMAScript)).first;
    }
    return it->second;
}

bool conforms(const Json &schema, const Json &instance, const Json &root) {
    if (schema.contains("$ref")) {
        const auto ref = schema.at("$ref").get<std::string>();
        if (!ref.starts_with("#/$defs/")) return false;
        return conforms(root.at("$defs").at(ref.substr(8)), instance, root);
    }
    if (schema.contains("type")) {
        const auto type = schema.at("type").get<std::string>();
        if (type == "object" && !instance.is_object()) return false;
        if (type == "array" && !instance.is_array()) return false;
        if (type == "string" && !instance.is_string()) return false;
        if (type == "boolean" && !instance.is_boolean()) return false;
        if (type == "null" && !instance.is_null()) return false;
        if (type == "number" && (!instance.is_number() || !std::isfinite(instance.as<f64>()))) return false;
        if (type == "integer" &&
            (!instance.is_number() || !std::isfinite(instance.as<f64>()) || std::trunc(instance.as<f64>()) != instance.as<f64>())) {
            return false;
        }
    }
    if (schema.contains("const") && !json_equal(instance, schema.at("const"))) return false;
    if (schema.contains("enum")) {
        const auto &choices = schema.at("enum").get_array();
        if (std::none_of(choices.begin(), choices.end(), [&](const Json &choice) { return json_equal(choice, instance); })) {
            return false;
        }
    }
    if (instance.is_number()) {
        const long double n = instance.as<long double>();
        if (schema.contains("minimum") && n < schema.at("minimum").as<long double>()) return false;
        if (schema.contains("maximum") && n > schema.at("maximum").as<long double>()) return false;
    }
    if (instance.is_string()) {
        const auto &s = instance.get<std::string>();
        const auto count = unicode_length(s);
        if (schema.contains("minLength") && count < schema.at("minLength").as<usize>()) return false;
        if (schema.contains("maxLength") && count > schema.at("maxLength").as<usize>()) return false;
        if (schema.contains("pattern")) {
            const auto &pattern = schema.at("pattern").get<std::string>();
            if (pattern.starts_with("^(?:[A-Za-z0-9+/]")) {
                if (!base64_shape(s)) return false;
            } else if (!std::regex_search(s, compiled_pattern(pattern))) {
                return false;
            }
        }
    }
    if (instance.is_array()) {
        if (schema.contains("maxItems") && instance.size() > schema.at("maxItems").as<usize>()) return false;
        if (schema.contains("items")) {
            for (const auto &v : instance.get_array()) {
                if (!conforms(schema.at("items"), v, root)) return false;
            }
        }
    }
    if (instance.is_object()) {
        if (schema.contains("required")) {
            for (const auto &key : schema.at("required").get_array()) {
                if (!instance.contains(key.get<std::string>())) return false;
            }
        }
        if (schema.contains("properties")) {
            const auto &props = schema.at("properties");
            for (const auto &[key, value] : instance.get_object()) {
                if (props.contains(key)) {
                    if (!conforms(props.at(key), value, root)) return false;
                } else if (!json_value<bool>(schema, "additionalProperties", true)) {
                    return false;
                }
            }
        }
    }
    if (schema.contains("allOf")) {
        for (const auto &s : schema.at("allOf").get_array()) {
            if (!conforms(s, instance, root)) return false;
        }
    }
    if (schema.contains("anyOf")) {
        bool found = false;
        for (const auto &s : schema.at("anyOf").get_array()) found |= conforms(s, instance, root);
        if (!found) return false;
    }
    if (schema.contains("oneOf")) {
        i32 count = 0;
        for (const auto &s : schema.at("oneOf").get_array()) count += conforms(s, instance, root) ? 1 : 0;
        if (count != 1) return false;
    }
    if (schema.contains("not") && conforms(schema.at("not"), instance, root)) return false;
    if (schema.contains("if")) {
        const char *branch = conforms(schema.at("if"), instance, root) ? "then" : "else";
        if (schema.contains(branch) && !conforms(schema.at(branch), instance, root)) return false;
    }
    return true;
}

const Json &schema_for(std::string_view tool) {
    const auto *spec = find_tool(tool);
    if (!spec) throw std::logic_error("unknown codec schema");
    return spec->input_schema();
}

Json fraction_json(const engrave::Fraction &f) { return {{"num", std::to_string(f.num)}, {"den", std::to_string(f.den)}}; }

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
    throw std::logic_error("unknown ValueKind");
}

const char *unit(engrave::Unit u) {
    switch (u) {
        case engrave::Unit::NONE: return "none";
        case engrave::Unit::SPATIUM: return "spatium";
        case engrave::Unit::ABSOLUTE: return "absolute";
        case engrave::Unit::MILLIMETRE: return "millimetre";
        case engrave::Unit::DEGREE: return "degree";
    }
    throw std::logic_error("unknown Unit");
}
} // namespace

namespace detail {

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

Result<void> validate_tool(std::string_view name, const Json &value) {
    const auto &root = schema_for(name);
    if (!conforms(root, value, root)) {
        return std::unexpected(invalid_error("arguments do not match the declared " + std::string(name) + " schema"));
    }
    return {};
}

Result<void> validate_def(std::string_view tool, std::string_view name, const Json &value) {
    const auto &root = schema_for(tool);
    if (!conforms(root.at("$defs").at(std::string(name)), value, root)) {
        return std::unexpected(invalid_error("value does not match the declared " + std::string(name) + " schema"));
    }
    return {};
}

Result<i64> signed_decimal(const Json &j) {
    if (!j.is_string()) return std::unexpected(invalid_error("integer is not a canonical int64 decimal"));
    const auto &s = j.get<std::string>();
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
    auto n = signed_decimal(j.at("num"));
    if (!n) return std::unexpected(std::move(n.error()));
    auto d = signed_decimal(j.at("den"));
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

Result<engrave::Address> address(const Json &j) {
    const auto &k = j.at("kind").get<std::string>();
    if (k == "segment") {
        auto measure = eid(j.at("measure"));
        if (!measure) return std::unexpected(std::move(measure.error()));
        auto tick = fraction(j.at("tick"));
        if (!tick) return std::unexpected(std::move(tick.error()));
        return engrave::SegmentAddress{*measure, j.at("staff").as<i32>(), j.at("voice").as<i32>(), *tick,
                                       j.at("segment_kind").get<std::string>()};
    }
    if (k == "page") {
        auto sel = score(j.at("score"));
        if (!sel) return std::unexpected(std::move(sel.error()));
        return engrave::PageAddress{*sel, j.at("index").as<i32>()};
    }
    if (k == "system") {
        auto sel = score(j.at("score"));
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
    const auto &k = j.at("kind").get<std::string>();
    if (k == "id") {
        auto id = eid(j.at("id"));
        if (!id) return std::unexpected(std::move(id.error()));
        return *id;
    }
    if (k == "address") {
        auto addr = address(j.at("address"));
        if (!addr) return std::unexpected(std::move(addr.error()));
        return *addr;
    }
    if (k == "op" && allow_op) {
        const auto op = j.at("op").as<usize>();
        if (op >= before) return std::unexpected(invalid_error("OpRef must refer to an earlier operation"));
        return engrave::OpRef{op, j.at("item").as<usize>()};
    }
    return std::unexpected(invalid_error("OpRef is only valid in an edit batch"));
}

Result<engrave::Value> value(const Json &j) {
    if (j.is_null()) return engrave::Value{std::monostate{}};
    const auto &k = j.at("kind").get<std::string>();
    const auto &v = j.at("value");
    if (k == "bool") return engrave::Value{v.get<bool>()};
    if (k == "int") {
        auto n = signed_decimal(v);
        if (!n) return std::unexpected(std::move(n.error()));
        return engrave::Value{*n};
    }
    if (k == "real") return engrave::Value{v.as<f64>()};
    if (k == "string") {
        auto text = v.get<std::string>();
        if (auto ok = validate_xml_text(text); !ok) return std::unexpected(std::move(ok.error()));
        return engrave::Value{std::move(text)};
    }
    if (k == "fraction") {
        auto f = fraction(v);
        if (!f) return std::unexpected(std::move(f.error()));
        return engrave::Value{*f};
    }
    if (k == "point") return engrave::Value{engrave::Point{v.at("x").as<f64>(), v.at("y").as<f64>()}};
    if (k == "size") return engrave::Value{engrave::Size{v.at("w").as<f64>(), v.at("h").as<f64>()}};
    if (k == "color") {
        return engrave::Value{engrave::Color{v.at("r").as<u8>(), v.at("g").as<u8>(), v.at("b").as<u8>(), v.at("a").as<u8>()}};
    }
    if (k == "enum") return engrave::Value{engrave::Enum{v.get<std::string>()}};
    if (k == "int_list") {
        std::vector<i64> xs;
        for (const auto &x : v.get_array()) {
            auto n = signed_decimal(x);
            if (!n) return std::unexpected(std::move(n.error()));
            xs.push_back(*n);
        }
        return engrave::Value{std::move(xs)};
    }
    return std::unexpected(invalid_error("unknown or read-only value kind"));
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

Result<engrave::ScoreSel> decode_score(const Json &j) {
    if (auto ok = detail::validate_def("score_outline", "score", j); !ok) return std::unexpected(std::move(ok.error()));
    return detail::score(j);
}

Result<engrave::Ref> decode_ref(const Json &j) {
    if (auto ok = detail::validate_def("score_query", "read_ref", j); !ok) return std::unexpected(std::move(ok.error()));
    return detail::ref(j, false, std::numeric_limits<usize>::max());
}

Result<engrave::Query> decode_query(const Json &j) {
    if (auto ok = detail::validate_tool("score_query", Json{{"document", "doc-A-1"}, {"mode", "elements"}, {"query", j}}); !ok) {
        return std::unexpected(std::move(ok.error()));
    }
    engrave::Query q;
    if (j.contains("score")) {
        auto sel = detail::score(j.at("score"));
        if (!sel) return std::unexpected(std::move(sel.error()));
        q.score = *sel;
    }
    if (j.contains("types")) q.types = strings(j.at("types"));
    if (j.contains("first_measure")) q.first_measure = j.at("first_measure").as<i32>();
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
    if (j.contains("staves")) q.staves = ints(j.at("staves"));
    if (j.contains("voices")) q.voices = ints(j.at("voices"));
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
                return {{"kind", "bool"}, {"value", x}};
            } else if constexpr (std::is_same_v<T, i64>) {
                return {{"kind", "int"}, {"value", std::to_string(x)}};
            } else if constexpr (std::is_same_v<T, f64>) {
                return {{"kind", "real"}, {"value", x}};
            } else if constexpr (std::is_same_v<T, std::string>) {
                return {{"kind", "string"}, {"value", x}};
            } else if constexpr (std::is_same_v<T, engrave::Fraction>) {
                return {{"kind", "fraction"}, {"value", fraction_json(x)}};
            } else if constexpr (std::is_same_v<T, engrave::Point>) {
                return {{"kind", "point"}, {"value", {{"x", x.x}, {"y", x.y}}}};
            } else if constexpr (std::is_same_v<T, engrave::Size>) {
                return {{"kind", "size"}, {"value", {{"w", x.w}, {"h", x.h}}}};
            } else if constexpr (std::is_same_v<T, engrave::Color>) {
                return {{"kind", "color"}, {"value", {{"r", x.r}, {"g", x.g}, {"b", x.b}, {"a", x.a}}}};
            } else if constexpr (std::is_same_v<T, engrave::Enum>) {
                return {{"kind", "enum"}, {"value", x.token}};
            } else if constexpr (std::is_same_v<T, std::vector<i64>>) {
                return {{"kind", "int_list"}, {"value", mapped(x, [](auto n) { return Json(std::to_string(n)); })}};
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
        return Json{{"id", engrave::to_string(p.id)},
                    {"name", p.name},
                    {"instrument_id", p.instrument_id},
                    {"staves", mapped(p.staves, [](const auto &s) {
                         return Json{
                             {"id", engrave::to_string(s.id)}, {"index", s.index}, {"part", s.part}, {"group", s.group}, {"lines", s.lines},
                             {"invisible", s.invisible}};
                     })}};
    });
    auto measures = mapped(o.measures, [](const auto &m) {
        return Json{
            {"id", engrave::to_string(m.id)}, {"index", m.index}, {"tick", fraction_json(m.tick)}, {"duration", fraction_json(m.duration)}};
    });
    auto sigs = mapped(o.signatures, [](const auto &s) {
        return Json{{"type", s.type},   {"tick", fraction_json(s.tick)},
                    {"staff", s.staff}, {"timesig", s.timesig ? fraction_json(*s.timesig) : Json(nullptr)},
                    {"key", s.key},     {"bpm", s.bpm},
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
                    {"measure_index", e.measure_index},
                    {"staff", e.staff},
                    {"voice", e.voice},
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

Json encode(const engrave::Positions &p) {
    return {{"page_size", {{"width", p.page_width}, {"height", p.page_height}}},
            {"elements", mapped(p.elements,
                                [](const auto &e) {
                                    return Json{{"id", e.id}, {"x", e.x}, {"y", e.y}, {"sx", e.sx}, {"sy", e.sy}, {"page", e.page}};
                                })},
            {"events", mapped(p.events, [](const auto &e) { return Json{{"elid", e.elid}, {"position", e.position}}; })}};
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
        throw std::logic_error("unknown Format");
    };
    auto render = [](engrave::RenderFormat f) -> const char * {
        switch (f) {
            case engrave::RenderFormat::PDF: return "pdf";
            case engrave::RenderFormat::PNG: return "png";
            case engrave::RenderFormat::SVG: return "svg";
        }
        throw std::logic_error("unknown RenderFormat");
    };
    auto audio = [](engrave::AudioFormat f) -> const char * {
        switch (f) {
            case engrave::AudioFormat::WAV: return "wav";
            case engrave::AudioFormat::FLAC: return "flac";
            case engrave::AudioFormat::OPUS: return "opus";
            case engrave::AudioFormat::MP3: return "mp3";
            case engrave::AudioFormat::AAC: return "aac";
        }
        throw std::logic_error("unknown AudioFormat");
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
    if (!e.published_artifacts.empty()) err["published_artifacts"] = e.published_artifacts;
    if (!e.export_warnings.empty()) err["export_warnings"] = encode(e.export_warnings);
    return {{"ok", false}, {"error", err}};
}

} // namespace music::api
