#include "registry.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "containers.h"
#include "schema_defs.h"
#include "tool_result.h"
#include "types.h"
#include "unique_fn.h"

namespace {
using namespace music::api;

template <std::size_t I>
Json mock_handler(void *, const Json &) {
    return I;
}

template <std::size_t... I>
constexpr HandlerTable handlers(std::index_sequence<I...>) {
    return {&mock_handler<I>...};
}

TEST(Registry, SyntheticPublicationBoundary) {
    // Synthetic artifact lists cross the real 16 MiB budget without
    // rendering thousands of pages or relying on host path-length limits.
    constexpr std::size_t limit = 16u * 1024u * 1024u;
    Json payload{
        {"ok", true}, {"source_revision", "1"}, {"artifacts", music::make_array<Json>()}, {"export_warnings", music::make_array<Json>()}};
    const std::string path = "/" + std::string(3000, '"') + "/page.svg";
    const Json artifact{{"mime", "image/svg+xml"}, {"path", path}, {"size", "1"}};
    auto report = [&](int count) {
        payload["artifacts"] = music::make_array<Json>();
        for (int page = 0; page < count; ++page) music::json_push(payload["artifacts"], artifact);
        return music::export_report_bound(payload);
    };
    int low = 0, high = 4096;
    ASSERT_GT(report(high), limit) << "synthetic list did not cross limit";
    while (high - low > 1) {
        const int middle = (low + high) / 2;
        if (report(middle) <= limit) {
            low = middle;
        } else {
            high = middle;
        }
    }
    const auto over = report(high), under = report(low);
    EXPECT_LE(under, limit);
    EXPECT_GT(over, limit);
    const std::string id(transport_request_limit - 256, 'x');
    Json legacy_response{{"jsonrpc", "2.0"}, {"id", id}, {"result", music::make_tool_result(payload)}};
    EXPECT_LE(music::dump_json(legacy_response).size() + 1, limit);
    Json modern_response = legacy_response;
    music::add_modern_result_fields(modern_response.at("result"));
    EXPECT_LE(music::dump_json(modern_response).size() + 1, limit);

    Json paths = music::make_array<Json>();
    for (const auto &entry : payload.at("artifacts").get_array()) music::json_push(paths, entry.at("path"));
    Json fallback_result = music::make_tool_result(Json{
        {"ok", false},
        {"error", {{"code", "EngineFailure"}, {"message", "Completed export wrapper failed"}, {"published_artifacts", std::move(paths)}}}});
    music::add_modern_result_fields(fallback_result);
    Json fallback_response{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(fallback_result)}};
    EXPECT_LE(music::dump_json(fallback_response).size() + 1, limit);

    const Json small{{"ok", true},
                     {"source_revision", "1"},
                     {"artifacts", music::json_array<Json>({artifact})},
                     {"export_warnings", music::make_array<Json>()}};
    Json small_result = music::make_tool_result(small);
    music::add_modern_result_fields(small_result);
    Json small_response{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(small_result)}};
    EXPECT_LE(music::dump_json(small_response).size() + 1, music::export_report_bound(small));
}

TEST(Registry, MusicSummaryAndTypedEvents) {
    Json bar{{"measure", 1}, {"duration", "3/4"}};
    const Json payload{{"ok", true}, {"revision", "2"}, {"summary", "Bar 1  s1v1 Eb4:3/4"}, {"measures", music::json_array<Json>({bar})}};
    auto result = music::make_tool_result(payload);
    EXPECT_EQ(result.at("content")[0].at("text"), "Bar 1  s1v1 Eb4:3/4");
    EXPECT_EQ(result.at("structuredContent").at("measures")[0], bar);
    EXPECT_FALSE(result.at("structuredContent").contains("summary"));
    const Json large{{"ok", true}, {"types", music::json_array<Json>({std::string(300u * 1024u, 'x')})}};
    const auto large_structured = music::make_tool_result(large);
    EXPECT_LT(large_structured.at("content")[0].at("text").get<std::string>().size(), 1024u);
    EXPECT_EQ(large_structured.at("structuredContent"), large);
}

