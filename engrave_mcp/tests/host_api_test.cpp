#include "host_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "api_json_internal.h"
#include "export_timing.h"
#include "phrase.h"

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

Json idref(const std::string &id) { return Json(id); }

Json opref(int op, int item) { return {{"from_op", op}, {"item", item}}; }

Json fraction(const char *num, const char *den) { return Json(std::string(num) + "/" + den); }

Json address(const std::string &measure, const char *num, const char *den) {
    return {{"kind", "segment"}, {"measure", measure},         {"staff", 1},
            {"voice", 1},        {"tick", fraction(num, den)}, {"segment_kind", "ChordRest"}};
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
    } else if (tool == "runtime_configure") {
        out = host.configure(arg);
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

Json pos(int measure, const char *offset = "0", int staff = 1, int voice = 1) {
    return {{"measure", measure}, {"offset", offset}, {"staff", staff}, {"voice", voice}};
}

Json enter_at(const Json &at, int pitch) {
    return {{"op", "enter_notes"}, {"at", at}, {"duration", "1/4"}, {"pitches", music::json_array<Json>({Json{{"pitch", pitch}}})}};
}

Json edit_ops(const Json &doc, std::initializer_list<Json> ops) { return {{"document", doc}, {"ops", music::json_array<Json>(ops)}}; }

// The MIDI pitch of the single-note chord at a 1-based staff and voice and a bar offset of a read_music bar, or -1.
int chord_pitch(const Json &bar, int staff, int voice, std::string_view offset) {
    for (const auto &v : bar.at("voices").get_array()) {
        if (v.at("staff") != staff || v.at("voice") != voice) continue;
        for (const auto &event : v.at("events").get_array()) {
            if (event.at("offset") == offset && event.contains("notes")) return event.at("notes")[0].at("midi").as<int>();
        }
    }
    return -1;
}

// The marking of a read_music bar whose `key` field equals `value`, or null.
const Json *find_marking(const Json &bar, std::string_view key, std::string_view value) {
    if (!bar.contains("markings")) return nullptr;
    for (const auto &mark : bar.at("markings").get_array()) {
        if (mark.contains(key) && mark.at(key) == value) return &mark;
    }
    return nullptr;
}

// Two piano staves whose first bar carries one marking of every family; eids name the elements the tests check.
constexpr std::string_view expressive_score = R"(<museScore version="5.00"><Score><Division>480</Division>
<Part id="1"><Staff/><trackName>Upper</trackName><Instrument id="piano"><longName>Upper</longName></Instrument></Part>
<Part id="2"><Staff/><trackName>Lower</trackName><Instrument id="piano"><longName>Lower</longName></Instrument></Part>
<Staff id="1">
<Measure><startRepeat/><voice>
<BarLine><subtype>start-repeat</subtype></BarLine>
<Clef><eid>B_A</eid><concertClefType>G</concertClefType><transposingClefType>G</transposingClefType><isHeader>1</isHeader></Clef>
<TimeSig><eid>B_B</eid><sigN>4</sigN><sigD>4</sigD></TimeSig>
<KeySig><eid>B_C</eid><concertKey>-2</concertKey></KeySig>
<Tempo><eid>B_D</eid><tempo>2</tempo><text>Andante</text></Tempo>
<SystemText><eid>B_E</eid><text>With motion</text></SystemText>
<RehearsalMark><eid>B_F</eid><text>A</text></RehearsalMark>
<StaffText><eid>B_G</eid><text><b>leggiero</b> &amp; clear</text></StaffText>
<Expression><eid>B_H</eid><text>dolce</text></Expression>
<Harmony><eid>B_I</eid><harmonyInfo><root>14</root><name>m7</name></harmonyInfo></Harmony>
<Dynamic><eid>B_J</eid><subtype>p</subtype></Dynamic>
<Fermata><eid>B_K</eid><subtype>fermataAbove</subtype></Fermata>
<Chord><eid>B_L</eid><durationType>16th</durationType><acciaccatura/>
<Note><pitch>62</pitch><tpc>16</tpc></Note><Articulation><eid>B_M</eid><subtype>articStaccatoAbove</subtype></Articulation></Chord>
<Chord><eid>B_N</eid><durationType>16th</durationType><acciaccatura/>
<Note><pitch>64</pitch><tpc>18</tpc></Note><Ornament><eid>B_O</eid><subtype>ornamentTurn</subtype></Ornament></Chord>
<Chord><eid>B_i</eid><durationType>eighth</durationType><grace8after/>
<Note><pitch>67</pitch><tpc>15</tpc></Note><Articulation><eid>B_j</eid><subtype>articTenutoAbove</subtype></Articulation></Chord>
<Chord><eid>B_P</eid><durationType>quarter</durationType>
<Note><pitch>65</pitch><tpc>13</tpc></Note><Note><pitch>69</pitch><tpc>17</tpc></Note>
<Articulation><eid>B_Q</eid><subtype>articAccentAbove</subtype></Articulation>
<Ornament><eid>B_R</eid><subtype>ornamentMordent</subtype></Ornament>
<Lyrics><eid>B_S</eid><syllabic>begin</syllabic><ticks_f>1/4</ticks_f><text>glo</text></Lyrics>
<Lyrics><eid>B_T</eid><no>1</no><text>light</text></Lyrics>
<Arpeggio><eid>B_U</eid><subtype>1</subtype></Arpeggio>
<TremoloSingleChord><eid>B_V</eid><subtype>r16</subtype></TremoloSingleChord>
</Chord>
<BarLine><subtype>normal</subtype></BarLine>
<Clef><eid>B_W</eid><concertClefType>F</concertClefType><transposingClefType>F</transposingClefType></Clef>
<Dynamic><eid>B_X</eid><subtype>mf</subtype><text>mezzoforte</text></Dynamic>
<Chord><durationType>quarter</durationType><Note><pitch>60</pitch><tpc>14</tpc></Note>
<Lyrics><eid>B_Y</eid><syllabic>middle</syllabic><text>ri</text></Lyrics></Chord>
<Chord><durationType>half</durationType><Note><pitch>62</pitch><tpc>16</tpc></Note>
<Lyrics><eid>B_Z</eid><syllabic>end</syllabic><text>a</text></Lyrics></Chord>
<BarLine><eid>B_a</eid><subtype>double</subtype></BarLine>
</voice><voice>
<Dynamic><eid>B_b</eid><subtype>sfz</subtype><voiceAssignment>currentVoiceOnly</voiceAssignment></Dynamic>
<Chord><durationType>whole</durationType><Note><pitch>55</pitch><tpc>15</tpc></Note></Chord>
</voice></Measure>
<Measure><endRepeat>2</endRepeat><voice>
<Dynamic><eid>B_c</eid><subtype>other</subtype><text>quasi niente</text></Dynamic>
<Chord><eid>B_d</eid><durationType>whole</durationType><duration>1/2</duration><Note><pitch>64</pitch><tpc>18</tpc></Note>
<TremoloTwoChord><eid>B_e</eid><subtype>c16</subtype></TremoloTwoChord></Chord>
<Chord><eid>B_f</eid><durationType>whole</durationType><duration>1/2</duration><Note><eid>B_k</eid><pitch>67</pitch><tpc>15</tpc></Note></Chord>
<BarLine><subtype>end-repeat</subtype></BarLine>
</voice></Measure>
<Measure><voice><Chord><durationType>whole</durationType><Note><eid>B_l</eid><pitch>67</pitch><tpc>15</tpc>
<Glissando><eid>B_m</eid><startElement>B_k</startElement><endElement>B_l</endElement><track>0</track><diagonal>1</diagonal></Glissando>
</Note></Chord></voice></Measure></Staff>
<Staff id="2"><Measure><voice><StaffText><eid>B_h</eid><text>bass only</text></StaffText>
<Rest><durationType>measure</durationType><duration>1/1</duration></Rest></voice></Measure>
<Measure><voice><Rest><durationType>measure</durationType><duration>1/1</duration></Rest></voice></Measure>
<Measure><voice><Rest><durationType>measure</durationType><duration>1/1</duration></Rest></voice></Measure></Staff>
</Score></museScore>)";

// The numeric pageWidth style a native MSCX export of the master or an excerpt writes, or nullopt when it omits it.
std::optional<double> exported_page_width(HostApi &api, const Json &doc, std::optional<Json> excerpt = {}) {
    std::string xml;
    const auto exported = api.with_document(doc.get<std::string>(), {}, [&](engrave::Document &d) -> music::api::Result<void> {
        engrave::ExportOptions options{.format = engrave::Format::MSCX};
        if (excerpt) {
            options.score.excerpt = engrave::element_id_from_string(excerpt->get<std::string>());
            require(options.score.excerpt.has_value(), "excerpt is not an EID");
        }
        auto bytes = d.export_score(options);
        if (!bytes) return std::unexpected(music::api::api_error(bytes.error()));
        xml.assign(reinterpret_cast<const char *>(bytes->data()), bytes->size());
        return {};
    });
    require(bool(exported), exported ? "" : exported.error().message);
    constexpr std::string_view open = "<pageWidth>";
    const auto start = xml.find(open);
    if (start == std::string::npos) return std::nullopt;
    return std::stod(xml.substr(start + open.size(), xml.find('<', start + open.size()) - start - open.size()));
}

// A default score cut down to its first `bars` bars by an ordinal remove_measures.
Json short_score(HostApi &api, int bars) {
    const auto created = ok(api, "score_create", music::make_object<Json>());
    const auto doc = created.at("document");
    const auto total = created.at("outline").at("measure_count").as<int>();
    require(total > bars, "template is shorter than the requested score");
    ok(api, "score_edit", edit_ops(doc, {Json{{"op", "remove_measures"}, {"first", bars + 1}, {"last", total}}}));
    require(ok(api, "score_outline", {{"document", doc}}).at("outline").at("measures").size() == static_cast<std::size_t>(bars),
            "ordinal remove_measures left the wrong bar count");
    return doc;
}
} // namespace

TEST(HostApi, RuntimeOperationExamplesDecodeAndAddAnArticulation) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto unknown = api.runtime_info({{"scope", "type"}, {"type", "AbsentType"}});
    ASSERT_EQ(unknown.size(), 2u) << music::dump_json(unknown);
    EXPECT_EQ(unknown.at("ok"), false);
    EXPECT_EQ(unknown.at("error").at("code"), "NotFound");

    const auto doc = ok(api, "score_create", music::make_object<Json>()).at("document").get<std::string>();
    music::api::ValueCatalogue catalogue;
    catalogue.properties = {{"visible", engrave::ValueKind::BOOL}, {"symbol", engrave::ValueKind::ENUM}};
    catalogue.styles = {{"pageWidth", engrave::ValueKind::REAL}};
    const auto ops = ok(api, "runtime_info", {{"scope", "ops"}}).at("ops").get_array();
    ASSERT_EQ(ops.size(), music::api::operation_names.size());
    for (const auto name : music::api::operation_names) {
        const auto same_op = [&](const Json &entry) { return entry.at("op") == name; };
        EXPECT_EQ(std::ranges::count_if(ops, same_op), 1) << name;
        const auto entry = std::ranges::find_if(ops, same_op);
        ASSERT_NE(entry, ops.end()) << name;
        const auto decoded = music::api::decode_batch(music::json_array<Json>({entry->at("example")}), catalogue);
        ASSERT_TRUE(decoded) << name << ": " << decoded.error().message;
        EXPECT_EQ(music::api::operation_names.at(decoded->ops.at(0).index()), name);
    }
    const auto selected = ok(api, "runtime_info", {{"scope", "ops"}, {"op", "add"}}).at("ops");
    ASSERT_EQ(selected.size(), 1u);
    const auto example = selected[0].at("example");
    ASSERT_EQ(example.at("anchor"), (Json{{"measure", 1}}));
    const auto entered = ok(api, "score_edit",
                            {{"document", doc},
                             {"detail", true},
                             {"ops", music::json_array<Json>({Json{{"op", "enter_notes"},
                                                                   {"at", Json{{"measure", 1}}},
                                                                   {"duration", "1/4"},
                                                                   {"pitches", music::json_array<Json>({Json{{"pitch", 60}}})}}})}});
    const auto added = ok(api, "score_edit", {{"document", doc}, {"ops", music::json_array<Json>({example})}});
    EXPECT_EQ(added.at("applied"), 1);
    const auto marks = ok(api, "score_query",
                          {{"document", doc}, {"mode", "elements"}, {"query", {{"types", music::json_array<Json>({"Articulation"})}}}});
    ASSERT_EQ(marks.at("elements").size(), 1u);
    EXPECT_EQ(marks.at("elements")[0].at("parent"), entered.at("ops")[0].at("created")[0]);
}

TEST(HostApi, InstrumentFilterAndDrumMetadata) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto lookup = [&](std::string_view query) {
        const auto result = api.runtime_info({{"scope", "instruments"}, {"query", query}});
        require(result.at("ok") == true, music::dump_json(result));
        return result.at("instruments");
    };
    const auto bass_drums = lookup("bAsS-dRuM");
    EXPECT_EQ(bass_drums, lookup("bass-drum"));
    EXPECT_LT(bass_drums.size(), lookup("").size());
    EXPECT_TRUE(lookup("no-such-instrument").empty());
    const auto &instruments = bass_drums.get_array();
    const auto bass = std::ranges::find_if(instruments, [](const Json &i) { return i.at("id") == "bass-drum"; });
    ASSERT_NE(bass, instruments.end());
    EXPECT_EQ(bass->at("uses_drumset"), true);
    EXPECT_EQ(bass->at("transpose"), (Json{{"diatonic", 0}, {"chromatic", 0}}));
    const auto drums = bass->at("drums");
    ASSERT_EQ(drums.size(), 1u);
    EXPECT_EQ(drums[0], (Json{{"key", 36}, {"name", "Concert Bass Drum"}, {"line", 0}, {"voice", 1}}));
    error(api, "runtime_info", {{"query", "bass-drum"}}, "InvalidArgument");
    error(api, "runtime_info", {{"scope", "templates"}, {"query", "bass-drum"}}, "InvalidArgument");
}

TEST(HostApi, ScoreEditRollsBackMidBatchByDefaultAndWhenExplicit) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto created = ok(api, "score_create", music::make_object<Json>());
    const auto doc = created.at("document");
    const auto revision = created.at("revision");
    const Json range{{"document", doc}, {"count", 1}, {"include_ids", true}};
    const auto before = api.read_music(range);
    ASSERT_EQ(before.at("ok"), true) << music::dump_json(before);
    const auto enter = [](std::string_view offset, int pitch) {
        return Json{{"op", "enter_notes"},
                    {"at", Json{{"measure", 1}, {"offset", offset}}},
                    {"duration", "1/4"},
                    {"pitches", music::json_array<Json>({Json{{"pitch", pitch}}})}};
    };
    Json batch{{"document", doc}, {"ops", music::json_array<Json>({enter("0", 60), enter("1/8", 62)})}};
    for (bool explicit_atomic : {false, true}) {
        if (explicit_atomic) batch["atomic"] = true;
        const auto failed = error(api, "score_edit", batch, "InvalidArgument").at("error");
        EXPECT_EQ(failed.at("op_index"), 1);
        EXPECT_EQ(failed.at("applied"), 0);
        EXPECT_EQ(failed.at("revision"), revision);
        EXPECT_EQ(api.read_music(range), before);
    }
}

TEST(HostApi, PresetCancellationStopsSnapshotCaptureAndLeavesTheScoreEditable) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto created = ok(api, "score_create", music::make_object<Json>());
    const auto doc = created.at("document");
    const auto revision = created.at("revision");
    const Json range{{"document", doc}, {"count", 1}};
    const auto before = api.read_music(range);
    ASSERT_EQ(before.at("ok"), true) << music::dump_json(before);

    std::atomic<bool> cancelled{true};
    const auto expect_cancelled = [&](const Json &out, std::string_view message) {
        ASSERT_EQ(out.at("ok"), false) << music::dump_json(out);
        const auto &e = out.at("error");
        EXPECT_EQ(e.at("code"), "Cancelled") << music::dump_json(out);
        EXPECT_TRUE(e.at("message").get<std::string>().contains(message)) << music::dump_json(out);
        EXPECT_FALSE(e.contains("poisoned")) << music::dump_json(out);
        EXPECT_EQ(api.read_music(range), before);
    };
    auto batch = edit_ops(doc, {enter_at(pos(1), 60)});
    expect_cancelled(api.edit(batch, &cancelled), "cancelled");
    batch["atomic"] = false;
    expect_cancelled(api.edit(batch, &cancelled), "batch cancelled before edit");
    const Json phrase{{"document", doc}, {"lanes", music::json_array<Json>({Json{{"measure", 1}, {"content", "Eb4"}}})}};
    expect_cancelled(api.write_phrase(phrase, &cancelled), "cancelled");
    const Json saved{{"document", doc}, {"name", "draft"}};
    expect_cancelled(api.checkpoint(saved, &cancelled), "snapshot capture cancelled");
    error(api, "restore_checkpoint", saved, "NotFound");

    cancelled = false;
    batch["atomic"] = true;
    const auto edited = api.edit(batch, &cancelled);
    ASSERT_EQ(edited.at("ok"), true) << music::dump_json(edited);
    EXPECT_NE(edited.at("revision"), revision);
    const auto checkpointed = api.checkpoint(saved, &cancelled);
    ASSERT_EQ(checkpointed.at("ok"), true) << music::dump_json(checkpointed);
    const auto written = api.write_phrase(phrase, &cancelled);
    ASSERT_EQ(written.at("ok"), true) << music::dump_json(written);
    EXPECT_EQ(chord_pitch(api.read_music(range).at("measures")[0], 1, 1, "0"), 63);
    ok(api, "restore_checkpoint", saved);
    EXPECT_EQ(chord_pitch(api.read_music(range).at("measures")[0], 1, 1, "0"), 60);
}

