#include "registry.h"

#include <array>
#include <cstdint>
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

    music::UniqueFn<int(int)> owned_from_empty_std = empty_std;
    EXPECT_FALSE(owned_from_empty_std);
    music::UniqueFn<int(int)> owned_from_null_ptr = null_fn;
    EXPECT_FALSE(owned_from_null_ptr);
    music::UniqueFn<int(int)> owned_from_empty_unique = music::UniqueFn<int(int) const>{};
    EXPECT_FALSE(owned_from_empty_unique);

    struct NonMovableArg {
        int value;

        explicit NonMovableArg(int initial) noexcept : value(initial) {}

        NonMovableArg(const NonMovableArg &) = delete;
        NonMovableArg(NonMovableArg &&) = delete;
    };

    const music::UniqueFn<int(NonMovableArg) const noexcept> read_arg = [](NonMovableArg &&arg) noexcept { return arg.value; };
    EXPECT_EQ(read_arg(NonMovableArg{29}), 29);

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

        int doubled() const { return value * 2; }

        int bump(int delta) {
            value += delta;
            return value;
        }
    };

    int MemberBox::*null_mp = nullptr;
    music::UniqueFn<int(const MemberBox &) const> empty_mp = null_mp;
    EXPECT_FALSE(static_cast<bool>(empty_mp));
    music::UniqueFn<int(const MemberBox &) const> live_mp = &MemberBox::value;
    ASSERT_TRUE(static_cast<bool>(live_mp));
    EXPECT_EQ(live_mp(MemberBox{42}), 42);

    int (MemberBox::*null_mfp)() const = nullptr;
    music::UniqueFn<int(const MemberBox &) const> empty_mfp = null_mfp;
    EXPECT_FALSE(static_cast<bool>(empty_mfp));
    music::UniqueFn<int(const MemberBox &) const> const_mfp = &MemberBox::doubled;
    EXPECT_EQ(const_mfp(MemberBox{21}), 42);
    music::UniqueFn<int(MemberBox &, int)> mut_mfp = &MemberBox::bump;
    MemberBox mutable_box{10};
    EXPECT_EQ(mut_mfp(mutable_box, 5), 15);
    EXPECT_EQ(mutable_box.value, 15);

    // Non-empty wrapper conversion and const/noexcept qualification dispatch and rejection.
    std::function<int(int)> live_std = [](int x) { return x + 9; };
    music::UniqueFn<int(int)> from_live_std = live_std;
    ASSERT_TRUE(static_cast<bool>(from_live_std));
    EXPECT_EQ(from_live_std(3), 12);
    music::UniqueFn<int(int) const> strict_unique = [](int x) { return x * 5; };
    music::UniqueFn<int(int)> widened_unique = std::move(strict_unique);
    EXPECT_FALSE(static_cast<bool>(strict_unique));
    ASSERT_TRUE(static_cast<bool>(widened_unique));
    EXPECT_EQ(widened_unique(4), 20);

    struct ConstOverload {
        int operator()() { return 1; }

        int operator()() const { return 2; }
    };

    music::UniqueFn<int()> call_non_const = ConstOverload{};
    const music::UniqueFn<int() const> call_const = ConstOverload{};
    EXPECT_EQ(call_non_const(), 1);
    EXPECT_EQ(call_const(), 2);

    auto mutable_only = [n = 0]() mutable { return ++n; };
    auto throwing_fn = []() { return 1; };
    static_assert(!std::is_constructible_v<music::UniqueFn<int() const>, decltype(mutable_only)>);
    static_assert(!std::is_constructible_v<music::UniqueFn<int() noexcept>, decltype(throwing_fn)>);

    int acc = 5;
    auto bump = [&acc](int delta) {
        acc += delta;
        return acc;
    };
    music::FnRef<int(int)> unwrapped = std::ref(bump);
    EXPECT_EQ(unwrapped(4), 9);
    EXPECT_EQ(acc, 9);
}

