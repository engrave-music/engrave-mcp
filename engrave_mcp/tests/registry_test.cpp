#include "registry.h"

#include <iostream>
#include <stdexcept>
#include <utility>

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

void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

template <class F>
void rejects(F &&function) {
    try {
        function();
    } catch (const std::logic_error &) {
        return;
    }
    throw std::runtime_error("invalid handler table accepted");
}

void utf8_replacement() {
    const std::string invalid = std::string("bad") + "\xFF" + "\xFE" + "tail";
    const std::string replaced = music::replace_invalid_utf8(invalid);
    require(replaced == std::string("bad") + "\xEF\xBF\xBD\xEF\xBF\xBD" + "tail", "invalid bytes are not one U+FFFD each");
    require(music::replace_invalid_utf8("x\xE2\x82") == std::string("x\xEF\xBF\xBD"), "incomplete sequence is not one U+FFFD");
    require(music::replace_invalid_utf8("caf\xC3\xA9") == std::string("caf\xC3\xA9"), "valid UTF-8 changed");
    require(!music::valid_utf8(invalid) && music::valid_utf8(replaced), "UTF-8 validation disagrees with replacement");

    const music::Json payload{{"ok", true}, {"text", invalid}};
    const std::string text = music::dump_json(payload);
    require(music::valid_utf8(text), "wire JSON is not valid UTF-8 after replacement");
    const auto parsed = music::parse_json<music::Json>(text);
    require(parsed && parsed->at("text").get<std::string>() == replaced, "wire JSON lost the replacement text");
    const std::string wrapped = music::dump_json(music::make_tool_result(payload));
    require(music::valid_utf8(wrapped), "tool result is not valid UTF-8 after replacement");
}
} // namespace

int main() {
    try {
        // Synthetic artifact lists cross the real 16 MiB budget without
        // rendering thousands of pages or relying on host path-length limits.
        constexpr std::size_t limit = 16u * 1024u * 1024u;
        Json payload{{"ok", true},
                     {"source_revision", "1"},
                     {"artifacts", music::make_array<Json>()},
                     {"export_warnings", music::make_array<Json>()}};
        const std::string path = "/" + std::string(3000, '"') + "/page.svg";
        const Json artifact{{"mime", "image/svg+xml"}, {"path", path}, {"size", "1"}};
        auto report = [&](int count) {
            payload["artifacts"] = music::make_array<Json>();
            for (int page = 0; page < count; ++page) music::json_push(payload["artifacts"], artifact);
            return music::export_report_bound(payload);
        };
        int low = 0, high = 4096;
        require(report(high) > limit, "synthetic list did not cross limit");
        while (high - low > 1) {
            const int middle = (low + high) / 2;
            if (report(middle) <= limit) {
                low = middle;
            } else {
                high = middle;
            }
        }
        const auto under = report(low), over = report(high);
        require(under <= limit && over > limit, "publication boundary incorrect");
        require(music::dump_json(music::make_tool_result(payload)).size() < over, "wire exceeds bound");
        std::cout << "synthetic publication boundary: " << under << " <= " << limit << " < " << over << " bytes\n";
        utf8_replacement();
        std::cout << "invalid UTF-8 replacement checks passed\n";
        validate_registry();
        auto bound = handlers(std::make_index_sequence<static_cast<std::size_t>(HandlerId::COUNT)>{});
        validate_handlers(bound);
        for (std::size_t i = 0; i < bound.size(); ++i) {
            auto missing = bound;
            missing[i] = nullptr;
            rejects([&] { validate_handlers(missing); });
        }
        auto shared = bound;
        shared[1] = shared[0];
        validate_handlers(shared); // Different IDs may use the same callable.
        require(find_tool("score_export")->scheduling == Scheduling::OWNER, "wrong export scheduling");
        require(find_tool("runtime_job") == nullptr, "retired job tool resolved");
        require(find_tool("unknown") == nullptr, "unknown tool resolved");
        for (const auto &spec : registry()) {
            require(find_tool(spec.name) == &spec, "registry lookup failed");
            if (spec.handler != HandlerId::EXPORT) require(spec.scheduling == Scheduling::OWNER, "wrong owner scheduling");
        }
        std::cout << "registry handler and scheduling checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