TEST(HostApi, OnlyUncompressedScoreFilesMayExceedTheByteInputLimit) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto limits = ok(api, "runtime_info", music::make_object<Json>()).at("limits");
    EXPECT_EQ(limits.at("input_bytes").get<std::size_t>(), 8u * 1024 * 1024);
    EXPECT_EQ(limits.at("uncompressed_file_bytes").get<std::size_t>(), 64u * 1024 * 1024);

    const auto created = ok(api, "score_create", {{"title", "Large native score"}});
    const auto doc = created.at("document").get<std::string>();
    const auto exported = [&](engrave::Format format) {
        std::string bytes;
        const auto done = api.with_document(doc, {}, [&](engrave::Document &d) -> music::api::Result<void> {
            auto out = d.export_score({.format = format});
            if (!out) return std::unexpected(music::api::api_error(out.error()));
            bytes.assign(reinterpret_cast<const char *>(out->data()), out->size());
            return {};
        });
        require(bool(done), done ? "" : done.error().message);
        return bytes;
    };
    auto xml = exported(engrave::Format::MSCX);
    const auto archive = exported(engrave::Format::MSCZ);

    struct Scratch {
        fs::path root;

        ~Scratch() {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    } scratch{fs::temp_directory_path() /
              ("host-api-input-limit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};

    fs::create_directories(scratch.root);
    const auto write = [](const fs::path &p, const std::string &bytes) {
        std::ofstream f(p, std::ios::binary);
        require(bool(f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))), "cannot write " + p.string());
    };

    // Trailing whitespace after the root element keeps the score readable while passing the byte limit.
    xml.resize((8u * 1024 * 1024) + 1, ' ');
    const auto large = scratch.root / "large.MSCX";
    write(large, xml);
    const auto opened = ok(api, "score_open", {{"path", path_text(large)}});
    EXPECT_EQ(opened.at("outline").at("measure_count"), created.at("outline").at("measure_count"));
    error(api, "score_open", {{"path", path_text(large)}, {"format", "mscz"}}, "ResourceLimit");
    // Called directly: the shared helper would log the 11 MiB base64 argument.
    const auto inline_data = api.open({{"data", b64(xml)}, {"format", "mscx"}});
    ASSERT_EQ(inline_data.at("ok"), false);
    EXPECT_EQ(inline_data.at("error").at("code"), "ResourceLimit");

    const auto compressed = scratch.root / "large.mscz";
    write(compressed, archive);
    fs::resize_file(compressed, (8u * 1024 * 1024) + 1);
    error(api, "score_open", {{"path", path_text(compressed)}}, "ResourceLimit");

    fs::resize_file(large, (64u * 1024 * 1024) + 1);
    error(api, "score_open", {{"path", path_text(large)}}, "ResourceLimit");
}

TEST(HostApi, ScoreEditResolvesOneBasedPositions) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto created = ok(api, "score_create", {{"template_id", "Piano"}});
    const auto doc = created.at("document");
    const auto &bars = created.at("outline").at("measures");
    ASSERT_GE(bars.size(), 4u);
    const auto bar_two = bars[1].at("id");
    const auto bar_three = bars[2].at("id");
    const auto enter = Json{{"op", "enter_notes"},
                            {"at", Json{{"measure", 2}, {"offset", "1/4"}, {"staff", 2}, {"voice", 2}}},
                            {"duration", "1/4"},
                            {"pitches", music::json_array<Json>({Json{{"pitch", 60}}})}};
    ok(api, "score_edit",
       {{"document", doc},
        {"ops", music::json_array<Json>(
                    {enter, Json{{"op", "set_key_sig"}, {"measure", 2}, {"staff", 2}, {"key", 2}},
                     Json{{"op", "add_spanner_to_bar_end"}, {"type", "HairPin"}, {"start", opref(0, 0)}, {"end", 3}},
                     Json{{"op", "add_volta"}, {"first_measure", 2}, {"last_measure", 3}, {"endings", music::json_array<Json>({1})}}})}});
    const auto window = api.read_music({{"document", doc}, {"count", 4}, {"include_ids", true}});
    ASSERT_EQ(window.at("ok"), true) << music::dump_json(window);
    const auto &bar = window.at("measures")[1];
    ASSERT_EQ(bar.at("id"), bar_two);
    const auto &voices = bar.at("voices").get_array();
    const auto voice = std::ranges::find_if(voices, [](const Json &v) { return v.at("staff") == 2 && v.at("voice") == 2; });
    ASSERT_NE(voice, voices.end());
    const auto &events = voice->at("events").get_array();
    const auto chord = std::ranges::find_if(events, [](const Json &event) { return event.contains("notes"); });
    ASSERT_NE(chord, events.end());
    EXPECT_EQ(chord->at("offset"), "1/4");
    const auto &changes = bar.at("signature_changes").get_array();
    EXPECT_EQ(
        std::ranges::count_if(changes, [](const Json &s) { return s.at("type") == "KeySig" && s.at("staff") == 2 && s.at("key") == 2; }),
        1);
    ASSERT_EQ(window.at("spanners").size(), 1u);
    const auto &hairpin = window.at("spanners")[0];
    EXPECT_EQ(hairpin.at("start").at("measure_id"), bar_two);
    EXPECT_EQ(hairpin.at("end").at("measure_id"), bars[3].at("id"));
    EXPECT_EQ(hairpin.at("end").at("offset"), "0");
    ASSERT_EQ(window.at("voltas").size(), 1u);
    const auto &volta = window.at("voltas")[0];
    EXPECT_EQ(volta.at("start").at("measure_id"), bar_two);
    EXPECT_EQ(volta.at("end").at("measure_id"), bar_three);
    EXPECT_EQ(volta.at("end").at("offset"), bars[2].at("duration"));

    ok(api, "score_edit",
       {{"document", doc},
        {"ops", music::json_array<Json>({Json{{"op", "insert_measures"}, {"before", bar_two}, {"count", 1}},
                                         Json{{"op", "set_repeat_barlines"}, {"measure", 2}, {"start", true}}})}});
    // Bar 2 is resolved after the insertion, so it names the inserted bar rather than the old bar two.
    const auto shifted = api.read_music({{"document", doc}, {"count", 4}, {"include_ids", true}});
    ASSERT_EQ(shifted.at("ok"), true) << music::dump_json(shifted);
    EXPECT_EQ(shifted.at("measures")[1].at("repeat_start"), true);
    EXPECT_EQ(shifted.at("measures")[2].at("id"), bar_two);
    EXPECT_EQ(shifted.at("measures")[2].at("repeat_start"), false);
}

TEST(EditCodec, OneBasedOrdinalsDecodeToZeroBasedSelectors) {
    music::api::ValueCatalogue catalogue;
    catalogue.styles["pageWidth"] = engrave::ValueKind::REAL;
    const auto decode = [&](const Json &op) {
        auto batch = music::api::decode_batch(music::json_array<Json>({op}), catalogue);
        require(bool(batch), music::dump_json(op) + ": " + (batch ? "" : batch.error().message));
        require(!batch->scope, "decode_batch set a batch scope");
        return std::move(batch->ops.at(0));
    };
    const auto entered =
        std::get<engrave::EnterNotes>(decode(enter_at(Json{{"measure", 3}, {"staff", 2}, {"voice", 4}, {"offset", "3/8"}}, 60)));
    const auto &relative = std::get<engrave::RelativePosition>(entered.at);
    EXPECT_EQ(relative.measure.index, 2);
    EXPECT_EQ(relative.staff, 1);
    EXPECT_EQ(relative.voice, 3);
    EXPECT_EQ(relative.offset.num, 3);
    EXPECT_EQ(relative.offset.den, 8);
    const auto defaults = std::get<engrave::RelativePosition>(std::get<engrave::EnterNotes>(decode(enter_at(Json{{"measure", 1}}, 60))).at);
    EXPECT_EQ(defaults.measure.index, 0);
    EXPECT_EQ(defaults.staff, 0);
    EXPECT_EQ(defaults.voice, 0);
    EXPECT_EQ(defaults.offset.num, 0);

    const auto segment =
        Json{{"kind", "segment"}, {"measure", "A_A"}, {"staff", 2}, {"voice", 3}, {"tick", "1/2"}, {"segment_kind", "ChordRest"}};
    const auto explicit_at = std::get<engrave::EnterNotes>(decode(enter_at(segment, 60))).at;
    const auto &address = std::get<engrave::SegmentAddress>(std::get<engrave::Address>(explicit_at));
    EXPECT_EQ(engrave::to_string(address.measure), "A_A");
    EXPECT_EQ(address.staff, 1);
    EXPECT_EQ(address.voice, 2);
    EXPECT_EQ(address.tick.num, 1);
    EXPECT_EQ(address.tick.den, 2);
    EXPECT_EQ(address.kind, "ChordRest");

    const auto key = std::get<engrave::SetKeySig>(decode({{"op", "set_key_sig"}, {"measure", 5}, {"staff", 2}, {"key", 1}}));
    EXPECT_EQ(std::get<engrave::MeasureIndex>(key.measure).index, 4);
    EXPECT_EQ(std::get<engrave::StaffIndex>(*key.staff).index, 1);
    EXPECT_EQ(std::get<engrave::StaffIndex>(std::get<engrave::RemoveStaff>(decode({{"op", "remove_staff"}, {"staff", 1}})).staff).index, 0);
    const auto inserted = std::get<engrave::InsertMeasures>(decode({{"op", "insert_measures"}, {"before", 2}, {"count", 1}}));
    EXPECT_EQ(std::get<engrave::MeasureIndex>(*inserted.before).index, 1);
    EXPECT_FALSE(std::get<engrave::InsertMeasures>(decode({{"op", "insert_measures"}, {"count", 1}})).before);
    const auto removed = std::get<engrave::RemoveMeasures>(decode({{"op", "remove_measures"}, {"first", 2}, {"last", "A_A"}}));
    EXPECT_EQ(std::get<engrave::MeasureIndex>(removed.first).index, 1);
    EXPECT_TRUE(std::holds_alternative<engrave::ElementId>(removed.last));
    const auto volta = std::get<engrave::AddVolta>(
        decode({{"op", "add_volta"}, {"first_measure", 1}, {"last_measure", 2}, {"endings", music::json_array<Json>({1})}}));
    EXPECT_EQ(std::get<engrave::MeasureIndex>(volta.first_measure).index, 0);
    EXPECT_EQ(std::get<engrave::MeasureIndex>(volta.last_measure).index, 1);
    const auto meter = std::get<engrave::SetTimeSig>(decode({{"op", "set_time_sig"}, {"measure", 7}, {"sig", "3/4"}}));
    EXPECT_EQ(std::get<engrave::MeasureIndex>(meter.measure).index, 6);
    const auto line_break =
        std::get<engrave::SetLayoutBreak>(decode({{"op", "set_layout_break"}, {"measure", 7}, {"kind", "line"}, {"on", true}}));
    EXPECT_EQ(std::get<engrave::MeasureIndex>(line_break.measure).index, 6);
    const auto repeat = std::get<engrave::SetRepeatBarlines>(decode({{"op", "set_repeat_barlines"}, {"measure", 7}}));
    EXPECT_EQ(std::get<engrave::MeasureIndex>(repeat.measure).index, 6);

    const auto bar_end = std::get<engrave::AddSpannerToBarEnd>(
        decode({{"op", "add_spanner_to_bar_end"}, {"type", "Pedal"}, {"start", pos(2, "1/4", 1, 2)}, {"end", 3}}));
    const auto &start = std::get<engrave::RelativePosition>(bar_end.start);
    EXPECT_EQ(start.measure.index, 1);
    EXPECT_EQ(start.voice, 1);
    EXPECT_EQ(start.offset.den, 4);
    EXPECT_EQ(std::get<engrave::MeasureIndex>(bar_end.end.measure).index, 2);
    const auto appended = music::api::decode_batch(
        music::json_array<Json>({Json{{"op", "insert_measures"}, {"count", 1}},
                                 Json{{"op", "add_spanner_to_bar_end"}, {"type", "Pedal"}, {"start", "A_A"}, {"end", opref(0, 0)}}}),
        catalogue);
    ASSERT_TRUE(appended) << appended.error().message;
    EXPECT_TRUE(std::holds_alternative<engrave::OpRef>(std::get<engrave::AddSpannerToBarEnd>(appended->ops.at(1)).end.measure));
    const auto spanner =
        std::get<engrave::AddSpanner>(decode({{"op", "add_spanner"}, {"type", "Slur"}, {"start", pos(1)}, {"end", pos(2)}}));
    EXPECT_EQ(std::get<engrave::RelativePosition>(spanner.end).measure.index, 1);
    const auto added = std::get<engrave::Add>(decode({{"op", "add"}, {"anchor", Json{{"measure", 4}}}, {"type", "Fermata"}}));
    EXPECT_EQ(std::get<engrave::RelativePosition>(added.anchor).measure.index, 3);

    const auto inherited = std::get<engrave::SetStyle>(decode({{"op", "set_style"}, {"style", "pageWidth"}, {"value", 9.0}}));
    EXPECT_FALSE(inherited.score);
    const auto selected =
        std::get<engrave::SetStyle>(decode({{"op", "set_style"}, {"excerpt", "A_B"}, {"style", "pageWidth"}, {"value", 9.0}}));
    ASSERT_TRUE(selected.score && selected.score->excerpt);
    EXPECT_EQ(engrave::to_string(*selected.score->excerpt), "A_B");

    for (const auto &invalid : std::vector<Json>{
             {{"op", "set_time_sig"}, {"measure", 0}, {"sig", "3/4"}},
             {{"op", "remove_staff"}, {"staff", -1}},
             {{"op", "insert_measures"}, {"before", segment}, {"count", 1}},
             {{"op", "set_key_sig"}, {"measure", 1}, {"staff", segment}, {"key", 0}},
             enter_at(Json{{"measure", 0}}, 60),
             enter_at(Json{{"measure", 1}, {"staff", 0}}, 60),
             enter_at(Json{{"measure", 1}, {"voice", 5}}, 60),
             {{"op", "add"}, {"anchor", Json{{"measure", 0}}}, {"type", "Fermata"}},
             {{"op", "add"}, {"anchor", 1}, {"type", "Fermata"}},
             {{"op", "add_part"}, {"instrument_id", "violin"}, {"before", 1}},
             {{"op", "set_pitch"}, {"note", Json{{"measure", 1}}}, {"pitch", 60}},
             {{"op", "add_tie"}, {"note", 1}},
             {{"op", "add_tuplet"}, {"first", Json{{"measure", 1}}}, {"last", "A_A"}},
             {{"op", "remove"}, {"target", Json{{"measure", 1}}}},
         }) {
        EXPECT_FALSE(music::api::decode_batch(music::json_array<Json>({invalid}), catalogue)) << music::dump_json(invalid);
    }
}

TEST(HostApi, ScoreEditOrdinalsFollowStructuralStepsInTheBatch) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto doc = short_score(api, 3);
    const auto original = ok(api, "score_outline", {{"document", doc}}).at("outline").at("measures");
    ok(api, "score_edit",
       edit_ops(doc, {Json{{"op", "insert_measures"}, {"before", 2}, {"count", 1}}, enter_at(pos(2), 62), enter_at(pos(3), 64),
                      Json{{"op", "add_spanner"}, {"type", "HairPin"}, {"start", pos(2)}, {"end", pos(3)}}}));
    const auto window = api.read_music({{"document", doc}, {"count", 4}, {"include_ids", true}});
    ASSERT_EQ(window.at("ok"), true) << music::dump_json(window);
    const auto &bars = window.at("measures");
    EXPECT_EQ(bars[0].at("id"), original[0].at("id"));
    for (const auto &old : original.get_array()) EXPECT_NE(bars[1].at("id"), old.at("id"));
    EXPECT_EQ(bars[2].at("id"), original[1].at("id"));
    EXPECT_EQ(bars[3].at("id"), original[2].at("id"));
    EXPECT_EQ(chord_pitch(bars[1], 1, 1, "0"), 62);
    EXPECT_EQ(chord_pitch(bars[2], 1, 1, "0"), 64);
    ASSERT_EQ(window.at("spanners").size(), 1u);
    const auto &hairpin = window.at("spanners")[0];
    EXPECT_EQ(hairpin.at("type"), "HairPin");
    EXPECT_EQ(hairpin.at("start").at("measure_id"), bars[1].at("id"));
    EXPECT_EQ(hairpin.at("end").at("measure_id"), original[1].at("id"));
    EXPECT_EQ(hairpin.at("end").at("offset"), "0");

    // Bars 2 and 3 are now the inserted bar and original bar 2, so bar 2 afterwards is original bar 3.
    ok(api, "score_edit", edit_ops(doc, {Json{{"op", "remove_measures"}, {"first", 2}, {"last", 3}}, enter_at(pos(2, "1/4"), 67)}));
    const auto shortened = api.read_music({{"document", doc}, {"include_ids", true}});
    ASSERT_EQ(shortened.at("ok"), true) << music::dump_json(shortened);
    ASSERT_EQ(shortened.at("measures").size(), 2u);
    EXPECT_EQ(shortened.at("measures")[1].at("id"), original[2].at("id"));
    EXPECT_EQ(chord_pitch(shortened.at("measures")[1], 1, 1, "1/4"), 67);
    EXPECT_TRUE(shortened.at("spanners").empty());
}