TEST(Vocabulary, UniqueFnStorage) {
    using AddressFn = music::UniqueFn<std::uintptr_t() const noexcept>;
    static_assert(sizeof(AddressFn) == 4 * sizeof(void *));
    static_assert(alignof(AddressFn) == alignof(void *));
    static_assert(std::is_nothrow_move_constructible_v<AddressFn>);
    static_assert(std::is_nothrow_move_assignable_v<AddressFn>);

    AddressFn small = [value = 17]() noexcept { return reinterpret_cast<std::uintptr_t>(&value); };
    const auto small_address = small();
    AddressFn moved_small = std::move(small);
    EXPECT_FALSE(small);
    EXPECT_NE(moved_small(), small_address);

    music::UniqueFn<int()> move_only = [value = std::make_unique<int>(19)] { return *value; };
    auto moved_only = std::move(move_only);
    EXPECT_FALSE(move_only);
    auto &same = moved_only;
    moved_only = std::move(same);
    ASSERT_TRUE(moved_only);
    EXPECT_EQ(moved_only(), 19);

    struct Lifetime {
        int &destructions;

        ~Lifetime() { ++destructions; }
    };

    struct LargeTarget {
        std::array<int, 32> values;
        std::unique_ptr<Lifetime> lifetime;

        int operator()(std::uintptr_t &address) const {
            address = reinterpret_cast<std::uintptr_t>(this);
            return values.front();
        }
    };

    static_assert(sizeof(LargeTarget) > 3 * sizeof(void *));

    int destructions = 0;
    {
        music::UniqueFn<int(std::uintptr_t &) const> large = LargeTarget{{23}, std::make_unique<Lifetime>(destructions)};
        std::uintptr_t address = 0;
        EXPECT_EQ(large(address), 23);
        auto moved_large = std::move(large);
        EXPECT_FALSE(large);
        std::uintptr_t moved_address = 0;
        EXPECT_EQ(moved_large(moved_address), 23);
        EXPECT_EQ(moved_address, address);
        EXPECT_EQ(destructions, 0);
    }
    EXPECT_EQ(destructions, 1);

    // Over-aligned target with sizeof <= SBO size still falls back to aligned global heap allocation.
    struct alignas(16) SmallAlignedTarget {
        std::uint64_t a = 1;
        std::uint64_t b = 2;

        std::uintptr_t operator()() const noexcept { return reinterpret_cast<std::uintptr_t>(this); }
    };

    static_assert(sizeof(SmallAlignedTarget) <= 3 * sizeof(void *));
    AddressFn small_aligned = SmallAlignedTarget{};
    const auto small_aligned_addr = small_aligned();
    EXPECT_EQ(small_aligned_addr % 16, 0u);
    AddressFn moved_small_aligned = std::move(small_aligned);
    EXPECT_FALSE(small_aligned);
    EXPECT_EQ(moved_small_aligned(), small_aligned_addr);

    struct alignas(64) AlignedTarget {
        std::uintptr_t operator()() const noexcept { return reinterpret_cast<std::uintptr_t>(this); }
    };

    AddressFn aligned = AlignedTarget{};
    const auto aligned_address = aligned();
    EXPECT_EQ(aligned_address % 64, 0u);
    AddressFn moved_aligned = std::move(aligned);
    EXPECT_FALSE(aligned);
    EXPECT_EQ(moved_aligned(), aligned_address);

    // Heap allocation uses ::new / ::delete and ignores class-specific operator new overrides.
    struct NullClassNewTarget {
        std::array<int, 16> pad{};

        static void *operator new(std::size_t) noexcept { return nullptr; }

        static void operator delete(void *) noexcept {}

        int operator()() const { return 73; }
    };

    struct DeletedClassNewTarget {
        std::array<int, 16> pad{};

        static void *operator new(std::size_t) = delete;
        static void operator delete(void *) = delete;

        int operator()() const { return 79; }
    };

    music::UniqueFn<int()> from_null_new = NullClassNewTarget{};
    ASSERT_TRUE(static_cast<bool>(from_null_new));
    EXPECT_EQ(from_null_new(), 73);
    music::UniqueFn<int()> from_deleted_new = DeletedClassNewTarget{};
    ASSERT_TRUE(static_cast<bool>(from_deleted_new));
    EXPECT_EQ(from_deleted_new(), 79);
}

