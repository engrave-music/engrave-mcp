#include "schema_defs.h"

namespace music::api {
namespace {
Json schema_array(const Json &item) { return {{"type", "array"}, {"items", item}}; }

Json choices(std::initializer_list<Json> values) { return {{"type", "string"}, {"enum", json_array<Json>(values)}}; }

Json alternatives(std::initializer_list<Json> schemas, const char *description) {
    return {{"anyOf", json_array<Json>(schemas)}, {"description", description}};
}

Json integer(int low, int high) { return {{"type", "integer"}, {"minimum", low}, {"maximum", high}}; }

Json object(Json props, Json required) {
    return {{"type", "object"}, {"properties", std::move(props)}, {"required", std::move(required)}, {"additionalProperties", false}};
}
} // namespace

Json declared_tools() {
    const Json text = Json{{"type", "string"}, {"maxLength", 4096}};
    const Json long_text = Json{{"type", "string"}, {"maxLength", 65536}};
    const Json data = Json{{"type", "string"}, {"maxLength", 11184812}};
    const Json revision = Json{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}};
    const Json eid = Json{{"type", "string"}, {"maxLength", 32}};
    const Json document = Json{{"type", "string"}, {"maxLength", 128}};
    // The published schemas carry no $defs, so every position repeats its shapes inline; they stay undescribed
    // and score_edit's description explains them once.
    const Json fraction = Json{{"type", "string"}, {"maxLength", 42}};
    auto described = [](Json schema, const char *description) {
        schema["description"] = description;
        return schema;
    };
    const Json offset_fraction = described(fraction, "Exact whole-note fraction, e.g. 3/8.");
    const Json time_sig = described(fraction, "Written time signature, e.g. 6/8; keeps its numerator and denominator.");
    const Json boolean = Json{{"type", "boolean"}};
    const Json idx = integer(0, 2147483647);
    const Json bar = integer(1, 2147483647);
    const Json count = integer(1, 128);
    const Json voice = integer(1, 4);
    const Json key = integer(-7, 7);
    const Json pitch = integer(0, 127);
    const Json tpc = integer(-8, 40);
    const Json n1024 = integer(1, 1024);
    const Json format = choices({"mscz", "mscx", "musicxml", "mxl", "midi", "guitarpro", "mnx"});
    const Json output = choices({"mscz", "mscx", "musicxml", "mxl", "midi", "pdf", "png", "svg", "wav", "flac", "opus", "mp3",
                                 "measure_positions", "segment_positions"});
    const Json scope = choices({"overview", "templates", "instruments", "types", "type", "styles", "ops"});
    const Json mode = choices({"elements", "properties", "page_count", "audio_tracks"});
    const Json pitch_mode = choices({"concert", "written"});
    const Json num = Json{{"type", "number"}};
    const Json object_type = Json{{"type", "object"}};
    const Json null_type = Json{{"type", "null"}};
    const Json value =
        alternatives({boolean, num, Json{{"type", "string"}}, schema_array(Json{{"type", "integer"}}), object_type, null_type},
                     "Native JSON value for the property or style.");
    const Json query_target = alternatives({eid, object_type}, "Persistent EID or layout address.");
    // Edit operands never accept page, system or spanner_segment addresses: the engine refuses to edit layout.
    const Json ref = alternatives({eid, object_type}, "EID, segment address or {from_op,item}.");
    const Json op_ref = object({{"from_op", integer(0, 1023)}, {"item", integer(0, 65535)}}, json_array<Json>({"from_op", "item"}));
    const Json compact_pos =
        object({{"measure", bar}, {"staff", bar}, {"voice", voice}, {"offset", fraction}}, json_array<Json>({"measure"}));
    const Json segment_addr = object({{"kind", choices({"segment"})},
                                      {"measure", eid},
                                      {"staff", bar},
                                      {"voice", voice},
                                      {"tick", fraction},
                                      {"segment_kind", Json{{"type", "string"}, {"maxLength", 128}}}},
                                     json_array<Json>({"kind", "measure", "staff", "voice", "tick", "segment_kind"}));
    const Json measure_ref = alternatives({bar, eid, op_ref}, "Bar number, measure EID or {from_op,item}.");
    const Json staff_ref = alternatives({bar, eid, op_ref}, "Staff number, staff EID or {from_op,item}.");
    const Json part_ref = alternatives({eid, op_ref}, "Part EID or {from_op,item}.");
    const Json anchor = alternatives({eid, compact_pos, segment_addr, op_ref}, "Position, EID or {from_op,item}.");
    const Json pos = alternatives({compact_pos, segment_addr}, "Compact position or segment address.");
    const Json volume = Json{{"type", "number"}, {"minimum", -120}, {"maximum", 24}};
    const Json balance = Json{{"type", "number"}, {"minimum", -1}, {"maximum", 1}};
    const Json dpi = integer(36, 1200);
    const Json last = integer(-1, 2147483647);
    const Json limit = integer(1, 4096);
    const Json cursor = integer(0, 4096);
    const Json sf = alternatives({idx, text, null_type}, "SoundFont index, path, name, or null.");
    const Json bit_depth = Json{{"type", "integer"}, {"enum", json_array<Json>({0, 16, 24, 32})}};
    const Json bitrate = integer(0, 512);
    const Json sample_rate = integer(1, 384000);
    const Json track = object({{"track", idx},
                               {"volume_db", volume},
                               {"balance", balance},
                               {"mute", boolean},
                               {"solo", boolean},
                               {"soundfont", sf},
                               {"preset", text}},
                              json_array<Json>({"track"}));
    const Json opt = object({{"midi_expand_repeats", boolean},
                             {"midi_rpns", boolean},
                             {"native_version", text},
                             {"first_page", idx},
                             {"last_page", last},
                             {"dpi", dpi},
                             {"background", boolean},
                             {"transparent", boolean},
                             {"bitrate_kbps", bitrate},
                             {"bit_depth", bit_depth},
                             {"use_repeats", boolean},
                             {"master_volume_db", volume},
                             {"master_balance", balance},
                             {"tracks", schema_array(track)},
                             {"reverb", boolean},
                             {"reverb_send", num},
                             {"sample_rate", sample_rate}},
                            json_array<Json>({}));
    const Json query = object({{"excerpt", eid},
                               {"types", schema_array(text)},
                               {"first_measure", bar},
                               {"measure_count", count},
                               {"from_measure", eid},
                               {"to_measure", eid},
                               {"staves", schema_array(bar)},
                               {"voices", schema_array(voice)},
                               {"from_tick", offset_fraction},
                               {"to_tick", offset_fraction},
                               {"props", schema_array(text)},
                               {"layout_boxes", boolean},
                               {"limit", limit},
                               {"cursor", cursor}},
                              json_array<Json>({}));
    auto content = long_text;
    content["description"] = std::string(phrase_grammar) + " " + phrase_grace_defaults;
    const Json lane = object(
        {{"measure", bar}, {"offset", offset_fraction}, {"staff", bar}, {"voice", voice}, {"pitch_mode", pitch_mode}, {"content", content}},
        json_array<Json>({"measure", "content"}));
    auto lanes = schema_array(lane);
    lanes["minItems"] = 1;
    Json op_fields = make_object<Json>();
    op_fields["op"] = choices({"set_property",
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
    op_fields["target"] = ref;
    op_fields["property"] = text;
    op_fields["value"] = value;
    op_fields["excerpt"] = eid;
    op_fields["style"] = text;
    op_fields["at"] = pos;
    op_fields["duration"] = offset_fraction;
    op_fields["pitches"] = schema_array(object({{"pitch", pitch}, {"tpc", tpc}}, json_array<Json>({"pitch"})));
    op_fields["note"] = ref;
    op_fields["pitch"] = pitch;
    op_fields["tpc"] = tpc;
    op_fields["chord_rest"] = ref;
    op_fields["before"] = alternatives({bar, eid, op_ref}, "insert_measures: as measure. add_part: part EID or {from_op,item}.");
    op_fields["count"] = described(n1024, "set_repeat_barlines: passes, at least 2.");
    op_fields["kind"] =
        Json{{"type", "string"},
             {"maxLength", 4096},
             {"description", "Measure kind (Measure|VBox|HBox|TBox|FBox), layout break (line|page|section|nobreak), or grace "
                             "kind (acciac|appog|grace4|grace16|grace32|after8|after16|after32)."}};
    const Json first_last = alternatives({bar, eid, segment_addr, op_ref}, "remove_measures: as measure. add_tuplet: as target.");
    op_fields["first"] = first_last;
    op_fields["last"] = first_last;
    op_fields["measure"] = measure_ref;
    op_fields["sig"] = time_sig;
    op_fields["key"] = key;
    op_fields["staff"] = staff_ref;
    op_fields["clef"] = text;
    op_fields["on"] = boolean;
    op_fields["instrument_id"] = text;
    op_fields["part"] = part_ref;
    op_fields["parts"] = schema_array(part_ref);
    op_fields["type"] = text;
    op_fields["start"] = alternatives({boolean, eid, compact_pos, segment_addr, op_ref}, "set_repeat_barlines: flag. Spanners: as anchor.");
    op_fields["end"] = alternatives({boolean, bar, eid, compact_pos, segment_addr, op_ref},
                                    "set_repeat_barlines: flag. add_spanner: as anchor. add_spanner_to_bar_end: as measure.");
    op_fields["props"] = schema_array(object({{"name", text}, {"value", value}}, json_array<Json>({"name", "value"})));
    op_fields["end_note"] = ref;
    op_fields["actual"] = n1024;
    op_fields["normal"] = n1024;
    op_fields["text"] = long_text;
    op_fields["anchor"] = anchor;
    op_fields["first_measure"] = measure_ref;
    op_fields["last_measure"] = measure_ref;
    op_fields["endings"] = schema_array(n1024);
    op_fields["closed"] = boolean;
    op_fields["principal"] = ref;
    op_fields["event_end"] = offset_fraction;
    op_fields["keys"] = schema_array(pitch);
    const Json op = object(std::move(op_fields), json_array<Json>({"op"}));
    auto ops = schema_array(op);
    ops["maxItems"] = 1024;
    Json tools = make_array<Json>();
    auto tool = [&](const char *name, const char *description, Json schema, bool read, bool destructive) {
        json_push(tools,
                  Json{{"name", name},
                       {"description", description},
                       {"inputSchema", std::move(schema)},
                       {"annotations",
                        {{"readOnlyHint", read}, {"destructiveHint", destructive}, {"idempotentHint", read}, {"openWorldHint", false}}}});
    };
    tool("score_open", "Open one path or base64 data and return its document handle and outline.",
         object({{"path", text}, {"data", data}, {"format", format}, {"filename_hint", text}, {"strict_ids", boolean}},
                json_array<Json>({})),
         false, false);
    tool("score_create", "Create a score from a template. The default is Treble_Clef.",
         object(
             {{"template_id", text}, {"template_path", text}, {"title", text}, {"subtitle", text}, {"composer", text}, {"lyricist", text}},
             json_array<Json>({})),
         false, false);
    tool("score_close", "Release this document and its checkpoints.", object({{"document", document}}, json_array<Json>({"document"})),
         false, true);
    tool("score_outline", "Read score metadata, staves, measures and signatures.",
         object({{"document", document}, {"expected_revision", revision}, {"excerpt", eid}}, json_array<Json>({"document"})), true, false);
    tool("score_query", "Inspect elements, properties, page count or audio tracks.",
         object({{"document", document},
                 {"expected_revision", revision},
                 {"mode", mode},
                 {"query", query},
                 {"target", query_target},
                 {"props", schema_array(text)},
                 {"excerpt", eid}},
                json_array<Json>({"document", "mode"})),
         true, false);
    tool("score_read_music", "Read notation by bar, staff and voice. Returns a musical summary and structured events.",
         object({{"document", document},
                 {"expected_revision", revision},
                 {"excerpt", eid},
                 {"measure", bar},
                 {"count", count},
                 {"staves", schema_array(bar)},
                 {"voices", schema_array(voice)},
                 {"include_ids", boolean}},
                json_array<Json>({"document"})),
         true, false);
    tool("score_edit",
         "Apply a batch atomically by default. Use runtime_info scope ops for operation shapes. excerpt scopes the batch to one "
         "excerpt. Bar, staff and voice numbers are 1-based and resolve when their op runs. A position is a compact "
         "{measure,staff?,voice?,offset?} with a bar-local offset, or a segment address with a score-wide tick; {from_op,item} "
         "names an item an earlier op created.",
         object({{"document", document},
                 {"expected_revision", revision},
                 {"excerpt", eid},
                 {"atomic", boolean},
                 {"detail", boolean},
                 {"ops", ops}},
                json_array<Json>({"document", "ops"})),
         false, true);
    tool("score_export", "Write a score, image, audio file or position map. PNG and SVG paths name directories of page files.",
         object({{"document", document},
                 {"expected_revision", revision},
                 {"format", output},
                 {"path", text},
                 {"excerpt", eid},
                 {"options", opt}},
                json_array<Json>({"document", "format", "path"})),
         false, true);
    tool("runtime_info", "Inspect capabilities, limits and selected engine catalogues.",
         object({{"scope", scope}, {"query", text}, {"type", text}, {"op", text}}, json_array<Json>({})), true, false);
    tool("checkpoint", "Save a named in-memory snapshot.",
         object({{"document", document}, {"expected_revision", revision}, {"name", text}}, json_array<Json>({"document", "name"})), false,
         true);
    tool("restore_checkpoint", "Restore a named snapshot and advance the revision.",
         object({{"document", document}, {"expected_revision", revision}, {"name", text}}, json_array<Json>({"document", "name"})), false,
         true);
    tool("drop_checkpoint", "Release a named snapshot.",
         object({{"document", document}, {"expected_revision", revision}, {"name", text}}, json_array<Json>({"document", "name"})), false,
         true);
    tool("score_write_phrase", "Enter one or more musical lanes together in an atomic edit.",
         object({{"document", document}, {"expected_revision", revision}, {"excerpt", eid}, {"lanes", lanes}},
                json_array<Json>({"document", "lanes"})),
         false, true);
    tool("runtime_configure", "Add a font or replace the ordered SoundFont list.",
         object({{"add_font", text}, {"soundfonts", schema_array(text)}}, json_array<Json>({})), false, true);
    return tools;
}
} // namespace music::api