TEST(Registry, DslSchemaDescribesItsEntryGrammar) {
    const auto tools = declared_tools();
    for (const auto &tool : tools.get_array()) {
        if (tool.at("name") != "score_write_phrase") continue;
        EXPECT_TRUE(tool.at("annotations").at("destructiveHint").get<bool>());
        const auto &lane = tool.at("inputSchema").at("properties").at("lanes").at("items");
        EXPECT_EQ(lane.at("required"), music::json_array<Json>({"measure", "content"}));
        EXPECT_FALSE(lane.at("properties").contains("events"));
        const auto &content = lane.at("properties").at("content");
        EXPECT_EQ(content.at("maxLength"), 65536);
        const auto &description = content.at("description").get<std::string>();
        for (auto token : {"Eb4", "F#5", "C###4", "d36", "[C4~ E4]", ":h..", ":3/8", "3:2(A4:e G4:e F4:e)", "|", "acciac(D5)", "appog(E5)",
                           "grace4(F5)", "grace16(G5)", "grace32(A5)", "after8(D5)", "after16(E5)", "after32(F5)"}) {
            EXPECT_NE(description.find(token), std::string::npos) << token;
        }
        return;
    }
    ADD_FAILURE() << "DSL phrase tool missing";
}

TEST(Registry, InvalidUtf8Replacement) {
    const std::string invalid = std::string("bad") + "\xFF" + "\xFE" + "tail";
    const std::string replaced = music::replace_invalid_utf8(invalid);
    EXPECT_EQ(replaced, std::string("bad") + "\xEF\xBF\xBD\xEF\xBF\xBD" + "tail");
    EXPECT_EQ(music::replace_invalid_utf8("x\xE2\x82"), std::string("x\xEF\xBF\xBD"));
    EXPECT_EQ(music::replace_invalid_utf8("caf\xC3\xA9"), std::string("caf\xC3\xA9"));
    EXPECT_FALSE(music::valid_utf8(invalid));
    EXPECT_TRUE(music::valid_utf8(replaced));

    const music::Json payload{{"ok", true}, {"text", invalid}};
    const std::string text = music::dump_json(payload);
    EXPECT_TRUE(music::valid_utf8(text));
    const auto parsed = music::parse_json<music::Json>(text);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->at("text").get<std::string>(), replaced);
    const std::string wrapped = music::dump_json(music::make_tool_result(payload));
    EXPECT_TRUE(music::valid_utf8(wrapped));
}

TEST(Registry, Handlers) {
    EXPECT_NO_THROW(validate_registry());
    auto bound = handlers(std::make_index_sequence<static_cast<std::size_t>(HandlerId::COUNT)>{});
    EXPECT_NO_THROW(validate_handlers(bound));
    for (std::size_t i = 0; i < bound.size(); ++i) {
        auto missing = bound;
        missing[i] = nullptr;
        EXPECT_THROW(validate_handlers(missing), std::logic_error);
    }
    auto shared = bound;
    shared[1] = shared[0];
    EXPECT_NO_THROW(validate_handlers(shared)); // Different IDs may use the same callable.
    ASSERT_NE(find_tool("score_export"), nullptr);
    EXPECT_EQ(find_tool("runtime_job"), nullptr);
    EXPECT_EQ(find_tool("unknown"), nullptr);
    for (const auto &spec : registry()) {
        EXPECT_EQ(find_tool(spec.name), &spec);
    }
}

int add_two(int x) noexcept { return x + 2; }

