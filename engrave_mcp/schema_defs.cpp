#include "schema_defs.h"

#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace music::api {
namespace {

using SchemaBuilder = Json (*)();

struct FieldSpec {
    std::string_view key;
    SchemaBuilder build;
};

struct ToolDef {
    const char *name;
    const char *description;
    SchemaBuilder schema;
    bool read;
    bool destructive;
};

[[gnu::noinline]] Json str_list(std::initializer_list<std::string_view> values) {
    Json arr = make_array<Json>();
    auto &items = arr.get_array();
    items.reserve(values.size());
    for (const std::string_view v : values) items.emplace_back(v);
    return arr;
}

[[gnu::noinline]] Json int_list(std::initializer_list<int> values) {
    Json arr = make_array<Json>();
    auto &items = arr.get_array();
    items.reserve(values.size());
    for (const int v : values) items.emplace_back(v);
    return arr;
}

[[gnu::noinline]] Json schema_array(Json item) {
    Json out = make_object<Json>();
    out["type"] = "array";
    out["items"] = std::move(item);
    return out;
}

[[gnu::noinline]] Json choices(std::initializer_list<std::string_view> values) {
    Json out = make_object<Json>();
    out["type"] = "string";
    out["enum"] = str_list(values);
    return out;
}

[[gnu::noinline]] Json alternatives(std::initializer_list<SchemaBuilder> builders, const char *description) {
    Json arr = make_array<Json>();
    auto &items = arr.get_array();
    items.reserve(builders.size());
    for (const SchemaBuilder build : builders) items.push_back(build());

    Json out = make_object<Json>();
    out["anyOf"] = std::move(arr);
    out["description"] = description;
    return out;
}

[[gnu::noinline]] Json integer(int low, int high) {
    Json out = make_object<Json>();
    out["type"] = "integer";
    out["minimum"] = low;
    out["maximum"] = high;
    return out;
}

[[gnu::noinline]] Json number_range(int low, int high) {
    Json out = make_object<Json>();
    out["type"] = "number";
    out["minimum"] = low;
    out["maximum"] = high;
    return out;
}

[[gnu::noinline]] Json object_from(std::span<const FieldSpec> fields, std::initializer_list<std::string_view> required) {
    Json props = make_object<Json>();
    for (const FieldSpec &field : fields) props[field.key] = field.build();

    Json out = make_object<Json>();
    out["type"] = "object";
    out["properties"] = std::move(props);
    out["required"] = str_list(required);
    out["additionalProperties"] = false;
    return out;
}

[[gnu::noinline]] Json described(Json schema, const char *description) {
    schema["description"] = description;
    return schema;
}

[[gnu::noinline]] Json str_max(int max_length) {
    Json out = make_object<Json>();
    out["type"] = "string";
    out["maxLength"] = max_length;
    return out;
}

[[gnu::noinline]] Json primitive_type(const char *type_name) {
    Json out = make_object<Json>();
    out["type"] = type_name;
    return out;
}

Json text_schema() { return str_max(4096); }

Json long_text_schema() { return str_max(65536); }

Json data_schema() { return str_max(11184812); }

Json eid_schema() { return str_max(32); }

Json document_schema() { return str_max(128); }

// The published schemas carry no $defs, so every position repeats its shapes inline; they stay undescribed
// and score_edit's description explains them once.
Json fraction_schema() { return str_max(42); }

Json offset_fraction_schema() { return described(fraction_schema(), "Exact whole-note fraction, e.g. 3/8."); }

Json time_sig_schema() { return described(fraction_schema(), "Written time signature, e.g. 6/8; keeps its numerator and denominator."); }

Json revision_schema() {
    Json out = make_object<Json>();
    out["type"] = "string";
    out["pattern"] = "^(0|[1-9][0-9]*)$";
    out["maxLength"] = 20;
    return out;
}

Json boolean_schema() { return primitive_type("boolean"); }

Json num_schema() { return primitive_type("number"); }

Json raw_string_schema() { return primitive_type("string"); }

Json int_array_schema() { return schema_array(primitive_type("integer")); }

Json object_type_schema() { return primitive_type("object"); }

Json null_type_schema() { return primitive_type("null"); }

Json idx_schema() { return integer(0, 2147483647); }

Json bar_schema() { return integer(1, 2147483647); }

Json count_schema() { return integer(1, 128); }

Json voice_schema() { return integer(1, 4); }

Json key_schema() { return integer(-7, 7); }

Json pitch_schema() { return integer(0, 127); }

Json tpc_schema() { return integer(-8, 40); }

Json n1024_schema() { return integer(1, 1024); }

Json from_op_schema() { return integer(0, 1023); }

Json op_item_schema() { return integer(0, 65535); }

Json volume_schema() { return number_range(-120, 24); }

Json balance_schema() { return number_range(-1, 1); }

Json dpi_schema() { return integer(36, 1200); }

Json last_page_schema() { return integer(-1, 2147483647); }

Json limit_schema() { return integer(1, 4096); }

Json cursor_schema() { return integer(0, 4096); }

Json bitrate_schema() { return integer(0, 512); }

Json sample_rate_schema() { return integer(1, 384000); }

Json text_array_schema() { return schema_array(text_schema()); }

Json bar_array_schema() { return schema_array(bar_schema()); }

Json voice_array_schema() { return schema_array(voice_schema()); }

Json n1024_array_schema() { return schema_array(n1024_schema()); }

Json pitch_array_schema() { return schema_array(pitch_schema()); }

Json format_schema() { return choices({"mscz", "mscx", "musicxml", "mxl", "midi", "guitarpro", "mnx"}); }

Json output_format_schema() {
    return choices({"mscz", "mscx", "musicxml", "mxl", "midi", "pdf", "png", "svg", "wav", "flac", "opus", "mp3", "measure_positions",
                    "segment_positions"});
}

Json scope_schema() { return choices({"overview", "templates", "instruments", "types", "type", "styles", "ops"}); }

Json mode_schema() { return choices({"elements", "properties", "page_count", "audio_tracks"}); }

Json pitch_mode_schema() { return choices({"concert", "written"}); }

Json segment_kind_tag_schema() { return choices({"segment"}); }

Json value_schema() {
    return alternatives({boolean_schema, num_schema, raw_string_schema, int_array_schema, object_type_schema, null_type_schema},
                        "Native JSON value for the property or style.");
}

Json query_target_schema() { return alternatives({eid_schema, object_type_schema}, "Persistent EID or layout address."); }

// Edit operands never accept page, system or spanner_segment addresses: the engine refuses to edit layout.
Json ref_schema() { return alternatives({eid_schema, object_type_schema}, "EID, segment address or {from_op,item}."); }

Json op_ref_schema() {
    static constexpr FieldSpec fields[] = {
        {"from_op", from_op_schema},
        {"item", op_item_schema},
    };
    return object_from(fields, {"from_op", "item"});
}

Json compact_pos_schema() {
    static constexpr FieldSpec fields[] = {
        {"measure", bar_schema},
        {"staff", bar_schema},
        {"voice", voice_schema},
        {"offset", fraction_schema},
    };
    return object_from(fields, {"measure"});
}

Json segment_addr_schema() {
    static constexpr FieldSpec fields[] = {
        {"kind", segment_kind_tag_schema}, {"measure", eid_schema},   {"staff", bar_schema},
        {"voice", voice_schema},           {"tick", fraction_schema}, {"segment_kind", document_schema},
    };
    return object_from(fields, {"kind", "measure", "staff", "voice", "tick", "segment_kind"});
}

Json measure_ref_schema() { return alternatives({bar_schema, eid_schema, op_ref_schema}, "Bar number, measure EID or {from_op,item}."); }

Json staff_ref_schema() { return alternatives({bar_schema, eid_schema, op_ref_schema}, "Staff number, staff EID or {from_op,item}."); }

Json part_ref_schema() { return alternatives({eid_schema, op_ref_schema}, "Part EID or {from_op,item}."); }

Json part_ref_array_schema() { return schema_array(part_ref_schema()); }

Json anchor_schema() {
    return alternatives({eid_schema, compact_pos_schema, segment_addr_schema, op_ref_schema}, "Position, EID or {from_op,item}.");
}

Json pos_schema() { return alternatives({compact_pos_schema, segment_addr_schema}, "Compact position or segment address."); }

Json soundfont_schema() { return alternatives({idx_schema, text_schema, null_type_schema}, "SoundFont index, path, name, or null."); }

Json bit_depth_schema() {
    Json out = make_object<Json>();
    out["type"] = "integer";
    out["enum"] = int_list({0, 16, 24, 32});
    return out;
}

Json track_array_schema() {
    static constexpr FieldSpec fields[] = {
        {"track", idx_schema},    {"volume_db", volume_schema},    {"balance", balance_schema}, {"mute", boolean_schema},
        {"solo", boolean_schema}, {"soundfont", soundfont_schema}, {"preset", text_schema},
    };
    return schema_array(object_from(fields, {"track"}));
}

Json export_options_schema() {
    static constexpr FieldSpec fields[] = {
        {"midi_expand_repeats", boolean_schema},
        {"midi_rpns", boolean_schema},
        {"native_version", text_schema},
        {"first_page", idx_schema},
        {"last_page", last_page_schema},
        {"dpi", dpi_schema},
        {"background", boolean_schema},
        {"transparent", boolean_schema},
        {"bitrate_kbps", bitrate_schema},
        {"bit_depth", bit_depth_schema},
        {"use_repeats", boolean_schema},
        {"master_volume_db", volume_schema},
        {"master_balance", balance_schema},
        {"tracks", track_array_schema},
        {"reverb", boolean_schema},
        {"reverb_send", num_schema},
        {"sample_rate", sample_rate_schema},
    };
    return object_from(fields, {});
}

Json query_schema() {
    static constexpr FieldSpec fields[] = {
        {"excerpt", eid_schema},
        {"types", text_array_schema},
        {"first_measure", bar_schema},
        {"measure_count", count_schema},
        {"from_measure", eid_schema},
        {"to_measure", eid_schema},
        {"staves", bar_array_schema},
        {"voices", voice_array_schema},
        {"from_tick", offset_fraction_schema},
        {"to_tick", offset_fraction_schema},
        {"props", text_array_schema},
        {"layout_boxes", boolean_schema},
        {"limit", limit_schema},
        {"cursor", cursor_schema},
    };
    return object_from(fields, {});
}

Json lane_content_schema() {
    Json content = long_text_schema();
    content["description"] = std::string(phrase_grammar) + " " + phrase_grace_defaults;
    return content;
}

Json lanes_schema() {
    static constexpr FieldSpec fields[] = {
        {"measure", bar_schema}, {"offset", offset_fraction_schema}, {"staff", bar_schema},
        {"voice", voice_schema}, {"pitch_mode", pitch_mode_schema},  {"content", lane_content_schema},
    };
    Json lanes = schema_array(object_from(fields, {"measure", "content"}));
    lanes["minItems"] = 1;
    return lanes;
}

Json op_name_schema() {
    return choices({"set_property",
                    "reset_property",
                    "set_style",
                    "enter_notes",
                    "set_pitch",
                    "set_duration",
                    "remove",
                    "insert_measures",
                    "remove_measures",
                    "set_time_sig",
                    "set_key_sig",
                    "insert_clef",
                    "set_layout_break",
                    "add_part",
                    "remove_part",
                    "add_staff",
                    "remove_staff",
                    "generate_excerpts",
                    "add_spanner",
                    "add_tie",
                    "add_tuplet",
                    "add_text",
                    "add",
                    "add_spanner_to_bar_end",
                    "set_repeat_barlines",
                    "add_volta",
                    "add_grace_chord",
                    "enter_drums"});
}

Json pitches_array_schema() {
    static constexpr FieldSpec fields[] = {
        {"pitch", pitch_schema},
        {"tpc", tpc_schema},
    };
    return schema_array(object_from(fields, {"pitch"}));
}

Json named_props_array_schema() {
    static constexpr FieldSpec fields[] = {
        {"name", text_schema},
        {"value", value_schema},
    };
    return schema_array(object_from(fields, {"name", "value"}));
}

Json op_before_schema() {
    return alternatives({bar_schema, eid_schema, op_ref_schema}, "insert_measures: as measure. add_part: part EID or {from_op,item}.");
}

Json op_count_schema() { return described(n1024_schema(), "set_repeat_barlines: passes, at least 2."); }

Json op_kind_schema() {
    return described(text_schema(), "Measure kind (Measure|VBox|HBox|TBox|FBox), layout break (line|page|section|nobreak), or grace "
                                    "kind (acciac|appog|grace4|grace16|grace32|after8|after16|after32).");
}

Json op_first_last_schema() {
    return alternatives({bar_schema, eid_schema, segment_addr_schema, op_ref_schema},
                        "remove_measures: as measure. add_tuplet: as target.");
}

Json op_start_schema() {
    return alternatives({boolean_schema, eid_schema, compact_pos_schema, segment_addr_schema, op_ref_schema},
                        "set_repeat_barlines: flag. Spanners: as anchor.");
}

Json op_end_schema() {
    return alternatives({boolean_schema, bar_schema, eid_schema, compact_pos_schema, segment_addr_schema, op_ref_schema},
                        "set_repeat_barlines: flag. add_spanner: as anchor. add_spanner_to_bar_end: as measure.");
}

Json ops_schema() {
    static constexpr FieldSpec fields[] = {
        {"op", op_name_schema},
        {"target", ref_schema},
        {"property", text_schema},
        {"value", value_schema},
        {"excerpt", eid_schema},
        {"style", text_schema},
        {"at", pos_schema},
        {"duration", offset_fraction_schema},
        {"pitches", pitches_array_schema},
        {"note", ref_schema},
        {"pitch", pitch_schema},
        {"tpc", tpc_schema},
        {"chord_rest", ref_schema},
        {"before", op_before_schema},
        {"count", op_count_schema},
        {"kind", op_kind_schema},
        {"first", op_first_last_schema},
        {"last", op_first_last_schema},
        {"measure", measure_ref_schema},
        {"sig", time_sig_schema},
        {"key", key_schema},
        {"staff", staff_ref_schema},
        {"clef", text_schema},
        {"on", boolean_schema},
        {"instrument_id", text_schema},
        {"part", part_ref_schema},
        {"parts", part_ref_array_schema},
        {"type", text_schema},
        {"start", op_start_schema},
        {"end", op_end_schema},
        {"props", named_props_array_schema},
        {"end_note", ref_schema},
        {"actual", n1024_schema},
        {"normal", n1024_schema},
        {"text", long_text_schema},
        {"anchor", anchor_schema},
        {"first_measure", measure_ref_schema},
        {"last_measure", measure_ref_schema},
        {"endings", n1024_array_schema},
        {"closed", boolean_schema},
        {"principal", ref_schema},
        {"event_end", offset_fraction_schema},
        {"keys", pitch_array_schema},
    };
    Json ops = schema_array(object_from(fields, {"op"}));
    ops["maxItems"] = 1024;
    return ops;
}

Json score_open_schema() {
    static constexpr FieldSpec fields[] = {
        {"path", text_schema},          {"data", data_schema},          {"format", format_schema},
        {"filename_hint", text_schema}, {"strict_ids", boolean_schema},
    };
    return object_from(fields, {});
}

Json score_create_schema() {
    static constexpr FieldSpec fields[] = {
        {"template_id", text_schema}, {"template_path", text_schema}, {"title", text_schema},
        {"subtitle", text_schema},    {"composer", text_schema},      {"lyricist", text_schema},
    };
    return object_from(fields, {});
}

Json score_close_schema() {
    static constexpr FieldSpec fields[] = {
        {"document", document_schema},
    };
    return object_from(fields, {"document"});
}

Json score_outline_schema() {
    static constexpr FieldSpec fields[] = {
        {"document", document_schema},
        {"expected_revision", revision_schema},
        {"excerpt", eid_schema},
    };
    return object_from(fields, {"document"});
}

Json score_query_schema() {
    static constexpr FieldSpec fields[] = {
        {"document", document_schema},   {"expected_revision", revision_schema}, {"mode", mode_schema},   {"query", query_schema},
        {"target", query_target_schema}, {"props", text_array_schema},           {"excerpt", eid_schema},
    };
    return object_from(fields, {"document", "mode"});
}

Json score_read_music_schema() {
    static constexpr FieldSpec fields[] = {
        {"document", document_schema},  {"expected_revision", revision_schema},
        {"excerpt", eid_schema},        {"measure", bar_schema},
        {"count", count_schema},        {"staves", bar_array_schema},
        {"voices", voice_array_schema}, {"include_ids", boolean_schema},
    };
    return object_from(fields, {"document"});
}

Json score_edit_schema() {
    static constexpr FieldSpec fields[] = {
        {"document", document_schema}, {"expected_revision", revision_schema},
        {"excerpt", eid_schema},       {"atomic", boolean_schema},
        {"detail", boolean_schema},    {"ops", ops_schema},
    };
    return object_from(fields, {"document", "ops"});
}

Json score_export_schema() {
    static constexpr FieldSpec fields[] = {
        {"document", document_schema}, {"expected_revision", revision_schema}, {"format", output_format_schema}, {"path", text_schema},
        {"excerpt", eid_schema},       {"options", export_options_schema},
    };
    return object_from(fields, {"document", "format", "path"});
}

Json runtime_info_schema() {
    static constexpr FieldSpec fields[] = {
        {"scope", scope_schema},
        {"query", text_schema},
        {"type", text_schema},
        {"op", text_schema},
    };
    return object_from(fields, {});
}

Json checkpoint_schema() {
    static constexpr FieldSpec fields[] = {
        {"document", document_schema},
        {"expected_revision", revision_schema},
        {"name", text_schema},
    };
    return object_from(fields, {"document", "name"});
}

Json score_write_phrase_schema() {
    static constexpr FieldSpec fields[] = {
        {"document", document_schema},
        {"expected_revision", revision_schema},
        {"excerpt", eid_schema},
        {"lanes", lanes_schema},
    };
    return object_from(fields, {"document", "lanes"});
}

Json runtime_configure_schema() {
    static constexpr FieldSpec fields[] = {
        {"add_font", text_schema},
        {"soundfonts", text_array_schema},
    };
    return object_from(fields, {});
}

[[gnu::noinline]] Json build_tool(const ToolDef &def) {
    Json annotations = make_object<Json>();
    annotations["readOnlyHint"] = def.read;
    annotations["destructiveHint"] = def.destructive;
    annotations["idempotentHint"] = def.read;
    annotations["openWorldHint"] = false;

    Json entry = make_object<Json>();
    entry["name"] = def.name;
    entry["description"] = def.description;
    entry["inputSchema"] = def.schema();
    entry["annotations"] = std::move(annotations);
    return entry;
}

} // namespace

