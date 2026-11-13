#include "schema_defs.h"

#include <algorithm>
#include <string>
#include <vector>

namespace music::api {

namespace {
using J = Json;

J str(int max = 4096) { return {{"type", "string"}, {"maxLength", max}}; }

J num() { return {{"type", "number"}}; }

J boolean() { return {{"type", "boolean"}}; }

J integer(int low = 0, int high = 2147483647) { return {{"type", "integer"}, {"minimum", low}, {"maximum", high}}; }

J lit(J v) { return {{"const", v}}; }

J en(J v) { return {{"type", "string"}, {"enum", v}}; }

J array(J v, int max = 4096) { return {{"type", "array"}, {"items", v}, {"maxItems", max}}; }

J object(J p, J required = nullptr) {
    if (required.is_null()) {
        required = json_array<J>();
        for (const auto &[key, value] : p.get_object()) json_push(required, key);
    }
    return {{"type", "object"}, {"properties", p}, {"required", required}, {"additionalProperties", false}};
}

J one(J v) { return {{"oneOf", v}}; }

J ref(const char *n) { return {{"$ref", std::string("#/$defs/") + n}}; }

J nullable(J v) { return one(json_array<J>({v, J{{"type", "null"}}})); }

J tagged(const char *kind, J value) { return object({{"kind", lit(kind)}, {"value", value}}); }

J success(J p) {
    p["ok"] = lit(true);
    return object(p);
}

void collect(const J &node, const J &defs, J &used) {
    if (node.is_object()) {
        if (node.contains("$ref")) {
            auto k = node["$ref"].get<std::string>().substr(8);
            if (!used.contains(k)) {
                used[k] = defs.at(k);
                collect(defs.at(k), defs, used);
            }
        }
        for (auto &[key, v] : node.get_object()) collect(v, defs, used);
    } else if (node.is_array()) {
        for (auto &v : node.get_array()) collect(v, defs, used);
    }
}

J schema(J body, const J &defs) {
    J used = make_object<J>();
    collect(body, defs, used);
    body["type"] = "object";
    body["unevaluatedProperties"] = false;
    body["$defs"] = used;
    body["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    return body;
}

// Input limits protect request decoding; they do not limit data imported by
// the engine. Build output schemas separately so shared definitions do not
// accidentally advertise those request limits for engine-produced strings,
// vectors, and numbers. Keep only bounds established by the output producers.
void output_bounds(J &node, const std::string &field = {}) {
    if (node.is_array()) {
        for (auto &item : node.get_array()) output_bounds(item, field);
        return;
    }
    if (!node.is_object()) return;
    const auto type = json_value<std::string>(node, "type", {});
    if (type == "string" && !node.contains("pattern")) json_erase(node, "maxLength");
    if (type == "array" && field != "elements" && field != "ops" && field != "artifacts") json_erase(node, "maxItems");
    if (type == "integer" || type == "number") {
        const bool color = json_value<int>(node, "minimum", -1) == 0 && json_value<int>(node, "maximum", -1) == 255;
        const bool paging = (field == "total" || field == "next_cursor") && json_value<int>(node, "maximum", -1) == 4096;
        const bool mix = field == "volume_db" || field == "balance";
        if (!color && !paging && !mix) {
            json_erase(node, "minimum");
            json_erase(node, "maximum");
            // These public DTO fields are int32_t, including any sentinel.
            static const std::vector<std::string> int32_fields = {
                "page", "index", "part",  "lines",         "pages", "file_version",     "measure_count",
                "key",  "staff", "voice", "measure_index", "track", "audio_sample_rate"};
            if (type == "integer" && std::find(int32_fields.begin(), int32_fields.end(), field) != int32_fields.end()) {
                node["minimum"] = -2147483647 - 1;
                node["maximum"] = 2147483647;
            }
        }
    }
    for (auto &[key, child] : node.get_object()) {
        if (key == "properties" && child.is_object()) {
            for (auto &[property_name, property] : child.get_object()) output_bounds(property, property_name);
        } else {
            output_bounds(child, field);
        }
    }
}
} // namespace

Json declared_tools() {
    const J b = boolean(), n = num(), idx = integer(), text = str();
    const J u = {{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}, {"maxLength", 20}};
    const J i = {{"type", "string"}, {"pattern", "^(0|-?[1-9][0-9]*)$"}, {"maxLength", 20}};
    const J eid = {{"type", "string"}, {"minLength", 3}, {"maxLength", 32}, {"pattern", "^[A-Za-z0-9+/_-]+$"}};
    const J doc = {{"type", "string"}, {"maxLength", 128}, {"pattern", "^doc-[A-Za-z0-9]+-[1-9][0-9]*$"}};
    J d = make_object<J>();
    d["fraction"] = object({{"num", i}, {"den", {{"type", "string"}, {"pattern", "^[1-9][0-9]*$"}, {"maxLength", 19}}}});
    d["score"] = object({{"excerpt", eid}}, json_array<J>());
    const J f = ref("fraction"), score = ref("score");
    d["address"] = one(json_array<J>({object({{"kind", lit("segment")},
                                              {"measure", eid},
                                              {"staff", idx},
                                              {"voice", integer(0, 3)},
                                              {"tick", f},
                                              {"segment_kind", str(128)}}),
                                      object({{"kind", lit("page")}, {"score", score}, {"index", idx}}),
                                      object({{"kind", lit("system")}, {"score", score}, {"page", idx}, {"index", idx}}),
                                      object({{"kind", lit("spanner_segment")}, {"spanner", eid}, {"index", idx}})}));
    const J a = ref("address");
    J refs = json_array<J>({object({{"kind", lit("id")}, {"id", eid}}), object({{"kind", lit("address")}, {"address", a}})});
    d["read_ref"] = one(refs);
    json_push(refs, object({{"kind", lit("op")}, {"op", integer(0, 1023)}, {"item", integer(0, 65535)}}));
    d["ref"] = one(refs);
    const J r = ref("ref");
    J values = json_array<J>(
        {J{{"type", "null"}}, tagged("bool", b), tagged("int", i), tagged("real", n), tagged("string", str(65536)), tagged("fraction", f),
         tagged("point", object({{"x", n}, {"y", n}})), tagged("size", object({{"w", n}, {"h", n}})),
         tagged("color", object({{"r", integer(0, 255)}, {"g", integer(0, 255)}, {"b", integer(0, 255)}, {"a", integer(0, 255)}})),
         tagged("enum", text), tagged("int_list", array(i))});
    d["edit_value"] = one(values);
    json_push(values, object({{"kind", lit("opaque")}, {"type", text}}));
    d["value"] = one(values);
    const J v = ref("edit_value"), props = array(object({{"name", text}, {"value", v}}), 1024);
    J ops = json_array<J>();
    auto op = [&](const char *name, J p, J optional = json_array<J>()) {
        p["op"] = lit(name);
        J req = json_array<J>();
        for (const auto &[key, value] : p.get_object()) {
            bool skip = false;
            for (const auto &x : optional.get_array()) {
                if (x == std::string_view(key)) skip = true;
            }
            if (!skip) json_push(req, key);
        }
        json_push(ops, object(p, req));
    };
    op("set_property", {{"target", r}, {"property", text}, {"value", v}});
    op("reset_property", {{"target", r}, {"property", text}});
    op("set_style", {{"score", score}, {"style", text}, {"value", v}});
    op("enter_notes", {{"at", a},
                       {"duration", f},
                       {"pitches", array(object({{"pitch", integer(0, 127)}, {"tpc", integer(-1, 33)}}, json_array<J>({"pitch"})), 128)}});
    op("set_pitch", {{"note", r}, {"pitch", integer(0, 127)}, {"tpc", integer(-1, 33)}}, json_array<J>({"tpc"}));
    op("set_duration", {{"chord_rest", r}, {"duration", f}});
    op("remove", {{"target", r}});
    op("insert_measures", {{"before", r}, {"count", integer(1, 1024)}, {"kind", en({"Measure", "VBox", "HBox", "TBox", "FBox"})}},
       json_array<J>({"before", "kind"}));
    op("remove_measures", {{"first", r}, {"last", r}});
    op("set_time_sig", {{"measure", r}, {"sig", f}});
    op("set_key_sig", {{"measure", r}, {"key", integer(-7, 7)}, {"staff", r}}, json_array<J>({"staff"}));
    op("insert_clef", {{"at", a}, {"clef", text}});
    op("set_layout_break", {{"measure", r}, {"kind", en({"line", "page", "section", "nobreak"})}, {"on", b}});
    op("add_part", {{"instrument_id", text}, {"before", r}}, json_array<J>({"before"}));
    op("remove_part", {{"part", r}});
    op("add_staff", {{"part", r}});
    op("remove_staff", {{"staff", r}});
    op("generate_excerpts", {{"parts", array(r, 256)}});
    op("add_spanner", {{"type", text}, {"start", r}, {"end", r}, {"props", props}}, json_array<J>({"props"}));
    op("add_tie", {{"note", r}});
    op("add_tuplet", {{"first", r}, {"last", r}, {"actual", integer(1, 1024)}, {"normal", integer(1, 1024)}},
       json_array<J>({"actual", "normal"}));
    op("add_text", {{"at", a}, {"style", text}, {"text", str(65536)}});
    op("add", {{"anchor", r}, {"type", text}, {"props", props}}, json_array<J>({"props"}));
    d["op"] = one(ops);
    d["diagnostic"] = object({{"severity", en({"info", "warning"})}, {"message", str(65536)}});
    const J diagnostics = array(ref("diagnostic"));
    d["export_warning"] = object({{"code", str()}, {"message", str()}, {"object", object({{"kind", str(128)}, {"id", str(256)}})}},
                                 json_array<J>({"code", "message"}));
    for (const auto *field : {"kind", "id"}) d["export_warning"]["properties"]["object"]["properties"][field]["minLength"] = 1;
    // Frozen 4.70 warnings always identify their source; other warning families
    // retain the optional object contract.
    d["export_warning"]["if"] = J{{"properties", {{"code", {{"pattern", "^B470_"}}}}}, {"required", {"code"}}};
    d["export_warning"]["then"] = J{{"required", {"object"}}};
    d["error"] = object({{"code", en({"InvalidArgument", "InvalidInput", "UnsupportedFormat", "NotFound", "StaleHandle", "ResourceLimit",
                                      "Unsupported", "Io", "RevisionConflict", "EditRejected", "Poisoned", "EngineFailure", "Cancelled",
                                      "DeadlineExceeded", "WorkerFailed"})},
                         {"message", str(65536)},
                         {"op_index", integer(0, 1023)},
                         {"applied", integer(0, 1024)},
                         {"poisoned", b},
                         {"document", doc},
                         {"current_revision", u},
                         {"published_artifacts", array(text)},
                         {"export_warnings", array(ref("export_warning"))}},
                        json_array<J>({"code", "message"}));
    d["property"] = object({{"name", text}, {"value", ref("value")}, {"is_default", b}, {"styled", b}});
    d["box"] = object({{"page", idx}, {"x", n}, {"y", n}, {"width", n}, {"height", n}});
    d["element"] = object({{"id", eid},
                           {"type", text},
                           {"score", score},
                           {"measure", nullable(eid)},
                           {"measure_index", integer(-1)},
                           {"staff", integer(-1)},
                           {"voice", integer(-1, 3)},
                           {"tick", f},
                           {"duration", f},
                           {"parent", nullable(eid)},
                           {"links", array(eid)},
                           {"box", nullable(ref("box"))},
                           {"props", array(ref("property"))}});
    d["staff"] = object({{"id", eid}, {"index", idx}, {"part", idx}, {"group", text}, {"lines", idx}, {"invisible", b}});
    d["part"] = object({{"id", eid}, {"name", text}, {"instrument_id", text}, {"staves", array(ref("staff"))}});
    d["measure"] = object({{"id", eid}, {"index", idx}, {"tick", f}, {"duration", f}});
    d["signature"] = object({{"type", en({"TimeSig", "KeySig", "Tempo"})},
                             {"tick", f},
                             {"staff", integer(-1)},
                             {"timesig", nullable(f)},
                             {"key", integer(-7, 7)},
                             {"bpm", n},
                             {"text", text}});
    d["excerpt"] = object({{"id", eid}, {"name", text}, {"pages", nullable(idx)}});
    d["outline"] = object({{"title", text},
                           {"subtitle", text},
                           {"composer", text},
                           {"poet", text},
                           {"timesig", text},
                           {"mscore_version", text},
                           {"file_version", idx},
                           {"pages", nullable(idx)},
                           {"measure_count", idx},
                           {"key", integer(-7, 7)},
                           {"duration", n},
                           {"parts", array(ref("part"))},
                           {"measures", array(ref("measure"), 100000)},
                           {"signatures", array(ref("signature"), 100000)},
                           {"excerpts", array(ref("excerpt"))}});
    const J volume = {{"type", "number"}, {"minimum", -120}, {"maximum", 24}},
            balance = {{"type", "number"}, {"minimum", -1}, {"maximum", 1}};
    d["mix"] = object({{"volume_db", volume}, {"balance", balance}, {"mute", b}, {"solo", b}});
    d["track"] = object({{"track", idx},
                         {"part_id", text},
                         {"instrument_id", text},
                         {"part_name", text},
                         {"instrument_name", text},
                         {"chord_symbols", b},
                         {"mix", ref("mix")}});
    d["audio_tracks"] = object({{"tracks", array(ref("track"), 256)}, {"master", ref("mix")}});
    const J kinds = en({"bool", "int", "real", "string", "point", "size", "color", "fraction", "enum", "int_list", "opaque"}),
            units = en({"none", "spatium", "absolute", "millimetre", "degree"});
    d["prop_info"] = object(
        {{"name", text}, {"kind", kinds}, {"unit", units}, {"enum_tokens", array(text)}, {"settable", b}, {"link", b}, {"group", text}});
    d["type_info"] = object({{"type", text}, {"props", array(ref("prop_info"))}});
    d["style_info"] =
        object({{"name", text}, {"kind", kinds}, {"unit", units}, {"enum_tokens", array(text)}, {"default_value", ref("value")}});
    d["artifact"] = object({{"mime", str(128)}, {"path", text}, {"size", u}});
    const J formats = en({"mscz", "mscx", "musicxml", "mxl", "midi", "guitarpro", "mnx"});
    const J export_formats = en({"mscz", "mscx", "musicxml", "mxl", "midi"});
    d["capabilities"] = object({{"engine_version", text},
                                {"import_formats", array(formats)},
                                {"export_formats", array(export_formats)},
                                {"render_formats", array(en({"pdf", "png", "svg"}))},
                                {"audio_formats", array(en({"wav", "flac", "opus", "mp3", "aac"}))},
                                {"soundfonts", array(text, 256)},
                                {"audio_sample_rate", idx}});
    d["template"] = object({{"id", text}, {"title", text}, {"category", text}, {"path", text}});
    d["limits"] = object({{"input_bytes", idx},
                          {"base64_chars", idx},
                          {"batch_ops", idx},
                          {"query_results", idx},
                          {"output_bytes", u},
                          {"deadline_ms", idx},
                          {"cancellation", lit("cooperative")},
                          {"transport_request_bytes", idx}});
    J tools = json_array<J>();
    auto tool = [&](const char *name, const char *description, J input, J output) {
        auto out = schema(one(json_array<J>({output, object({{"ok", lit(false)}, {"error", ref("error")}})})), d);
        output_bounds(out);
        json_push(tools,
                  J{{"name", name}, {"description", description}, {"inputSchema", schema(input, d)}, {"outputSchema", std::move(out)}});
    };
    J input = object(
        {{"path", text},
         {"data",
          {{"type", "string"}, {"maxLength", 11184812}, {"pattern", "^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$"}}},
         {"format", formats},
         {"filename_hint", text},
         {"strict_ids", b}},
        json_array<J>());
    input["oneOf"] = json_array<J>(
        {J{{"required", {"path"}}, {"not", {{"required", {"data"}}}}}, J{{"required", {"data"}}, {"not", {{"required", {"path"}}}}}});
    const J opened = success({{"document", doc}, {"revision", u}, {"diagnostics", diagnostics}});
    tool("score_open", "Open a score; decoded cap 8 MiB, subject to lower transport line budget.", input, opened);
    input = object(
        {{"template_id", text}, {"template_path", text}, {"title", text}, {"subtitle", text}, {"composer", text}, {"lyricist", text}},
        json_array<J>());
    input["oneOf"] = json_array<J>({J{{"required", {"template_id"}}, {"not", {{"required", {"template_path"}}}}},
                                    J{{"required", {"template_path"}}, {"not", {{"required", {"template_id"}}}}}});
    tool("score_create", "Create from exactly one engine template.", input, opened);
    tool("score_close", "Destroy the document on its owner thread.", object({{"document", doc}}), success(make_object<J>()));
    tool("score_outline",
         "Read structure and source revision without layout; pages may be null until layout is current. Use score_query mode page_count "
         "for a fresh count; expected_revision guards the read.",
         object({{"document", doc}, {"expected_revision", u}, {"score", score}}, json_array<J>({"document"})),
         success({{"revision", u}, {"outline", ref("outline")}}));
    J query = object({{"score", score},
                      {"types", array(text)},
                      {"first_measure", idx},
                      {"measure_count", integer(1, 128)},
                      {"from_measure", eid},
                      {"to_measure", eid},
                      {"staves", array(idx)},
                      {"voices", array(integer(0, 3), 4)},
                      {"from_tick", f},
                      {"to_tick", f},
                      {"props", array(text)},
                      {"layout_boxes", b},
                      {"limit", integer(1, 4096)},
                      {"cursor", integer(0, 4096)}},
                     json_array<J>());
    query["not"] =
        J{{"allOf", json_array<J>({J{{"anyOf", json_array<J>({J{{"required", {"first_measure"}}}, J{{"required", {"measure_count"}}}})}},
                                   J{{"anyOf", json_array<J>({J{{"required", {"from_measure"}}}, J{{"required", {"to_measure"}}}})}}})}};
    tool("score_query",
         "Queued reads; mode page_count lays out the selected score and returns a fresh pages count. expected_revision guards pagination. "
         "Layout addresses are ephemeral.",
         one(json_array<J>({object({{"document", doc}, {"expected_revision", u}, {"mode", lit("elements")}, {"query", query}},
                                   json_array<J>({"document", "mode"})),
                            object({{"document", doc},
                                    {"expected_revision", u},
                                    {"mode", lit("properties")},
                                    {"target", ref("read_ref")},
                                    {"props", array(text)}},
                                   json_array<J>({"document", "mode", "target"})),
                            object({{"document", doc}, {"expected_revision", u}, {"mode", lit("page_count")}, {"score", score}},
                                   json_array<J>({"document", "mode"})),
                            object({{"document", doc}, {"expected_revision", u}, {"mode", lit("audio_tracks")}, {"score", score}},
                                   json_array<J>({"document", "mode"}))})),
         one(json_array<J>({success({{"mode", lit("elements")},
                                     {"revision", u},
                                     {"elements", array(ref("element"))},
                                     {"total", integer(0, 4096)},
                                     {"next_cursor", nullable(integer(0, 4096))}}),
                            success({{"mode", lit("properties")}, {"revision", u}, {"properties", array(ref("property"))}}),
                            success({{"mode", lit("page_count")}, {"revision", u}, {"pages", idx}}),
                            success({{"mode", lit("audio_tracks")}, {"revision", u}, {"audio_tracks", ref("audio_tracks")}})})));
    tool("score_describe", "Read property and style catalogue.",
         one(json_array<J>(
             {object({{"scope", lit("types")}}), object({{"scope", lit("type")}, {"type", text}}), object({{"scope", lit("styles")}})})),
         one(json_array<J>({success({{"scope", lit("types")}, {"types", array(ref("type_info"))}}),
                            success({{"scope", lit("type")}, {"type", ref("type_info")}}),
                            success({{"scope", lit("styles")}, {"styles", array(ref("style_info"))}})})));
    tool("score_edit",
         "Decode and preflight batch, then apply operations in order as a prefix commit (each op is all-or-nothing; no implicit batch "
         "rollback). On rejection at op_index, ops 0..op_index-1 stay applied, applied reports the count completed, and the batch stops. "
         "Only an engine failure after writes poisons the document until restore_checkpoint or reopen. Use checkpoint/restore_checkpoint "
         "for all-or-nothing batches. Opaque is read-only.",
         object({{"document", doc}, {"expected_revision", u}, {"ops", array(ref("op"), 1024)}}, json_array<J>({"document", "ops"})),
         success({{"revision", u},
                  {"applied", idx},
                  {"ops", array(object({{"created", array(nullable(eid), 65536)}}), 1024)},
                  {"created", array(eid, 65536)},
                  {"removed", array(eid, 65536)},
                  {"replaced", array(eid, 65536)},
                  {"diagnostics", diagnostics}}));
    J score_options = object(
        {{"score", score},
         {"format", en({"mscz", "mscx", "musicxml", "mxl", "midi"})},
         {"midi_expand_repeats", b},
         {"midi_rpns", b},
         {"native_version",
          {{"type", "string"},
           {"minLength", 1},
           {"maxLength", 64},
           {"default", "5.00"},
           {"description",
            "Requested native version; supported: 5.00 (default), 4.70. Other values are rejected by the engine with export warnings."}}}},
        json_array<J>({"format"}));
    score_options["if"] = J{{"properties", {{"format", {{"not", lit("midi")}}}}}};
    score_options["then"] =
        J{{"not", {{"anyOf", json_array<J>({J{{"required", {"midi_expand_repeats"}}}, J{{"required", {"midi_rpns"}}}})}}}};
    J render_options = object({{"score", score},
                               {"format", en({"pdf", "png", "svg"})},
                               {"first_page", idx},
                               {"last_page", integer(-1)},
                               {"dpi", integer(36, 1200)},
                               {"background", b},
                               {"transparent", b}},
                              json_array<J>({"format"}));
    render_options["if"] = J{{"properties", {{"format", en({"pdf", "svg"})}}}};
    render_options["then"] = J{{"not", {{"anyOf", json_array<J>({J{{"required", {"dpi"}}}, J{{"required", {"transparent"}}}})}}}};
    J track_options = object({{"track", idx},
                              {"volume_db", volume},
                              {"balance", balance},
                              {"mute", b},
                              {"solo", b},
                              {"soundfont", one(json_array<J>({J{{"type", "null"}}, idx, text}))},
                              {"preset", text}},
                             json_array<J>({"track"}));
    J audio_options = object({{"score", score},
                              {"format", en({"wav", "flac", "opus", "mp3", "aac"})},
                              {"bitrate_kbps", one(json_array<J>({lit(0), integer(32, 512)}))},
                              {"bit_depth", {{"enum", {0, 16, 24, 32}}}},
                              {"use_repeats", b},
                              {"master_volume_db", volume},
                              {"master_balance", balance},
                              {"tracks", array(track_options, 256)},
                              {"reverb", b},
                              {"reverb_send", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                              {"sample_rate", idx}},
                             json_array<J>({"format"}));
    J destination =
        one(json_array<J>({object({{"kind", lit("file")}, {"path", text}}), object({{"kind", lit("directory")}, {"path", text}})}));
    audio_options["allOf"] = json_array<J>(
        {J{{"if", {{"properties", {{"format", en({"wav", "flac"})}}}}},
           {"then", {{"properties", {{"bitrate_kbps", lit(0)}}}}},
           {"else", {{"properties", {{"bit_depth", lit(0)}}}}}},
         J{{"if", {{"properties", {{"format", lit("flac")}}}}}, {"then", {{"properties", {{"bit_depth", {{"enum", {0, 16, 24}}}}}}}}}});
    J exports = json_array<J>();
    J export_variants = json_array<J>(
        {json_array<J>({"score", score_options}), json_array<J>({"render", render_options}), json_array<J>({"audio", audio_options}),
         json_array<J>(
             {"positions", object({{"score", score}, {"position_kind", en({"measure", "segment"})}}, json_array<J>({"position_kind"}))})});
    for (auto &entry : export_variants.get_array()) {
        J variant = object(
            {{"document", doc}, {"expected_revision", u}, {"kind", lit(entry[0])}, {"options", entry[1]}, {"destination", destination}},
            json_array<J>({"document", "kind", "options", "destination"}));
        if (entry[0].get<std::string>() == "render") {
            variant["if"] = J{{"properties", {{"options", {{"properties", {{"format", en({"png", "svg"})}}}}}}}};
            variant["then"] = J{{"properties", {{"destination", {{"properties", {{"kind", lit("directory")}}}}}}}};
            variant["else"] = J{{"properties", {{"destination", {{"properties", {{"kind", lit("file")}}}}}}}};
        } else {
            variant["properties"]["destination"] = object({{"kind", lit("file")}, {"path", text}});
        }
        json_push(exports, variant);
    }
    tool("score_export",
         "Export synchronously from the live document. Report source revision. Select master or excerpt for score, render, audio and "
         "positions. PNG/SVG require a directory with page-0001.ext names. Stage all; failed publication returns Io/published_artifacts "
         "and cleans remaining staging. Positions map Document::positions measure/segment to JSON page_size, elements and events: "
         "page-local 1200-DPI geometry, result-local element IDs and repeat-expanded milliseconds; see docs/dev/tools.md.",
         one(exports),
         object({{"ok", lit(true)},
                 {"source_revision", u},
                 {"artifacts", array(ref("artifact"))},
                 {"export_warnings", array(ref("export_warning"))}},
                json_array<J>({"ok", "source_revision", "artifacts"})));
    tool("runtime_info",
         "Inspect runtime or add_font(path)/set_soundfonts(files) on owner thread. Empty files disables audio. Changes preserve document "
         "revisions; fonts trigger relayout. Sample rate stays fixed.",
         one(json_array<J>({object({{"action", lit("get")}}, json_array<J>()), object({{"action", lit("add_font")}, {"path", text}}),
                            object({{"action", lit("set_soundfonts")}, {"files", array(text, 256)}})})),
         success({{"tool_revision", lit("2")},
                  {"capabilities", ref("capabilities")},
                  {"templates", array(ref("template"))},
                  {"fonts", array(text)},
                  {"limits", ref("limits")}}));
    const J checkpoint_input = object({{"document", doc}, {"name", text}, {"expected_revision", u}}, json_array<J>({"document", "name"}));
    tool("checkpoint",
         "Save a named in-memory checkpoint of the document (64 MiB shared checkpoint budget). In memory only; checkpoints are the only "
         "way to undo edits apart from reopening the file.",
         checkpoint_input, success({{"revision", u}}));
    tool("restore_checkpoint",
         "Restore a named in-memory checkpoint, preserving element IDs, advancing revision, dirtying layout, and clearing poison. In "
         "memory only; this is the only way to undo edits apart from reopening the file.",
         checkpoint_input, success({{"revision", u}}));
    tool("drop_checkpoint",
         "Delete a named in-memory checkpoint and free its share of the 64 MiB budget. In memory only; checkpoints are the only way to "
         "undo edits apart from reopening the file.",
         checkpoint_input, success({{"revision", u}}));
    return tools;
}
} // namespace music::api
