#include "registry.h"

#include <cstdio>
#include <stdexcept>
#include <utility>

#include <gtest/gtest.h>

#include "tool_result.h"

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
    const auto under = report(low), over = report(high);
    EXPECT_LE(under, limit);
    EXPECT_GT(over, limit);
    EXPECT_LT(music::dump_json(music::make_tool_result(payload)).size(), over);
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

TEST(Registry, HandlersAndScheduling) {
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
    EXPECT_EQ(find_tool("score_export")->scheduling, Scheduling::OWNER);
    EXPECT_EQ(find_tool("runtime_job"), nullptr);
    EXPECT_EQ(find_tool("unknown"), nullptr);
    for (const auto &spec : registry()) {
        EXPECT_EQ(find_tool(spec.name), &spec);
        if (spec.handler != HandlerId::EXPORT) EXPECT_EQ(spec.scheduling, Scheduling::OWNER);
    }
}

} // namespace

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