TEST(HostApi, ScoreEditOffsetsAndOrdinalsResolveAgainstTheLiveScore) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto first_bar = [&](const Json &doc) { return ok(api, "score_outline", {{"document", doc}}).at("outline").at("measures"); };

    const auto widened = short_score(api, 1);
    ok(api, "score_edit", edit_ops(widened, {Json{{"op", "set_time_sig"}, {"measure", 1}, {"sig", "5/4"}}, enter_at(pos(1, "1/1"), 70)}));
    EXPECT_EQ(first_bar(widened)[0].at("duration"), "5/4");
    const auto wide = api.read_music({{"document", widened}});
    ASSERT_EQ(wide.at("ok"), true) << music::dump_json(wide);
    EXPECT_EQ(chord_pitch(wide.at("measures")[0], 1, 1, "1/1"), 70);

    // Each second step names an onset or bar that exists only before the first step runs.
    const auto shrink = Json{{"op", "set_time_sig"}, {"measure", 1}, {"sig", "3/4"}};
    const auto drop = Json{{"op", "remove_measures"}, {"first", 2}, {"last", 2}};
    for (const bool atomic : {true, false}) {
        for (const auto &[first, second, field, bars, before, after] :
             {std::tuple{shrink, enter_at(pos(1, "3/4"), 62), "at:", 1, "1/1", "3/4"},
              std::tuple{drop, enter_at(pos(2), 62), "at:", 2, "2", "1"}}) {
            const auto doc = short_score(api, bars);
            const auto revision = std::stoull(ok(api, "score_outline", {{"document", doc}}).at("revision").get<std::string>());
            auto request = edit_ops(doc, {first, second});
            request["atomic"] = atomic;
            const auto rejected = error(api, "score_edit", request, "EditRejected").at("error");
            EXPECT_EQ(rejected.at("op_index"), 1);
            EXPECT_TRUE(rejected.at("message").get<std::string>().starts_with(field)) << rejected.at("message").get<std::string>();
            EXPECT_FALSE(rejected.contains("poisoned"));
            EXPECT_EQ(rejected.at("applied"), atomic ? 0 : 1);
            EXPECT_EQ(rejected.at("revision"), std::to_string(revision + (atomic ? 0 : 1)));
            const auto measures = first_bar(doc);
            const auto shape = bars == 1 ? measures[0].at("duration").get<std::string>() : std::to_string(measures.size());
            EXPECT_EQ(shape, atomic ? before : after);
            ok(api, "score_edit", edit_ops(doc, {enter_at(pos(1, "1/4"), 65)}));
            const auto window = api.read_music({{"document", doc}, {"count", 1}});
            EXPECT_EQ(chord_pitch(window.at("measures")[0], 1, 1, "1/4"), 65);
        }
    }
}

TEST(HostApi, ScoreEditStaffOrdinalsFollowAddedPartsAndStaves) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto created = ok(api, "score_create", music::make_object<Json>());
    const auto doc = created.at("document");
    const auto part = created.at("outline").at("parts")[0].at("id");
    const auto staff = created.at("outline").at("parts")[0].at("staves")[0].at("id");
    ok(api, "score_edit",
       edit_ops(doc,
                {Json{{"op", "add_part"}, {"instrument_id", "violin"}, {"before", part}}, Json{{"op", "add_staff"}, {"part", opref(0, 0)}},
                 Json{{"op", "set_key_sig"}, {"measure", 1}, {"staff", 3}, {"key", 2}}, enter_at(pos(1, "0", 3), 68),
                 enter_at(pos(1, "0", 2), 72)}));
    const auto staff_number = [&](const Json &outline, const Json &id) {
        for (const auto &p : outline.at("parts").get_array()) {
            for (const auto &s : p.at("staves").get_array()) {
                if (s.at("id") == id) return s.at("staff").as<int>();
            }
        }
        return 0;
    };
    const auto outline = ok(api, "score_outline", {{"document", doc}}).at("outline");
    EXPECT_EQ(staff_number(outline, staff), 3);
    const auto &signatures = outline.at("signatures").get_array();
    EXPECT_EQ(std::ranges::count_if(signatures, [](const Json &s) { return s.at("type") == "KeySig" && s.at("key") == 2; }), 1);
    EXPECT_EQ(
        std::ranges::count_if(signatures, [](const Json &s) { return s.at("type") == "KeySig" && s.at("key") == 2 && s.at("staff") == 3; }),
        1);
    const auto window = api.read_music({{"document", doc}, {"count", 1}});
    ASSERT_EQ(window.at("ok"), true) << music::dump_json(window);
    EXPECT_EQ(chord_pitch(window.at("measures")[0], 3, 1, "0"), 68);
    EXPECT_EQ(chord_pitch(window.at("measures")[0], 2, 1, "0"), 72);
    EXPECT_EQ(chord_pitch(window.at("measures")[0], 1, 1, "0"), -1);

    ok(api, "score_edit", edit_ops(doc, {Json{{"op", "remove_staff"}, {"staff", 2}}}));
    EXPECT_EQ(staff_number(ok(api, "score_outline", {{"document", doc}}).at("outline"), staff), 2);
}

TEST(HostApi, ScoreEditExcerptScopesOrdinalsStylesAndOperands) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto created = ok(api, "score_create", music::make_object<Json>());
    const auto doc = created.at("document");
    const auto part = created.at("outline").at("parts")[0].at("id");
    const auto master_bar = created.at("outline").at("measures")[0].at("id");
    auto setup = edit_ops(doc, {Json{{"op", "add_part"}, {"instrument_id", "violin"}},
                                Json{{"op", "generate_excerpts"}, {"parts", music::json_array<Json>({opref(0, 0)})}},
                                Json{{"op", "generate_excerpts"}, {"parts", music::json_array<Json>({part})}}});
    setup["detail"] = true;
    const auto generated = ok(api, "score_edit", setup);
    const auto violin = generated.at("ops")[1].at("created")[0];
    const auto treble = generated.at("ops")[2].at("created")[0];
    const auto outline = ok(api, "score_outline", {{"document", doc}}).at("outline");
    const auto &excerpts = outline.at("excerpts").get_array();
    for (const auto &id : {violin, treble}) {
        EXPECT_EQ(std::ranges::count_if(excerpts, [&](const Json &e) { return e.at("id") == id; }), 1) << music::dump_json(id);
    }

    // Excerpt staff 1 is the violin, which is master staff 2.
    const auto scoped = [&](std::initializer_list<Json> ops, std::optional<bool> atomic = {}) {
        auto request = edit_ops(doc, ops);
        request["excerpt"] = violin;
        if (atomic) request["atomic"] = *atomic;
        return request;
    };
    const auto style = [](std::optional<Json> excerpt, double width) {
        Json op{{"op", "set_style"}, {"style", "pageWidth"}, {"value", width}};
        if (excerpt) op["excerpt"] = *excerpt;
        return op;
    };
    ok(api, "score_edit", scoped({enter_at(pos(1), 69), style({}, 9.0), style(violin, 9.5)}));
    const auto part_window = api.read_music({{"document", doc}, {"excerpt", violin}, {"count", 1}});
    ASSERT_EQ(part_window.at("ok"), true) << music::dump_json(part_window);
    EXPECT_EQ(chord_pitch(part_window.at("measures")[0], 1, 1, "0"), 69);
    const auto master_window = api.read_music({{"document", doc}, {"count", 1}});
    EXPECT_EQ(chord_pitch(master_window.at("measures")[0], 1, 1, "0"), -1);
    EXPECT_EQ(chord_pitch(master_window.at("measures")[0], 2, 1, "0"), 69);

    const auto revision = ok(api, "score_outline", {{"document", doc}}).at("revision");
    for (const auto &refused : {
             style(treble, 10.0),
             Json{{"op", "set_layout_break"}, {"measure", master_bar}, {"kind", "line"}, {"on", true}},
             Json{{"op", "insert_measures"}, {"count", 1}},
             Json{{"op", "remove_measures"}, {"first", 1}, {"last", 1}},
             Json{{"op", "add_part"}, {"instrument_id", "violin"}},
             Json{{"op", "remove_part"}, {"part", part}},
             Json{{"op", "generate_excerpts"}, {"parts", music::json_array<Json>({part})}},
             Json{{"op", "set_time_sig"}, {"measure", 1}, {"sig", "3/4"}},
             Json{{"op", "set_repeat_barlines"}, {"measure", 1}, {"end", true}},
         }) {
        // Preflight refuses the batch before the valid first operation runs, in either mode.
        for (const bool atomic : {true, false}) {
            const auto failed = error(api, "score_edit", scoped({style({}, 11.0), refused}, atomic), "InvalidArgument").at("error");
            EXPECT_EQ(failed.at("op_index"), 1) << music::dump_json(refused);
            EXPECT_EQ(failed.at("applied"), 0) << music::dump_json(refused);
            EXPECT_EQ(ok(api, "score_outline", {{"document", doc}}).at("revision"), revision);
        }
    }
    auto not_an_excerpt = edit_ops(doc, {style({}, 9.0)});
    for (const Json &selector : {master_bar, Json("not-an-eid"), Json(nullptr), Json("")}) {
        not_an_excerpt["excerpt"] = selector;
        error(api, "score_edit", not_an_excerpt, "InvalidArgument");
        error(api, "score_edit", edit_ops(doc, {style(selector, 9.0)}), "InvalidArgument");
    }
    EXPECT_EQ(ok(api, "score_outline", {{"document", doc}}).at("revision"), revision);
    ok(api, "score_edit", edit_ops(doc, {style(treble, 9.0)}));

    // The scoped batch's last set_style wins on the violin part; the treble part
    // got its own width, and the master was never styled.
    EXPECT_EQ(exported_page_width(api, doc, violin), 9.5);
    EXPECT_EQ(exported_page_width(api, doc, treble), 9.0);
    const auto master_width = exported_page_width(api, doc);
    EXPECT_NE(master_width, 9.5);
    EXPECT_NE(master_width, 9.0);
}

TEST(HostApi, ScoreEditBoundaryOperandsAndOneBasedDiagnostics) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto doc = short_score(api, 2);
    const auto revision = ok(api, "score_outline", {{"document", doc}}).at("revision");
    const std::int64_t int32_max = 2147483647;
    const auto layout_break = [](const Json &measure) {
        return Json{{"op", "set_layout_break"}, {"measure", measure}, {"kind", "line"}, {"on", true}};
    };
    const auto key_sig = [](const Json &staff) { return Json{{"op", "set_key_sig"}, {"measure", 1}, {"staff", staff}, {"key", 1}}; };
    const auto bar_end = [](const Json &end) {
        return Json{{"op", "add_spanner_to_bar_end"}, {"type", "HairPin"}, {"start", pos(1)}, {"end", end}};
    };
    const auto at = [](Json position) { return enter_at(position, 60); };

    // The two-bar, one-staff score has neither the named bar nor the named staff.
    for (const auto &[op, message] : std::vector<std::pair<Json, std::string>>{
             {layout_break(3), "measure: the score has no measure 3 (2 available)"},
             {layout_break(int32_max), "measure: the score has no measure 2147483647 (2 available)"},
             {key_sig(2), "staff: the score has no staff 2 (1 available)"},
             {key_sig(int32_max), "staff: the score has no staff 2147483647 (1 available)"},
             {at(Json{{"measure", 3}}), "at: the score has no measure 3 (2 available)"},
             {at(Json{{"measure", 1}, {"staff", 2}}), "at: the score has no staff 2 (1 available)"},
             {bar_end(5), "end: the score has no measure 5 (2 available)"},
         }) {
        const auto failed = error(api, "score_edit", edit_ops(doc, {op}), "EditRejected").at("error");
        EXPECT_EQ(failed.at("message"), message) << music::dump_json(op);
        EXPECT_EQ(failed.at("op_index"), 0) << music::dump_json(op);
        EXPECT_EQ(failed.at("applied"), 0) << music::dump_json(op);
    }

    // Out-of-domain wire values never reach the engine.
    for (const auto &[op, field] : std::vector<std::pair<Json, std::string>>{
             {layout_break(int32_max + 1), "ops[0].measure:"},
             {layout_break(1.5), "ops[0].measure:"},
             {layout_break(0), "ops[0].measure:"},
             {key_sig(int32_max + 1), "ops[0].staff:"},
             {key_sig(1.5), "ops[0].staff:"},
             {bar_end(int32_max + 1), "ops[0].end:"},
             {at(Json{{"measure", int32_max + 1}}), "ops[0].at.measure:"},
             {at(Json{{"measure", 1.5}}), "ops[0].at.measure:"},
             {at(Json{{"measure", 1}, {"offset", "1/0"}}), "ops[0].at.offset:"},
             {at(Json{{"measure", 1}, {"extra", true}}), "ops[0].at.extra:"},
         }) {
        const auto failed = error(api, "score_edit", edit_ops(doc, {op}), "InvalidArgument").at("error");
        EXPECT_TRUE(failed.at("message").get<std::string>().starts_with(field)) << failed.at("message").get<std::string>();
    }
    // A negative offset is well-formed on the wire, so the engine refuses it before the batch runs.
    const auto negative = error(api, "score_edit", edit_ops(doc, {at(Json{{"measure", 1}, {"offset", "-1/4"}})}), "InvalidArgument");
    EXPECT_TRUE(negative.at("error").at("message").get<std::string>().starts_with("at:")) << music::dump_json(negative);
    EXPECT_EQ(ok(api, "score_outline", {{"document", doc}}).at("revision"), revision);
}

TEST(HostApi, ScoreEditPositionalAnchorsReachTheirOnsets) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto doc = short_score(api, 2);
    const auto prop = [](const char *name, const char *value) { return music::json_array<Json>({Json{{"name", name}, {"value", value}}}); };
    auto request =
        edit_ops(doc, {enter_at(pos(1), 60), enter_at(pos(1, "1/4"), 62),
                       Json{{"op", "add"}, {"anchor", pos(1)}, {"type", "Articulation"}, {"props", prop("symbol", "articStaccatoAbove")}},
                       Json{{"op", "add"}, {"anchor", pos(1)}, {"type", "Dynamic"}, {"props", prop("DYNAMIC_TYPE", "ff")}},
                       Json{{"op", "add"}, {"anchor", pos(1, "1/4")}, {"type", "Fermata"}, {"props", prop("symbol", "fermataAbove")}},
                       Json{{"op", "add"}, {"anchor", pos(1)}, {"type", "Harmony"}, {"props", prop("text", "Dm")}},
                       Json{{"op", "add_spanner"}, {"type", "Slur"}, {"start", pos(1)}, {"end", pos(1, "1/4")}},
                       Json{{"op", "add_spanner"}, {"type", "HairPin"}, {"start", pos(1)}, {"end", pos(1, "1/4")}},
                       Json{{"op", "insert_measures"}, {"count", 1}},
                       Json{{"op", "add_spanner_to_bar_end"}, {"type", "TextLine"}, {"start", pos(1)}, {"end", 3}},
                       Json{{"op", "add_spanner_to_bar_end"}, {"type", "Pedal"}, {"start", pos(1, "1/4")}, {"end", opref(8, 0)}}});
    request["detail"] = true;
    const auto edited = ok(api, "score_edit", request);
    const auto first_chord = edited.at("ops")[0].at("created")[0];
    const auto appended = edited.at("ops")[8].at("created")[0];

    const auto marks = ok(api, "score_query",
                          {{"document", doc},
                           {"mode", "elements"},
                           {"query", {{"types", music::json_array<Json>({"Articulation", "Dynamic", "Fermata", "Harmony"})}}}})
                           .at("elements")
                           .get_array();
    ASSERT_EQ(marks.size(), 4u);
    for (const auto &[type, tick] : {std::pair{"Articulation", "0"}, {"Dynamic", "0"}, {"Fermata", "1/4"}, {"Harmony", "0"}}) {
        const auto found = std::ranges::find_if(marks, [&](const Json &e) { return e.at("type") == type; });
        ASSERT_NE(found, marks.end()) << type;
        EXPECT_EQ(found->at("measure_number"), 1) << type;
        EXPECT_EQ(found->at("tick"), tick) << type;
        EXPECT_EQ(found->at("staff"), 1) << type;
        if (std::string_view(type) == "Articulation") EXPECT_EQ(found->at("parent"), first_chord);
    }

    const auto window = api.read_music({{"document", doc}, {"count", 3}, {"include_ids", true}});
    ASSERT_EQ(window.at("ok"), true) << music::dump_json(window);
    EXPECT_EQ(window.at("measures")[2].at("id"), appended);
    const auto &lines = window.at("spanners").get_array();
    ASSERT_EQ(lines.size(), 4u) << music::dump_json(window.at("spanners"));
    for (const auto &[type, start, end_measure, end] :
         {std::tuple{"Slur", "0", 1, "1/4"}, {"HairPin", "0", 1, "1/4"}, {"TextLine", "0", 3, "1/1"}, {"Pedal", "1/4", 3, "1/1"}}) {
        const auto found = std::ranges::find_if(lines, [&](const Json &s) { return s.at("type") == type; });
        ASSERT_NE(found, lines.end()) << type;
        EXPECT_EQ(found->at("start").at("measure"), 1) << type;
        EXPECT_EQ(found->at("start").at("offset"), start) << type;
        EXPECT_EQ(found->at("end").at("measure"), end_measure) << type;
        EXPECT_EQ(found->at("end").at("offset"), end) << type;
    }
}

