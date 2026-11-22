#include "schema_defs.h"

namespace music::api {
namespace {
Json array(const Json &item) { return {{"type", "array"}, {"items", item}}; }

Json choices(std::initializer_list<Json> values) { return {{"type", "string"}, {"enum", json_array<Json>(values)}}; }

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
    const Json fraction =
        Json{{"type", "string"},
             {"maxLength", 42},
             {"description", "Exact whole-note fraction, e.g. 0 or 3/8. Written time signatures retain their numerator and denominator."}};
    const Json boolean = Json{{"type", "boolean"}};
    const Json idx = integer(0, 2147483647);
    const Json bar = integer(1, 2147483647);
    const Json count = integer(1, 128);
    const Json voice = integer(1, 4);
    const Json key = integer(-7, 7);
    const Json pitch = integer(0, 127);
    const Json tpc = integer(-1, 33);
    const Json n1024 = integer(1, 1024);
    const Json format = choices({"mscz", "mscx", "musicxml", "mxl", "midi", "guitarpro", "mnx"});
    const Json output = choices({"mscz", "mscx", "musicxml", "mxl", "midi", "pdf", "png", "svg", "wav", "flac", "opus", "mp3", "aac",
                                 "measure_positions", "segment_positions"});
    const Json scope = choices({"overview", "templates", "instruments", "types", "type", "styles", "ops"});
    const Json mode = choices({"elements", "properties", "page_count", "audio_tracks"});
    const Json pitch_mode = choices({"concert", "written"});
    const Json value = Json{{"description", "Native JSON value matching the engine property or style descriptor."}};
    const Json ref = Json{{"description", "Persistent EID or a batch-local reference {from_op,item}."}};
    const Json pos = Json{{"description", "Use {measure,offset,staff,voice} with a 1-based measure number and exact measure-local offset. "
                                          "Layout addresses use kind and zero-based index."}};
    const Json duration = Json{{"type", "string"}, {"maxLength", 42}};
    const Json grace = choices({"acciac", "appog", "grace4", "grace16", "grace32", "after8", "after16", "after32"});
    const Json tie = Json{{"description", "true ties the chord, or supply an array of chord pitches to tie."}};
    const Json num = Json{{"type", "number"}};
    const Json volume = Json{{"type", "number"}, {"minimum", -120}, {"maximum", 24}};
    const Json balance = Json{{"type", "number"}, {"minimum", -1}, {"maximum", 1}};
    const Json dpi = integer(36, 1200);
    const Json last = integer(-1, 2147483647);
    const Json limit = integer(1, 4096);
    const Json cursor = integer(0, 4096);
    const Json sf = Json{{"description", "SoundFont index, path, name, or null."}};
    const Json bit_depth = Json{{"enum", json_array<Json>({0, 16, 24, 32})}};
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
                             {"tracks", array(track)},
                             {"reverb", boolean},
                             {"reverb_send", num},
                             {"sample_rate", sample_rate}},
                            json_array<Json>({}));
    const Json query = object({{"excerpt", eid},
                               {"types", array(text)},
                               {"first_measure", bar},
                               {"measure_count", count},
                               {"from_measure", eid},
                               {"to_measure", eid},
                               {"staves", array(bar)},
                               {"voices", array(voice)},
                               {"from_tick", fraction},
                               {"to_tick", fraction},
                               {"props", array(text)},
                               {"layout_boxes", boolean},
                               {"limit", limit},
                               {"cursor", cursor}},
                              json_array<Json>({}));
    const Json gracenote =
        object({{"kind", grace}, {"duration", duration}, {"pitches", array(text)}, {"drums", array(pitch)}}, json_array<Json>({"kind"}));
    const Json note = object({{"pitches", array(text)},
                              {"drums", array(pitch)},
                              {"duration", duration},
                              {"tie", tie},
                              {"grace_before", array(gracenote)},
                              {"grace_after", array(gracenote)}},
                             json_array<Json>({}));
    const Json event = object({{"pitches", array(text)},
                               {"drums", array(pitch)},
                               {"duration", duration},
                               {"tie", tie},
                               {"grace_before", array(gracenote)},
                               {"grace_after", array(gracenote)},
                               {"tuplet", text},
                               {"events", array(note)},
                               {"bar", boolean}},
                              json_array<Json>({}));
    const Json lane = object(
        {{"measure", bar}, {"offset", fraction}, {"staff", bar}, {"voice", voice}, {"pitch_mode", pitch_mode}, {"events", array(event)}},
        json_array<Json>({"measure", "events"}));
    const Json dsl_lane = object(
        {{"measure", bar}, {"offset", fraction}, {"staff", bar}, {"voice", voice}, {"pitch_mode", pitch_mode}, {"content", long_text}},
        json_array<Json>({"measure", "content"}));
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
    op_fields["duration"] = fraction;
    op_fields["pitches"] = array(object({{"pitch", pitch}, {"tpc", tpc}}, json_array<Json>({"pitch"})));
    op_fields["note"] = ref;
    op_fields["pitch"] = pitch;
    op_fields["tpc"] = tpc;
    op_fields["chord_rest"] = ref;
    op_fields["before"] = ref;
    op_fields["count"] = n1024;
    op_fields["kind"] = Json{{"description", "See the named operation in runtime_info scope ops."}};
    op_fields["first"] = ref;
    op_fields["last"] = ref;
    op_fields["measure"] = ref;
    op_fields["sig"] = fraction;
    op_fields["key"] = key;
    op_fields["staff"] = ref;
    op_fields["clef"] = text;
    op_fields["on"] = boolean;
    op_fields["instrument_id"] = text;
    op_fields["part"] = ref;
    op_fields["parts"] = array(ref);
    op_fields["type"] = text;
    op_fields["start"] = Json{{"description", "See the named operation in runtime_info scope ops."}};
    op_fields["end"] = Json{{"description", "See the named operation in runtime_info scope ops."}};
    op_fields["props"] = array(object({{"name", text}, {"value", value}}, json_array<Json>({"name", "value"})));
    op_fields["end_note"] = ref;
    op_fields["actual"] = n1024;
    op_fields["normal"] = n1024;
    op_fields["text"] = long_text;
    op_fields["anchor"] = ref;
    op_fields["first_measure"] = ref;
    op_fields["last_measure"] = ref;
    op_fields["endings"] = array(n1024);
    op_fields["closed"] = boolean;
    op_fields["principal"] = ref;
    op_fields["event_end"] = fraction;
    op_fields["keys"] = array(pitch);
    const Json op = object(std::move(op_fields), json_array<Json>({"op"}));
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
                 {"target", ref},
                 {"props", array(text)},
                 {"excerpt", eid}},
                json_array<Json>({"document", "mode"})),
         true, false);
    tool("score_read_music", "Read notation by bar, staff and voice. Returns a musical summary and structured events.",
         object({{"document", document},
                 {"expected_revision", revision},
                 {"excerpt", eid},
                 {"measure", bar},
                 {"count", count},
                 {"staves", array(bar)},
                 {"voices", array(voice)},
                 {"include_ids", boolean}},
                json_array<Json>({"document"})),
         true, false);
    tool("score_edit", "Apply a batch atomically by default. Use runtime_info scope ops for operation shapes.",
         object({{"document", document}, {"expected_revision", revision}, {"atomic", boolean}, {"detail", boolean}, {"ops", array(op)}},
                json_array<Json>({"document", "ops"})),
         false, false);
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
         false);
    tool("restore_checkpoint", "Restore a named snapshot and advance the revision.",
         object({{"document", document}, {"expected_revision", revision}, {"name", text}}, json_array<Json>({"document", "name"})), false,
         true);
    tool("drop_checkpoint", "Release a named snapshot.",
         object({{"document", document}, {"expected_revision", revision}, {"name", text}}, json_array<Json>({"document", "name"})), false,
         true);
    tool("score_write_phrase", "Enter one or more musical lanes together in an atomic edit.",
         object({{"document", document},
                 {"expected_revision", revision},
                 {"excerpt", eid},
                 {"lanes", array(dsl_phrase_syntax ? dsl_lane : lane)}},
                json_array<Json>({"document", "lanes"})),
         false, false);
    tool("runtime_configure", "Add a font or replace the ordered SoundFont list.",
         object({{"add_font", text}, {"soundfonts", array(text)}}, json_array<Json>({})), false, false);
    return tools;
}
} // namespace music::api