TEST(Vocabulary, FnRefAndUniqueFn) {
    static_assert(sizeof(music::FnRef<int(int)>) == 2 * sizeof(void *));
    static_assert(std::is_trivially_copyable_v<music::FnRef<int(int)>>);
    static_assert(!std::is_assignable_v<music::FnRef<int(int)> &, decltype(&add_two)>);
    static_assert(!std::is_constructible_v<music::FnRef<const int &(int)>, decltype(&add_two)>);
    static_assert(std::is_constructible_v<music::FnRef<int(int)>, music::FnRef<int(int) const noexcept>>);
    static_assert(!std::is_constructible_v<music::FnRef<int(int) const>, music::FnRef<int(int)>>);
    static_assert(!std::is_constructible_v<music::UniqueFn<int(int)>, music::FnRef<int(int)>>);
    static_assert(!std::is_constructible_v<music::UniqueFn<int(int) const>, music::FnRef<int(int)>>);
    static_assert(!std::is_assignable_v<music::UniqueFn<int(int)> &, music::FnRef<int(int)>>);

    music::FnRef<int(int)> empty_ref;
    EXPECT_FALSE(static_cast<bool>(empty_ref));
    int (*null_fn)(int) = nullptr;
    music::FnRef<int(int)> from_null_ptr(null_fn);
    EXPECT_FALSE(static_cast<bool>(from_null_ptr));
    std::function<int(int)> empty_std;
    music::FnRef<int(int)> from_empty_std(empty_std);
    EXPECT_FALSE(static_cast<bool>(from_empty_std));

    music::FnRef<int(int) const noexcept> strict_ref(add_two);
    music::FnRef<int(int)> widened_ref(strict_ref);
    ASSERT_TRUE(static_cast<bool>(widened_ref));
    EXPECT_EQ(widened_ref(5), 7);
    widened_ref = nullptr;
    EXPECT_FALSE(static_cast<bool>(widened_ref));

    music::UniqueFn<int(int)> owned = [bias = std::make_unique<int>(10)](int x) mutable {
        *bias += x;
        return *bias;
    };
    ASSERT_TRUE(static_cast<bool>(owned));
    music::FnRef<int(int)> view_owned(owned);
    EXPECT_EQ(view_owned(3), 13);
    EXPECT_EQ(owned(4), 17);
    owned = nullptr;
    EXPECT_FALSE(static_cast<bool>(owned));
    music::FnRef<int(int)> view_empty_owned(owned);
    EXPECT_FALSE(static_cast<bool>(view_empty_owned));

    // Reassignment, conversion adapter (void discard / implicit conversion), member pointer, and std::ref unwrapping.
    owned = [](int x) { return x * 3; };
    ASSERT_TRUE(static_cast<bool>(owned));
    EXPECT_EQ(owned(4), 12);

    int adapted_sum = 0;
    music::UniqueFn<void(short)> adapted = [&](int x) {
        adapted_sum += x;
        return adapted_sum;
    };
    adapted(static_cast<short>(6));
    EXPECT_EQ(adapted_sum, 6);

    struct MemberBox {
        int value = 0;
    };

    int MemberBox::*null_mp = nullptr;
    music::UniqueFn<int(const MemberBox &) const> empty_mp = null_mp;
    EXPECT_FALSE(static_cast<bool>(empty_mp));
    music::UniqueFn<int(const MemberBox &) const> live_mp = &MemberBox::value;
    ASSERT_TRUE(static_cast<bool>(live_mp));
    EXPECT_EQ(live_mp(MemberBox{42}), 42);

    int acc = 5;
    auto bump = [&acc](int delta) {
        acc += delta;
        return acc;
    };
    music::FnRef<int(int)> unwrapped = std::ref(bump);
    EXPECT_EQ(unwrapped(4), 9);
    EXPECT_EQ(acc, 9);
}

TEST(Vocabulary, HashContainersTransparentStringLookup) {
    music::HashMap<std::string, int> map;
    map.emplace("alpha", 1);
    map.emplace("beta", 2);
    const std::string_view key = "beta";
    auto it = map.find(key);
    ASSERT_NE(it, map.end());
    EXPECT_EQ(it->second, 2);
    EXPECT_EQ(map.find(std::string_view("gamma")), map.end());

    music::HashSet<std::string> set;
    set.insert("token-1");
    EXPECT_TRUE(set.contains(std::string_view("token-1")));
    EXPECT_FALSE(set.contains(std::string_view("token-2")));

    music::SegmentedHashMap<std::string, int> smap;
    smap.emplace("one", 11);
    auto &ref = smap.at("one");
    for (int i = 0; i < 64; ++i) smap.emplace("k" + std::to_string(i), i);
    EXPECT_EQ(&smap.find(std::string_view("one"))->second, &ref);

    music::SegmentedHashSet<std::string> sset;
    sset.insert("seg-1");
    EXPECT_TRUE(sset.contains(std::string_view("seg-1")));
    EXPECT_FALSE(sset.contains(std::string_view("seg-2")));
}

} // namespace

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