TEST(HostApi, ScoreEditSegmentAddressesMixWithCompactEndpoints) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto doc = short_score(api, 2);
    const auto bar = ok(api, "score_outline", {{"document", doc}}).at("outline").at("measures")[0].at("id").get<std::string>();
    const auto prop = [](const char *name, const char *value) { return music::json_array<Json>({Json{{"name", name}, {"value", value}}}); };
    // Segment address ticks are score positions; bar 1 starts at 0, so they equal the bar offsets here.
    auto request = edit_ops(
        doc,
        {enter_at(pos(1), 60), enter_at(pos(1, "1/4"), 62), enter_at(pos(1, "1/2"), 64),
         Json{{"op", "add"}, {"anchor", address(bar, "0", "1")}, {"type", "Articulation"}, {"props", prop("symbol", "articAccentAbove")}},
         Json{{"op", "add"}, {"anchor", address(bar, "1", "4")}, {"type", "Dynamic"}, {"props", prop("DYNAMIC_TYPE", "mf")}},
         Json{{"op", "add_spanner"}, {"type", "Slur"}, {"start", address(bar, "0", "1")}, {"end", pos(1, "1/4")}},
         Json{{"op", "add_spanner"}, {"type", "HairPin"}, {"start", pos(1, "1/4")}, {"end", address(bar, "1", "2")}},
         Json{{"op", "add_spanner_to_bar_end"}, {"type", "Pedal"}, {"start", address(bar, "1", "2")}, {"end", 2}}});
    request["detail"] = true;
    const auto edited = ok(api, "score_edit", request);
    const auto first_chord = edited.at("ops")[0].at("created")[0];

    const auto marks =
        ok(api, "score_query",
           {{"document", doc}, {"mode", "elements"}, {"query", {{"types", music::json_array<Json>({"Articulation", "Dynamic"})}}}})
            .at("elements")
            .get_array();
    ASSERT_EQ(marks.size(), 2u);
    for (const auto &[type, tick] : {std::pair{"Articulation", "0"}, {"Dynamic", "1/4"}}) {
        const auto found = std::ranges::find_if(marks, [&](const Json &e) { return e.at("type") == type; });
        ASSERT_NE(found, marks.end()) << type;
        EXPECT_EQ(found->at("measure_number"), 1) << type;
        EXPECT_EQ(found->at("tick"), tick) << type;
        EXPECT_EQ(found->at("staff"), 1) << type;
        if (std::string_view(type) == "Articulation") EXPECT_EQ(found->at("parent"), first_chord);
    }

    const auto window = api.read_music({{"document", doc}, {"count", 2}});
    ASSERT_EQ(window.at("ok"), true) << music::dump_json(window);
    const auto &lines = window.at("spanners").get_array();
    ASSERT_EQ(lines.size(), 3u) << music::dump_json(window.at("spanners"));
    for (const auto &[type, start, end_measure, end] :
         {std::tuple{"Slur", "0", 1, "1/4"}, {"HairPin", "1/4", 1, "1/2"}, {"Pedal", "1/2", 2, "1/1"}}) {
        const auto found = std::ranges::find_if(lines, [&](const Json &s) { return s.at("type") == type; });
        ASSERT_NE(found, lines.end()) << type;
        EXPECT_EQ(found->at("start"), (Json{{"measure", 1}, {"offset", start}, {"staff", 1}, {"voice", 1}})) << type;
        EXPECT_EQ(found->at("end").at("measure"), end_measure) << type;
        EXPECT_EQ(found->at("end").at("offset"), end) << type;
    }
}

TEST(HostApi, ScoreEditRefusesAReversedPositionalSlur) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto reversed = Json{{"op", "add_spanner"}, {"type", "Slur"}, {"start", pos(1, "1/4")}, {"end", pos(1)}};
    for (const bool atomic : {true, false}) {
        SCOPED_TRACE(atomic ? "atomic" : "prefix");
        const auto doc = short_score(api, 2);
        ok(api, "score_edit", edit_ops(doc, {enter_at(pos(1), 60), enter_at(pos(1, "1/4"), 62)}));
        const auto revision = std::stoull(ok(api, "score_outline", {{"document", doc}}).at("revision").get<std::string>());
        auto request = edit_ops(doc, {enter_at(pos(2), 64), reversed});
        request["atomic"] = atomic;
        const auto rejected = error(api, "score_edit", request, "EditRejected").at("error");
        EXPECT_EQ(rejected.at("op_index"), 1);
        EXPECT_TRUE(rejected.at("message").get<std::string>().starts_with("end:")) << rejected.at("message").get<std::string>();
        EXPECT_EQ(rejected.at("applied"), atomic ? 0 : 1);
        EXPECT_EQ(rejected.at("revision"), std::to_string(revision + (atomic ? 0 : 1)));
        const auto window = api.read_music({{"document", doc}, {"count", 2}});
        EXPECT_TRUE(window.at("spanners").empty()) << music::dump_json(window.at("spanners"));
        EXPECT_EQ(chord_pitch(window.at("measures")[1], 1, 1, "0"), atomic ? -1 : 64);
    }
}

TEST(HostApi, EditOperandsRefuseLayoutAddressesThatQueriesAccept) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto doc = short_score(api, 1);
    const auto bar = ok(api, "score_outline", {{"document", doc}}).at("outline").at("measures")[0].at("id");
    const auto revision = ok(api, "score_outline", {{"document", doc}}).at("revision");
    for (const auto &layout : {Json{{"kind", "page"}, {"index", 0}}, Json{{"kind", "system"}, {"page", 0}, {"index", 0}},
                               Json{{"kind", "spanner_segment"}, {"spanner", bar}, {"index", 0}}}) {
        SCOPED_TRACE(music::dump_json(layout));
        for (const auto &[op, field] : std::vector<std::pair<Json, std::string>>{
                 {Json{{"op", "set_property"}, {"target", layout}, {"property", "visible"}, {"value", false}}, "ops[0].target.kind:"},
                 {Json{{"op", "remove"}, {"target", layout}}, "ops[0].target.kind:"},
                 {Json{{"op", "add"}, {"anchor", layout}, {"type", "Fermata"}}, "ops[0].anchor.kind:"},
                 {Json{{"op", "add_spanner"}, {"type", "Slur"}, {"start", layout}, {"end", pos(1)}}, "ops[0].start.kind:"},
                 {Json{{"op", "add_spanner"}, {"type", "Slur"}, {"start", pos(1)}, {"end", layout}}, "ops[0].end.kind:"},
                 {enter_at(layout, 60), "ops[0].at.kind:"},
                 {Json{{"op", "add_tuplet"}, {"first", layout}, {"last", layout}}, "ops[0].first.kind:"},
                 {Json{{"op", "generate_excerpts"}, {"parts", music::json_array<Json>({layout})}}, "ops[0].parts[0].kind:"},
             }) {
            const auto failed = error(api, "score_edit", edit_ops(doc, {op}), "InvalidArgument").at("error");
            EXPECT_TRUE(failed.at("message").get<std::string>().starts_with(field)) << failed.at("message").get<std::string>();
        }
        const auto query =
            music::api::detail::validate_tool("score_query", {{"document", doc}, {"mode", "properties"}, {"target", layout}});
        EXPECT_TRUE(query.has_value()) << query.error().message;
    }
    EXPECT_EQ(ok(api, "score_outline", {{"document", doc}}).at("revision"), revision);
}

TEST(HostApi, QueryReportsNullCoordinatesForElementsOffTheStaves) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto doc = short_score(api, 1);
    const auto elements =
        ok(api, "score_query", {{"document", doc}, {"mode", "elements"}, {"query", {{"types", music::json_array<Json>({"Part", "Rest"})}}}})
            .at("elements")
            .get_array();
    const auto of_type = [&](const char *type) {
        const auto found = std::ranges::find_if(elements, [&](const Json &e) { return e.at("type") == type; });
        return found == elements.end() ? Json(nullptr) : *found;
    };
    // A part sits in no measure and on no track.
    const auto part = of_type("Part");
    ASSERT_FALSE(part.is_null()) << music::dump_json(elements);
    EXPECT_TRUE(part.at("measure_number").is_null());
    EXPECT_TRUE(part.at("staff").is_null());
    EXPECT_TRUE(part.at("voice").is_null());
    const auto rest = of_type("Rest");
    ASSERT_FALSE(rest.is_null()) << music::dump_json(elements);
    EXPECT_EQ(rest.at("measure_number"), 1);
    EXPECT_EQ(rest.at("staff"), 1);
    EXPECT_EQ(rest.at("voice"), 1);
}

TEST(HostApi, NarrowReadSkipsLaterBarsAndClampsOnlyTheDefaultCount) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto doc = short_score(api, 1);
    // Engine text handling grows quadratically with one text's length, so the texts stay short.
    std::string words;
    for (int i = 0; i < 1024; ++i) words += "\xe4\xb8\x80";
    Json ops = music::json_array<Json>({Json{{"op", "insert_measures"}, {"count", 48}}});
    for (int bar = 2; bar <= 49; ++bar) {
        music::json_push(ops, Json{{"op", "add_text"}, {"at", pos(bar)}, {"style", "tempo"}, {"text", words}});
    }
    // Not through ok(): it would echo the 147 KiB request into the test log.
    const auto edited = api.edit({{"document", doc}, {"ops", ops}});
    ASSERT_EQ(edited.at("ok"), true) << music::dump_json(edited.at("error"));

    const auto first = api.read_music({{"document", doc}, {"count", 1}});
    ASSERT_EQ(first.at("ok"), true) << music::dump_json(first.at("error"));
    ASSERT_EQ(first.at("measures").size(), 1u);
    EXPECT_EQ(first.at("summary").get<std::string>().find("\xe4\xb8\x80"), std::string::npos);
    const auto second = api.read_music({{"document", doc}, {"measure", 2}, {"count", 1}});
    ASSERT_EQ(second.at("ok"), true) << music::dump_json(second.at("error"));
    EXPECT_NE(second.at("summary").get<std::string>().find(words), std::string::npos);

    // Only an omitted count stops at the last bar.
    const auto last = api.read_music({{"document", doc}, {"measure", 48}});
    ASSERT_EQ(last.at("ok"), true) << music::dump_json(last.at("error"));
    ASSERT_EQ(last.at("measures").size(), 2u);
    EXPECT_EQ(last.at("measures")[1].at("measure"), 49);
    const auto past = api.read_music({{"document", doc}, {"measure", 48}, {"count", 3}});
    EXPECT_EQ(past.at("error").at("code"), "InvalidArgument");
    EXPECT_TRUE(past.at("error").at("message").get<std::string>().starts_with("count:")) << music::dump_json(past.at("error"));
    const auto beyond = api.read_music({{"document", doc}, {"measure", 50}});
    EXPECT_EQ(beyond.at("error").at("code"), "InvalidArgument");
    EXPECT_TRUE(beyond.at("error").at("message").get<std::string>().starts_with("measure:")) << music::dump_json(beyond.at("error"));
}

TEST(EditCodec, PositionErrorsCountVoicesFromOne) {
    engrave::Error engine{engrave::ErrorCode::EDIT_REJECTED, "the score has no voice at index 4"};
    engine.position = engrave::PositionError{engrave::PositionError::Kind::VOICE, "at", 4, 4};
    const auto wire = music::api::api_error(engine);
    EXPECT_EQ(wire.code, "EditRejected");
    EXPECT_EQ(wire.message, "at: the score has no voice 5 (4 available)");
}

TEST(HostApi, SoundFontSelectorBounds) {
    for (const auto &selector : {std::string(4096, 'x'), std::string(4097, 'x'), std::string("kit\0.sf3", 8)}) {
        const auto request =
            music::api::decode_export({{"document", "doc"},
                                       {"format", "wav"},
                                       {"path", "test.wav"},
                                       {"options", {{"tracks", music::json_array<Json>({Json{{"track", 0}, {"soundfont", selector}}})}}}});
        EXPECT_EQ(request.has_value(), selector.size() == 4096) << selector.size();
    }
}

