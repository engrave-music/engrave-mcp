#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "host_api.h"

namespace fs = std::filesystem;
using music::v2::HostApi;
using music::v2::Json;

namespace {
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
    } else if (tool == "runtime_info") {
        out = host.runtime_info(arg);
    } else {
        throw std::logic_error("unknown test tool");
    }
    std::cout << "HOST_CASE " << Json{{"tool", tool}, {"arguments", arg}, {"result", out}}.dump() << '\n';
    return out;
}

Json ok(HostApi &host, const std::string &tool, const Json &arg) {
    auto out = call(host, tool, arg);
    require(out.value("ok", false), tool + " failed: " + out.dump());
    return out;
}

Json error(HostApi &host, const std::string &tool, const Json &arg, const std::string &code) {
    auto out = call(host, tool, arg);
    require(!out.value("ok", true) && out.at("error").at("code") == code, tool + " expected " + code + ": " + out.dump());
    return out;
}
} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 4, "usage: host_api_test ASSETS SOUNDFONT REPLACEMENT_SOUNDFONT");
        const auto assets = fs::absolute(fs::u8path(argv[1]));
        const auto original_sf = fs::absolute(fs::u8path(argv[2]));
        const auto replacement_sf = fs::absolute(fs::u8path(argv[3]));
        const auto root = fs::absolute("fixtures/golden");
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
        music::v2::ExportLimits configured;
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

        auto info = ok(*host, "runtime_info", Json::object());
        require(info.at("tool_revision") == "2" && !info.at("templates").empty(), "runtime catalogue is empty");
        const auto &limits = info.at("limits");
        require(limits.at("output_bytes") == "234567" && limits.at("deadline_ms") == 3456 &&
                    limits.at("transport_request_bytes") == 4194304,
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
        auto measure = outline.at("outline").at("measures").at(0).at("id").get<std::string>();
        auto foreign = error(*host, "score_outline", {{"document", "doc-foreign-1"}}, "NotFound");
        require(foreign.at("error").at("document") == "doc-foreign-1", "foreign document omitted");
        auto conflict = error(*host, "score_outline", {{"document", doc}, {"expected_revision", "2"}}, "RevisionConflict");
        require(conflict.at("error").at("current_revision") == "1", "read conflict omitted current revision");
        error(*host, "score_query", {{"document", doc}, {"mode", "elements"}, {"expected_revision", "18446744073709551616"}},
              "InvalidArgument");
        error(*host, "score_query", {{"document", doc}, {"mode", "elements"}, {"query", {{"unknown", 1}}}}, "InvalidArgument");
        auto no_op = ok(*host, "score_edit", {{"document", doc}, {"expected_revision", "1"}, {"ops", Json::array()}});
        require(no_op.at("revision") == "1", "empty batch changed the revision");
        error(*host, "score_edit", {{"document", doc}, {"expected_revision", "0"}, {"ops", Json::array()}}, "RevisionConflict");

        auto elements = ok(*host, "score_query", {{"document", doc}, {"mode", "elements"}, {"query", {{"limit", 1}}}});
        require(!elements.at("elements").empty(), "element query is empty");
        require(elements.at("total").get<std::size_t>() > 1 && elements.at("next_cursor") == 1, "query did not expose its next page");
        auto next_page =
            ok(*host, "score_query",
               {{"document", doc}, {"mode", "elements"}, {"expected_revision", "1"}, {"query", {{"limit", 1}, {"cursor", 1}}}});
        require(next_page.at("elements").size() == 1 && next_page.at("elements").at(0).at("id") != elements.at("elements").at(0).at("id"),
                "pagination did not advance");
        ok(*host, "score_query", {{"document", doc}, {"mode", "properties"}, {"target", idref(measure)}});
        ok(*host, "score_query", {{"document", doc}, {"mode", "audio_tracks"}});
        ok(*host, "score_describe", {{"scope", "types"}});
        ok(*host, "score_describe", {{"scope", "type"}, {"type", "Note"}});
        ok(*host, "score_describe", {{"scope", "styles"}});
        error(*host, "score_describe", {{"scope", "type"}, {"type", "NoSuchType"}}, "NotFound");
        bool visited = false;
        auto visit = host->with_document(doc, 1, [&](engrave::Document &d) -> music::v2::Result<void> {
            visited = d.revision() == 1;
            return {};
        });
        require(visit && visited, "with_document did not visit owner document");
        auto guarded = host->with_document(doc, 1, [&](engrave::Document &) -> music::v2::Result<void> {
            error(*host, "score_close", {{"document", doc}}, "InvalidArgument");
            return {};
        });
        require(bool(guarded), "with_document did not guard its document's lifetime");
        require(!host->with_document(doc, 2, [](engrave::Document &) -> music::v2::Result<void> { return {}; }),
                "with_document ignored revision");
        Json wrong_thread;
        std::thread other([&] { wrong_thread = host->outline({{"document", doc}}); });
        other.join();
        require(wrong_thread.at("error").at("code") == "EngineFailure", "wrong-thread call reached the engine");

        // This fixture has consecutive quarter-note segments, which are the
        // existing positions the public EnterNotes address API requires.
        auto editable = ok(*host, "score_open", {{"path", path_text(root / "chordline_path.mscx")}});
        doc = editable.at("document").get<std::string>();
        measure = ok(*host, "score_outline", {{"document", doc}}).at("outline").at("measures").at(0).at("id").get<std::string>();

        // Independent examples cover every declared Op variant and strict decoding.
        const Json r = idref(measure), a = address(measure, "0", "1"), f = fraction("1", "4");
        const Json samples = Json::array(
            {{{"op", "set_property"}, {"target", r}, {"property", "color"}, {"value", nullptr}},
             {{"op", "reset_property"}, {"target", r}, {"property", "color"}},
             {{"op", "set_style"}, {"score", Json::object()}, {"style", "staffDistance"}, {"value", {{"kind", "real"}, {"value", 9.0}}}},
             {{"op", "enter_notes"}, {"at", a}, {"duration", f}, {"pitches", Json::array({Json{{"pitch", 60}}})}},
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
             {{"op", "generate_excerpts"}, {"parts", Json::array({r})}},
             {{"op", "add_spanner"}, {"type", "Slur"}, {"start", r}, {"end", r}},
             {{"op", "add_tie"}, {"note", r}},
             {{"op", "add_tuplet"}, {"first", r}, {"last", r}},
             {{"op", "add_text"}, {"at", a}, {"style", "staff"}, {"text", "hello"}},
             {{"op", "add"}, {"anchor", r}, {"type", "Dynamic"}}});
        require(samples.size() == music::v2::operation_names.size(), "operation coverage count differs");
        for (std::size_t i = 0; i < samples.size(); ++i) {
            auto decoded = music::v2::decode_batch(Json::array({samples.at(i)}));
            require(decoded && decoded->ops.at(0).index() == i && samples.at(i).at("op") == std::string(music::v2::operation_names[i]),
                    "operation failed codec coverage: " + samples.at(i).dump());
            auto extra = samples.at(i);
            extra["unknown"] = 1;
            require(!music::v2::decode_batch(Json::array({extra})), "unknown operation key accepted");
        }
        require(!music::v2::decode_batch(Json::array({Json{{"op", "remove"}, {"target", opref(0, 0)}}})), "forward OpRef accepted");
        auto bad_fraction = samples.at(3);
        bad_fraction["duration"]["num"] = "9223372036854775808";
        require(!music::v2::decode_batch(Json::array({bad_fraction})), "overflowing int64 accepted");
        auto malformed = error(*host, "score_edit", {{"document", doc}, {"ops", Json::array({bad_fraction})}}, "InvalidArgument");
        require(malformed.at("error").at("op_index") == 0, "invalid operation omitted its index");
        const Json values = Json::array(
            {nullptr, Json{{"kind", "bool"}, {"value", true}}, Json{{"kind", "int"}, {"value", "-9223372036854775808"}},
             Json{{"kind", "int"}, {"value", "9223372036854775807"}}, Json{{"kind", "real"}, {"value", 1.5}},
             Json{{"kind", "string"}, {"value", "text"}}, Json{{"kind", "fraction"}, {"value", fraction("-1", "4")}},
             Json{{"kind", "point"}, {"value", {{"x", 1.5}, {"y", -2.0}}}}, Json{{"kind", "size"}, {"value", {{"w", 4.0}, {"h", 5.0}}}},
             Json{{"kind", "color"}, {"value", {{"r", 1}, {"g", 2}, {"b", 3}, {"a", 4}}}}, Json{{"kind", "enum"}, {"value", "up"}},
             Json{{"kind", "int_list"}, {"value", Json::array({"-1", "0", "9223372036854775807"})}}});
        for (const auto &value : values) {
            auto item = samples.at(0);
            item["value"] = value;
            auto decoded = music::v2::decode_batch(Json::array({item}));
            require(decoded && music::v2::encode(std::get<engrave::SetProperty>(decoded->ops.at(0)).value) == value,
                    "value codec changed a tagged value");
        }
        auto opaque = samples.at(0);
        opaque["value"] = {{"kind", "opaque"}, {"type", "path"}};
        require(!music::v2::decode_batch(Json::array({opaque})), "opaque edit value accepted");
        require(music::v2::decode_revision("18446744073709551615").has_value() &&
                    !music::v2::decode_revision("18446744073709551616").has_value() && !music::v2::decode_revision("01").has_value(),
                "revision decimal bounds not enforced");
        for (const auto &addr : Json::array({a, Json{{"kind", "page"}, {"score", Json::object()}, {"index", 0}},
                                             Json{{"kind", "system"}, {"score", Json::object()}, {"page", 0}, {"index", 0}},
                                             Json{{"kind", "spanner_segment"}, {"spanner", measure}, {"index", 0}}})) {
            require(music::v2::decode_ref(Json{{"kind", "address"}, {"address", addr}}).has_value(),
                    "address codec rejected a declared address");
        }
        engrave::Positions positions;
        positions.page_width = 100;
        positions.page_height = 200;
        positions.elements.push_back({3, 1, 2, 4, 5, 0});
        positions.events.push_back({3, 123});
        require(music::v2::encode(positions) ==
                    Json({{"page_size", {{"width", 100}, {"height", 200}}},
                          {"elements", Json::array({Json{{"id", 3}, {"x", 1}, {"y", 2}, {"sx", 4}, {"sy", 5}, {"page", 0}}})},
                          {"events", Json::array({Json{{"elid", 3}, {"position", 123}}})}}),
                "position codec mapping differs");

        // Exercise every variant through Document::edit, including invalid
        // combinations that the engine must reject with an operation index.
        auto all_ops_doc = ok(*host, "score_open", {{"path", path_text(root / "chordline_path.mscx")}}).at("document").get<std::string>();
        for (const auto &operation : samples) {
            auto out = call(*host, "score_edit", {{"document", all_ops_doc}, {"ops", Json::array({operation})}});
            if (!out.at("ok").get<bool>()) {
                require(out.at("error").value("op_index", -1) == 0 && out.at("error").at("code") != "EngineFailure",
                        "operation did not reach the engine: " + operation.dump() + " => " + out.dump());
            }
        }
        ok(*host, "score_close", {{"document", all_ops_doc}});

        Json enter1 = {
            {"op", "enter_notes"}, {"at", address(measure, "0", "1")}, {"duration", f}, {"pitches", Json::array({Json{{"pitch", 60}}})}};
        auto bad_text = error(
            *host, "score_edit",
            {{"document", doc},
             {"ops", Json::array({Json{{"op", "add_text"}, {"at", a}, {"style", "staff"}, {"text", std::string("bad\xef\xbf\xbe")}}})}},
            "InvalidArgument");
        require(bad_text.at("error").at("op_index") == 0, "invalid text did not identify its operation");
        error(*host, "score_edit",
              {{"document", doc},
               {"ops", Json::array({Json{{"op", "set_property"},
                                         {"target", r},
                                         {"property", "text"},
                                         {"value", {{"kind", "string"}, {"value", std::string("bad\x01")}}}}})}},
              "InvalidArgument");
        Json enter2 = {
            {"op", "enter_notes"}, {"at", address(measure, "1", "4")}, {"duration", f}, {"pitches", Json::array({Json{{"pitch", 60}}})}};
        auto entered = ok(
            *host, "score_edit",
            {{"document", doc}, {"ops", Json::array({enter1, enter2, Json{{"op", "set_pitch"}, {"note", opref(0, 1)}, {"pitch", 60}}})}});
        auto first_chord = entered.at("ops").at(0).at("created").at(0).get<std::string>();
        auto first_note = entered.at("ops").at(0).at("created").at(1).get<std::string>();
        auto second_chord = entered.at("ops").at(1).at("created").at(0).get<std::string>();
        ok(*host, "score_edit",
           {{"document", doc}, {"ops", Json::array({Json{{"op", "set_pitch"}, {"note", idref(first_note)}, {"pitch", 60}}})}});
        ok(*host, "score_edit",
           {{"document", doc},
            {"ops",
             Json::array({Json{{"op", "add_spanner"}, {"type", "Slur"}, {"start", idref(first_chord)}, {"end", idref(second_chord)}}})}});
        auto missing_slot = error(
            *host, "score_edit",
            {{"document", doc},
             {"ops", Json::array({Json{{"op", "add_tie"}, {"note", idref(first_note)}}, Json{{"op", "remove"}, {"target", opref(0, 0)}}})}},
            "StaleHandle");
        require(missing_slot.at("error").at("op_index") == 1, "null OpRef slot did not identify the operation");
        auto tie =
            ok(*host, "score_edit", {{"document", doc}, {"ops", Json::array({Json{{"op", "add_tie"}, {"note", idref(first_note)}}})}});
        require(!tie.at("ops").at(0).at("created").empty() && tie.at("ops").at(0).at("created").at(0).is_null(),
                "tie did not preserve its null created slot");
        ok(*host, "score_edit",
           {{"document", doc}, {"ops", Json::array({Json{{"op", "add_text"}, {"at", a}, {"style", "staff"}, {"text", "hello"}}})}});
        ok(*host, "score_edit",
           {{"document", doc},
            {"ops", Json::array({Json{{"op", "set_style"},
                                      {"score", Json::object()},
                                      {"style", "staffDistance"},
                                      {"value", {{"kind", "real"}, {"value", 9.0}}}}})}});
        ok(*host, "score_edit", {{"document", doc}, {"ops", Json::array({Json{{"op", "insert_measures"}, {"count", 1}}})}});
        ok(*host, "score_edit", {{"document", doc}, {"ops", Json::array({Json{{"op", "add_part"}, {"instrument_id", "piano"}}})}});

        auto linked = ok(*host, "score_open", {{"path", path_text(input)}});
        const auto linked_doc = linked.at("document").get<std::string>();
        auto notes =
            ok(*host, "score_query", {{"document", linked_doc}, {"mode", "elements"}, {"query", {{"types", Json::array({"Note"})}}}});
        Json target;
        for (const auto &note : notes.at("elements")) {
            if (note.at("links").size() > 1) {
                target = note;
                break;
            }
        }
        require(target.is_object(), "fixture has no linked note");
        const auto note = target.at("id").get<std::string>(), copy = target.at("links").at(1).get<std::string>();
        Json color = {{"kind", "color"}, {"value", {{"r", 12}, {"g", 34}, {"b", 56}, {"a", 255}}}};
        ok(*host, "score_edit",
           {{"document", linked_doc},
            {"ops", Json::array({Json{{"op", "set_property"}, {"target", idref(note)}, {"property", "color"}, {"value", color}}})}});
        ok(*host, "score_edit",
           {{"document", linked_doc}, {"ops", Json::array({Json{{"op", "set_pitch"}, {"note", idref(note)}, {"pitch", 74}}})}});
        auto linked_prop =
            ok(*host, "score_query",
               {{"document", linked_doc}, {"mode", "properties"}, {"target", idref(copy)}, {"props", Json::array({"pitch"})}});
        require(linked_prop.at("properties").at(0).at("value") == Json({{"kind", "int"}, {"value", "74"}}),
                "linked pitch did not propagate");
        auto before = linked_prop.at("revision").get<std::string>();
        Json blue = {{"kind", "color"}, {"value", {{"r", 0}, {"g", 0}, {"b", 255}, {"a", 255}}}};
        auto rolled = error(
            *host, "score_edit",
            {{"document", linked_doc},
             {"ops", Json::array({Json{{"op", "set_property"}, {"target", idref(note)}, {"property", "color"}, {"value", blue}},
                                  Json{{"op", "set_pitch"}, {"note", idref(target.at("measure").get<std::string>())}, {"pitch", 72}}})}},
            "InvalidArgument");
        require(rolled.at("error").at("op_index") == 1, "failed batch omitted failing operation");
        auto after = ok(*host, "score_query",
                        {{"document", linked_doc}, {"mode", "properties"}, {"target", idref(note)}, {"props", Json::array({"color"})}});
        require(after.at("revision") == before && after.at("properties").at(0).at("value") == color, "failed batch did not roll back");

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
        auto by_template_path = ok(*host, "score_create", {{"template_path", info.at("templates").at(0).at("path")}});
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
            recovery_doc, {}, [](engrave::Document &) -> music::v2::Result<void> { throw std::runtime_error("callback failure"); });
        require(!failed && failed.error().code == "EngineFailure", "callback failure was not reported");
        ok(*host, "score_query", {{"document", recovery_doc}, {"mode", "audio_tracks"}});
        bool reached = false;
        require(bool(host->with_document(recovery_doc, {},
                                         [&](engrave::Document &) -> music::v2::Result<void> {
                                             reached = true;
                                             return {};
                                         })) &&
                    reached,
                "healthy document was not recovered after callback failure");
        const Json style = {
            {"op", "set_style"}, {"score", Json::object()}, {"style", "staffDistance"}, {"value", {{"kind", "real"}, {"value", 9.0}}}};
        ok(*host, "score_edit", {{"document", recovery_doc}, {"ops", Json::array({style})}});
        ok(*host, "score_close", {{"document", recovery_doc}});
        fs::remove(corrupt_path);

        const auto revision_before_assets = ok(*host, "score_outline", {{"document", doc}}).at("revision");
        auto font_info = ok(*host, "runtime_info", {{"action", "add_font"}, {"path", path_text(add_font_input)}});
        require(font_info.at("fonts").size() == 2, "runtime font missing");
        auto duplicate_font = ok(*host, "runtime_info", {{"action", "add_font"}, {"path", path_text(add_font_input)}});
        require(duplicate_font.at("fonts") == font_info.at("fonts"), "duplicate font was not idempotent");
        auto font_capture = host->runtime_options();
        require(bool(font_capture), "font capture failed");
        const auto new_font = font_capture->fonts.at(1);
        const auto new_font_bytes = read(new_font);
        {
            std::ofstream f(add_font_input, std::ios::binary | std::ios::trunc);
            f << "changed";
        }
        require(read(new_font) == new_font_bytes && initial->fonts.size() == 1, "font snapshot changed");
        error(*host, "runtime_info", {{"action", "add_font"}, {"path", path_text(add_font_input)}}, "InvalidInput");
        require(host->runtime_options()->fonts.size() == 2, "failed font change altered the selection");
        auto selected = ok(*host, "runtime_info", {{"action", "set_soundfonts"}, {"files", Json::array({path_text(replacement_sf)})}});
        auto replacement = host->runtime_options();
        require(bool(replacement), "soundfont capture failed");
        const auto selected_path = replacement->soundfonts.at(0);
        require(read(selected_path) == read(replacement_sf), "selected SoundFont bytes differ");
        error(*host, "runtime_info", {{"action", "set_soundfonts"}, {"files", Json::array({path_text(invalid_sf)})}}, "InvalidInput");
        auto retained = host->runtime_options();
        require(retained && retained->soundfonts.at(0) == selected_path, "failed SoundFont change replaced selection");
        ok(*host, "runtime_info", {{"action", "set_soundfonts"}, {"files", Json::array()}});
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
        std::cout << "host_api_test passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "host_api_test: " << e.what() << '\n';
        return 1;
    }
}
