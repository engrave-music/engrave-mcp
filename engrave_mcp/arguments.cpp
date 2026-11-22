#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "api_json_internal.h"
#include "utf8_text.h"

namespace music::api::detail {
namespace {
using View = std::string_view;
using Check = void (*)(const Json &, const std::string &);

struct Field {
    View name;
    Check check;
    bool required = false;
};

struct ArgumentError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string &p, View why, View expected) {
    throw ArgumentError((p.empty() ? "arguments" : p) + ": " + std::string(why) + "; expected " + std::string(expected));
}

std::string child(const std::string &p, View key) { return p.empty() ? std::string(key) : p + "." + std::string(key); }

void object(const Json &j, const std::string &p, std::initializer_list<Field> fields) {
    if (!j.is_object()) fail(p, "wrong type", "an object");
    for (const auto &field : fields) {
        if (field.required && !j.contains(std::string(field.name))) fail(child(p, field.name), "missing field", "a value");
    }
    for (const auto &[key, v] : j.get_object()) {
        auto it = std::ranges::find(fields, View(key), &Field::name);
        if (it == fields.end()) fail(child(p, key), "unknown field", "a supported parameter");
        it->check(v, child(p, key));
    }
}

template <Check Item>
void list(const Json &j, const std::string &p) {
    if (!j.is_array()) fail(p, "wrong type", "an array");
    for (usize i = 0; i < j.size(); ++i) Item(j[i], p + "[" + std::to_string(i) + "]");
}

template <int Max>
void text(const Json &j, const std::string &p) {
    if (!j.is_string()) fail(p, "wrong type", "a string");
    const auto &s = j.get<std::string>();
    if (utf8_exceeds_length(s, Max)) {
        fail(p, "text too long", "at most " + std::to_string(Max) + " Unicode characters");
    }
}

template <int Min, int Max>
void integer(const Json &j, const std::string &p) {
    if ((!j.holds<u64>() && !j.holds<i64>()) || j.as<double>() < Min || j.as<double>() > Max) {
        fail(p, "invalid integer", std::to_string(Min) + ".." + std::to_string(Max));
    }
}

void choice(const Json &j, const std::string &p, std::initializer_list<View> choices) {
    if (!j.is_string() || std::ranges::find(choices, View(j.get<std::string>())) == choices.end()) {
        fail(p, "invalid choice", "a listed token");
    }
}

void boolean(const Json &j, const std::string &p) {
    if (!j.is_boolean()) fail(p, "wrong type", "true or false");
}

void any(const Json &, const std::string &) {}

void numeric(const Json &j, const std::string &p) {
    if (!j.is_number() || !std::isfinite(j.as<double>())) fail(p, "wrong type", "a finite number");
}

void filename(const Json &j, const std::string &p) {
    text<4096>(j, p);
    if (j.get<std::string>().empty() || j.get<std::string>().find('\0') != std::string::npos) {
        fail(p, "invalid path", "a nonempty path without NUL");
    }
}

void xml_text(const Json &j, const std::string &p) {
    text<4096>(j, p);
    if (!validate_xml_text(j.get<std::string>())) fail(p, "invalid XML character", "XML 1.0 text");
}

void check_eid(const Json &j, const std::string &p) {
    if (!eid(j)) fail(p, "invalid element ID", "a canonical EID");
}

void check_revision(const Json &j, const std::string &p) {
    if (!decode_revision(j)) fail(p, "invalid revision", "a canonical unsigned decimal string");
}

void check_fraction(const Json &j, const std::string &p) {
    if (!fraction(j)) fail(p, "invalid fraction", "0 or numerator/positive-denominator");
}

void check_pos(const Json &j, const std::string &p) {
    if (j.is_object() && !j.contains("kind")) {
        object(j, p,
               {{"measure", integer<1, INT32_MAX>, true},
                {"offset", check_fraction},
                {"staff", integer<1, INT32_MAX>},
                {"voice", integer<1, 4>}});
        return;
    }
    if (!j.is_object() || !j.contains("kind")) fail(child(p, "kind"), "missing address kind", "segment, page, system or spanner_segment");
    const auto &kind = j.at("kind");
    if (kind == "segment") {
        object(j, p,
               {{"kind", text<32>, true},
                {"measure", check_eid, true},
                {"staff", integer<1, INT32_MAX>, true},
                {"voice", integer<1, 4>, true},
                {"tick", check_fraction, true},
                {"segment_kind", text<128>, true}});
    } else if (kind == "page" || kind == "system") {
        if (kind == "system") {
            object(j, p,
                   {{"kind", text<32>, true},
                    {"excerpt", check_eid},
                    {"page", integer<0, INT32_MAX>, true},
                    {"index", integer<0, INT32_MAX>, true}});
        } else {
            object(j, p, {{"kind", text<32>, true}, {"excerpt", check_eid}, {"index", integer<0, INT32_MAX>, true}});
        }
    } else if (kind == "spanner_segment") {
        object(j, p, {{"kind", text<32>, true}, {"spanner", check_eid, true}, {"index", integer<0, INT32_MAX>, true}});
    } else {
        fail(child(p, "kind"), "unknown address kind", "segment, page, system or spanner_segment");
    }
}

void check_ref(const Json &j, const std::string &p) {
    if (j.is_string()) {
        check_eid(j, p);
    } else if (j.is_object() && j.contains("kind")) {
        check_pos(j, p);
    } else {
        object(j, p, {{"from_op", integer<0, 1023>, true}, {"item", integer<0, 65535>, true}});
    }
}

void score_position(const Json &j, const std::string &p) {
    if (j.is_number()) {
        integer<1, INT32_MAX>(j, p);
    } else {
        check_ref(j, p);
    }
}

void check_sf(const Json &j, const std::string &p) {
    if (j.is_string()) {
        text<4096>(j, p);
        if (j.get<std::string>().contains('\0')) fail(p, "invalid SoundFont selector", "a string without NUL");
    } else if (!j.is_null()) {
        integer<0, INT32_MAX>(j, p);
    }
}

void check_bit_depth(const Json &j, const std::string &p) {
    integer<0, 32>(j, p);
    if (j != 0 && j != 16 && j != 24 && j != 32) fail(p, "invalid bit depth", "0, 16, 24 or 32");
}

void check_format(const Json &j, const std::string &p) { choice(j, p, {"mscz", "mscx", "musicxml", "mxl", "midi", "guitarpro", "mnx"}); }

void check_output(const Json &j, const std::string &p) {
    choice(j, p,
           {"mscz", "mscx", "musicxml", "mxl", "midi", "pdf", "png", "svg", "wav", "flac", "opus", "mp3", "aac", "measure_positions",
            "segment_positions"});
}

void check_scope(const Json &j, const std::string &p) {
    choice(j, p, {"overview", "templates", "instruments", "types", "type", "styles", "ops"});
}

void check_mode(const Json &j, const std::string &p) { choice(j, p, {"elements", "properties", "page_count", "audio_tracks"}); }

void check_pitch_mode(const Json &j, const std::string &p) { choice(j, p, {"concert", "written"}); }

void check_grace(const Json &j, const std::string &p) {
    if (!j.is_string() || !grace_kind(j.get<std::string>())) fail(p, "invalid choice", "a listed grace kind");
}

void check_num(const Json &j, const std::string &p) { numeric(j, p); }

void check_volume(const Json &j, const std::string &p) {
    numeric(j, p);
    if (j.as<double>() < -120 || j.as<double>() > 24) fail(p, "out of range", "-120..24");
}

void check_balance(const Json &j, const std::string &p) {
    numeric(j, p);
    if (j.as<double>() < -1 || j.as<double>() > 1) fail(p, "out of range", "-1..1");
}

void check_track(const Json &j, const std::string &p) {
    object(j, p,
           {{"track", integer<0, 2147483647>, true},
            {"volume_db", check_volume},
            {"balance", check_balance},
            {"mute", boolean},
            {"solo", boolean},
            {"soundfont", check_sf},
            {"preset", text<4096>}});
}

void check_opt(const Json &j, const std::string &p) {
    object(j, p,
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
            {"reverb_send", check_num},
            {"sample_rate", integer<1, 384000>}});
}