TEST(Phrase, CompactEntryAtPickupBoundary) {
    engrave::Outline outline{};
    outline.measure_count = 2;
    outline.measures = {{{}, 0, {0, 1}, {1, 4}}, {{}, 1, {1, 4}, {1, 1}}};
    const auto textual =
        music::parse_json(R"dsl([{"measure":1,"content":"acciac([D5 F#5]:e) [Eb4~ B#3]:q | 3:2([d36 d42]:e r:e Cb4:e)"}])dsl");
    ASSERT_TRUE(textual);
    auto phrase = music::api::decode_phrase(*textual, outline, {});
    ASSERT_TRUE(phrase) << phrase.error().message;
    ASSERT_EQ(phrase->lanes.size(), 1u);
    const auto &items = phrase->lanes.front().items;
    ASSERT_EQ(items.size(), 3u);
    const auto &chord = std::get<engrave::PhraseEvent>(items[0]);
    ASSERT_EQ(chord.notes.size(), 2u);
    EXPECT_TRUE(chord.notes[0].tie_to_next);
    EXPECT_FALSE(chord.notes[1].tie_to_next);
    EXPECT_EQ(std::get<engrave::SpelledPitch>(chord.notes[0].pitch).alter, -1);
    EXPECT_EQ(std::get<engrave::SpelledPitch>(chord.notes[1].pitch).step, engrave::StepName::B);
    ASSERT_EQ(chord.before.size(), 1u);
    EXPECT_EQ(chord.before.front().kind, engrave::GraceKind::Acciaccatura);
    EXPECT_EQ(chord.before.front().notes.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<engrave::BarCheck>(items[1]));
    const auto &tuplet = std::get<engrave::TupletGroup>(items[2]);
    EXPECT_EQ(tuplet.actual, 3);
    EXPECT_EQ(tuplet.normal, 2);
    ASSERT_EQ(tuplet.members.size(), 3u);
    ASSERT_EQ(tuplet.members[0].notes.size(), 2u);
    EXPECT_EQ(std::get<engrave::DrumKey>(tuplet.members[0].notes[1].pitch).key, 42);
    EXPECT_TRUE(tuplet.members[1].notes.empty());
}

TEST(Phrase, BadTokensIdentifyTheirLaneAndExpectedSyntax) {
    const engrave::Outline outline{.measure_count = 1, .measures = {{{}, 0, {0, 1}, {1, 1}}}};
    auto decode = [&](const std::string &content) {
        return music::api::decode_phrase(music::json_array<Json>({Json{{"measure", 1}, {"content", content}}}), outline, {});
    };
    auto rejects = [&](const std::string &content, std::string_view location, std::string_view expected) {
        const auto decoded = decode(content);
        ASSERT_FALSE(decoded) << content;
        const auto &message = decoded.error().message;
        EXPECT_TRUE(message.starts_with("lanes[0].content (" + std::string(location) + "): ")) << message;
        EXPECT_NE(message.find(expected), std::string::npos) << message;
    };
    rejects("C4 q4", "item 1, token 'q4'", "expected a spelled pitch such as Eb4, drum d36, or rest r");
    rejects("C4:q D4:q [C4 q]", "item 2, token 'q'", "expected a spelled pitch such as Eb4, drum d36, or rest r");
    rejects("C0004", "item 0, token 'C0004'", "canonical decimal integer");
    rejects("C-0", "item 0, token 'C-0'", "canonical decimal integer");
    rejects("d128", "item 0, token 'd128'", "d0 to d127");
    rejects("[C4 d36]", "item 0, token 'd36'", "all spelled pitches or all drum keys");
    rejects("D4:q...", "item 0, token 'q...'", "at most two duration dots");
    rejects("D4:01/8", "item 0, token '01/8'", "canonical decimal terms");
    rejects("3:02(A4:e G4:e F4:e)", "item 0, token '02'", "positive canonical decimal integer");
    rejects("3:2(A4:e |", "item 0, member 1, token '|'", "a note, chord, or rest");
    rejects("acciac D5", "item 0, token 'D5'", "expected '('");
    rejects("C4 after8(D5", "item 0, end of content", "expected ')'");
    rejects("r~", "item 0, token '~'", "rests cannot be tied");
    rejects("r:q~", "item 0, token '~'", "rests cannot be tied");
    rejects("acciac(D5~) C4", "item 0, token '~'", "no tie on a grace note or chord");
    rejects("acciac([D5 E5]~) C4", "item 0, token '~'", "no tie on a grace note or chord");
    rejects("acciac(D5:e~) C4", "item 0, token '~'", "no tie on a grace note or chord");
    rejects(std::string("C4 ") + std::string("\0oops", 5), "item 1, token '\\u0000oops'",
            "expected a spelled pitch such as Eb4, drum d36, or rest r");

    std::string long_token;
    for (int i = 0; i < 40; ++i) long_token += "\xc3\xa9";
    const auto decoded = decode(long_token);
    ASSERT_FALSE(decoded);
    const auto &message = decoded.error().message;
    const std::string_view prefix = "lanes[0].content (item 0, token '";
    ASSERT_TRUE(message.starts_with(prefix)) << message;
    const auto end = message.find("'): ", prefix.size());
    ASSERT_NE(end, std::string::npos) << message;
    const auto snippet = std::string_view(message).substr(prefix.size(), end - prefix.size());
    EXPECT_LE(snippet.size(), 32u);
    EXPECT_NE(snippet.find("..."), std::string::npos);
    EXPECT_TRUE(music::valid_utf8(snippet));

    EXPECT_TRUE(decode(std::string(65534, ' ') + "C4"));
    const auto oversized = decode(std::string(65535, ' ') + "C4");
    ASSERT_FALSE(oversized);
    EXPECT_NE(oversized.error().message.find("65536 Unicode characters"), std::string::npos);
}

TEST(Phrase, AllGraceDefaultsAndDurationSuffixes) {
    using namespace engrave;
    const Outline outline{.measure_count = 1, .measures = {{{}, 0, {0, 1}, {1, 1}}}};
    const auto lanes = music::parse_json(
        R"dsl([{"measure":1,"content":"acciac(D5) appog(E5) grace4(F5) grace16(G5) grace32(A5) [Cbbb4 C###5]~:h.. after8(B4) after16(A4) after32(G4) r:3/8 r:w r:q. r:e r:s r:32 r:64"}])dsl");
    ASSERT_TRUE(lanes);
    const auto phrase = music::api::decode_phrase(*lanes, outline, {});
    ASSERT_TRUE(phrase) << phrase.error().message;
    const auto &items = phrase->lanes.at(0).items;
    const auto &chord = std::get<PhraseEvent>(items.at(0));
    ASSERT_EQ(chord.notes.size(), 2u);
    EXPECT_TRUE(chord.notes[0].tie_to_next);
    EXPECT_TRUE(chord.notes[1].tie_to_next);
    EXPECT_EQ(std::get<SpelledPitch>(chord.notes[0].pitch).alter, -3);
    EXPECT_EQ(std::get<SpelledPitch>(chord.notes[1].pitch).alter, 3);
    const auto check_graces = [](const auto &graces, std::initializer_list<int> denominators) {
        ASSERT_EQ(graces.size(), denominators.size());
        std::size_t i = 0;
        for (const auto denominator : denominators) {
            EXPECT_EQ(graces[i].written_duration.num, 1);
            EXPECT_EQ(graces[i].written_duration.den, denominator);
            ++i;
        }
    };
    check_graces(chord.before, {8, 8, 4, 16, 32});
    check_graces(chord.after, {8, 16, 32});
    const std::vector<Fraction> expected{{7, 8}, {3, 8}, {1, 1}, {3, 8}, {1, 8}, {1, 16}, {1, 32}, {1, 64}};
    ASSERT_EQ(items.size(), expected.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto actual = std::get<PhraseEvent>(items[i]).written_duration;
        EXPECT_EQ(actual.num, expected[i].num);
        EXPECT_EQ(actual.den, expected[i].den);
    }
}

TEST(Phrase, AtomicWritesPreserveNotation) {
    ASSERT_FALSE(g_args.empty());
    engrave::EngineOptions options;
    options.resources = fs::absolute(fs::u8path(g_args.front()));
    auto engine = engrave::Engine::create(options);
    ASSERT_TRUE(engine) << engine.error().message;
    const std::string clarinet = R"(<museScore version="4.70"><Score><Division>480</Division>
<Part id="1"><Staff/><trackName>Clarinet</trackName><Instrument id="clarinet">
<transposeDiatonic>-1</transposeDiatonic><transposeChromatic>-2</transposeChromatic></Instrument></Part>
<Staff id="1"><Measure len="1/8"><voice><TimeSig><sigN>4</sigN><sigD>4</sigD></TimeSig>
<Rest><durationType>eighth</durationType></Rest></voice></Measure>
<Measure><voice><Rest><durationType>measure</durationType></Rest></voice></Measure></Staff></Score></museScore>)";
    const std::string kit = R"(<museScore version="4.70"><Score><Division>480</Division>
<Part id="1"><Staff><StaffType group="percussion"/></Staff><trackName>Kit</trackName><Instrument id="drumset"><useDrumset>1</useDrumset>
<Drum pitch="36"><name>Custom low drum</name><head>normal</head><line>4</line><stem>1</stem><voice>0</voice></Drum></Instrument></Part>
<Staff id="1"><Measure><voice><Rest><durationType>measure</durationType></Rest></voice></Measure></Staff></Score></museScore>)";
    auto check_readback = [&](const std::string &source, const char *content, bool drums) {
        auto bytes = std::as_bytes(std::span(source.data(), source.size()));
        auto doc = (*engine)->open(bytes, {.format = engrave::Format::MSCX});
        ASSERT_TRUE(doc) << doc.error().message;
        auto outline = (*doc)->outline();
        ASSERT_TRUE(outline);
        const Json lane = {{"measure", 1}, {"pitch_mode", "written"}, {"content", content}};
        auto phrase = music::api::decode_phrase(music::json_array<Json>({lane}), *outline, {});
        ASSERT_TRUE(phrase) << phrase.error().message;
        auto edit = (*doc)->write_phrase(*phrase, {1, engrave::EditMode::Atomic});
        ASSERT_TRUE(edit) << edit.error().message;
        ASSERT_FALSE(edit->rejection) << edit->rejection->error.message;
        auto read = (*doc)->read_music({.measure_count = outline->measure_count});
        ASSERT_TRUE(read) << read.error().message;
        auto window = music::api::music_result(*read, false);
        const auto &note = window.at("measures")[0].at("voices")[0].at("events")[0].at("notes")[0];
        if (drums) {
            EXPECT_EQ(note.at("drum"), 36);
            EXPECT_EQ(note.at("kit_name"), "Custom low drum");
        } else {
            EXPECT_EQ(note.at("written"), "B#3");
            EXPECT_NE(note.at("concert"), note.at("written"));
            EXPECT_EQ(window.at("tuplets").size(), 1u);
            const auto &principal = window.at("measures")[1].at("voices")[0].at("events")[4];
            EXPECT_EQ(principal.at("grace_before")[0].at("duration"), "1/8");
            EXPECT_EQ(principal.at("grace_before")[1].at("duration"), "1/16");
            EXPECT_EQ(principal.at("grace_after")[0].at("duration"), "1/16");
        }
    };
    check_readback(clarinet, "[B#3 Eb4~]:e | [Cb4 Eb4] 3:2(A4:e G4:e F4:e) acciac(D5) grace16(F5) C4 after16(E5)", false);
    check_readback(kit, "d36:e r:e", true);
}

TEST(MusicWindow, NestedGroupsGapsAndNotation) {
    using namespace engrave;
    const ElementId bar_id{0, 1}, first_group{0, 2}, inner_group{0, 3}, excerpt{0, 4};
    const ElementId event_id{0, 5}, note_id{0, 6}, spanner_id{0, 7}, volta_id{0, 8}, tie_id{0, 9}, hairpin_id{0, 10}, coda_id{0, 11};
    const MeasureInfo bar{bar_id, 0, {0, 1}, {1, 1}};
    auto note = [](StepName step, int octave, ElementId id = {}) {
        const SpelledPitch pitch{step, 0, octave};
        return NoteMusic{id, PitchedNote{0, pitch, pitch}};
    };
    auto tied = note(StepName::E, 4, note_id);
    tied.outgoing = TieLink{tie_id};
    const ChordMusic main{{tied},
                          {{ElementId{}, GraceKind::Acciaccatura, {1, 8}, {note(StepName::D, 5)}}},
                          {{ElementId{}, GraceKind::Grace16After, {1, 16}, {note(StepName::D, 5)}}}};
    const ChordMusic drums{{NoteMusic{{}, DrumNote{36}}, NoteMusic{{}, DrumNote{42}}}};
    const Position start{bar_id, {1, 8}, 0, 0, 0}, end{bar_id, {1, 1}, 0, 0, 0};
    MusicWindow window{
        .score = {excerpt},
        .revision = 7,
        .measures = {{bar,
                      true,
                      true,
                      3,
                      {{"TimeSig", {0, 1}, 0, Fraction{4, 4}}, {"KeySig", {0, 1}, 0, {}, -2}, {"Tempo", {1, 4}, -1, {}, 0, 96}},
                      {{0,
                        0,
                        {{event_id, {1, 8}, {1, 8}, {1, 12}, {first_group}, main},
                         {{}, {5, 24}, {1, 8}, {1, 20}, {first_group, inner_group}, drums},
                         {{}, {1, 2}, {1, 4}, {1, 4}, {}, RestMusic{true, 2}}}}}}},
        .tuplets = {{first_group, 3, 2, {1, 8}}, {inner_group, 5, 3, {1, 8}}},
        .spanners = {{spanner_id, "Slur", {}, start, end},
                     {hairpin_id, "HairPin", "crescendo", start, end, {}, {}, SpanDetail{.text = "poco a poco"}}},
        .voltas = {{{volta_id, "Volta", {}, start, end}, {1, 2}, true}, {{coda_id, "Volta", {}, end, end}, {3}, false}}};
    const auto result = music::api::music_result(window, false);
    EXPECT_EQ(result.at("score"), "excerpt");
    EXPECT_EQ(result.at("excerpt"), to_string(excerpt));
    const auto &summary = result.at("summary").get<std::string>();
    EXPECT_TRUE(summary.starts_with("Excerpt " + to_string(excerpt) + "\nBar 1 len=1/1 |:"));
    EXPECT_NE(summary.find("time(s1)=4/4@0 key(s1)=-2@0 tempo=96bpm@1/4"), std::string::npos);
    EXPECT_NE(summary.find("s1v1 @1/8 3:2(acciac(D5:1/8) E4~:1/8 after16(D5:1/16) 5:3([d36 d42]:1/8)) "
                           "@1/2 repeat(2):1/4 :|x3"),
              std::string::npos);
    EXPECT_NE(
        summary.find("\nSpanners: Slur 1@1/8(s1v1)-1@1/1(s1v1) \xc2\xb7 HairPin(crescendo) 1@1/8(s1v1)-1@1/1(s1v1) text=\"poco a poco\"\n"
                     "Voltas: Volta 1@1/8(s1v1)-1@1/1(s1v1) endings=[1,2] closed \xc2\xb7 Volta 1@1/1(s1v1)-1@1/1(s1v1) endings=[3] open"),
        std::string::npos);
    const auto tagged = music::api::music_result(window, true).at("summary").get<std::string>();
    for (auto id : {bar_id, first_group, inner_group, event_id, note_id, tie_id, spanner_id, volta_id, hairpin_id, coda_id}) {
        EXPECT_NE(tagged.find("#" + to_string(id)), std::string::npos);
    }

    window.measures[0].voices[0].events[0].elapsed_duration = {1, 24};
    const auto stretched = music::api::music_result(window, false).at("summary").get<std::string>();
    EXPECT_NE(stretched.find("after16(D5:1/16)(elapsed=1/24)"), std::string::npos);

    auto &events = window.measures[0].voices[0].events;
    events.resize(4000, MusicEvent{{}, {}, {1, 4}, {1, 4}, {}, main});
    const auto truncated = music::api::music_result(window, true);
    const auto &text = truncated.at("summary").get<std::string>();
    EXPECT_LE(text.size(), 32768u);
    EXPECT_TRUE(text.ends_with("\n... [truncated]"));
    EXPECT_EQ(truncated.at("measures")[0].at("voices")[0].at("events").size(), 4000u);
}

TEST(Phrase, HostTripleAccidentalsAndBarDiagnostics) {
    ASSERT_FALSE(g_args.empty());
    auto made = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(made) << made.error().message;
    auto &host = **made;
    const auto document = ok(host, "score_create", music::make_object<Json>()).at("document");
    auto write = [&](std::string_view content, std::string_view offset = "0") {
        return host.write_phrase(
            {{"document", document}, {"lanes", music::json_array<Json>({Json{{"measure", 1}, {"offset", offset}, {"content", content}}})}});
    };
    const auto legacy =
        host.write_phrase({{"document", document}, {"lanes", *music::parse_json(R"([{"measure":1,"events":[{"pitches":["C4"]}]}])")}});
    ASSERT_FALSE(legacy.at("ok").get<bool>());
    EXPECT_EQ(legacy.at("error").at("code"), "InvalidArgument");
    EXPECT_TRUE(legacy.at("error").at("message").get<std::string>().starts_with("lanes[0].events:"));
    const auto bad_token = write("C4 q4");
    ASSERT_FALSE(bad_token.at("ok").get<bool>());
    EXPECT_EQ(bad_token.at("error").at("code"), "InvalidArgument");
    EXPECT_EQ(bad_token.at("error").at("message"),
              "lanes[0].content (item 1, token 'q4'): invalid pitch; expected a spelled pitch such as Eb4, drum d36, or rest r");

    const auto bad = write("C###4:w | C4:1/3");
    ASSERT_FALSE(bad.at("ok").get<bool>());
    EXPECT_TRUE(bad.at("error").at("message").get<std::string>().starts_with("lane 0, item 2: "));
    EXPECT_FALSE(bad.at("error").contains("op_index"));
    const auto entry = write("C###4");
    ASSERT_TRUE(entry.at("ok").get<bool>()) << music::dump_json(entry);
    const auto inside_note = write("F4", "1/8");
    ASSERT_FALSE(inside_note.at("ok").get<bool>());
    const auto &rejection = inside_note.at("error");
    EXPECT_TRUE(rejection.at("message").get<std::string>().starts_with("lane 0, item 0: "));
    EXPECT_NE(rejection.at("message").get<std::string>().find("inside a sounding event"), std::string::npos);
    EXPECT_FALSE(rejection.contains("op_index"));

    const auto next_entry = write("C###4", "1/4");
    ASSERT_TRUE(next_entry.at("ok").get<bool>()) << music::dump_json(next_entry);
    auto read_first = [&] { return host.read_music({{"document", document}, {"count", 1}, {"include_ids", true}}); };
    const auto window = read_first();
    EXPECT_EQ(window.at("score"), "master");
    EXPECT_TRUE(window.at("excerpt").is_null());
    const auto &events = window.at("measures")[0].at("voices")[0].at("events");
    EXPECT_EQ(events[1].at("notes")[0].at("concert"), "C###4");
    const auto &note = events[0].at("notes")[0];
    ASSERT_EQ(note.at("concert"), "C###4");
    auto set_pitch = [&](int pitch, int tpc) {
        ok(host, "score_edit",
           {{"document", document},
            {"ops", music::json_array<Json>({Json{{"op", "set_pitch"}, {"note", note.at("id")}, {"pitch", pitch}, {"tpc", tpc}}})}});
    };
    set_pitch(60, 14);
    set_pitch(63, 35);
    const auto edited = read_first();
    EXPECT_EQ(edited.at("measures")[0].at("voices")[0].at("events")[0].at("notes")[0].at("concert"), "C###4");
}

TEST(MusicWindow, HairpinSubtypesInToolReadback) {
    ASSERT_FALSE(g_args.empty());
    auto made = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(made) << made.error().message;
    auto &host = **made;
    const auto document = ok(host, "score_create", music::make_object<Json>()).at("document");
    const auto written =
        host.write_phrase({{"document", document}, {"lanes", music::json_array<Json>({Json{{"measure", 1}, {"content", "C4 D4 E4"}}})}});
    ASSERT_TRUE(written.at("ok").get<bool>()) << music::dump_json(written);
    const Json range = {{"document", document}, {"count", 1}, {"include_ids", true}};
    const auto source = host.read_music(range);
    const auto &events = source.at("measures")[0].at("voices")[0].at("events");
    auto hairpin = [&](int start, int end, int kind) -> Json {
        return {{"op", "add_spanner"},
                {"type", "HairPin"},
                {"start", events[start].at("id")},
                {"end", events[end].at("id")},
                {"props", music::json_array<Json>({Json{{"name", "HAIRPIN_TYPE"}, {"value", kind}}})}};
    };
    ok(host, "score_edit", {{"document", document}, {"ops", music::json_array<Json>({hairpin(0, 1, 0), hairpin(1, 2, 1)})}});
    const auto window = host.read_music(range);
    ASSERT_TRUE(window.at("ok").get<bool>()) << music::dump_json(window);
    const auto &spanners = window.at("spanners");
    ASSERT_EQ(spanners.size(), 2u);
    EXPECT_EQ(spanners[0].at("type"), "HairPin");
    EXPECT_EQ(spanners[0].at("subtype"), "crescendo");
    EXPECT_EQ(spanners[1].at("type"), "HairPin");
    EXPECT_EQ(spanners[1].at("subtype"), "diminuendo");
    const auto &summary = window.at("summary").get<std::string>();
    EXPECT_NE(summary.find("HairPin(crescendo)#" + spanners[0].at("id").get<std::string>()), std::string::npos);
    EXPECT_NE(summary.find("HairPin(diminuendo)#" + spanners[1].at("id").get<std::string>()), std::string::npos);
}

TEST(MusicWindow, ExpressiveReadbackAcrossMarkingFamilies) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto doc = ok(api, "score_open", {{"data", b64(std::string(expressive_score))}, {"format", "mscx"}}).at("document");
    const auto plain = api.read_music({{"document", doc}, {"count", 3}});
    const auto tagged = api.read_music({{"document", doc}, {"count", 3}, {"include_ids", true}});
    ASSERT_EQ(plain.at("ok"), true) << music::dump_json(plain);
    ASSERT_EQ(tagged.at("ok"), true) << music::dump_json(tagged);
    EXPECT_EQ(tagged.at("staves")[0].at("clef"), "G");

    const auto &bar = tagged.at("measures")[0];
    const auto &second = tagged.at("measures")[1];
    const auto by = [](const Json &in, const char *key, const char *value) {
        const auto *found = find_marking(in, key, value);
        return found ? *found : Json(nullptr);
    };
    const auto mark = [&](const Json &in, const char *id) { return by(in, "id", id); };
    const auto voice_one = [](Json fields) {
        music::json_merge(fields, Json{{"scope", "voice"}, {"staff", 1}, {"voice", 1}});
        return fields;
    };
    // Dynamics and expressions default to the whole instrument; their staff is the anchor.
    EXPECT_EQ(mark(bar, "B_J"),
              (Json{{"offset", "0"}, {"scope", "instrument"}, {"staff", 1}, {"id", "B_J"}, {"type", "dynamic"}, {"value", "p"}}));
    EXPECT_EQ(mark(bar, "B_X"), (Json{{"offset", "1/4"},
                                      {"scope", "instrument"},
                                      {"staff", 1},
                                      {"id", "B_X"},
                                      {"type", "dynamic"},
                                      {"value", "mf"},
                                      {"custom_text", "mezzoforte"}}));
    EXPECT_EQ(
        mark(bar, "B_b"),
        (Json{{"offset", "0"}, {"scope", "voice"}, {"staff", 1}, {"voice", 2}, {"id", "B_b"}, {"type", "dynamic"}, {"value", "sfz"}}));
    EXPECT_EQ(mark(bar, "B_K"),
              (Json{{"offset", "0"}, {"scope", "staff"}, {"staff", 1}, {"id", "B_K"}, {"type", "fermata"}, {"symbol", "fermataAbove"}}));
    EXPECT_EQ(mark(bar, "B_I"),
              (Json{{"offset", "0"}, {"scope", "staff"}, {"staff", 1}, {"id", "B_I"}, {"type", "harmony"}, {"text", "Cm7"}}));
    EXPECT_EQ(
        mark(bar, "B_E"),
        (Json{{"offset", "0"}, {"scope", "system"}, {"id", "B_E"}, {"type", "text"}, {"kind", "system_text"}, {"text", "With motion"}}));
    EXPECT_EQ(mark(bar, "B_F"),
              (Json{{"offset", "0"}, {"scope", "system"}, {"id", "B_F"}, {"type", "text"}, {"kind", "rehearsal_mark"}, {"text", "A"}}));
    EXPECT_EQ(mark(bar, "B_G"), (Json{{"offset", "0"},
                                      {"scope", "staff"},
                                      {"staff", 1},
                                      {"id", "B_G"},
                                      {"type", "text"},
                                      {"kind", "staff_text"},
                                      {"text", "leggiero & clear"}}));
    EXPECT_EQ(mark(bar, "B_H"), (Json{{"offset", "0"},
                                      {"scope", "instrument"},
                                      {"staff", 1},
                                      {"id", "B_H"},
                                      {"type", "text"},
                                      {"kind", "expression"},
                                      {"text", "dolce"}}));
    EXPECT_EQ(mark(bar, "B_h"), (Json{{"offset", "0"},
                                      {"scope", "staff"},
                                      {"staff", 2},
                                      {"id", "B_h"},
                                      {"type", "text"},
                                      {"kind", "staff_text"},
                                      {"text", "bass only"}}));
    EXPECT_EQ(mark(bar, "B_M"),
              voice_one({{"offset", "0"},
                         {"id", "B_M"},
                         {"attachment", {{"event", "B_P"}, {"grace", {{"side", "before"}, {"ordinal", 1}, {"id", "B_L"}}}}},
                         {"type", "articulation"},
                         {"symbol", "articStaccatoAbove"}}));
    EXPECT_EQ(mark(bar, "B_O"),
              voice_one({{"offset", "0"},
                         {"id", "B_O"},
                         {"attachment", {{"event", "B_P"}, {"grace", {{"side", "before"}, {"ordinal", 2}, {"id", "B_N"}}}}},
                         {"type", "articulation"},
                         {"symbol", "ornamentTurn"},
                         {"ornament", true}}));
    EXPECT_EQ(mark(bar, "B_j"),
              voice_one({{"offset", "0"},
                         {"id", "B_j"},
                         {"attachment", {{"event", "B_P"}, {"grace", {{"side", "after"}, {"ordinal", 1}, {"id", "B_i"}}}}},
                         {"type", "articulation"},
                         {"symbol", "articTenutoAbove"}}));
    EXPECT_EQ(mark(bar, "B_R"), voice_one({{"offset", "0"},
                                           {"id", "B_R"},
                                           {"attachment", {{"event", "B_P"}}},
                                           {"type", "articulation"},
                                           {"symbol", "ornamentMordent"},
                                           {"ornament", true}}));
    EXPECT_EQ(mark(bar, "B_S"), voice_one({{"offset", "0"},
                                           {"id", "B_S"},
                                           {"attachment", {{"event", "B_P"}}},
                                           {"type", "lyric"},
                                           {"verse", 1},
                                           {"text", "glo"},
                                           {"syllabic", "begin"},
                                           {"melisma", "1/4"}}));
    EXPECT_EQ(
        mark(bar, "B_T"),
        voice_one(
            {{"offset", "0"}, {"id", "B_T"}, {"attachment", {{"event", "B_P"}}}, {"type", "lyric"}, {"verse", 2}, {"text", "light"}}));
    EXPECT_EQ(mark(bar, "B_Z").at("syllabic"), "end");
    EXPECT_EQ(mark(bar, "B_Z").at("offset"), "1/2");
    EXPECT_EQ(mark(bar, "B_U"),
              voice_one({{"offset", "0"}, {"id", "B_U"}, {"attachment", {{"event", "B_P"}}}, {"type", "arpeggio"}, {"subtype", "up"}}));
    EXPECT_EQ(mark(bar, "B_V"),
              voice_one({{"offset", "0"}, {"id", "B_V"}, {"attachment", {{"event", "B_P"}}}, {"type", "tremolo"}, {"subtype", "r16"}}));
    // The opening header clef is reported in staves[].clef only; a later clef change remains a marking.
    EXPECT_EQ(mark(bar, "B_A"), Json(nullptr));
    EXPECT_EQ(by(bar, "clef", "G"), Json(nullptr));
    EXPECT_EQ(mark(bar, "B_W"),
              (Json{{"offset", "1/4"}, {"scope", "staff"}, {"staff", 1}, {"id", "B_W"}, {"type", "clef"}, {"clef", "F"}}));
    EXPECT_EQ(mark(bar, "B_a"),
              (Json{{"offset", "1/1"}, {"scope", "staff"}, {"staff", 1}, {"id", "B_a"}, {"type", "barline"}, {"subtype", "double"}}));
    EXPECT_EQ(std::ranges::count_if(bar.at("markings").get_array(), [](const Json &m) { return m.at("type") == "barline"; }), 1);
    EXPECT_EQ(
        mark(second, "B_c"),
        (Json{{"offset", "0"}, {"scope", "instrument"}, {"staff", 1}, {"id", "B_c"}, {"type", "dynamic"}, {"value", "quasi niente"}}));
    EXPECT_EQ(mark(second, "B_e"),
              voice_one({{"offset", "0"},
                         {"id", "B_e"},
                         {"attachment", {{"event", "B_d"}}},
                         {"type", "tremolo"},
                         {"subtype", "c16"},
                         {"two_chord", true},
                         {"end", {{"measure", 2}, {"offset", "1/2"}, {"staff", 1}, {"voice", 1}, {"measure_id", second.at("id")}}},
                         {"end_event", "B_f"}}));
    const auto &signatures = bar.at("signature_changes").get_array();
    // A tempo belongs to the whole system, so it has no staff.
    const std::tuple<const char *, const char *, Json> expected_signatures[] = {
        {"TimeSig", "B_B", Json(1)}, {"KeySig", "B_C", Json(1)}, {"Tempo", "B_D", Json(nullptr)}};
    const auto outline = ok(api, "score_outline", {{"document", doc}}).at("outline").at("signatures").get_array();
    for (const auto &[type, id, staff] : expected_signatures) {
        const auto found = std::ranges::find_if(signatures, [&](const Json &s) { return s.at("type") == type; });
        ASSERT_NE(found, signatures.end()) << type;
        EXPECT_EQ(found->at("id"), id) << type;
        EXPECT_EQ(found->at("staff"), staff) << type;
        const auto listed = std::ranges::find_if(outline, [&](const Json &s) { return s.contains("id") && s.at("id") == id; });
        ASSERT_NE(listed, outline.end()) << id;
        EXPECT_EQ(listed->at("staff"), staff) << id;
    }

    const auto &plain_bar = plain.at("measures")[0];
    EXPECT_EQ(by(plain_bar, "symbol", "articAccentAbove"),
              voice_one({{"offset", "0"}, {"type", "articulation"}, {"symbol", "articAccentAbove"}}));
    EXPECT_EQ(by(plain_bar, "symbol", "articStaccatoAbove"), voice_one({{"offset", "0"},
                                                                        {"attachment", {{"grace", {{"side", "before"}, {"ordinal", 1}}}}},
                                                                        {"type", "articulation"},
                                                                        {"symbol", "articStaccatoAbove"}}));
    for (const auto &m : plain.at("measures").get_array()) {
        if (!m.contains("markings")) continue;
        for (const auto &row : m.at("markings").get_array()) EXPECT_FALSE(row.contains("id")) << music::dump_json(row);
        for (const auto &s : m.at("signature_changes").get_array()) EXPECT_FALSE(s.contains("id")) << music::dump_json(s);
    }
    EXPECT_FALSE(plain.at("measures")[2].contains("markings"));

    const auto &text = plain.at("summary").get<std::string>();
    EXPECT_TRUE(text.starts_with("Staff 1 Upper clef=G time=4/4 key=-2\n")) << text;
    for (const auto *part : {"time(s1)=4/4@0",
                             "key(s1)=-2@0",
                             "tempo=\"Andante\" 120bpm@0\n  marks: ",
                             "@0 system_text \"With motion\"",
                             "@0 rehearsal_mark \"A\"",
                             "inst(s1) @0 p \xc2\xb7 ",
                             "inst(s1) @0 expression \"dolce\"",
                             "s1 @0 harmony \"Cm7\"",
                             "s1 @0 fermata(fermataAbove)",
                             "s1 @0 staff_text \"leggiero & clear\"",
                             "s1v1 @0 grace(before:1) artic(articStaccatoAbove)",
                             "s1v1 @0 grace(before:2) ornament(ornamentTurn)",
                             "s1v1 @0 grace(after:1) artic(articTenutoAbove)",
                             "s1v1 @0 ornament(ornamentMordent)",
                             "s1v1 @0 lyric[1] \"glo\" begin melisma=1/4",
                             "s1v1 @0 lyric[2] \"light\"",
                             "s1v1 @0 arpeggio(up)",
                             "s1v1 @0 tremolo(r16)",
                             "s1v2 @0 sfz",
                             "inst(s1) @1/4 mf \"mezzoforte\"",
                             "s1 @1/4 clef(F)",
                             "s1v1 @1/4 lyric[1] \"ri\" middle",
                             "s1 @1/1 barline(double)",
                             "s2 @0 staff_text \"bass only\"",
                             "Bar 2 len=1/1\n  marks: ",
                             "inst(s1) @0 dynamic \"quasi niente\"",
                             "s1v1 @0 tremolo(c16)-2@1/2(s1v1)"}) {
        EXPECT_NE(text.find(part), std::string::npos) << part << "\n" << text;
    }
    EXPECT_EQ(text.find("clef(G)"), std::string::npos) << text;
    EXPECT_EQ(text.find("#B_"), std::string::npos) << text;
    ASSERT_NE(text.find("Bar 3"), std::string::npos) << text;
    EXPECT_EQ(text.find("marks:", text.find("Bar 3")), std::string::npos) << text;

    const auto &ids = tagged.at("summary").get<std::string>();
    for (const auto *part : {"time(s1)=4/4@0#B_B", "key(s1)=-2@0#B_C", "tempo=\"Andante\" 120bpm@0#B_D", "inst(s1) @0 p#B_J",
                             "s1v1 @0 grace(before:1) artic(articStaccatoAbove)#B_M", "lyric[1] \"glo\" begin melisma=1/4#B_S",
                             "s1v1 @0 tremolo(c16)-2@1/2(s1v1)#B_e", "s1 @1/1 barline(double)#B_a"}) {
        EXPECT_NE(ids.find(part), std::string::npos) << part << "\n" << ids;
    }

    const auto props = [](std::initializer_list<std::pair<const char *, Json>> values) {
        Json list = music::make_array<Json>();
        for (const auto &[name, value] : values) music::json_push(list, Json{{"name", name}, {"value", value}});
        return list;
    };
    const auto line = [](const char *type, Json values) {
        return Json{{"op", "add_spanner"}, {"type", type}, {"start", pos(1, "1/4")}, {"end", pos(3)}, {"props", std::move(values)}};
    };
    ok(api, "score_edit",
       edit_ops(doc, {line("Slur", props({{"up", "down"}})), line("HairPin", props({{"HAIRPIN_TYPE", 0}, {"hairpinCircledTip", true}})),
                      line("Ottava", props({{"OTTAVA_TYPE", 2}})), line("Trill", props({{"TRILL_TYPE", 3}})),
                      line("Pedal", props({{"beginText", "Ped."},
                                           {"continueText", "hold"},
                                           {"endText", "release"},
                                           {"beginHookType", "1"},
                                           {"endHookType", "2"}})),
                      line("TextLine", props({{"beginText", "sul pont."},
                                              {"continueText", "(sul pont.)"},
                                              {"endText", "ord."},
                                              {"beginHookType", "2"},
                                              {"endHookType", "1"}})),
                      Json{{"op", "set_property"}, {"target", "B_m"}, {"property", "GLISS_TYPE"}, {"value", "1"}},
                      Json{{"op", "set_property"}, {"target", "B_m"}, {"property", "GLISS_TEXT"}, {"value", "glide"}}}));
    const auto crossing = api.read_music({{"document", doc}, {"measure", 2}, {"count", 2}});
    ASSERT_EQ(crossing.at("ok"), true) << music::dump_json(crossing);
    const auto &lines = crossing.at("spanners").get_array();
    ASSERT_EQ(lines.size(), 7u) << music::dump_json(crossing.at("spanners"));
    const auto span = [&](const char *type) {
        const auto found = std::ranges::find_if(lines, [&](const Json &s) { return s.at("type") == type; });
        return found == lines.end() ? Json(nullptr) : *found;
    };
    const Json from = {{"measure", 1}, {"offset", "1/4"}, {"staff", 1}, {"voice", 1}};
    const Json to = {{"measure", 3}, {"offset", "0"}, {"staff", 1}, {"voice", 1}};
    EXPECT_EQ(span("Slur"), (Json{{"type", "Slur"}, {"start", from}, {"end", to}, {"direction", "down"}}));
    EXPECT_EQ(span("HairPin"), (Json{{"type", "HairPin"}, {"subtype", "crescendo"}, {"start", from}, {"end", to}, {"niente", true}}));
    EXPECT_EQ(span("Ottava"), (Json{{"type", "Ottava"}, {"subtype", "15ma"}, {"start", from}, {"end", to}}));
    EXPECT_EQ(span("Trill"), (Json{{"type", "Trill"}, {"subtype", "prallprall"}, {"start", from}, {"end", to}}));
    EXPECT_EQ(span("Pedal"), (Json{{"type", "Pedal"},
                                   {"start", from},
                                   {"end", to},
                                   {"begin_text", "Ped."},
                                   {"continue_text", "hold"},
                                   {"end_text", "release"},
                                   {"begin_hook", "hook_90"},
                                   {"end_hook", "hook_45"}}));
    EXPECT_EQ(span("TextLine"), (Json{{"type", "TextLine"},
                                      {"start", from},
                                      {"end", to},
                                      {"begin_text", "sul pont."},
                                      {"continue_text", "(sul pont.)"},
                                      {"end_text", "ord."},
                                      {"begin_hook", "hook_45"},
                                      {"end_hook", "hook_90"}}));
    EXPECT_EQ(span("Glissando"), (Json{{"type", "Glissando"},
                                       {"subtype", "wavy"},
                                       {"start", {{"measure", 2}, {"offset", "1/2"}, {"staff", 1}, {"voice", 1}}},
                                       {"end", to},
                                       {"text", "glide"}}));
    const auto &spans = crossing.at("summary").get<std::string>();
    for (const auto *part : {" \xe2\x86\x90Slur 1@1/4(s1v1)-3@0(s1v1) down", " \xe2\x86\x90HairPin(crescendo) 1@1/4(s1v1)-3@0(s1v1) niente",
                             " \xe2\x86\x90Ottava(15ma) 1@1/4(s1v1)-3@0(s1v1)",
                             " \xe2\x86\x90Pedal 1@1/4(s1v1)-3@0(s1v1) begin_text=\"Ped.\" continue_text=\"hold\" end_text=\"release\" "
                             "begin_hook=hook_90 end_hook=hook_45",
                             " Glissando(wavy) 2@1/2(s1v1)-3@0(s1v1) text=\"glide\""}) {
        EXPECT_NE(spans.find(part), std::string::npos) << part << "\n" << spans;
    }
    const auto tagged_spans = api.read_music({{"document", doc}, {"measure", 2}, {"count", 2}, {"include_ids", true}});
    const auto slur_id =
        std::ranges::find_if(tagged_spans.at("spanners").get_array(), [](const Json &s) { return s.at("type") == "Slur"; });
    ASSERT_NE(slur_id, tagged_spans.at("spanners").get_array().end());
    EXPECT_NE(tagged_spans.at("summary").get<std::string>().find("\xe2\x86\x90Slur#" + slur_id->at("id").get<std::string>() + " 1@1/4"),
              std::string::npos);
}

TEST(MusicWindow, SummaryEscapesAuthoredTextAndKeepsItsCap) {
    using namespace engrave;
    const ElementId bar_id{0, 1}, later_id{0, 2}, missing{0, 99}, event_id{0, 3}, grace_id{0, 4}, mark_id{0, 5}, tempo_id{0, 6},
        span_id{0, 7};
    const MeasureInfo bar{bar_id, 0, {0, 1}, {1, 1}}, later{later_id, 1, {1, 1}, {1, 1}};
    MusicWindow window{
        .staves = {{0, "Pi\nano", "piano", {4, 4}, 0, {}, false, "G"}},
        .measures = {{bar,
                      false,
                      false,
                      2,
                      {{"Tempo", {0, 1}, -1, {}, 0, 90, "Allegro\r\t\"vivo\"", tempo_id}},
                      {},
                      {{mark_id, {0, 1}, MarkScope::STAFF, 0, -1, std::nullopt, TextMark{TextMark::Kind::STAFF, "x\nBar 99 len=4/4"}},
                       {{},
                        {1, 4},
                        MarkScope::VOICE,
                        0,
                        0,
                        MarkAttachment{event_id, GraceAttachment{true, 1, grace_id}},
                        LyricMark{0, "say \"hi\" \\ bye", "single"}},
                       {{},
                        {1, 2},
                        MarkScope::VOICE,
                        0,
                        1,
                        MarkAttachment{event_id},
                        TremoloMark{"c8", true, std::nullopt, Position{missing, {1, 2}, 0, 0}}},
                       {{}, {1, 2}, MarkScope::SYSTEM, -1, -1, std::nullopt, HarmonyMark{"C\x01"}}}},
                     {later}},
        .spanners = {{span_id,
                      "TextLine",
                      {},
                      {bar_id, {0, 1}, 0, 0, 0},
                      {missing, {0, 1}},
                      {},
                      {},
                      SpanDetail{.begin_text = "a\xe2\x80\xa8"
                                               "Bar 98"}}}};

    const auto plain = music::api::music_result(window, false);
    const auto &text = plain.at("summary").get<std::string>();
    for (std::size_t start = 0; start < text.size();) {
        const auto end = std::min(text.find('\n', start), text.size());
        const auto line = std::string_view(text).substr(start, end - start);
        EXPECT_TRUE(line.starts_with("Staff ") || line.starts_with("Bar ") || line.starts_with("  ") || line.starts_with("Spanners:"))
            << line;
        start = end + 1;
    }
    for (const auto *part :
         {R"(Staff 1 Pi\nano clef=G time=4/4 key=0)", R"(tempo="Allegro\r\t\"vivo\"" 90bpm@0)", R"(s1 @0 staff_text "x\nBar 99 len=4/4")",
          R"(s1v1 @1/4 grace(after:2) lyric[1] "say \"hi\" \\ bye")", R"(s1v2 @1/2 tremolo(c8)-?@1/2(s1v1))", R"(@1/2 harmony "C\u0001")",
          R"(TextLine 1@0(s1v1)-?@0(s1v1) begin_text="a\u2028Bar 98")"}) {
        EXPECT_NE(text.find(part), std::string::npos) << part << "\n" << text;
    }
    const auto &marks = plain.at("measures")[0].at("markings");
    EXPECT_EQ(marks[0].at("text"), "x\nBar 99 len=4/4");
    EXPECT_FALSE(marks[0].contains("id"));
    EXPECT_EQ(marks[1].at("attachment"), (Json{{"grace", {{"side", "after"}, {"ordinal", 2}}}}));
    EXPECT_FALSE(marks[1].contains("syllabic"));
    EXPECT_FALSE(marks[2].contains("attachment"));
    EXPECT_TRUE(marks[2].at("end").at("measure").is_null());
    EXPECT_EQ(marks[3], (Json{{"offset", "1/2"}, {"scope", "system"}, {"type", "harmony"}, {"text", "C\x01"}}));
    EXPECT_FALSE(plain.at("measures")[1].contains("markings"));
    EXPECT_TRUE(plain.at("spanners")[0].at("end").at("measure").is_null());
    EXPECT_EQ(plain.at("spanners")[0].at("begin_text"), "a\xe2\x80\xa8"
                                                        "Bar 98");

    const auto tagged = music::api::music_result(window, true);
    const auto &tagged_marks = tagged.at("measures")[0].at("markings");
    EXPECT_EQ(tagged_marks[1].at("attachment"),
              (Json{{"event", to_string(event_id)}, {"grace", {{"side", "after"}, {"ordinal", 2}, {"id", to_string(grace_id)}}}}));
    EXPECT_EQ(tagged_marks[2].at("attachment"), (Json{{"event", to_string(event_id)}}));
    const auto &ids = tagged.at("summary").get<std::string>();
    EXPECT_NE(ids.find(R"(staff_text "x\nBar 99 len=4/4"#)" + to_string(mark_id)), std::string::npos) << ids;
    EXPECT_NE(ids.find("90bpm@0#" + to_string(tempo_id)), std::string::npos) << ids;

    auto &dense = window.measures[0].markings;
    dense.resize(4000,
                 {{}, {0, 1}, MarkScope::STAFF, 0, -1, std::nullopt, TextMark{TextMark::Kind::EXPRESSION, "\xc3\xa9\xc3\xa9\xc3\xa9"}});
    const auto truncated = music::api::music_result(window, false);
    const auto &capped = truncated.at("summary").get<std::string>();
    EXPECT_LE(capped.size(), 32768u);
    EXPECT_TRUE(capped.ends_with("\n... [truncated]"));
    EXPECT_TRUE(music::valid_utf8(capped));
    EXPECT_EQ(truncated.at("measures")[0].at("markings").size(), 4000u);
}

