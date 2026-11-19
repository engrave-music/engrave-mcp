#include "host_api.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace fs = std::filesystem;
using music::api::HostApi;
using music::api::Json;

namespace {
std::vector<std::string> g_args;

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    require(bool(f), "cannot read " + p.string());
    return {std::istreambuf_iterator<char>(f), {}};
}

std::string b64(const std::string &s) {
    constexpr char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < s.size(); i += 3) {
        unsigned a = static_cast<unsigned char>(s[i]), b = i + 1 < s.size() ? static_cast<unsigned char>(s[i + 1]) : 0,
                 c = i + 2 < s.size() ? static_cast<unsigned char>(s[i + 2]) : 0;
        out += chars[a >> 2];
        out += chars[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < s.size() ? chars[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < s.size() ? chars[c & 63] : '=';
    }
    return out;
}

Json idref(const std::string &id) { return {{"kind", "id"}, {"id", id}}; }

Json opref(int op, int item) { return {{"kind", "op"}, {"op", op}, {"item", item}}; }

Json fraction(const char *num, const char *den) { return {{"num", num}, {"den", den}}; }

Json address(const std::string &measure, const char *num, const char *den) {
    return {{"kind", "segment"}, {"measure", measure},         {"staff", 0},
            {"voice", 0},        {"tick", fraction(num, den)}, {"segment_kind", "ChordRest"}};
}

std::string path_text(const fs::path &p) {
    auto s = p.generic_u8string();
    return {reinterpret_cast<const char *>(s.data()), s.size()};
}

Json call(HostApi &host, const std::string &tool, const Json &arg) {
    Json out;
    if (tool == "score_open") {
        out = host.open(arg);
    } else if (tool == "score_create") {
        out = host.create_document(arg);
    } else if (tool == "score_close") {
        out = host.close(arg);
    } else if (tool == "score_outline") {
        out = host.outline(arg);
    } else if (tool == "score_query") {
        out = host.query(arg);
    } else if (tool == "score_describe") {
        out = host.describe(arg);
    } else if (tool == "score_edit") {
        out = host.edit(arg);
    } else if (tool == "checkpoint") {
        out = host.checkpoint(arg);
    } else if (tool == "restore_checkpoint") {
        out = host.restore_checkpoint(arg);
    } else if (tool == "drop_checkpoint") {
        out = host.drop_checkpoint(arg);
    } else if (tool == "runtime_info") {
        out = host.runtime_info(arg);
    } else {
        throw std::logic_error("unknown test tool");
    }
    const std::string line = "HOST_CASE " + music::dump_json(Json{{"tool", tool}, {"arguments", arg}, {"result", out}}) + '\n';
    std::fwrite(line.data(), 1, line.size(), stdout);
    return out;
}

Json ok(HostApi &host, const std::string &tool, const Json &arg) {
    auto out = call(host, tool, arg);
    require(music::json_value<bool>(out, "ok", false), tool + " failed: " + music::dump_json(out));
    return out;
}

Json error(HostApi &host, const std::string &tool, const Json &arg, const std::string &code) {
    auto out = call(host, tool, arg);
    require(!music::json_value<bool>(out, "ok", true) && out.at("error").at("code") == code,
            tool + " expected " + code + ": " + music::dump_json(out));
    return out;
}
} // namespace

