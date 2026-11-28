#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <span>
#include <utility>

#include "api_json_internal.h"
#include "utf8_text.h"

namespace music::api::detail {
namespace {
using View = std::string_view;
using Check = Result<void> (*)(const Json &, const std::string &);

struct Field {
    View name;
    Check check;
    bool required = false;
};

Result<void> fail(const std::string &p, View why, View expected) {
    auto message = (p.empty() ? "arguments" : p) + ": " + std::string(why) + "; expected " + std::string(expected);
    // Unknown field names may contain NUL. Preserve the original diagnostic prefix.
    message.erase(std::ranges::find(message, '\0'), message.end());
    return std::unexpected(invalid_error(std::move(message)));
}

std::string child(const std::string &p, View key) { return p.empty() ? std::string(key) : p + "." + std::string(key); }

Result<void> object(const Json &j, const std::string &p, std::span<const Field> fields) {
    if (!j.is_object()) return fail(p, "wrong type", "an object");
    for (const auto &field : fields) {
        if (field.required && !j.get_object().contains(field.name)) return fail(child(p, field.name), "missing field", "a value");
    }
    for (const auto &[key, v] : j.get_object()) {
        auto it = std::ranges::find(fields, View(key), &Field::name);
        if (it == fields.end()) return fail(child(p, key), "unknown field", "a supported parameter");
        if (auto checked = it->check(v, child(p, key)); !checked) return checked;
    }
    return {};
}

Result<void> object(const Json &j, const std::string &p, std::initializer_list<Field> fields) {
    return object(j, p, std::span<const Field>(fields));
}

template <Check Item>
Result<void> list(const Json &j, const std::string &p) {
    if (!j.is_array()) return fail(p, "wrong type", "an array");
    for (usize i = 0; i < j.size(); ++i) {
        if (auto checked = Item(j[i], p + "[" + std::to_string(i) + "]"); !checked) return checked;
    }
    return {};
}

template <int Max>
Result<void> text(const Json &j, const std::string &p) {
    if (!j.is_string()) return fail(p, "wrong type", "a string");
    const auto &s = j.get<std::string>();
    if (utf8_exceeds_length(s, Max)) {
        return fail(p, "text too long", "at most " + std::to_string(Max) + " Unicode characters");
    }
    return {};
}

template <int Min, int Max>
Result<void> integer(const Json &j, const std::string &p) {
    if ((!j.holds<u64>() && !j.holds<i64>()) || j.as<double>() < Min || j.as<double>() > Max) {
        return fail(p, "invalid integer", std::to_string(Min) + ".." + std::to_string(Max));
    }
    return {};
}

Result<void> choice(const Json &j, const std::string &p, std::initializer_list<View> choices) {
    if (!j.is_string() || std::ranges::find(choices, View(j.get<std::string>())) == choices.end()) {
        return fail(p, "invalid choice", "a listed token");
    }
    return {};
}

Result<void> boolean(const Json &j, const std::string &p) {
    if (!j.is_boolean()) return fail(p, "wrong type", "true or false");
    return {};
}

Result<void> any(const Json &, const std::string &) { return {}; }

Result<void> numeric(const Json &j, const std::string &p) {
    if (!j.is_number() || !std::isfinite(j.as<double>())) return fail(p, "wrong type", "a finite number");
    return {};
}

Result<void> filename(const Json &j, const std::string &p) {
    if (auto checked = text<4096>(j, p); !checked) return checked;
    if (j.get<std::string>().empty() || j.get<std::string>().find('\0') != std::string::npos) {
        return fail(p, "invalid path", "a nonempty path without NUL");
    }
    return {};
}

Result<void> xml_text(const Json &j, const std::string &p) {
    if (auto checked = text<4096>(j, p); !checked) return checked;
    if (!validate_xml_text(j.get<std::string>())) return fail(p, "invalid XML character", "XML 1.0 text");
    return {};
}

Result<void> check_eid(const Json &j, const std::string &p) {
    if (!eid(j)) return fail(p, "invalid element ID", "a canonical EID");
    return {};
}

Result<void> check_revision(const Json &j, const std::string &p) {
    if (!decode_revision(j)) return fail(p, "invalid revision", "a canonical unsigned decimal string");
    return {};
}

Result<void> check_fraction(const Json &j, const std::string &p) {
    if (!fraction(j)) return fail(p, "invalid fraction", "0 or numerator/positive-denominator");
    return {};
}

// Any address kind; only a query may name the page, system and spanner segment elements layout builds.
Result<void> check_address(const Json &j, const std::string &p) {
    const auto &kind = j.at("kind");
    if (kind == "segment") {
        return object(j, p,
                      {{"kind", text<32>, true},
                       {"measure", check_eid, true},
                       {"staff", integer<1, INT32_MAX>, true},
                       {"voice", integer<1, 4>, true},
                       {"tick", check_fraction, true},
                       {"segment_kind", text<128>, true}});
    } else if (kind == "system") {
        return object(j, p,
                      {{"kind", text<32>, true},
                       {"excerpt", check_eid},
                       {"page", integer<0, INT32_MAX>, true},
                       {"index", integer<0, INT32_MAX>, true}});
    } else if (kind == "page") {
        return object(j, p, {{"kind", text<32>, true}, {"excerpt", check_eid}, {"index", integer<0, INT32_MAX>, true}});
    } else if (kind == "spanner_segment") {
        return object(j, p, {{"kind", text<32>, true}, {"spanner", check_eid, true}, {"index", integer<0, INT32_MAX>, true}});
    } else {
        return fail(child(p, "kind"), "unknown address kind", "segment, page, system or spanner_segment");
    }
}

// The engine refuses to edit what layout builds, so an edit operand's address must be a segment.
Result<void> check_edit_address(const Json &j, const std::string &p) {
    if (j.at("kind") != "segment") return fail(child(p, "kind"), "address not editable", "segment");
    return check_address(j, p);
}

// A compact position or a segment address.
Result<void> check_pos(const Json &j, const std::string &p) {
    if (j.is_object() && !j.contains("kind")) {
        return object(j, p,
                      {{"measure", integer<1, INT32_MAX>, true},
                       {"offset", check_fraction},
                       {"staff", integer<1, INT32_MAX>},
                       {"voice", integer<1, 4>}});
    }
    if (!j.is_object()) return fail(p, "wrong type", "a compact position or segment address");
    return check_edit_address(j, p);
}

Result<void> check_op_ref(const Json &j, const std::string &p) {
    return object(j, p, {{"from_op", integer<0, 1023>, true}, {"item", integer<0, 65535>, true}});
}

Result<void> check_edit_ref(const Json &j, const std::string &p) {
    if (j.is_string()) return check_eid(j, p);
    if (j.is_object() && j.contains("kind")) return check_edit_address(j, p);
    return check_op_ref(j, p);
}

Result<void> check_query_target(const Json &j, const std::string &p) {
    if (j.is_string()) return check_eid(j, p);
    if (j.is_object() && j.contains("kind")) return check_address(j, p);
    return fail(p, "wrong type", "an EID or address");
}

// Measure and staff operands: a positive 1-based ordinal, an EID or an earlier operation's element.
Result<void> check_ordinal(const Json &j, const std::string &p) {
    if (j.is_number()) return integer<1, INT32_MAX>(j, p);
    if (j.is_object() && j.contains("kind")) return fail(p, "address not accepted", "a 1-based number, EID or {from_op,item}");
    return check_edit_ref(j, p);
}

Result<void> check_anchor(const Json &j, const std::string &p) {
    if (is_relative_position(j)) return check_pos(j, p);
    return check_edit_ref(j, p);
}

Result<void> check_sf(const Json &j, const std::string &p) {
    if (j.is_string()) {
        if (auto checked = text<4096>(j, p); !checked) return checked;
        if (j.get<std::string>().contains('\0')) return fail(p, "invalid SoundFont selector", "a string without NUL");
    } else if (!j.is_null()) {
        return integer<0, INT32_MAX>(j, p);
    }
    return {};
}

Result<void> check_bit_depth(const Json &j, const std::string &p) {
    if (auto checked = integer<0, 32>(j, p); !checked) return checked;
    if (j != 0 && j != 16 && j != 24 && j != 32) return fail(p, "invalid bit depth", "0, 16, 24 or 32");
    return {};
}

Result<void> check_format(const Json &j, const std::string &p) {
    return choice(j, p, {"mscz", "mscx", "musicxml", "mxl", "midi", "guitarpro", "mnx"});
}

Result<void> check_output(const Json &j, const std::string &p) {
    return choice(j, p,
                  {"mscz", "mscx", "musicxml", "mxl", "midi", "pdf", "png", "svg", "wav", "flac", "opus", "mp3", "measure_positions",
                   "segment_positions"});
}

Result<void> check_scope(const Json &j, const std::string &p) {
    return choice(j, p, {"overview", "templates", "instruments", "types", "type", "styles", "ops"});
}

Result<void> check_mode(const Json &j, const std::string &p) {
    return choice(j, p, {"elements", "properties", "page_count", "audio_tracks"});
}

Result<void> check_grace(const Json &j, const std::string &p) {
    if (!j.is_string() || !grace_kind(j.get<std::string>())) return fail(p, "invalid choice", "a listed grace kind");
    return {};
}

Result<void> check_volume(const Json &j, const std::string &p) {
    if (auto checked = numeric(j, p); !checked) return checked;
    if (j.as<double>() < -120 || j.as<double>() > 24) return fail(p, "out of range", "-120..24");
    return {};
}

Result<void> check_balance(const Json &j, const std::string &p) {
    if (auto checked = numeric(j, p); !checked) return checked;
    if (j.as<double>() < -1 || j.as<double>() > 1) return fail(p, "out of range", "-1..1");
    return {};
}

Result<void> check_track(const Json &j, const std::string &p) {
    return object(j, p,
                  {{"track", integer<0, 2147483647>, true},
                   {"volume_db", check_volume},
                   {"balance", check_balance},
                   {"mute", boolean},
                   {"solo", boolean},
                   {"soundfont", check_sf},
                   {"preset", text<4096>}});
}

Result<void> check_opt(const Json &j, const std::string &p) {
    return object(j, p,
                  {{"midi_expand_repeats", boolean},
                   {"midi_rpns", boolean},
                   {"native_version", text<4096>},
                   {"first_page", integer<0, 2147483647>},
                   {"last_page", integer<-1, 2147483647>},
                   {"dpi", integer<36, 1200>},
                   {"background", boolean},
                   {"transparent", boolean},
                   {"bitrate_kbps", integer<0, 512>},
                   {"bit_depth", check_bit_depth},
                   {"use_repeats", boolean},
                   {"master_volume_db", check_volume},
                   {"master_balance", check_balance},
                   {"tracks", list<check_track>},
                   {"reverb", boolean},
                   {"reverb_send", numeric},
                   {"sample_rate", integer<1, 384000>}});
}

Result<void> check_query(const Json &j, const std::string &p) {
    if (j.is_object() && (j.contains("first_measure") || j.contains("measure_count")) &&
        (j.contains("from_measure") || j.contains("to_measure"))) {
        return fail(child(p, "first_measure"), "mixed selectors", "either measure numbers or EIDs");
    }
    return object(j, p,
                  {{"excerpt", check_eid},
                   {"types", list<text<4096>>},
                   {"first_measure", integer<1, 2147483647>},
                   {"measure_count", integer<1, 128>},
                   {"from_measure", check_eid},
                   {"to_measure", check_eid},
                   {"staves", list<integer<1, 2147483647>>},
                   {"voices", list<integer<1, 4>>},
                   {"from_tick", check_fraction},
                   {"to_tick", check_fraction},
                   {"props", list<text<4096>>},
                   {"layout_boxes", boolean},
                   {"limit", integer<1, 4096>},
                   {"cursor", integer<0, 4096>}});
}

Result<void> check_midinote(const Json &j, const std::string &p) {
    return object(j, p, {{"pitch", integer<0, 127>, true}, {"tpc", integer<-8, 40>}});
}

Result<void> check_prop(const Json &j, const std::string &p) { return object(j, p, {{"name", text<4096>, true}, {"value", any, true}}); }

Check op_field_check(OpFieldKind kind) {
    switch (kind) {
        case OpFieldKind::EDIT_REF:
        case OpFieldKind::PART_REF: return check_edit_ref;
        case OpFieldKind::MEASURE_REF:
        case OpFieldKind::STAFF_REF: return check_ordinal;
        case OpFieldKind::ANCHOR: return check_anchor;
        case OpFieldKind::AT_POS:
        case OpFieldKind::ENTRY_AT_POS: return check_pos;
        case OpFieldKind::SCORE_EXCERPT: return check_eid;
        case OpFieldKind::FRACTION:
        case OpFieldKind::TIME_SIG: return check_fraction;
        case OpFieldKind::TEXT:
        case OpFieldKind::LAYOUT_KIND:
        case OpFieldKind::MEASURE_KIND: return text<4096>;
        case OpFieldKind::XML_LONG_TEXT: return text<65536>;
        case OpFieldKind::GRACE_KIND: return check_grace;
        case OpFieldKind::BOOL: return boolean;
        case OpFieldKind::KEY_INT: return integer<-7, 7>;
        case OpFieldKind::PITCH_INT: return integer<0, 127>;
        case OpFieldKind::TPC_INT: return integer<-8, 40>;
        case OpFieldKind::INT_1_1024: return integer<1, 1024>;
        case OpFieldKind::INT_2_1024: return integer<2, 1024>;
        case OpFieldKind::PITCH_LIST: return list<check_midinote>;
        case OpFieldKind::DRUM_KEY_LIST: return list<integer<0, 127>>;
        case OpFieldKind::PART_REF_LIST: return list<check_edit_ref>;
        case OpFieldKind::INT_1_1024_LIST: return list<integer<1, 1024>>;
        case OpFieldKind::PROP_VALUE:
        case OpFieldKind::STYLE_VALUE: return any;
        case OpFieldKind::NAMED_PROPS: return list<check_prop>;
    }
    assert(false);
    std::unreachable();
}

Result<void> validate_arguments(View name, const Json &j) {
    if (name == "score_open") {
        auto checked = object(
            j, "",
            {{"path", filename}, {"data", text<11184812>}, {"format", check_format}, {"filename_hint", filename}, {"strict_ids", boolean}});
        if (!checked) return checked;
        if (j.contains("path") == j.contains("data")) return fail("path", "ambiguous source", "exactly one of path or data");
        return {};
    }
    if (name == "score_create") {
        auto checked = object(j, "",
                              {{"template_id", text<4096>},
                               {"template_path", filename},
                               {"title", xml_text},
                               {"subtitle", xml_text},
                               {"composer", xml_text},
                               {"lyricist", xml_text}});
        if (!checked) return checked;
        if (j.contains("template_id") && j.contains("template_path")) {
            return fail("template_path", "conflicting template", "only one template selector");
        }
        return {};
    }
    if (name == "score_close") {
        return object(j, "", {{"document", text<128>, true}});
    }
    if (name == "score_outline") {
        return object(j, "", {{"document", text<128>, true}, {"expected_revision", check_revision}, {"excerpt", check_eid}});
    }
    if (name == "score_query") {
        auto checked = object(j, "",
                              {{"document", text<128>, true},
                               {"expected_revision", check_revision},
                               {"mode", check_mode, true},
                               {"query", check_query},
                               {"target", check_query_target},
                               {"props", list<text<4096>>},
                               {"excerpt", check_eid}});
        if (!checked) return checked;
        auto mode = j.at("mode").get<std::string>();
        if (mode == "properties" && !j.contains("target")) return fail("target", "missing field", "an EID or address");
        for (auto key : {"props", "target", "query", "excerpt"}) {
            if (j.contains(key) && !(mode == "properties" && (View(key) == "target" || View(key) == "props")) &&
                !(mode == "elements" && (View(key) == "query" || View(key) == "excerpt")) &&
                !((mode == "audio_tracks" || mode == "page_count") && View(key) == "excerpt")) {
                return fail(key, "invalid for mode", mode);
            }
        }
        if (mode == "elements" && j.contains("excerpt") && j.contains("query") && j.at("query").contains("excerpt") &&
            j.at("excerpt") != j.at("query").at("excerpt")) {
            return fail("excerpt", "conflicting score selectors", "matching excerpt IDs");
        }
        return {};
    }
    if (name == "score_read_music") {
        return object(j, "",
                      {{"document", text<128>, true},
                       {"expected_revision", check_revision},
                       {"excerpt", check_eid},
                       {"measure", integer<1, 2147483647>},
                       {"count", integer<1, 128>},
                       {"staves", list<integer<1, 2147483647>>},
                       {"voices", list<integer<1, 4>>},
                       {"include_ids", boolean}});
    }
    if (name == "score_edit") {
        return object(j, "",
                      {{"document", text<128>, true},
                       {"expected_revision", check_revision},
                       {"excerpt", check_eid},
                       {"atomic", boolean},
                       {"detail", boolean},
                       {"ops", list<any>, true}});
    }
    if (name == "score_export") {
        return object(j, "",
                      {{"document", text<128>, true},
                       {"expected_revision", check_revision},
                       {"format", check_output, true},
                       {"path", filename, true},
                       {"excerpt", check_eid},
                       {"options", check_opt}});
    }
    if (name == "runtime_info") {
        auto checked = object(j, "", {{"scope", check_scope}, {"query", text<4096>}, {"type", text<4096>}, {"op", text<4096>}});
        if (!checked) return checked;
        if (j.contains("type") != (j.contains("scope") && j.at("scope") == "type")) {
            return fail("type", "invalid selector", "type only with scope type");
        }
        if (j.contains("op") && (!j.contains("scope") || j.at("scope") != "ops")) return fail("op", "invalid selector", "scope ops");
        if (j.contains("query") && (!j.contains("scope") || j.at("scope") != "instruments")) {
            return fail("query", "invalid selector", "scope instruments");
        }
        return {};
    }
    if (name == "checkpoint") {
        return object(j, "", {{"document", text<128>, true}, {"expected_revision", check_revision}, {"name", text<4096>, true}});
    }
    if (name == "restore_checkpoint") {
        return object(j, "", {{"document", text<128>, true}, {"expected_revision", check_revision}, {"name", text<4096>, true}});
    }
    if (name == "drop_checkpoint") {
        return object(j, "", {{"document", text<128>, true}, {"expected_revision", check_revision}, {"name", text<4096>, true}});
    }
    if (name == "score_write_phrase") {
        return object(
            j, "",
            {{"document", text<128>, true}, {"expected_revision", check_revision}, {"excerpt", check_eid}, {"lanes", list<any>, true}});
    }
    if (name == "runtime_configure") {
        if (auto checked = object(j, "", {{"add_font", filename}, {"soundfonts", list<filename>}}); !checked) return checked;
        if (j.empty()) return fail("add_font", "empty configuration", "add_font or soundfonts");
        return {};
    }
    assert(false);
    std::unreachable();
}
} // namespace

Result<void> validate_tool(View name, const Json &j) { return validate_arguments(name, j); }

Result<void> validate_op(const Json &j, usize index) {
    const auto path = "ops[" + std::to_string(index) + "]";
    if (!j.is_object() || !j.contains("op")) return fail(path + ".op", "missing operation", "an operation name");
    const auto &name = j.at("op");
    if (!name.is_string()) return fail(path + ".op", "unknown operation", "a runtime_info ops entry");
    const OpSpec *spec = find_op_spec(name.get<std::string>());
    if (!spec) return fail(path + ".op", "unknown operation", "a runtime_info ops entry");
    constexpr usize max_fields = max_op_fields() + 1;
    std::array<Field, max_fields> fields{};
    fields[0] = {"op", text<4096>, true};
    for (usize i = 0; i < spec->fields.size(); ++i) {
        const auto &f = spec->fields[i];
        fields[i + 1] = {f.name, op_field_check(f.kind), f.required};
    }
    return object(j, path, std::span<const Field>(fields.data(), spec->fields.size() + 1));
}
} // namespace music::api::detail