TEST(MusicWindow, InstrumentDynamicReachesTheOtherPianoStaff) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    constexpr std::string_view grand = R"(<museScore version="5.00"><Score><Division>480</Division>
<Part id="1"><Staff/><Staff/><trackName>Piano</trackName><Instrument id="piano"><longName>Piano</longName></Instrument></Part>
<Staff id="1"><Measure><voice><Dynamic><eid>P_A</eid><subtype>mp</subtype></Dynamic>
<Chord><durationType>whole</durationType><Note><pitch>72</pitch><tpc>14</tpc></Note></Chord></voice></Measure></Staff>
<Staff id="2"><Measure><voice><Rest><durationType>measure</durationType><duration>1/1</duration></Rest></voice></Measure></Staff>
</Score></museScore>)";
    const auto doc = ok(api, "score_open", {{"data", b64(std::string(grand))}, {"format", "mscx"}}).at("document");
    const Json expected = {{"offset", "0"}, {"scope", "instrument"}, {"staff", 1}, {"id", "P_A"}, {"type", "dynamic"}, {"value", "mp"}};
    for (const auto &staves : {music::json_array<Json>({2}), music::json_array<Json>({1, 2})}) {
        SCOPED_TRACE(music::dump_json(staves));
        const auto window = api.read_music({{"document", doc}, {"staves", staves}, {"include_ids", true}});
        ASSERT_EQ(window.at("ok"), true) << music::dump_json(window);
        const auto &bar = window.at("measures")[0];
        ASSERT_TRUE(bar.contains("markings")) << music::dump_json(window);
        EXPECT_EQ(bar.at("markings").get_array().size(), 1u) << music::dump_json(bar.at("markings"));
        EXPECT_EQ(bar.at("markings")[0], expected);
        EXPECT_NE(window.at("summary").get<std::string>().find("  marks: inst(s1) @0 mp#P_A"), std::string::npos)
            << window.at("summary").get<std::string>();
    }
}