TEST(HostApi, EndToEndToolContract) {
    ASSERT_EQ(g_args.size(), 4u) << "usage: host_api_test ASSETS SOUNDFONT REPLACEMENT_SOUNDFONT DATA_ROOT";
    const auto assets = fs::absolute(fs::u8path(g_args[0]));
    const auto original_sf = fs::absolute(fs::u8path(g_args[1]));
    const auto replacement_sf = fs::absolute(fs::u8path(g_args[2]));
    const auto root = fs::absolute(fs::u8path(g_args[3])) / "inputs/golden";
    const auto input = root / "linked_parts.mscx";
    const auto scratch =
        fs::temp_directory_path() / ("host-api-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    require(fs::create_directory(scratch), "cannot create test directory");
    const auto font_input = scratch / "font.otf", add_font_input = scratch / "another.ttf", sf_input = scratch / "initial.sf3",
               invalid_sf = scratch / "invalid.sf2";
    fs::copy_file(assets / "fonts/petaluma/PetalumaScript.otf", font_input);
    fs::copy_file(assets / "fonts/FreeSerif.ttf", add_font_input);
    fs::copy_file(original_sf, sf_input);
    {
        std::ofstream f(invalid_sf, std::ios::binary);
        f << "not a soundfont";
    }
    engrave::EngineOptions options;
    options.resources = assets;
    options.fonts = {font_input};
    options.soundfonts = {sf_input};
    music::api::ExportLimits configured;
    configured.output_bytes = 234567;
    configured.deadline = std::chrono::milliseconds(3456);
    auto made = HostApi::create(options, configured);
    require(bool(made), made ? "" : made.error().message);
    auto host = std::move(*made);
    auto initial = host->runtime_options();
    require(bool(initial), "initial capture failed");
    require(initial->fonts.size() == 1 && initial->soundfonts.size() == 1, "startup assets absent");
    const auto initial_font = initial->fonts[0], initial_sf = initial->soundfonts[0];
    require(initial_font != font_input && initial_sf != sf_input, "startup assets were not copied");
    const auto font_bytes = read(initial_font), sf_bytes = read(initial_sf);
    {
        std::ofstream f(font_input, std::ios::binary | std::ios::trunc);
        f << "changed";
    }
    {
        std::ofstream f(sf_input, std::ios::binary | std::ios::trunc);
        f << "changed";
    }
    require(read(initial_font) == font_bytes && read(initial_sf) == sf_bytes, "startup asset changed with caller input");

    auto info = ok(*host, "runtime_info", music::make_object<Json>());
    require(info.at("tool_revision") == "2" && !info.at("templates").empty(), "runtime catalogue is empty");
    const auto &limits = info.at("limits");
    require(limits.at("output_bytes") == "234567" && limits.at("deadline_ms") == 3456 && limits.at("transport_request_bytes") == 4194304,
            "runtime limits differ from configuration");
    for (const auto *field : {"title", "subtitle", "composer", "lyricist"}) {
        for (const auto &text : {std::string("bad\xef\xbf\xbe"), std::string("bad\xef\xbf\xbf"), std::string("bad\x01")}) {
            Json invalid_create = {{"template_id", "Treble_Clef"}, {field, text}};
            error(*host, "score_create", invalid_create, "InvalidArgument");
        }
    }
    auto created = ok(*host, "score_create", {{"template_id", "Treble_Clef"}, {"title", "Store test"}});
    auto doc = created.at("document").get<std::string>();
    require(created.at("revision") == "1", "new revision differs from 1");
    auto outline = ok(*host, "score_outline", {{"document", doc}, {"expected_revision", "1"}});
    require(outline.at("outline").at("title") == "Store test", "template metadata missing");
    auto measure = outline.at("outline").at("measures")[0].at("id").get<std::string>();
    auto foreign = error(*host, "score_outline", {{"document", "doc-foreign-1"}}, "NotFound");
    require(foreign.at("error").at("document") == "doc-foreign-1", "foreign document omitted");
    auto conflict = error(*host, "score_outline", {{"document", doc}, {"expected_revision", "2"}}, "RevisionConflict");
    require(conflict.at("error").at("current_revision") == "1", "read conflict omitted current revision");
    error(*host, "score_query", {{"document", doc}, {"mode", "elements"}, {"expected_revision", "18446744073709551616"}},
          "InvalidArgument");
    error(*host, "score_query", {{"document", doc}, {"mode", "elements"}, {"query", {{"unknown", 1}}}}, "InvalidArgument");
    auto no_op = ok(*host, "score_edit", {{"document", doc}, {"expected_revision", "1"}, {"ops", music::make_array<Json>()}});
    require(no_op.at("revision") == "1", "empty batch changed the revision");
    error(*host, "score_edit", {{"document", doc}, {"expected_revision", "0"}, {"ops", music::make_array<Json>()}}, "RevisionConflict");

    auto elements = ok(*host, "score_query", {{"document", doc}, {"mode", "elements"}, {"query", {{"limit", 1}}}});
    require(!elements.at("elements").empty(), "element query is empty");
    require(elements.at("total").get<std::size_t>() > 1 && elements.at("next_cursor") == 1, "query did not expose its next page");
    auto next_page = ok(*host, "score_query",
                        {{"document", doc}, {"mode", "elements"}, {"expected_revision", "1"}, {"query", {{"limit", 1}, {"cursor", 1}}}});
    require(next_page.at("elements").size() == 1 && next_page.at("elements")[0].at("id") != elements.at("elements")[0].at("id"),
            "pagination did not advance");
    ok(*host, "score_query", {{"document", doc}, {"mode", "properties"}, {"target", idref(measure)}});
    ok(*host, "score_query", {{"document", doc}, {"mode", "audio_tracks"}});
    ok(*host, "score_describe", {{"scope", "types"}});
    ok(*host, "score_describe", {{"scope", "type"}, {"type", "Note"}});
    ok(*host, "score_describe", {{"scope", "styles"}});
    error(*host, "score_describe", {{"scope", "type"}, {"type", "NoSuchType"}}, "NotFound");
    bool visited = false;
    auto visit = host->with_document(doc, 1, [&](engrave::Document &d) -> music::api::Result<void> {
        visited = d.revision() == 1;
        return {};
    });
    require(visit && visited, "with_document did not visit owner document");
    auto guarded = host->with_document(doc, 1, [&](engrave::Document &) -> music::api::Result<void> {
        error(*host, "score_close", {{"document", doc}}, "InvalidArgument");
        return {};
    });
    require(bool(guarded), "with_document did not guard its document's lifetime");
    require(!host->with_document(doc, 2, [](engrave::Document &) -> music::api::Result<void> { return {}; }),
            "with_document ignored revision");
    Json wrong_thread;
    std::thread other([&] { wrong_thread = host->outline({{"document", doc}}); });
    other.join();
    require(wrong_thread.at("error").at("code") == "EngineFailure", "wrong-thread call reached the engine");

    // This fixture has consecutive quarter-note segments, which are the
    // existing positions the public EnterNotes address API requires.
    auto editable = ok(*host, "score_open", {{"path", path_text(root / "chordline_path.mscx")}});
    doc = editable.at("document").get<std::string>();
    measure = ok(*host, "score_outline", {{"document", doc}}).at("outline").at("measures")[0].at("id").get<std::string>();

    // Independent examples cover every declared Op variant and strict decoding.
    const Json r = idref(measure), a = address(measure, "0", "1"), f = fraction("1", "4");
    const Json samples = music::json_array<Json>(
        {{{"op", "set_property"}, {"target", r}, {"property", "color"}, {"value", nullptr}},
         {{"op", "reset_property"}, {"target", r}, {"property", "color"}},
         {{"op", "set_style"},
          {"score", music::make_object<Json>()},
          {"style", "staffDistance"},
          {"value", {{"kind", "real"}, {"value", 9.0}}}},
         {{"op", "enter_notes"}, {"at", a}, {"duration", f}, {"pitches", music::json_array<Json>({Json{{"pitch", 60}}})}},
         {{"op", "set_pitch"}, {"note", r}, {"pitch", 62}},
         {{"op", "set_duration"}, {"chord_rest", r}, {"duration", f}},
         {{"op", "remove"}, {"target", r}},
         {{"op", "insert_measures"}, {"count", 1}},
         {{"op", "remove_measures"}, {"first", r}, {"last", r}},
         {{"op", "set_time_sig"}, {"measure", r}, {"sig", f}},
         {{"op", "set_key_sig"}, {"measure", r}, {"key", 1}},
         {{"op", "insert_clef"}, {"at", a}, {"clef", "G"}},
         {{"op", "set_layout_break"}, {"measure", r}, {"kind", "page"}, {"on", true}},
         {{"op", "add_part"}, {"instrument_id", "keyboard.piano"}},
         {{"op", "remove_part"}, {"part", r}},
         {{"op", "add_staff"}, {"part", r}},
         {{"op", "remove_staff"}, {"staff", r}},
         {{"op", "generate_excerpts"}, {"parts", music::json_array<Json>({r})}},
         {{"op", "add_spanner"}, {"type", "Slur"}, {"start", r}, {"end", r}},
         {{"op", "add_tie"}, {"note", r}},
         {{"op", "add_tuplet"}, {"first", r}, {"last", r}},
         {{"op", "add_text"}, {"at", a}, {"style", "staff"}, {"text", "hello"}},
         {{"op", "add"}, {"anchor", r}, {"type", "Dynamic"}}});
    require(samples.size() == music::api::operation_names.size(), "operation coverage count differs");
    for (std::size_t i = 0; i < samples.size(); ++i) {
        auto decoded = music::api::decode_batch(music::json_array<Json>({samples[i]}));
        require(decoded && decoded->ops[0].index() == i && samples[i].at("op") == std::string(music::api::operation_names[i]),
                "operation failed codec coverage: " + music::dump_json(samples[i]));
        auto extra = samples[i];
        extra["unknown"] = 1;
        require(!music::api::decode_batch(music::json_array<Json>({extra})), "unknown operation key accepted");
    }
    require(!music::api::decode_batch(music::json_array<Json>({Json{{"op", "remove"}, {"target", opref(0, 0)}}})),
            "forward OpRef accepted");
    auto bad_fraction = samples[3];
    bad_fraction["duration"]["num"] = "9223372036854775808";
    require(!music::api::decode_batch(music::json_array<Json>({bad_fraction})), "overflowing int64 accepted");
    auto malformed = error(*host, "score_edit", {{"document", doc}, {"ops", music::json_array<Json>({bad_fraction})}}, "InvalidArgument");
    require(malformed.at("error").at("op_index") == 0, "invalid operation omitted its index");
    const Json values = music::json_array<Json>(
        {nullptr, Json{{"kind", "bool"}, {"value", true}}, Json{{"kind", "int"}, {"value", "-9223372036854775808"}},
         Json{{"kind", "int"}, {"value", "9223372036854775807"}}, Json{{"kind", "real"}, {"value", 1.5}},
         Json{{"kind", "string"}, {"value", "text"}}, Json{{"kind", "fraction"}, {"value", fraction("-1", "4")}},
         Json{{"kind", "point"}, {"value", {{"x", 1.5}, {"y", -2.0}}}}, Json{{"kind", "size"}, {"value", {{"w", 4.0}, {"h", 5.0}}}},
         Json{{"kind", "color"}, {"value", {{"r", 1}, {"g", 2}, {"b", 3}, {"a", 4}}}}, Json{{"kind", "enum"}, {"value", "up"}},
         Json{{"kind", "int_list"}, {"value", music::json_array<Json>({"-1", "0", "9223372036854775807"})}}});
    for (const auto &value : values.get_array()) {
        auto item = samples[0];
        item["value"] = value;
        auto decoded = music::api::decode_batch(music::json_array<Json>({item}));
        require(decoded && music::api::encode(std::get<engrave::SetProperty>(decoded->ops[0]).value) == value,
                "value codec changed a tagged value");
    }
    auto opaque = samples[0];
    opaque["value"] = Json{{"kind", "opaque"}, {"type", "path"}};
    require(!music::api::decode_batch(music::json_array<Json>({opaque})), "opaque edit value accepted");
    require(music::api::decode_revision("18446744073709551615").has_value() &&
                !music::api::decode_revision("18446744073709551616").has_value() && !music::api::decode_revision("01").has_value(),
            "revision decimal bounds not enforced");
    const Json addresses =
        music::json_array<Json>({a, Json{{"kind", "page"}, {"score", music::make_object<Json>()}, {"index", 0}},
                                 Json{{"kind", "system"}, {"score", music::make_object<Json>()}, {"page", 0}, {"index", 0}},
                                 Json{{"kind", "spanner_segment"}, {"spanner", measure}, {"index", 0}}});
    for (const auto &addr : addresses.get_array()) {
        require(music::api::decode_ref(Json{{"kind", "address"}, {"address", addr}}).has_value(),
                "address codec rejected a declared address");
    }
    engrave::Positions positions;
    positions.page_width = 100;
    positions.page_height = 200;
    positions.elements.push_back({3, 1, 2, 4, 5, 0});
    positions.events.push_back({3, 123});
    require(music::api::encode(positions) ==
                Json({{"page_size", {{"width", 100}, {"height", 200}}},
                      {"elements", music::json_array<Json>({Json{{"id", 3}, {"x", 1}, {"y", 2}, {"sx", 4}, {"sy", 5}, {"page", 0}}})},
                      {"events", music::json_array<Json>({Json{{"elid", 3}, {"position", 123}}})}}),
            "position codec mapping differs");

    // Exercise every variant through Document::edit, including invalid
    // combinations that the engine must reject with an operation index.
    auto all_ops_doc = ok(*host, "score_open", {{"path", path_text(root / "chordline_path.mscx")}}).at("document").get<std::string>();
    for (const auto &operation : samples.get_array()) {
        auto out = call(*host, "score_edit", {{"document", all_ops_doc}, {"ops", music::json_array<Json>({operation})}});
        if (!out.at("ok").get<bool>()) {
            require(music::json_value<int>(out.at("error"), "op_index", -1) == 0 && out.at("error").at("code") != "EngineFailure",
                    "operation did not reach the engine: " + music::dump_json(operation) + " => " + music::dump_json(out));
        }
    }
    ok(*host, "score_close", {{"document", all_ops_doc}});

    Json enter1 = {{"op", "enter_notes"},
                   {"at", address(measure, "0", "1")},
                   {"duration", f},
                   {"pitches", music::json_array<Json>({Json{{"pitch", 60}}})}};
    auto bad_text = error(
        *host, "score_edit",
        {{"document", doc},
         {"ops",
          music::json_array<Json>({Json{{"op", "add_text"}, {"at", a}, {"style", "staff"}, {"text", std::string("bad\xef\xbf\xbe")}}})}},
        "InvalidArgument");
    require(bad_text.at("error").at("op_index") == 0, "invalid text did not identify its operation");
    error(*host, "score_edit",
          {{"document", doc},
           {"ops", music::json_array<Json>({Json{{"op", "set_property"},
                                                 {"target", r},
                                                 {"property", "text"},
                                                 {"value", {{"kind", "string"}, {"value", std::string("bad\x01")}}}}})}},
          "InvalidArgument");
    Json enter2 = {{"op", "enter_notes"},
                   {"at", address(measure, "1", "4")},
                   {"duration", f},
                   {"pitches", music::json_array<Json>({Json{{"pitch", 60}}})}};
    auto entered =
        ok(*host, "score_edit",
           {{"document", doc},
            {"ops", music::json_array<Json>({enter1, enter2, Json{{"op", "set_pitch"}, {"note", opref(0, 1)}, {"pitch", 60}}})}});
    auto first_chord = entered.at("ops")[0].at("created")[0].get<std::string>();
    auto first_note = entered.at("ops")[0].at("created")[1].get<std::string>();
    auto second_chord = entered.at("ops")[1].at("created")[0].get<std::string>();
    ok(*host, "score_edit",
       {{"document", doc}, {"ops", music::json_array<Json>({Json{{"op", "set_pitch"}, {"note", idref(first_note)}, {"pitch", 60}}})}});
    ok(*host, "score_edit",
       {{"document", doc},
        {"ops", music::json_array<Json>(
                    {Json{{"op", "add_spanner"}, {"type", "Slur"}, {"start", idref(first_chord)}, {"end", idref(second_chord)}}})}});
    ok(*host, "checkpoint", {{"document", doc}, {"name", "before_stale_opref"}});
    auto stale_opref = error(*host, "score_edit",
                             {{"document", doc},
                              {"ops", music::json_array<Json>({Json{{"op", "add_tie"}, {"note", idref(first_note)}},
                                                               Json{{"op", "remove"}, {"target", opref(0, 0)}},
                                                               Json{{"op", "remove"}, {"target", opref(0, 0)}}})}},
                             "StaleHandle");
    require(stale_opref.at("error").at("op_index") == 2 && stale_opref.at("error").at("applied") == 2 &&
                !stale_opref.at("error").contains("poisoned") && stale_opref.at("error").contains("revision") &&
                stale_opref.at("error").at("created").empty() && stale_opref.at("error").at("removed").empty() &&
                stale_opref.at("error").at("replaced").empty(),
            "stale OpRef did not identify the operation, applied count, and prefix diff");
    ok(*host, "restore_checkpoint", {{"document", doc}, {"name", "before_stale_opref"}});
    ok(*host, "drop_checkpoint", {{"document", doc}, {"name", "before_stale_opref"}});
    auto ephemeral_tie = ok(*host, "score_edit",
                            {{"document", doc},
                             {"ops", music::json_array<Json>({Json{{"op", "add_tie"}, {"note", idref(first_note)}},
                                                              Json{{"op", "remove"}, {"target", opref(0, 0)}}})}});
    require(ephemeral_tie.at("ops")[0].at("created").size() == 1 && ephemeral_tie.at("ops")[0].at("created")[0].is_null() &&
                ephemeral_tie.at("created").empty() && ephemeral_tie.at("removed").empty(),
            "in-batch removal did not yield a null created slot");
    auto tie = ok(*host, "score_edit",
                  {{"document", doc}, {"ops", music::json_array<Json>({Json{{"op", "add_tie"}, {"note", idref(first_note)}}})}});
    require(tie.at("ops")[0].at("created").size() == 1 && tie.at("ops")[0].at("created")[0].is_string(),
            "tie did not return a persisted created ID");
    const auto tie_id = tie.at("ops")[0].at("created")[0].get<std::string>();
    auto queried_ties =
        ok(*host, "score_query", {{"document", doc}, {"mode", "elements"}, {"query", {{"types", music::json_array<Json>({"Tie"})}}}});
    bool found_tie = false;
    for (const auto &element : queried_ties.at("elements").get_array()) {
        if (element.at("id") == tie_id) {
            found_tie = true;
            break;
        }
    }
    require(found_tie, "queried ties did not include created tie ID");
    ok(*host, "score_edit",
       {{"document", doc}, {"ops", music::json_array<Json>({Json{{"op", "add_text"}, {"at", a}, {"style", "staff"}, {"text", "hello"}}})}});
    ok(*host, "score_edit",
       {{"document", doc},
        {"ops", music::json_array<Json>({Json{{"op", "set_style"},
                                              {"score", music::make_object<Json>()},
                                              {"style", "staffDistance"},
                                              {"value", {{"kind", "real"}, {"value", 9.0}}}}})}});
    ok(*host, "score_edit", {{"document", doc}, {"ops", music::json_array<Json>({Json{{"op", "insert_measures"}, {"count", 1}}})}});
    ok(*host, "score_edit", {{"document", doc}, {"ops", music::json_array<Json>({Json{{"op", "add_part"}, {"instrument_id", "piano"}}})}});

    auto linked = ok(*host, "score_open", {{"path", path_text(input)}});
    const auto linked_doc = linked.at("document").get<std::string>();
    auto notes = ok(*host, "score_query",
                    {{"document", linked_doc}, {"mode", "elements"}, {"query", {{"types", music::json_array<Json>({"Note"})}}}});
    Json target;
    for (const auto &note : notes.at("elements").get_array()) {
        if (note.at("links").size() > 1) {
            target = note;
            break;
        }
    }
    require(target.is_object(), "fixture has no linked note");
    const auto note = target.at("id").get<std::string>(), copy = target.at("links")[1].get<std::string>();
    auto initial_note_props = ok(*host, "score_query",
                                 {{"document", linked_doc},
                                  {"mode", "properties"},
                                  {"target", idref(note)},
                                  {"props", music::json_array<Json>({"color", "pitch"})}});
    const Json initial_color = initial_note_props.at("properties")[0].at("value");
    const Json initial_pitch = initial_note_props.at("properties")[1].at("value");
    require(initial_pitch != Json({{"kind", "int"}, {"value", "74"}}), "fixture note already at test pitch");

    ok(*host, "checkpoint", {{"document", linked_doc}, {"name", "initial"}});
    Json color = {{"kind", "color"}, {"value", {{"r", 12}, {"g", 34}, {"b", 56}, {"a", 255}}}};
    ok(*host, "score_edit",
       {{"document", linked_doc},
        {"ops",
         music::json_array<Json>({Json{{"op", "set_property"}, {"target", idref(note)}, {"property", "color"}, {"value", color}}})}});
    ok(*host, "score_edit",
       {{"document", linked_doc}, {"ops", music::json_array<Json>({Json{{"op", "set_pitch"}, {"note", idref(note)}, {"pitch", 74}}})}});
    auto linked_prop =
        ok(*host, "score_query",
           {{"document", linked_doc}, {"mode", "properties"}, {"target", idref(copy)}, {"props", music::json_array<Json>({"pitch"})}});
    require(linked_prop.at("properties")[0].at("value") == Json({{"kind", "int"}, {"value", "74"}}), "linked pitch did not propagate");

    auto restored = ok(*host, "restore_checkpoint", {{"document", linked_doc}, {"name", "initial"}});
    require(restored.at("revision") != linked_prop.at("revision"), "restore_checkpoint did not advance revision");
    auto after_restore = ok(*host, "score_query",
                            {{"document", linked_doc},
                             {"mode", "properties"},
                             {"target", idref(note)},
                             {"props", music::json_array<Json>({"color", "pitch"})}});
    require(after_restore.at("properties")[0].at("value") == initial_color &&
                after_restore.at("properties")[1].at("value") == initial_pitch,
            "restore_checkpoint did not revert note edits");
    auto copy_after_restore =
        ok(*host, "score_query",
           {{"document", linked_doc}, {"mode", "properties"}, {"target", idref(copy)}, {"props", music::json_array<Json>({"pitch"})}});
    require(copy_after_restore.at("properties")[0].at("value") == initial_pitch, "restore_checkpoint did not revert linked copy pitch");

    auto export_mscx = [&](const std::string &token) {
        std::vector<std::byte> out;
        auto res = host->with_document(token, {}, [&](engrave::Document &d) -> music::api::Result<void> {
            auto saved = d.export_score({.format = engrave::Format::MSCX});
            if (!saved) return std::unexpected(music::api::api_error(saved.error()));
            out = std::move(*saved);
            return {};
        });
        require(bool(res), res ? "" : res.error().message);
        return out;
    };
    const auto mscx_before = export_mscx(linked_doc);

    // Preflight failures change nothing and leave the document unpoisoned.
    Json blue = {{"kind", "color"}, {"value", {{"r", 0}, {"g", 0}, {"b", 255}, {"a", 255}}}};
    const auto before_preflight_rev = after_restore.at("revision");
    auto preflight_fail = error(
        *host, "score_edit",
        {{"document", linked_doc},
         {"ops", music::json_array<Json>({Json{{"op", "set_property"}, {"target", idref(note)}, {"property", "color"}, {"value", blue}},
                                          Json{{"op", "set_property"},
                                               {"target", idref(target.at("measure").get<std::string>())},
                                               {"property", "pitch"},
                                               {"value", {{"kind", "int"}, {"value", "72"}}}}})}},
        "InvalidArgument");
    require(preflight_fail.at("error").at("op_index") == 1 && !preflight_fail.at("error").contains("poisoned") &&
                preflight_fail.at("error").at("applied") == 0,
            "preflight failure reported wrong state");
    auto after_preflight =
        ok(*host, "score_query",
           {{"document", linked_doc}, {"mode", "properties"}, {"target", idref(note)}, {"props", music::json_array<Json>({"color"})}});
    require(after_preflight.at("revision") == before_preflight_rev && after_preflight.at("properties")[0].at("value") == initial_color,
            "preflight failure altered document");

    // Mid-batch rejection keeps prefix ops applied without poisoning the document.
    auto mid_fail = error(
        *host, "score_edit",
        {{"document", linked_doc},
         {"ops",
          music::json_array<Json>({Json{{"op", "set_property"}, {"target", idref(note)}, {"property", "color"}, {"value", blue}},
                                   Json{{"op", "set_pitch"}, {"note", idref(target.at("measure").get<std::string>())}, {"pitch", 72}}})}},
        "InvalidArgument");
    require(mid_fail.at("error").at("op_index") == 1 && mid_fail.at("error").at("applied") == 1 &&
                !mid_fail.at("error").contains("poisoned") && mid_fail.at("error").at("created").empty() &&
                mid_fail.at("error").at("removed").empty() && mid_fail.at("error").at("replaced").empty(),
            "mid-batch failure did not report op_index, applied count, and unpoisoned prefix diff");
    auto after_mid =
        ok(*host, "score_query",
           {{"document", linked_doc}, {"mode", "properties"}, {"target", idref(note)}, {"props", music::json_array<Json>({"color"})}});
    require(after_mid.at("revision") != before_preflight_rev && mid_fail.at("error").at("revision") == after_mid.at("revision") &&
                after_mid.at("properties")[0].at("value") == blue,
            "mid-batch failure did not retain completed prefix op");
    require(export_mscx(linked_doc) != mscx_before, "mid-batch edit did not change native export");
    ok(*host, "score_edit",
       {{"document", linked_doc},
        {"expected_revision", mid_fail.at("error").at("revision")},
        {"ops", music::json_array<Json>({Json{{"op", "set_pitch"}, {"note", idref(note)}, {"pitch", 75}}})}});
    auto after_followup =
        ok(*host, "score_query",
           {{"document", linked_doc}, {"mode", "properties"}, {"target", idref(note)}, {"props", music::json_array<Json>({"pitch"})}});
    require(after_followup.at("properties")[0].at("value") == Json({{"kind", "int"}, {"value", "75"}}),
            "follow-up edit after mid-batch rejection failed");

    ok(*host, "restore_checkpoint", {{"document", linked_doc}, {"name", "initial"}});
    require(export_mscx(linked_doc) == mscx_before, "restore_checkpoint did not restore original native export bytes");

    // Execution-time rejection after an op creates IDs exposes those IDs on the error for follow-up edits.
    const auto prefix_doc =
        ok(*host, "score_create", {{"template_id", "Treble_Clef"}, {"title", "Prefix test"}}).at("document").get<std::string>();
    const auto prefix_m0 =
        ok(*host, "score_outline", {{"document", prefix_doc}}).at("outline").at("measures")[0].at("id").get<std::string>();
    auto prefix_rests = ok(*host, "score_query",
                           {{"document", prefix_doc},
                            {"mode", "elements"},
                            {"query", {{"types", music::json_array<Json>({"Rest"})}, {"first_measure", 0}, {"measure_count", 1}}}});
    require(prefix_rests.at("elements").size() == 1, "expected one initial rest in template measure");
    const auto prefix_rest = prefix_rests.at("elements")[0].at("id").get<std::string>();
    auto prefix_fail = error(*host, "score_edit",
                             {{"document", prefix_doc},
                              {"expected_revision", "1"},
                              {"ops", music::json_array<Json>({Json{{"op", "enter_notes"},
                                                                    {"at", address(prefix_m0, "0", "1")},
                                                                    {"duration", fraction("1", "4")},
                                                                    {"pitches", music::json_array<Json>({Json{{"pitch", 60}}})}},
                                                               Json{{"op", "remove"}, {"target", idref(prefix_rest)}}})}},
                             "StaleHandle");
    const auto &prefix_err = prefix_fail.at("error");
    require(prefix_err.at("op_index") == 1 && prefix_err.at("applied") == 1 && !prefix_err.contains("poisoned") &&
                prefix_err.at("revision") == "2" && prefix_err.at("created").size() == 4 && prefix_err.at("removed").empty() &&
                prefix_err.at("replaced") == music::json_array<Json>({prefix_rest}),
            "prefix rejection returned unexpected error metadata");
    auto prefix_notes = ok(*host, "score_query",
                           {{"document", prefix_doc},
                            {"mode", "elements"},
                            {"query", {{"types", music::json_array<Json>({"Note"})}, {"props", music::json_array<Json>({"pitch"})}}}});
    require(prefix_notes.at("elements").size() == 1, "expected one created note in score");
    const auto created_note = prefix_notes.at("elements")[0].at("id").get<std::string>();
    const auto &created_ids = prefix_err.at("created").get_array();
    require(prefix_notes.at("elements")[0].at("props")[0].at("value") == Json({{"kind", "int"}, {"value", "60"}}) &&
                std::ranges::find(created_ids, Json(created_note)) != created_ids.end(),
            "created note ID or pitch missing from score after prefix rejection");
    ok(*host, "score_edit",
       {{"document", prefix_doc},
        {"expected_revision", prefix_err.at("revision")},
        {"ops", music::json_array<Json>({Json{{"op", "set_pitch"}, {"note", idref(created_note)}, {"pitch", 67}}})}});
    auto pitched_notes = ok(*host, "score_query",
                            {{"document", prefix_doc},
                             {"mode", "elements"},
                             {"query", {{"types", music::json_array<Json>({"Note"})}, {"props", music::json_array<Json>({"pitch"})}}}});
    require(pitched_notes.at("elements").size() == 1 &&
                pitched_notes.at("elements")[0].at("props")[0].at("value") == Json({{"kind", "int"}, {"value", "67"}}),
            "follow-up edit using prefix-created note ID did not update score");
    ok(*host, "score_close", {{"document", prefix_doc}});
    auto recovered_props = ok(*host, "score_query",
                              {{"document", linked_doc},
                               {"mode", "properties"},
                               {"target", idref(note)},
                               {"props", music::json_array<Json>({"color", "pitch"})}});
    require(recovered_props.at("properties")[0].at("value") == initial_color &&
                recovered_props.at("properties")[1].at("value") == initial_pitch,
            "restore_checkpoint did not clear mid-batch edit");
    ok(*host, "score_edit",
       {{"document", linked_doc}, {"ops", music::json_array<Json>({Json{{"op", "set_pitch"}, {"note", idref(note)}, {"pitch", 76}}})}});

    error(*host, "restore_checkpoint", {{"document", linked_doc}, {"name", "missing"}}, "NotFound");
    error(*host, "drop_checkpoint", {{"document", linked_doc}, {"name", "missing"}}, "NotFound");
    ok(*host, "drop_checkpoint", {{"document", linked_doc}, {"name", "initial"}});
    error(*host, "restore_checkpoint", {{"document", linked_doc}, {"name", "initial"}}, "NotFound");

    auto bytes = read(input);
    auto opened_bytes = ok(*host, "score_open", {{"data", b64(bytes)}, {"format", "mscx"}});
    ok(*host, "score_close", {{"document", opened_bytes.at("document")}});
    auto old = bytes;
    const auto pos = old.find("version=\"4.70\"");
    require(pos != std::string::npos, "fixture version missing");
    old.replace(pos, 14, "version=\"4.50\"");
    error(*host, "score_open", {{"data", b64(old)}, {"format", "mscx"}}, "Unsupported");
    const auto old_path = scratch / "old.mscx";
    {
        std::ofstream f(old_path, std::ios::binary);
        f << old;
    }
    error(*host, "score_open", {{"path", path_text(old_path)}}, "Unsupported");
    ok(*host, "score_close", {{"document", linked_doc}});
    error(*host, "score_outline", {{"document", linked_doc}}, "NotFound");
    auto by_template_path = ok(*host, "score_create", {{"template_path", info.at("templates")[0].at("path")}});
    require(by_template_path.at("document") != linked_doc, "closed token was reused");
    ok(*host, "score_close", {{"document", by_template_path.at("document")}});

    // Reject a genuinely corrupt score without damaging another open document.
    const auto corrupt_path = scratch / "corrupt.mscx";
    {
        std::ofstream f(corrupt_path, std::ios::binary);
        f << "not a MuseScore score";
    }
    error(*host, "score_open", {{"path", path_text(corrupt_path)}}, "InvalidInput");
    ok(*host, "score_outline", {{"document", doc}});

    // A public callback failure exercises the host's recovery probe and
    // lifetime guard without access to the engine's private edit hooks.
    auto recovery = ok(*host, "score_open", {{"path", path_text(input)}});
    const auto recovery_doc = recovery.at("document").get<std::string>();
    auto failed = host->with_document(
        recovery_doc, {}, [](engrave::Document &) -> music::api::Result<void> { throw std::runtime_error("callback failure"); });
    require(!failed && failed.error().code == "EngineFailure", "callback failure was not reported");
    ok(*host, "score_query", {{"document", recovery_doc}, {"mode", "audio_tracks"}});
    bool reached = false;
    require(bool(host->with_document(recovery_doc, {},
                                     [&](engrave::Document &) -> music::api::Result<void> {
                                         reached = true;
                                         return {};
                                     })) &&
                reached,
            "healthy document was not recovered after callback failure");
    const Json style = {{"op", "set_style"},
                        {"score", music::make_object<Json>()},
                        {"style", "staffDistance"},
                        {"value", {{"kind", "real"}, {"value", 9.0}}}};
    ok(*host, "score_edit", {{"document", recovery_doc}, {"ops", music::json_array<Json>({style})}});
    ok(*host, "score_close", {{"document", recovery_doc}});
    fs::remove(corrupt_path);

    const auto revision_before_assets = ok(*host, "score_outline", {{"document", doc}}).at("revision");
    auto font_info = ok(*host, "runtime_info", {{"action", "add_font"}, {"path", path_text(add_font_input)}});
    require(font_info.at("fonts").size() == 2, "runtime font missing");
    auto duplicate_font = ok(*host, "runtime_info", {{"action", "add_font"}, {"path", path_text(add_font_input)}});
    require(duplicate_font.at("fonts") == font_info.at("fonts"), "duplicate font was not idempotent");
    auto font_capture = host->runtime_options();
    require(bool(font_capture), "font capture failed");
    const auto new_font = font_capture->fonts[1];
    const auto new_font_bytes = read(new_font);
    {
        std::ofstream f(add_font_input, std::ios::binary | std::ios::trunc);
        f << "changed";
    }
    require(read(new_font) == new_font_bytes && initial->fonts.size() == 1, "font snapshot changed");
    error(*host, "runtime_info", {{"action", "add_font"}, {"path", path_text(add_font_input)}}, "InvalidInput");
    require(host->runtime_options()->fonts.size() == 2, "failed font change altered the selection");
    auto selected =
        ok(*host, "runtime_info", {{"action", "set_soundfonts"}, {"files", music::json_array<Json>({path_text(replacement_sf)})}});
    auto replacement = host->runtime_options();
    require(bool(replacement), "soundfont capture failed");
    const auto selected_path = replacement->soundfonts[0];
    require(read(selected_path) == read(replacement_sf), "selected SoundFont bytes differ");
    error(*host, "runtime_info", {{"action", "set_soundfonts"}, {"files", music::json_array<Json>({path_text(invalid_sf)})}},
          "InvalidInput");
    auto retained = host->runtime_options();
    require(retained && retained->soundfonts[0] == selected_path, "failed SoundFont change replaced selection");
    ok(*host, "runtime_info", {{"action", "set_soundfonts"}, {"files", music::make_array<Json>()}});
    require(!fs::exists(initial_sf) && !fs::exists(selected_path), "replaced assets survived selection removal");
    auto still_open = ok(*host, "score_outline", {{"document", doc}});
    require(still_open.at("revision") == revision_before_assets, "runtime assets changed document revision");
    // Keep a document open: HostApi's destructor must close it before Engine.
    host.reset();
    require(!fs::exists(initial_font) && !fs::exists(initial_sf) && !fs::exists(new_font) && !fs::exists(selected_path),
            "host did not release assets");
    fs::remove(font_input);
    fs::remove(add_font_input);
    fs::remove(sf_input);
    fs::remove(invalid_sf);
    fs::remove(old_path);
    fs::remove(scratch);
    std::fputs("host_api_test passed\n", stdout);
    std::fflush(stdout);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    for (int i = 1; i < argc; ++i) g_args.emplace_back(argv[i]);
    return RUN_ALL_TESTS();
}