void check_query(const Json &j, const std::string &p) {
    if (j.is_object() && (j.contains("first_measure") || j.contains("measure_count")) &&
        (j.contains("from_measure") || j.contains("to_measure"))) {
        fail(child(p, "first_measure"), "mixed selectors", "either measure numbers or EIDs");
    }
    object(j, p,
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

void check_midinote(const Json &j, const std::string &p) { object(j, p, {{"pitch", integer<0, 127>, true}, {"tpc", integer<-8, 40>}}); }

void check_prop(const Json &j, const std::string &p) { object(j, p, {{"name", text<4096>, true}, {"value", any, true}}); }

void validate_arguments(View name, const Json &j) {
    if (name == "score_open") {
        object(
            j, "",
            {{"path", filename}, {"data", text<11184812>}, {"format", check_format}, {"filename_hint", filename}, {"strict_ids", boolean}});
        if (j.contains("path") == j.contains("data")) fail("path", "ambiguous source", "exactly one of path or data");
        return;
    }
    if (name == "score_create") {
        object(j, "",
               {{"template_id", text<4096>},
                {"template_path", filename},
                {"title", xml_text},
                {"subtitle", xml_text},
                {"composer", xml_text},
                {"lyricist", xml_text}});
        if (j.contains("template_id") && j.contains("template_path")) {
            fail("template_path", "conflicting template", "only one template selector");
        }
        return;
    }
    if (name == "score_close") {
        object(j, "", {{"document", text<128>, true}});
        return;
    }
    if (name == "score_outline") {
        object(j, "", {{"document", text<128>, true}, {"expected_revision", check_revision}, {"excerpt", check_eid}});
        return;
    }
    if (name == "score_query") {
        object(j, "",
               {{"document", text<128>, true},
                {"expected_revision", check_revision},
                {"mode", check_mode, true},
                {"query", check_query},
                {"target", check_ref},
                {"props", list<text<4096>>},
                {"excerpt", check_eid}});
        auto mode = j.at("mode").get<std::string>();
        if (mode == "properties" && !j.contains("target")) fail("target", "missing field", "an EID or address");
        for (auto key : {"props", "target", "query", "excerpt"}) {
            if (j.contains(key) && !(mode == "properties" && (View(key) == "target" || View(key) == "props")) &&
                !(mode == "elements" && (View(key) == "query" || View(key) == "excerpt")) &&
                !((mode == "audio_tracks" || mode == "page_count") && View(key) == "excerpt")) {
                fail(key, "invalid for mode", mode);
            }
        }
        if (mode == "elements" && j.contains("excerpt") && j.contains("query") && j.at("query").contains("excerpt") &&
            j.at("excerpt") != j.at("query").at("excerpt")) {
            fail("excerpt", "conflicting score selectors", "matching excerpt IDs");
        }
        return;
    }
    if (name == "score_read_music") {
        object(j, "",
               {{"document", text<128>, true},
                {"expected_revision", check_revision},
                {"excerpt", check_eid},
                {"measure", integer<1, 2147483647>},
                {"count", integer<1, 128>},
                {"staves", list<integer<1, 2147483647>>},
                {"voices", list<integer<1, 4>>},
                {"include_ids", boolean}});
        return;
    }
    if (name == "score_edit") {
        object(j, "",
               {{"document", text<128>, true},
                {"expected_revision", check_revision},
                {"atomic", boolean},
                {"detail", boolean},
                {"ops", list<any>, true}});
        return;
    }
    if (name == "score_export") {
        object(j, "",
               {{"document", text<128>, true},
                {"expected_revision", check_revision},
                {"format", check_output, true},
                {"path", filename, true},
                {"excerpt", check_eid},
                {"options", check_opt}});
        return;
    }
    if (name == "runtime_info") {
        object(j, "", {{"scope", check_scope}, {"query", text<4096>}, {"type", text<4096>}, {"op", text<4096>}});
        if (j.contains("type") != (j.contains("scope") && j.at("scope") == "type")) {
            fail("type", "invalid selector", "type only with scope type");
        }
        if (j.contains("op") && (!j.contains("scope") || j.at("scope") != "ops")) fail("op", "invalid selector", "scope ops");
        if (j.contains("query") && (!j.contains("scope") || j.at("scope") != "instruments")) {
            fail("query", "invalid selector", "scope instruments");
        }
        return;
    }
    if (name == "checkpoint") {
        object(j, "", {{"document", text<128>, true}, {"expected_revision", check_revision}, {"name", text<4096>, true}});
        return;
    }
    if (name == "restore_checkpoint") {
        object(j, "", {{"document", text<128>, true}, {"expected_revision", check_revision}, {"name", text<4096>, true}});
        return;
    }
    if (name == "drop_checkpoint") {
        object(j, "", {{"document", text<128>, true}, {"expected_revision", check_revision}, {"name", text<4096>, true}});
        return;
    }
    if (name == "score_write_phrase") {
        object(j, "",
               {{"document", text<128>, true}, {"expected_revision", check_revision}, {"excerpt", check_eid}, {"lanes", list<any>, true}});
        return;
    }
    if (name == "runtime_configure") {
        object(j, "", {{"add_font", filename}, {"soundfonts", list<filename>}});
        if (j.empty()) fail("add_font", "empty configuration", "add_font or soundfonts");
        return;
    }
    throw std::logic_error("unknown tool decoder");
}
} // namespace

Result<void> validate_tool(View name, const Json &j) {
    try {
        validate_arguments(name, j);
        return {};
    } catch (const ArgumentError &e) {
        return std::unexpected(invalid_error(e.what()));
    }
}

Result<void> validate_op(const Json &j, usize index) {
    try {
        const auto path = "ops[" + std::to_string(index) + "]";
        if (!j.is_object() || !j.contains("op")) fail(path + ".op", "missing operation", "an operation name");
        const auto &name = j.at("op");
        if (name == "set_property") {
            object(j, path, {{"op", text<4096>, true}, {"target", check_ref, true}, {"property", text<4096>, true}, {"value", any, true}});
            return {};
        }
        if (name == "reset_property") {
            object(j, path, {{"op", text<4096>, true}, {"target", check_ref, true}, {"property", text<4096>, true}});
            return {};
        }
        if (name == "set_style") {
            object(j, path, {{"op", text<4096>, true}, {"excerpt", check_eid}, {"style", text<4096>, true}, {"value", any, true}});
            return {};
        }
        if (name == "enter_notes") {
            object(j, path,
                   {{"op", text<4096>, true},
                    {"at", check_pos, true},
                    {"duration", check_fraction, true},
                    {"pitches", list<check_midinote>, true}});
            return {};
        }
        if (name == "set_pitch") {
            object(j, path,
                   {{"op", text<4096>, true}, {"note", check_ref, true}, {"pitch", integer<0, 127>, true}, {"tpc", integer<-8, 40>}});
            return {};
        }
        if (name == "set_duration") {
            object(j, path, {{"op", text<4096>, true}, {"chord_rest", check_ref, true}, {"duration", check_fraction, true}});
            return {};
        }
        if (name == "remove") {
            object(j, path, {{"op", text<4096>, true}, {"target", check_ref, true}});
            return {};
        }
        if (name == "insert_measures") {
            object(j, path, {{"op", text<4096>, true}, {"before", check_ref}, {"count", integer<1, 1024>, true}, {"kind", text<4096>}});
            return {};
        }
        if (name == "remove_measures") {
            object(j, path, {{"op", text<4096>, true}, {"first", check_ref, true}, {"last", check_ref, true}});
            return {};
        }
        if (name == "set_time_sig") {
            object(j, path, {{"op", text<4096>, true}, {"measure", score_position, true}, {"sig", check_fraction, true}});
            return {};
        }
        if (name == "set_key_sig") {
            object(j, path,
                   {{"op", text<4096>, true}, {"measure", score_position, true}, {"key", integer<-7, 7>, true}, {"staff", score_position}});
            return {};
        }
        if (name == "insert_clef") {
            object(j, path, {{"op", text<4096>, true}, {"at", check_pos, true}, {"clef", text<4096>, true}});
            return {};
        }
        if (name == "set_layout_break") {
            object(j, path,
                   {{"op", text<4096>, true}, {"measure", score_position, true}, {"kind", text<4096>, true}, {"on", boolean, true}});
            return {};
        }
        if (name == "add_part") {
            object(j, path, {{"op", text<4096>, true}, {"instrument_id", text<4096>, true}, {"before", check_ref}});
            return {};
        }
        if (name == "remove_part") {
            object(j, path, {{"op", text<4096>, true}, {"part", check_ref, true}});
            return {};
        }
        if (name == "add_staff") {
            object(j, path, {{"op", text<4096>, true}, {"part", check_ref, true}});
            return {};
        }
        if (name == "remove_staff") {
            object(j, path, {{"op", text<4096>, true}, {"staff", score_position, true}});
            return {};
        }
        if (name == "generate_excerpts") {
            object(j, path, {{"op", text<4096>, true}, {"parts", list<check_ref>, true}});
            return {};
        }
        if (name == "add_spanner") {
            object(j, path,
                   {{"op", text<4096>, true},
                    {"type", text<4096>, true},
                    {"start", check_ref, true},
                    {"end", check_ref, true},
                    {"props", list<check_prop>}});
            return {};
        }
        if (name == "add_tie") {
            object(j, path, {{"op", text<4096>, true}, {"note", check_ref, true}, {"end_note", check_ref}});
            return {};
        }
        if (name == "add_tuplet") {
            object(j, path,
                   {{"op", text<4096>, true},
                    {"first", check_ref, true},
                    {"last", check_ref, true},
                    {"actual", integer<1, 1024>},
                    {"normal", integer<1, 1024>}});
            return {};
        }
        if (name == "add_text") {
            object(j, path, {{"op", text<4096>, true}, {"at", check_pos, true}, {"style", text<4096>, true}, {"text", text<65536>, true}});
            return {};
        }
        if (name == "add") {
            object(j, path,
                   {{"op", text<4096>, true}, {"anchor", check_ref, true}, {"type", text<4096>, true}, {"props", list<check_prop>}});
            return {};
        }
        if (name == "add_spanner_to_bar_end") {
            object(j, path,
                   {{"op", text<4096>, true},
                    {"type", text<4096>, true},
                    {"start", check_ref, true},
                    {"end", score_position, true},
                    {"props", list<check_prop>}});
            return {};
        }
        if (name == "set_repeat_barlines") {
            object(j, path,
                   {{"op", text<4096>, true},
                    {"measure", score_position, true},
                    {"start", boolean},
                    {"end", boolean},
                    {"count", integer<1, 1024>}});
            return {};
        }
        if (name == "add_volta") {
            object(j, path,
                   {{"op", text<4096>, true},
                    {"first_measure", score_position, true},
                    {"last_measure", score_position, true},
                    {"endings", list<integer<1, 1024>>, true},
                    {"closed", boolean}});
            return {};
        }
        if (name == "add_grace_chord") {
            object(j, path,
                   {{"op", text<4096>, true},
                    {"principal", check_ref, true},
                    {"kind", check_grace, true},
                    {"duration", check_fraction, true},
                    {"pitches", list<check_midinote>, true},
                    {"event_end", check_fraction}});
            return {};
        }
        if (name == "enter_drums") {
            object(j, path,
                   {{"op", text<4096>, true},
                    {"at", check_pos, true},
                    {"duration", check_fraction, true},
                    {"keys", list<integer<0, 127>>, true}});
            return {};
        }
        fail(path + ".op", "unknown operation", "a runtime_info ops entry");
    } catch (const ArgumentError &e) {
        return std::unexpected(invalid_error(e.what()));
    }
}
} // namespace music::api::detail