TEST(MusicWindow, TracklessRowsInstrumentScopeAndVoicelessRepeatEnd) {
    using namespace engrave;
    const ElementId bar_id{0, 1}, span_id{0, 2};
    const MeasureInfo bar{bar_id, 0, {0, 1}, {1, 1}};
    const MusicWindow window{
        .staves = {{1, "Piano", "piano", {4, 4}, 0, {}, false, "F"}},
        .measures = {{bar,
                      false,
                      true,
                      2,
                      {{"Tempo", {0, 1}, -1, {}, 0, 72}},
                      {},
                      {{{}, {0, 1}, MarkScope::INSTRUMENT, 0, -1, std::nullopt, DynamicMark{"p"}},
                       {{}, {1, 2}, MarkScope::SYSTEM, -1, -1, std::nullopt, TextMark{TextMark::Kind::REHEARSAL, "B"}}}}},
        .spanners = {{span_id, "TextLine", {}, {bar_id, {0, 1}, -1, -1, 0}, {bar_id, {1, 2}, 1, -1, 0}}}};
    const auto result = music::api::music_result(window, false);

    const auto &row = result.at("measures")[0];
    EXPECT_TRUE(row.at("signature_changes")[0].at("staff").is_null()) << music::dump_json(row);
    EXPECT_EQ(row.at("markings")[0], (Json{{"offset", "0"}, {"scope", "instrument"}, {"staff", 1}, {"type", "dynamic"}, {"value", "p"}}));
    EXPECT_EQ(result.at("spanners")[0].at("start"), (Json{{"measure", 1}, {"offset", "0"}, {"staff", nullptr}, {"voice", nullptr}}));
    EXPECT_EQ(result.at("spanners")[0].at("end"), (Json{{"measure", 1}, {"offset", "1/2"}, {"staff", 2}, {"voice", nullptr}}));

    const auto &text = result.at("summary").get<std::string>();
    EXPECT_EQ(text, "Staff 2 Piano clef=F time=4/4 key=0\n"
                    "Bar 1 len=1/1 tempo=72bpm@0 :|\n"
                    "  marks: inst(s1) @0 p \xc2\xb7 @1/2 rehearsal_mark \"B\"\n"
                    "Spanners: TextLine 1@0-1@1/2(s2)");
}

TEST(MusicWindow, LastLyricVerseNumbersPastInt32) {
    using namespace engrave;
    const MeasureInfo bar{ElementId{0, 1}, 0, {0, 1}, {1, 1}};
    const MusicWindow window{
        .measures = {{bar,
                      false,
                      false,
                      2,
                      {},
                      {},
                      {{{}, {0, 1}, MarkScope::VOICE, 0, 0, std::nullopt, LyricMark{std::numeric_limits<int>::max(), "la", "single"}}}}}};
    const auto result = music::api::music_result(window, false);
    EXPECT_EQ(result.at("measures")[0].at("markings")[0].at("verse"), Json(std::int64_t{2147483648}));
    const auto &summary = result.at("summary").get<std::string>();
    EXPECT_NE(summary.find("s1v1 @0 lyric[2147483648] \"la\""), std::string::npos) << summary;
}

TEST(MusicWindow, SummaryRepairsInvalidUtf8BeforeItsCap) {
    using namespace engrave;
    const ElementId bar_id{0, 1};
    const MeasureInfo bar{bar_id, 0, {0, 1}, {1, 1}};
    const auto text_mark = [](std::string text) {
        return MarkMusic{{}, {0, 1}, MarkScope::SYSTEM, -1, -1, std::nullopt, TextMark{TextMark::Kind::SYSTEM, std::move(text)}};
    };
    // Each invalid byte serializes as a 3-byte U+FFFD; the long run straddles the 32,768-byte cut.
    for (const auto &bad : {std::string("x\xff\xc3y"), std::string(40000, '\xff'), std::string(32700, 'a') + std::string(200, '\xfe')}) {
        MusicWindow window{.measures = {{bar, false, false, 2, {}, {}, {text_mark(bad)}}}};
        const auto result = music::api::music_result(window, false);
        const auto wire = music::parse_json(music::dump_json(result));
        ASSERT_TRUE(wire);
        const auto &summary = wire->at("summary").get<std::string>();
        EXPECT_TRUE(music::valid_utf8(summary));
        EXPECT_LE(summary.size(), 32768u);
        EXPECT_EQ(summary, result.at("summary").get<std::string>());
        if (bad.size() < 10) {
            EXPECT_NE(summary.find("system_text \"x\xef\xbf\xbd\xef\xbf\xbdy\""), std::string::npos) << summary;
        } else {
            EXPECT_TRUE(summary.ends_with("\n... [truncated]"));
        }
    }
}