TEST(Vocabulary, UniqueFnFailedReplacement) {
    struct ThrowingTarget {
        bool reject_copy = false;

        ThrowingTarget() = default;

        ThrowingTarget(const ThrowingTarget &other) : reject_copy(other.reject_copy) {
            if (reject_copy) {
                throw std::runtime_error("copy failed");
            }
        }

        ThrowingTarget(ThrowingTarget &&) { throw std::runtime_error("move failed"); }

        int operator()() const noexcept { return 31; }
    };

    using Fn = music::UniqueFn<int() noexcept>;
    static_assert(std::is_nothrow_move_constructible_v<Fn>);
    static_assert(std::is_nothrow_move_assignable_v<Fn>);

    ThrowingTarget source;
    Fn original = source;
    Fn moved = std::move(original);
    EXPECT_FALSE(original);
    original = std::move(moved);
    EXPECT_FALSE(moved);
    ASSERT_TRUE(original);
    EXPECT_EQ(original(), 31);

    source.reject_copy = true;
    EXPECT_THROW(original = source, std::runtime_error);
    ASSERT_TRUE(original);
    EXPECT_EQ(original(), 31);
}

TEST(Vocabulary, UniqueFnReentrantReplacement) {
    using Fn = music::UniqueFn<int()>;

    struct OnDestroy {
        Fn *owner;
        int *destructions;
        void (*callback)(Fn &);

        OnDestroy(Fn &fn, int &count, void (*action)(Fn &)) : owner(&fn), destructions(&count), callback(action) {}

        OnDestroy(OnDestroy &&other) noexcept
            : owner(std::exchange(other.owner, nullptr)), destructions(std::exchange(other.destructions, nullptr)),
              callback(other.callback) {}

        ~OnDestroy() {
            if (destructions) {
                ++(*destructions);
            }
            if (owner) {
                callback(*owner);
            }
        }

        int operator()() const { return 11; }
    };

    int reset_destructions = 0;
    Fn fn;
    fn = OnDestroy(fn, reset_destructions, [](Fn &owner) {
        EXPECT_FALSE(owner);
        owner.reset();
        owner = [] { return 37; };
    });
    fn.reset();
    EXPECT_EQ(reset_destructions, 1);
    ASSERT_TRUE(fn);
    EXPECT_EQ(fn(), 37);

    // Move-assignment publishes incoming rhs before running the old target's destructor once.
    int assign_destructions = 0;
    fn = OnDestroy(fn, assign_destructions, [](Fn &owner) {
        ASSERT_TRUE(owner);
        EXPECT_EQ(owner(), 41);
        owner = [] { return 43; };
    });
    Fn incoming = [] { return 41; };
    fn = std::move(incoming);
    EXPECT_FALSE(incoming);
    EXPECT_EQ(assign_destructions, 1);
    ASSERT_TRUE(fn);
    EXPECT_EQ(fn(), 43);

    // A non-trivially-destructible target with defaulted move ctor must neither double-destroy nor corrupt storage on move-assign.
    struct ReentrantDefaultedMove {
        std::unique_ptr<int> live;
        Fn *owner;
        int *destructions;

        ReentrantDefaultedMove(std::unique_ptr<int> v, Fn *o, int *d) noexcept : live(std::move(v)), owner(o), destructions(d) {}

        ReentrantDefaultedMove(ReentrantDefaultedMove &&) noexcept = default;

        ~ReentrantDefaultedMove() {
            if (live) {
                if (destructions) {
                    ++(*destructions);
                }
                if (owner) {
                    EXPECT_TRUE(*owner);
                    EXPECT_EQ((*owner)(), 61);
                    *owner = [p = std::make_unique<int>(67)] { return *p; };
                }
            }
        }

        int operator()() const { return *live; }
    };

    int defaulted_destructions = 0;
    fn = ReentrantDefaultedMove(std::make_unique<int>(59), &fn, &defaulted_destructions);
    Fn rhs = [] { return 61; };
    fn = std::move(rhs);
    EXPECT_FALSE(rhs);
    EXPECT_EQ(defaulted_destructions, 1);
    ASSERT_TRUE(fn);
    EXPECT_EQ(fn(), 67);

    // Scope-exit ~UniqueFnBase drains heap targets installed re-entrantly during destruction.
    int scope_inner_destructions = 0;
    int scope_outer_destructions = 0;
    {
        Fn scoped;

        struct ScopeInstall {
            Fn *owner;
            int *outer_count;
            int *inner_count;

            ScopeInstall(Fn *o, int *outer, int *inner) noexcept : owner(o), outer_count(outer), inner_count(inner) {}

            ScopeInstall(ScopeInstall &&other) noexcept
                : owner(std::exchange(other.owner, nullptr)), outer_count(std::exchange(other.outer_count, nullptr)),
                  inner_count(other.inner_count) {}

            ~ScopeInstall() {
                if (outer_count) {
                    ++(*outer_count);
                }
                if (owner) {
                    int *count = inner_count;
                    *owner = [guard = std::shared_ptr<int>(nullptr, [count](int *) { ++(*count); })] { return guard ? 1 : 0; };
                }
            }

            int operator()() const { return 0; }
        };

        scoped = ScopeInstall(&scoped, &scope_outer_destructions, &scope_inner_destructions);
    }
    EXPECT_EQ(scope_outer_destructions, 1);
    EXPECT_EQ(scope_inner_destructions, 1);

    fn = [&fn] {
        fn.reset();
        return 47;
    };
    EXPECT_EQ(fn(), 47);
    EXPECT_FALSE(fn);

    fn = [&fn] {
        fn = [] { return 53; };
        return 51;
    };
    EXPECT_EQ(fn(), 51);
    ASSERT_TRUE(fn);
    EXPECT_EQ(fn(), 53);
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

TEST(Vocabulary, SortedJsonBTreeMap) {
    static_assert(std::is_nothrow_move_constructible_v<music::SortedJsonMap<music::Json>>);
    static_assert(std::is_nothrow_move_assignable_v<music::SortedJsonMap<music::Json>>);

    music::Json empty = music::make_object<music::Json>();
    EXPECT_TRUE(empty.empty());
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_EQ(empty.get_object().begin(), empty.get_object().end());
    EXPECT_EQ(empty.get_object().find(std::string_view("missing")), empty.get_object().end());
    EXPECT_EQ(music::dump_json(empty), "{}");

    music::Json obj = music::make_object<music::Json>();
    obj["zeta"] = 3;
    obj["alpha"] = 1;
    obj["mu"] = 2;
    EXPECT_EQ(music::dump_json(obj), R"({"alpha":1,"mu":2,"zeta":3})");

    const std::string_view sv = "mu";
    EXPECT_TRUE(obj.contains(sv));
    EXPECT_EQ(music::json_value<int>(obj, sv, -1), 2);
    music::json_erase(obj, sv);
    EXPECT_FALSE(obj.contains(sv));
    EXPECT_EQ(music::dump_json(obj), R"({"alpha":1,"zeta":3})");

    music::Json copy = obj;
    copy["beta"] = 4;
    EXPECT_EQ(music::dump_json(obj), R"({"alpha":1,"zeta":3})");
    EXPECT_EQ(music::dump_json(copy), R"({"alpha":1,"beta":4,"zeta":3})");

    const auto parsed = music::parse_json<music::Json>(R"({"b":2,"a":1,"nested":{"y":true,"x":null}})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(music::dump_json(*parsed), R"({"a":1,"b":2,"nested":{"x":null,"y":true}})");
}

} // namespace

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
