#include "api_json_internal.h"
#include "registry.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <regex>
#include <type_traits>

namespace music::v2 {
namespace {
using detail::invalid;

std::size_t unicode_length(const std::string &s) {
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xc0) != 0x80) ++n;
    return n;
}

bool base64_shape(std::string_view s) {
    if (s.size() % 4) return false;
    std::size_t padding = 0;
    if (!s.empty() && s.back() == '=') ++padding;
    if (s.size() > 1 && s[s.size() - 2] == '=') ++padding;
    for (std::size_t i = 0; i < s.size() - padding; ++i) {
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
    static thread_local std::map<std::string, std::regex> cache;
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
        if (type == "number" && (!instance.is_number() || !std::isfinite(instance.get<double>()))) return false;
        if (type == "integer" && (!instance.is_number() || !std::isfinite(instance.get<double>()) ||
                                  std::trunc(instance.get<double>()) != instance.get<double>()))
            return false;
    }
    if (schema.contains("const") && instance != schema.at("const")) return false;
    if (schema.contains("enum") && std::find(schema.at("enum").begin(), schema.at("enum").end(), instance) == schema.at("enum").end())
        return false;
    if (instance.is_number()) {
        const long double n = instance.get<long double>();
        if (schema.contains("minimum") && n < schema.at("minimum").get<long double>()) return false;
        if (schema.contains("maximum") && n > schema.at("maximum").get<long double>()) return false;
    }
    if (instance.is_string()) {
        const auto &s = instance.get_ref<const std::string &>();
        const auto count = unicode_length(s);
        if (schema.contains("minLength") && count < schema.at("minLength").get<std::size_t>()) return false;
        if (schema.contains("maxLength") && count > schema.at("maxLength").get<std::size_t>()) return false;
        if (schema.contains("pattern")) {
            const auto &pattern = schema.at("pattern").get_ref<const std::string &>();
            if (pattern.starts_with("^(?:[A-Za-z0-9+/]")) {
                if (!base64_shape(s)) return false;
            } else if (!std::regex_search(s, compiled_pattern(pattern)))
                return false;
        }
    }
    if (instance.is_array()) {
        if (schema.contains("maxItems") && instance.size() > schema.at("maxItems").get<std::size_t>()) return false;
        if (schema.contains("items"))
            for (const auto &v : instance)
                if (!conforms(schema.at("items"), v, root)) return false;
    }
    if (instance.is_object()) {
        if (schema.contains("required"))
            for (const auto &key : schema.at("required"))
                if (!instance.contains(key.get<std::string>())) return false;
        if (schema.contains("properties")) {
            const auto &props = schema.at("properties");
            for (auto it = instance.begin(); it != instance.end(); ++it) {
                if (props.contains(it.key())) {
                    if (!conforms(props.at(it.key()), it.value(), root)) return false;
                } else if (schema.value("additionalProperties", true) == false)
                    return false;
            }
        }
    }
    if (schema.contains("allOf"))
        for (const auto &s : schema.at("allOf"))
            if (!conforms(s, instance, root)) return false;
    if (schema.contains("anyOf")) {
        bool found = false;
        for (const auto &s : schema.at("anyOf")) found |= conforms(s, instance, root);
        if (!found) return false;
    }
    if (schema.contains("oneOf")) {
        int count = 0;
        for (const auto &s : schema.at("oneOf")) count += conforms(s, instance, root) ? 1 : 0;
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
Json score_json(const engrave::ScoreSel &s) { return s.excerpt ? Json{{"excerpt", engrave::to_string(*s.excerpt)}} : Json::object(); }
template <class T, class F>
Json mapped(const std::vector<T> &items, F f) {
    Json out = Json::array();
    for (const auto &x : items) out.push_back(f(x));
    return out;
}
Json ids(const std::vector<engrave::ElementId> &items) {
    return mapped(items, [](const auto &x) { return Json(engrave::to_string(x)); });
}
Json mix(const engrave::AudioMix &m) { return {{"volume_db", m.volume_db}, {"balance", m.balance}, {"mute", m.mute}, {"solo", m.solo}}; }
const char *kind(engrave::ValueKind k) {
    switch (k) {
        case engrave::ValueKind::Bool: return "bool";
        case engrave::ValueKind::Int: return "int";
        case engrave::ValueKind::Real: return "real";
        case engrave::ValueKind::String: return "string";
        case engrave::ValueKind::Point: return "point";
        case engrave::ValueKind::Size: return "size";
        case engrave::ValueKind::Color: return "color";
        case engrave::ValueKind::Fraction: return "fraction";
        case engrave::ValueKind::Enum: return "enum";
        case engrave::ValueKind::IntList: return "int_list";
        case engrave::ValueKind::Opaque: return "opaque";
    }
    throw std::logic_error("unknown ValueKind");
}
const char *unit(engrave::Unit u) {
    switch (u) {
        case engrave::Unit::None: return "none";
        case engrave::Unit::Spatium: return "spatium";
        case engrave::Unit::Absolute: return "absolute";
        case engrave::Unit::Millimetre: return "millimetre";
        case engrave::Unit::Degree: return "degree";
    }
    throw std::logic_error("unknown Unit");
}
} // namespace

namespace detail {
[[noreturn]] void invalid(std::string message) { throw DecodeError{HostError{"InvalidArgument", std::move(message)}}; }
void validate_xml_text(std::string_view value) {
    for (unsigned char c : value)
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') invalid("text contains a character forbidden by XML 1.0");
    if (value.find("\xef\xbf\xbe") != std::string_view::npos || value.find("\xef\xbf\xbf") != std::string_view::npos)
        invalid("text contains a character forbidden by XML 1.0");
}
void validate_tool(std::string_view name, const Json &value) {
    const auto &root = schema_for(name);
    if (!conforms(root, value, root)) invalid("arguments do not match the declared " + std::string(name) + " schema");
}
void validate_def(std::string_view tool, std::string_view name, const Json &value) {
    const auto &root = schema_for(tool);
    if (!conforms(root.at("$defs").at(std::string(name)), value, root))
        invalid("value does not match the declared " + std::string(name) + " schema");
}
std::int64_t signed_decimal(const Json &j) {
    const auto &s = j.get_ref<const std::string &>();
    std::int64_t n{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size() || std::to_string(n) != s) invalid("integer is not a canonical int64 decimal");
    return n;
}
engrave::ElementId eid(const Json &j) {
    auto id = engrave::element_id_from_string(j.get<std::string>());
    if (!id || engrave::to_string(*id) != j.get<std::string>()) invalid("invalid canonical element id");
    return *id;
}
engrave::Fraction fraction(const Json &j) {
    auto n = signed_decimal(j.at("num"));
    auto d = signed_decimal(j.at("den"));
    if (d <= 0) invalid("fraction denominator must be positive");
    return {n, d};
}
engrave::ScoreSel score(const Json &j) { return {j.contains("excerpt") ? std::optional(eid(j.at("excerpt"))) : std::nullopt}; }
engrave::Address address(const Json &j) {
    const auto k = j.at("kind").get<std::string>();
    if (k == "segment")
        return engrave::SegmentAddress{eid(j.at("measure")), j.at("staff").get<int>(), j.at("voice").get<int>(), fraction(j.at("tick")),
                                       j.at("segment_kind").get<std::string>()};
    if (k == "page") return engrave::PageAddress{score(j.at("score")), j.at("index").get<int>()};
    if (k == "system") return engrave::SystemAddress{score(j.at("score")), j.at("page").get<int>(), j.at("index").get<int>()};
    if (k == "spanner_segment") return engrave::SpannerSegmentAddress{eid(j.at("spanner")), j.at("index").get<int>()};
    invalid("unknown address kind");
}
engrave::Ref ref(const Json &j, bool allow_op, std::size_t before) {
    const auto k = j.at("kind").get<std::string>();
    if (k == "id") return eid(j.at("id"));
    if (k == "address") return address(j.at("address"));
    if (k == "op" && allow_op) {
        const auto op = j.at("op").get<std::size_t>();
        if (op >= before) invalid("OpRef must refer to an earlier operation");
        return engrave::OpRef{op, j.at("item").get<std::size_t>()};
    }
    invalid("OpRef is only valid in an edit batch");
}
engrave::Value value(const Json &j) {
    if (j.is_null()) return std::monostate{};
    const auto k = j.at("kind").get<std::string>();
    const auto &v = j.at("value");
    if (k == "bool") return v.get<bool>();
    if (k == "int") return signed_decimal(v);
    if (k == "real") return v.get<double>();
    if (k == "string") {
        auto text = v.get<std::string>();
        validate_xml_text(text);
        return text;
    }
    if (k == "fraction") return fraction(v);
    if (k == "point") return engrave::Point{v.at("x").get<double>(), v.at("y").get<double>()};
    if (k == "size") return engrave::Size{v.at("w").get<double>(), v.at("h").get<double>()};
    if (k == "color")
        return engrave::Color{v.at("r").get<std::uint8_t>(), v.at("g").get<std::uint8_t>(), v.at("b").get<std::uint8_t>(),
                              v.at("a").get<std::uint8_t>()};
    if (k == "enum") return engrave::Enum{v.get<std::string>()};
    if (k == "int_list") {
        std::vector<std::int64_t> xs;
        for (const auto &x : v) xs.push_back(signed_decimal(x));
        return xs;
    }
    invalid("unknown or read-only value kind");
}
std::filesystem::path path(const Json &j) {
    const auto &s = j.get_ref<const std::string &>();
    if (s.find('\0') != std::string::npos) invalid("path contains a NUL byte");
    return std::filesystem::u8path(s);
}
std::string path_text(const std::filesystem::path &p) {
    auto s = p.generic_u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}
} // namespace detail

Result<std::uint64_t> decode_revision(const Json &j) {
    try {
        if (!j.is_string()) invalid("revision must be a decimal string");
        const auto &s = j.get_ref<const std::string &>();
        std::uint64_t n{};
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
        if (ec != std::errc{} || end != s.data() + s.size() || std::to_string(n) != s)
            invalid("revision is not a canonical uint64 decimal");
        return n;
    } catch (const detail::DecodeError &e) {
        return std::unexpected(e.error);
    }
}
Result<engrave::ScoreSel> decode_score(const Json &j) {
    try {
        detail::validate_def("score_outline", "score", j);
        return detail::score(j);
    } catch (const detail::DecodeError &e) {
        return std::unexpected(e.error);
    }
}
Result<engrave::Ref> decode_ref(const Json &j) {
    try {
        detail::validate_def("score_query", "read_ref", j);
        return detail::ref(j, false, std::numeric_limits<std::size_t>::max());
    } catch (const detail::DecodeError &e) {
        return std::unexpected(e.error);
    }
}
Result<engrave::Query> decode_query(const Json &j) {
    try {
        detail::validate_tool("score_query", Json{{"document", "doc-A-1"}, {"mode", "elements"}, {"query", j}});
        engrave::Query q;
        if (j.contains("score")) q.score = detail::score(j.at("score"));
        if (j.contains("types")) q.types = j.at("types").get<std::vector<std::string>>();
        if (j.contains("first_measure")) q.first_measure = j.at("first_measure").get<int>();
        if (j.contains("measure_count")) q.measure_count = j.at("measure_count").get<int>();
        if (j.contains("from_measure")) q.from_measure = detail::eid(j.at("from_measure"));
        if (j.contains("to_measure")) q.to_measure = detail::eid(j.at("to_measure"));
        if (j.contains("staves")) q.staves = j.at("staves").get<std::vector<std::int32_t>>();
        if (j.contains("voices")) q.voices = j.at("voices").get<std::vector<std::int32_t>>();
        if (j.contains("from_tick")) q.from_tick = detail::fraction(j.at("from_tick"));
        if (j.contains("to_tick")) q.to_tick = detail::fraction(j.at("to_tick"));
        if (j.contains("props")) q.props = j.at("props").get<std::vector<std::string>>();
        if (j.contains("layout_boxes")) q.layout_boxes = j.at("layout_boxes").get<bool>();
        if (j.contains("limit")) q.limit = j.at("limit").get<std::size_t>();
        if (j.contains("cursor")) q.cursor = j.at("cursor").get<std::size_t>();
        return q;
    } catch (const detail::DecodeError &e) {
        return std::unexpected(e.error);
    }
}

Json encode(const engrave::Value &v) {
    return std::visit(
        [](const auto &x) -> Json {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return nullptr;
            else if constexpr (std::is_same_v<T, bool>)
                return {{"kind", "bool"}, {"value", x}};
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return {{"kind", "int"}, {"value", std::to_string(x)}};
            else if constexpr (std::is_same_v<T, double>)
                return {{"kind", "real"}, {"value", x}};
            else if constexpr (std::is_same_v<T, std::string>)
                return {{"kind", "string"}, {"value", x}};
            else if constexpr (std::is_same_v<T, engrave::Fraction>)
                return {{"kind", "fraction"}, {"value", fraction_json(x)}};
            else if constexpr (std::is_same_v<T, engrave::Point>)
                return {{"kind", "point"}, {"value", {{"x", x.x}, {"y", x.y}}}};
            else if constexpr (std::is_same_v<T, engrave::Size>)
                return {{"kind", "size"}, {"value", {{"w", x.w}, {"h", x.h}}}};
            else if constexpr (std::is_same_v<T, engrave::Color>)
                return {{"kind", "color"}, {"value", {{"r", x.r}, {"g", x.g}, {"b", x.b}, {"a", x.a}}}};
            else if constexpr (std::is_same_v<T, engrave::Enum>)
                return {{"kind", "enum"}, {"value", x.token}};
            else if constexpr (std::is_same_v<T, std::vector<std::int64_t>>)
                return {{"kind", "int_list"}, {"value", mapped(x, [](auto n) { return Json(std::to_string(n)); })}};
            else if constexpr (std::is_same_v<T, engrave::Opaque>)
                return {{"kind", "opaque"}, {"type", x.type}};
        },
        v);
}
Json encode(const engrave::PropertyRead &p) {
    return {{"name", p.name}, {"value", encode(p.value)}, {"is_default", p.is_default}, {"styled", p.styled}};
}
Json encode(const engrave::Diagnostic &d) {
    return {{"severity", d.severity == engrave::Severity::Info ? "info" : "warning"}, {"message", d.message}};
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
    auto excerpts =
        mapped(o.excerpts, [](const auto &e) { return Json{{"id", engrave::to_string(e.id)}, {"name", e.name}, {"pages", e.pages}}; });
    return {{"title", o.title},
            {"subtitle", o.subtitle},
            {"composer", o.composer},
            {"poet", o.poet},
            {"timesig", o.timesig},
            {"mscore_version", o.mscore_version},
            {"file_version", o.file_version},
            {"pages", o.pages},
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
        if (e.box) box = {{"page", e.box->page}, {"x", e.box->x}, {"y", e.box->y}, {"width", e.box->width}, {"height", e.box->height}};
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
    return {{"type", t.type},
            {"props", mapped(t.props, [](const auto &p) {
                 return Json{{"name", p.name},         {"kind", kind(p.kind)}, {"unit", unit(p.unit)}, {"enum_tokens", p.enum_tokens},
                             {"settable", p.settable}, {"link", p.link},       {"group", p.group}};
             })}};
}
Json encode(const engrave::StyleInfo &s) {
    return {{"name", s.name},
            {"kind", kind(s.kind)},
            {"unit", unit(s.unit)},
            {"enum_tokens", s.enum_tokens},
            {"default_value", encode(s.default_value)}};
}
Json encode(const engrave::TemplateInfo &t) {
    return {{"id", t.id}, {"title", t.title}, {"category", t.category}, {"path", detail::path_text(t.path)}};
}
Json encode(const engrave::Capabilities &c) {
    auto format = [](engrave::Format f) -> const char * {
        switch (f) {
            case engrave::Format::Mscz: return "mscz";
            case engrave::Format::Mscx: return "mscx";
            case engrave::Format::MusicXml: return "musicxml";
            case engrave::Format::Mxl: return "mxl";
            case engrave::Format::Midi: return "midi";
            case engrave::Format::GuitarPro: return "guitarpro";
            case engrave::Format::Mnx: return "mnx";
        }
        throw std::logic_error("unknown Format");
    };
    auto render = [](engrave::RenderFormat f) -> const char * {
        switch (f) {
            case engrave::RenderFormat::Pdf: return "pdf";
            case engrave::RenderFormat::Png: return "png";
            case engrave::RenderFormat::Svg: return "svg";
        }
        throw std::logic_error("unknown RenderFormat");
    };
    auto audio = [](engrave::AudioFormat f) -> const char * {
        switch (f) {
            case engrave::AudioFormat::Wav: return "wav";
            case engrave::AudioFormat::Flac: return "flac";
            case engrave::AudioFormat::Opus: return "opus";
            case engrave::AudioFormat::Mp3: return "mp3";
            case engrave::AudioFormat::Aac: return "aac";
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
    Json result = Json::array();
    for (const auto &warning : warnings) {
        Json item = {{"code", warning.code}, {"message", warning.message}};
        if (warning.object) item["object"] = Json{{"kind", warning.object->kind}, {"id", warning.object->id}};
        result.push_back(std::move(item));
    }
    return result;
}
HostError api_error(const engrave::Error &e) {
    HostError result{std::string(engrave::to_string(e.code)), e.message, e.op_index};
    result.export_warnings = e.export_warnings;
    return result;
}
Json error_envelope(const HostError &e) {
    Json err = {{"code", e.code}, {"message", e.message}};
    if (e.op_index) err["op_index"] = *e.op_index;
    if (e.document) err["document"] = *e.document;
    if (e.current_revision) err["current_revision"] = std::to_string(*e.current_revision);
    if (!e.published_artifacts.empty()) err["published_artifacts"] = e.published_artifacts;
    if (!e.export_warnings.empty()) err["export_warnings"] = encode(e.export_warnings);
    return {{"ok", false}, {"error", err}};
}
} // namespace music::v2