TEST(HostApi, PositionalMarkingsEditAndReopenThroughReturnedIds) {
    ASSERT_FALSE(g_args.empty());
    const auto host = HostApi::create({.resources = fs::u8path(g_args.front())});
    ASSERT_TRUE(host) << host.error().message;
    auto &api = **host;
    const auto doc = short_score(api, 2);
    const auto phrase = api.write_phrase(
        {{"document", doc}, {"lanes", music::json_array<Json>({Json{{"measure", 1}, {"content", "C4 D4 E4 F4 | G4:w"}}})}});
    ASSERT_EQ(phrase.at("ok"), true) << music::dump_json(phrase);
    const auto add = [](const Json &anchor, const char *type, const char *name, const char *value) {
        return Json{{"op", "add"},
                    {"anchor", anchor},
                    {"type", type},
                    {"props", music::json_array<Json>({Json{{"name", name}, {"value", value}}})}};
    };
    ok(api, "score_edit",
       edit_ops(doc, {add(pos(1), "Dynamic", "DYNAMIC_TYPE", "p"), add(pos(1, "1/4"), "Articulation", "symbol", "articStaccatoAbove"),
                      add(pos(1, "1/2"), "Harmony", "text", "Am"), add(pos(2), "Dynamic", "DYNAMIC_TYPE", "f"),
                      add(pos(2), "Fermata", "symbol", "fermataAbove"),
                      Json{{"op", "add_text"}, {"at", pos(1, "3/4")}, {"style", "lyrics_odd"}, {"text", "la"}},
                      Json{{"op", "add_spanner"}, {"type", "Slur"}, {"start", pos(1)}, {"end", pos(1, "3/4")}},
                      Json{{"op", "add_spanner"}, {"type", "HairPin"}, {"start", pos(1, "1/4")}, {"end", pos(2)}}}));

    const auto window = api.read_music({{"document", doc}, {"count", 2}, {"include_ids", true}});
    ASSERT_EQ(window.at("ok"), true) << music::dump_json(window);
    const auto &bars = window.at("measures");
    const auto *piano = find_marking(bars[0], "value", "p");
    const auto *staccato = find_marking(bars[0], "symbol", "articStaccatoAbove");
    const auto *harmony = find_marking(bars[0], "text", "Am");
    const auto *lyric = find_marking(bars[0], "text", "la");
    const auto *forte = find_marking(bars[1], "value", "f");
    const auto *fermata = find_marking(bars[1], "symbol", "fermataAbove");
    for (const auto *found : {piano, staccato, harmony, lyric, forte, fermata}) {
        ASSERT_NE(found, nullptr) << music::dump_json(window);
        ASSERT_TRUE(found->contains("id")) << music::dump_json(*found);
    }
    EXPECT_EQ(piano->at("offset"), "0");
    EXPECT_EQ(staccato->at("offset"), "1/4");
    EXPECT_EQ(staccato->at("attachment").at("event"), bars[0].at("voices")[0].at("events")[1].at("id"));
    EXPECT_EQ(harmony->at("offset"), "1/2");
    EXPECT_EQ(lyric->at("type"), "lyric");
    EXPECT_EQ(lyric->at("verse"), 1);
    EXPECT_EQ(lyric->at("offset"), "3/4");
    EXPECT_EQ(fermata->at("offset"), "0");
    const auto id_of = [](const Json *found) { return found->at("id").get<std::string>(); };
    const auto piano_id = id_of(piano), fermata_id = id_of(fermata);
    const std::vector<std::pair<std::string, const char *>> survivors{
        {id_of(staccato), "articStaccatoAbove"}, {id_of(harmony), "Am"}, {id_of(lyric), "la"}, {id_of(forte), "f"}};

    ok(api, "score_edit",
       edit_ops(doc, {Json{{"op", "set_property"}, {"target", piano_id}, {"property", "DYNAMIC_TYPE"}, {"value", "mp"}},
                      Json{{"op", "remove"}, {"target", fermata_id}}}));
    for (const auto &[format, name] : {std::pair{engrave::Format::MSCX, "mscx"}, {engrave::Format::MSCZ, "mscz"}}) {
        SCOPED_TRACE(name);
        std::string bytes;
        const auto saved = api.with_document(doc.get<std::string>(), {}, [&](engrave::Document &d) -> music::api::Result<void> {
            auto out = d.export_score({.format = format});
            if (!out) return std::unexpected(music::api::api_error(out.error()));
            bytes.assign(reinterpret_cast<const char *>(out->data()), out->size());
            return {};
        });
        ASSERT_TRUE(saved) << saved.error().message;
        const auto reopened = ok(api, "score_open", {{"data", b64(bytes)}, {"format", name}}).at("document");
        const auto after = api.read_music({{"document", reopened}, {"count", 2}, {"include_ids", true}});
        ASSERT_EQ(after.at("ok"), true) << music::dump_json(after);
        const auto &restored = after.at("measures");
        const auto *edited = find_marking(restored[0], "id", piano_id);
        ASSERT_NE(edited, nullptr) << music::dump_json(after);
        EXPECT_EQ(edited->at("value"), "mp");
        for (const auto &b : restored.get_array()) {
            EXPECT_EQ(find_marking(b, "id", fermata_id), nullptr);
            EXPECT_EQ(find_marking(b, "symbol", "fermataAbove"), nullptr);
        }
        for (const auto &[id, value] : survivors) {
            const auto *kept = find_marking(restored[0], "id", id);
            if (!kept) kept = find_marking(restored[1], "id", id);
            ASSERT_NE(kept, nullptr) << id;
            const auto field = kept->contains("symbol") ? "symbol" : kept->contains("value") ? "value" : "text";
            EXPECT_EQ(kept->at(field), value) << id;
        }
        const auto &lines = after.at("spanners").get_array();
        EXPECT_EQ(std::ranges::count_if(lines, [](const Json &s) { return s.at("type") == "Slur"; }), 1);
        EXPECT_EQ(std::ranges::count_if(lines, [](const Json &s) { return s.at("type") == "HairPin"; }), 1);
        ok(api, "score_close", {{"document", reopened}});
    }
}

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
    const auto initial_font = initial->fonts[0], initial_sf = initial->soundfonts[0].copy;
    EXPECT_EQ(initial->soundfonts[0].source, sf_input);
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

    auto info = ok(*host, "runtime_info", {{"scope", "templates"}});
    require(info.at("tool_revision") == "3" && !info.at("templates").empty(), "runtime catalogue is empty");
    const auto &limits = info.at("limits");
    require(limits.at("output_bytes") == 234567 && limits.at("deadline_ms") == 3456 && limits.at("transport_request_bytes") == 4194304,
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
    const Json phrase_lane = {{"measure", 1}, {"content", "Eb4"}};
    std::atomic<bool> cancelled{true};
    auto interrupted = host->write_phrase(Json{{"document", doc}, {"lanes", music::json_array<Json>({phrase_lane})}}, &cancelled);
    EXPECT_EQ(interrupted.at("error").at("code"), "Cancelled");
    auto after_cancel = host->read_music(Json{{"document", doc}, {"count", 1}, {"expected_revision", "1"}});
    EXPECT_TRUE(after_cancel.at("ok").get<bool>());
    EXPECT_TRUE(after_cancel.at("measures")[0].at("voices")[0].at("events")[0].at("rest").get<bool>());
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
    ok(*host, "runtime_info", {{"scope", "types"}});
    ok(*host, "runtime_info", {{"scope", "type"}, {"type", "Note"}});
    ok(*host, "runtime_info", {{"scope", "styles"}});
    error(*host, "runtime_info", {{"scope", "type"}, {"type", "NoSuchType"}}, "NotFound");
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

          {"style", "staffDistance"},
          {"value", 9.0}},
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
         {{"op", "add"}, {"anchor", r}, {"type", "Dynamic"}},
         {{"op", "add_spanner_to_bar_end"}, {"type", "Pedal"}, {"start", r}, {"end", r}},
         {{"op", "set_repeat_barlines"}, {"measure", r}, {"start", true}},
         {{"op", "add_volta"}, {"first_measure", r}, {"last_measure", r}, {"endings", music::json_array<Json>({1})}},
         {{"op", "add_grace_chord"},
          {"principal", r},
          {"kind", "acciac"},
          {"duration", "1/8"},
          {"pitches", music::json_array<Json>({Json{{"pitch", 62}}})}},
         {{"op", "enter_drums"}, {"at", a}, {"duration", f}, {"keys", music::json_array<Json>({36})}}});
    music::api::ValueCatalogue catalogue;
    catalogue.properties["color"] = engrave::ValueKind::COLOR;
    catalogue.styles["staffDistance"] = engrave::ValueKind::REAL;
    require(samples.size() == music::api::operation_names.size(), "operation coverage count differs");
    for (std::size_t i = 0; i < samples.size(); ++i) {
        auto decoded = music::api::decode_batch(music::json_array<Json>({samples[i]}), catalogue);
        require(decoded && samples[i].at("op") == std::string(music::api::operation_names[decoded->ops[0].index()]),
                "operation failed codec coverage: " + music::dump_json(samples[i]));
        auto extra = samples[i];
        extra["unknown"] = 1;
        require(!music::api::decode_batch(music::json_array<Json>({extra})), "unknown operation key accepted");
    }
    require(!music::api::decode_batch(music::json_array<Json>({Json{{"op", "remove"}, {"target", opref(0, 0)}}})),
            "forward OpRef accepted");
    auto bad_fraction = samples[3];
    bad_fraction["duration"] = "9223372036854775808/4";
    require(!music::api::decode_batch(music::json_array<Json>({bad_fraction})), "overflowing int64 accepted");
    auto malformed = error(*host, "score_edit", {{"document", doc}, {"ops", music::json_array<Json>({bad_fraction})}}, "InvalidArgument");
    require(malformed.at("error").at("op_index") == 0, "invalid operation omitted its index");
    for (const auto &[kind, v] :
         std::vector<std::pair<engrave::ValueKind, Json>>{{engrave::ValueKind::BOOL, true},
                                                          {engrave::ValueKind::INT, INT64_MIN},
                                                          {engrave::ValueKind::REAL, 1.5},
                                                          {engrave::ValueKind::STRING, "text"},
                                                          {engrave::ValueKind::FRACTION, "-1/4"},
                                                          {engrave::ValueKind::POINT, Json{{"x", 1.5}, {"y", -2}}},
                                                          {engrave::ValueKind::SIZE, Json{{"w", 4}, {"h", 5}}},
                                                          {engrave::ValueKind::COLOR, Json{{"r", 1}, {"g", 2}, {"b", 3}, {"a", 4}}},
                                                          {engrave::ValueKind::ENUM, "up"},
                                                          {engrave::ValueKind::INT_LIST, music::json_array<Json>({-1, 0, INT64_MAX})}}) {
        auto item = samples[0];
        item["property"] = "test";
        item["value"] = v;
        catalogue.properties["test"] = kind;
        auto decoded = music::api::decode_batch(music::json_array<Json>({item}), catalogue);
        require(decoded && music::api::encode(std::get<engrave::SetProperty>(decoded->ops[0]).value) == v,
                "native property codec mismatch");
    }
    auto opaque = samples[0];
    opaque["value"] = Json{{"kind", "opaque"}, {"type", "path"}};
    require(!music::api::decode_batch(music::json_array<Json>({opaque})), "opaque edit value accepted");
    require(music::api::decode_revision("18446744073709551615").has_value() &&
                !music::api::decode_revision("18446744073709551616").has_value() && !music::api::decode_revision("01").has_value(),
            "revision decimal bounds not enforced");
    const Json addresses =
        music::json_array<Json>({a, Json{{"kind", "page"}, {"index", 0}}, Json{{"kind", "system"}, {"page", 0}, {"index", 0}},
                                 Json{{"kind", "spanner_segment"}, {"spanner", measure}, {"index", 0}}});
    for (const auto &addr : addresses.get_array()) {
        require(music::api::decode_ref(addr).has_value(), "address codec rejected a declared address");
    }
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
           {"ops", music::json_array<Json>(
                       {Json{{"op", "set_property"}, {"target", r}, {"property", "text"}, {"value", std::string("bad\x01")}}})}},
          "InvalidArgument");
    Json enter2 = {{"op", "enter_notes"},
                   {"at", address(measure, "1", "4")},
                   {"duration", f},
                   {"pitches", music::json_array<Json>({Json{{"pitch", 60}}})}};
    auto entered =
        ok(*host, "score_edit",
           {{"document", doc},
            {"detail", true},
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
                              {"atomic", false},
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
                             {"detail", true},
                             {"ops", music::json_array<Json>({Json{{"op", "add_tie"}, {"note", idref(first_note)}},
                                                              Json{{"op", "remove"}, {"target", opref(0, 0)}}})}});
    require(ephemeral_tie.at("ops")[0].at("created").size() == 1 && ephemeral_tie.at("ops")[0].at("created")[0].is_null() &&
                ephemeral_tie.at("created").empty() && ephemeral_tie.at("removed").empty(),
            "in-batch removal did not yield a null created slot");
    auto tie =
        ok(*host, "score_edit",
           {{"document", doc}, {"detail", true}, {"ops", music::json_array<Json>({Json{{"op", "add_tie"}, {"note", idref(first_note)}}})}});
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
    const auto pitch_rejection =
        error(*host, "score_edit",
              {{"document", doc},
               {"atomic", false},
               {"ops", music::json_array<Json>({Json{{"op", "set_pitch"}, {"note", idref(first_note)}, {"pitch", 62}, {"tpc", 16}},
                                                Json{{"op", "add_tie"}, {"note", idref(first_note)}}})}},
              "EditRejected");
    const auto &pitch_error = pitch_rejection.at("error");
    EXPECT_EQ(pitch_error.at("applied"), 1);
    EXPECT_EQ(pitch_error.at("op_index"), 1);
    const auto &pitch_diagnostics = pitch_error.at("diagnostics");
    ASSERT_EQ(pitch_diagnostics.size(), 1u);
    EXPECT_EQ(pitch_diagnostics[0].at("severity"), "warning");
    const auto warning = pitch_diagnostics[0].at("message").get<std::string>();
    EXPECT_TRUE(warning.contains(tie_id) && warning.contains("different pitches")) << warning;
    ok(*host, "score_edit",
       {{"document", doc}, {"ops", music::json_array<Json>({Json{{"op", "add_text"}, {"at", a}, {"style", "staff"}, {"text", "hello"}}})}});
    ok(*host, "score_edit",
       {{"document", doc},
        {"ops", music::json_array<Json>({Json{{"op", "set_style"},

                                              {"style", "staffDistance"},
                                              {"value", 9.0}}})}});
    ok(*host, "score_edit", {{"document", doc}, {"ops", music::json_array<Json>({Json{{"op", "insert_measures"}, {"count", 1}}})}});
    ok(*host, "score_edit", {{"document", doc}, {"ops", music::json_array<Json>({Json{{"op", "add_part"}, {"instrument_id", "piano"}}})}});

    auto linked = ok(*host, "score_open", {{"path", path_text(input)}});
    const auto linked_doc = linked.at("document").get<std::string>();
    const auto excerpt = linked.at("outline").at("excerpts")[0].at("id");
    const auto part_window = host->read_music({{"document", linked_doc}, {"excerpt", excerpt}, {"count", 1}, {"include_ids", true}});
    ASSERT_TRUE(part_window.at("ok").get<bool>()) << music::dump_json(part_window);
    EXPECT_EQ(part_window.at("score"), "excerpt");
    EXPECT_EQ(part_window.at("excerpt"), excerpt);
    EXPECT_TRUE(part_window.at("summary").get<std::string>().starts_with("Excerpt " + excerpt.get<std::string>() + "\n"));
    Json part_query{{"document", linked_doc}, {"mode", "elements"}, {"query", {{"types", music::json_array<Json>({"Note"})}}}};
    part_query["query"]["excerpt"] = excerpt;
    const auto nested = ok(*host, "score_query", part_query);
    ASSERT_FALSE(nested.at("elements").empty());
    EXPECT_EQ(nested.at("elements")[0].at("score").at("excerpt"), excerpt);
    part_query["excerpt"] = excerpt;
    EXPECT_EQ(ok(*host, "score_query", part_query), nested);
    part_query["query"] = Json{{"types", music::json_array<Json>({"Note"})}};
    EXPECT_EQ(ok(*host, "score_query", part_query), nested);
    const auto bar_id = linked.at("outline").at("measures")[0].at("id");
    part_query["query"]["excerpt"] = bar_id;
    error(*host, "score_query", part_query, "InvalidArgument");
    const auto invalid_read = host->read_music({{"document", linked_doc}, {"excerpt", bar_id}});
    const auto invalid_write =
        host->write_phrase({{"document", linked_doc}, {"excerpt", bar_id}, {"lanes", music::json_array<Json>({phrase_lane})}});
    EXPECT_EQ(invalid_read.at("error").at("document"), linked_doc);
    EXPECT_EQ(invalid_write.at("error").at("document"), linked_doc);
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
    require(initial_pitch != Json(74), "fixture note already at test pitch");

    ok(*host, "checkpoint", {{"document", linked_doc}, {"name", "initial"}});
    Json color = {{"r", 12}, {"g", 34}, {"b", 56}, {"a", 255}};
    ok(*host, "score_edit",
       {{"document", linked_doc},
        {"ops",
         music::json_array<Json>({Json{{"op", "set_property"}, {"target", idref(note)}, {"property", "color"}, {"value", color}}})}});
    ok(*host, "score_edit",
       {{"document", linked_doc}, {"ops", music::json_array<Json>({Json{{"op", "set_pitch"}, {"note", idref(note)}, {"pitch", 74}}})}});
    auto linked_prop =
        ok(*host, "score_query",
           {{"document", linked_doc}, {"mode", "properties"}, {"target", idref(copy)}, {"props", music::json_array<Json>({"pitch"})}});
    require(linked_prop.at("properties")[0].at("value") == Json(74), "linked pitch did not propagate");

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
    Json blue = {{"r", 0}, {"g", 0}, {"b", 255}, {"a", 255}};
    const auto before_preflight_rev = after_restore.at("revision");
    auto preflight_fail = error(
        *host, "score_edit",
        {{"document", linked_doc},
         {"ops", music::json_array<Json>({Json{{"op", "set_property"}, {"target", idref(note)}, {"property", "color"}, {"value", blue}},
                                          Json{{"op", "set_property"},
                                               {"target", idref(target.at("measure").get<std::string>())},
                                               {"property", "pitch"},
                                               {"value", 72}}})}},
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
         {"atomic", false},
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
    require(after_followup.at("properties")[0].at("value") == Json(75), "follow-up edit after mid-batch rejection failed");

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
                            {"query", {{"types", music::json_array<Json>({"Rest"})}, {"first_measure", 1}, {"measure_count", 1}}}});
    require(prefix_rests.at("elements").size() == 1, "expected one initial rest in template measure");
    const auto prefix_rest = prefix_rests.at("elements")[0].at("id").get<std::string>();
    auto prefix_fail = error(*host, "score_edit",
                             {{"document", prefix_doc},
                              {"expected_revision", "1"},
                              {"atomic", false},
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
    require(prefix_notes.at("elements")[0].at("props")[0].at("value") == Json(60) &&
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
    require(pitched_notes.at("elements").size() == 1 && pitched_notes.at("elements")[0].at("props")[0].at("value") == Json(67),
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

                        {"style", "staffDistance"},
                        {"value", 9.0}};
    ok(*host, "score_edit", {{"document", recovery_doc}, {"ops", music::json_array<Json>({style})}});
    ok(*host, "score_close", {{"document", recovery_doc}});
    fs::remove(corrupt_path);

    const auto revision_before_assets = ok(*host, "score_outline", {{"document", doc}}).at("revision");
    error(*host, "runtime_configure",
          {{"add_font", path_text(add_font_input)}, {"soundfonts", music::json_array<Json>({path_text(invalid_sf)})}}, "InvalidInput");
    ASSERT_EQ(host->runtime_options()->fonts.size(), 1u);
    ok(*host, "runtime_configure", {{"add_font", path_text(add_font_input)}});
    auto font_info = ok(*host, "runtime_info", music::make_object<Json>());
    require(font_info.at("fonts").size() == 2, "runtime font missing");
    ok(*host, "runtime_configure", {{"add_font", path_text(add_font_input)}});
    auto duplicate_font = ok(*host, "runtime_info", music::make_object<Json>());
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
    error(*host, "runtime_configure", {{"add_font", path_text(add_font_input)}}, "InvalidInput");
    require(host->runtime_options()->fonts.size() == 2, "failed font change altered the selection");
    ok(*host, "runtime_configure", {{"soundfonts", music::json_array<Json>({path_text(replacement_sf)})}});
    auto replacement = host->runtime_options();
    require(bool(replacement), "soundfont capture failed");
    const auto selected_path = replacement->soundfonts[0].copy;
    require(read(selected_path) == read(replacement_sf), "selected SoundFont bytes differ");
    error(*host, "runtime_configure", {{"soundfonts", music::json_array<Json>({path_text(invalid_sf)})}}, "InvalidInput");
    auto retained = host->runtime_options();
    require(retained && retained->soundfonts[0].copy == selected_path, "failed SoundFont change replaced selection");
    error(*host, "runtime_configure", {{"add_font", path_text(add_font_input)}, {"soundfonts", music::make_array<Json>()}}, "InvalidInput");
    ASSERT_EQ(host->runtime_options()->soundfonts[0].copy, selected_path);
    ASSERT_EQ(host->runtime_options()->fonts.size(), 2u);

    const auto export_doc = ok(*host, "score_create", music::make_object<Json>()).at("document").get<std::string>();
    ASSERT_TRUE(host->write_phrase({{"document", export_doc}, {"lanes", music::json_array<Json>({phrase_lane})}}).at("ok").get<bool>());
    std::vector<fs::path> selectors{replacement_sf, replacement_sf.parent_path() / "." / replacement_sf.filename(), selected_path,
                                    replacement_sf.filename(), replacement_sf.stem()};
    std::error_code rel_ec;
    if (const auto rel = fs::relative(replacement_sf, fs::current_path(), rel_ec); !rel_ec && !rel.empty()) {
        selectors.push_back(rel);
    }
    for (const auto &selector : selectors) {
        auto target = scratch / "soundfont-selection.wav";
        auto request = music::api::decode_export(
            {{"document", export_doc},
             {"format", "wav"},
             {"path", path_text(target)},
             {"options", {{"tracks", music::json_array<Json>({Json{{"track", 0}, {"soundfont", path_text(selector)}}})}}}});
        ASSERT_TRUE(request) << request.error().message;
        const auto rendered = host->with_document(export_doc, {}, [&](engrave::Document &document) -> music::api::Result<void> {
            music::api::ExportTiming timing("test", target);
            auto result = music::api::live_export(
                document, *request, {}, [] { return false; }, music::api::ExportTiming::Clock::now(), timing, *retained);
            if (!result) return std::unexpected(result.error());
            return {};
        });
        ASSERT_TRUE(rendered) << path_text(selector) << ": " << rendered.error().message;
        EXPECT_TRUE(read(target).starts_with("RIFF"));
        fs::remove(target);
    }

    // Publication gate rejection must preserve exact Cancelled vs DeadlineExceeded
    // status even when request_cancel has not yet flipped to true.
    {
        const auto cancel_target = scratch / "gate-cancel.pdf";
        auto cancel_req = music::api::decode_export({{"document", export_doc}, {"format", "pdf"}, {"path", path_text(cancel_target)}});
        ASSERT_TRUE(cancel_req) << cancel_req.error().message;
        const auto res = host->with_document(export_doc, {}, [&](engrave::Document &document) -> music::api::Result<void> {
            music::api::ExportTiming timing("test", cancel_target);
            auto output = music::api::live_export(
                document, *cancel_req, {}, [] { return false; }, music::api::ExportTiming::Clock::now(), timing, *retained,
                [](music::FnRef<bool()>) { return music::api::PublicationAdmission::CANCELLED; });
            if (output) return std::unexpected(music::api::HostError{"EngineFailure", "expected Cancelled"});
            if (output.error().code != "Cancelled") return std::unexpected(output.error());
            return {};
        });
        ASSERT_TRUE(res) << res.error().code << ": " << res.error().message;
        EXPECT_FALSE(fs::exists(cancel_target));

        const auto deadline_target = scratch / "gate-deadline.pdf";
        auto deadline_req = music::api::decode_export({{"document", export_doc}, {"format", "pdf"}, {"path", path_text(deadline_target)}});
        ASSERT_TRUE(deadline_req) << deadline_req.error().message;
        const auto dl_res = host->with_document(export_doc, {}, [&](engrave::Document &document) -> music::api::Result<void> {
            music::api::ExportTiming timing("test", deadline_target);
            auto output = music::api::live_export(
                document, *deadline_req, {}, [] { return false; }, music::api::ExportTiming::Clock::now(), timing, *retained,
                [](music::FnRef<bool()>) { return music::api::PublicationAdmission::DEADLINE_EXCEEDED; });
            if (output) return std::unexpected(music::api::HostError{"EngineFailure", "expected DeadlineExceeded"});
            if (output.error().code != "DeadlineExceeded") return std::unexpected(output.error());
            return {};
        });
        ASSERT_TRUE(dl_res) << dl_res.error().code << ": " << dl_res.error().message;
        EXPECT_FALSE(fs::exists(deadline_target));
    }
    ok(*host, "score_close", {{"document", export_doc}});
    ok(*host, "runtime_configure", {{"soundfonts", music::make_array<Json>()}});
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