Json declared_tools() {
    static constexpr ToolDef defs[] = {
        {"score_open", "Open one path or base64 data and return its document handle and outline.", score_open_schema, false, false},
        {"score_create", "Create a score from a template. The default is Treble_Clef.", score_create_schema, false, false},
        {"score_close", "Release this document and its checkpoints.", score_close_schema, false, true},
        {"score_outline", "Read score metadata, staves, measures and signatures.", score_outline_schema, true, false},
        {"score_query", "Inspect elements, properties, page count or audio tracks.", score_query_schema, true, false},
        {"score_read_music", "Read notation by bar, staff and voice. Returns a musical summary and structured events.",
         score_read_music_schema, true, false},
        {"score_edit",
         "Apply a batch atomically by default. Use runtime_info scope ops for operation shapes. excerpt scopes the batch to one "
         "excerpt. Bar, staff and voice numbers are 1-based and resolve when their op runs. A position is a compact "
         "{measure,staff?,voice?,offset?} with a bar-local offset, or a segment address with a score-wide tick; {from_op,item} "
         "names an item an earlier op created.",
         score_edit_schema, false, true},
        {"score_export", "Write a score, image, audio file or position map. PNG and SVG paths name directories of page files.",
         score_export_schema, false, true},
        {"runtime_info", "Inspect capabilities, limits and selected engine catalogues.", runtime_info_schema, true, false},
        {"checkpoint", "Save a named in-memory snapshot.", checkpoint_schema, false, true},
        {"restore_checkpoint", "Restore a named snapshot and advance the revision.", checkpoint_schema, false, true},
        {"drop_checkpoint", "Release a named snapshot.", checkpoint_schema, false, true},
        {"score_write_phrase", "Enter one or more musical lanes together in an atomic edit.", score_write_phrase_schema, false, true},
        {"runtime_configure", "Add a font or replace the ordered SoundFont list.", runtime_configure_schema, false, true},
    };
    Json tools = make_array<Json>();
    auto &items = tools.get_array();
    items.reserve(std::size(defs));
    for (const ToolDef &def : defs) items.push_back(build_tool(def));
    return tools;
}

} // namespace music::api
