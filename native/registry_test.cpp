#include "registry.h"
#include "tool_result.h"
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace music::v2;

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
} // namespace

int main() {
    try {
        // Synthetic artifact lists cross the real 16 MiB budget without
        // rendering thousands of pages or relying on host path-length limits.
        constexpr std::size_t limit = 16u * 1024u * 1024u;
        Json payload{{"ok", true}, {"source_revision", "1"}, {"artifacts", Json::array()}, {"export_warnings", Json::array()}};
        const std::string path = "/" + std::string(3000, '"') + "/page.svg";
        const Json artifact{{"mime", "image/svg+xml"}, {"path", path}, {"size", "1"}};
        auto report = [&](int count) {
            payload["artifacts"] = Json::array();
            for (int page = 0; page < count; ++page) payload["artifacts"].push_back(artifact);
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
        require(music::make_tool_result(payload).dump().size() < over, "wire exceeds bound");
        std::cout << "synthetic publication boundary: " << under << " <= " << limit << " < " << over << " bytes\n";
        validate_registry();
        auto bound = handlers(std::make_index_sequence<static_cast<std::size_t>(HandlerId::Count)>{});
        validate_handlers(bound);
        for (std::size_t i = 0; i < bound.size(); ++i) {
            auto missing = bound;
            missing[i] = nullptr;
            rejects([&] { validate_handlers(missing); });
        }
        auto shared = bound;
        shared[1] = shared[0];
        validate_handlers(shared); // Different IDs may use the same callable.
        require(find_tool("score_export")->scheduling == Scheduling::Owner, "wrong export scheduling");
        require(find_tool("runtime_job") == nullptr, "retired job tool resolved");
        require(find_tool("unknown") == nullptr, "unknown tool resolved");
        for (const auto &spec : registry()) {
            require(find_tool(spec.name) == &spec, "registry lookup failed");
            if (spec.handler != HandlerId::Export) require(spec.scheduling == Scheduling::Owner, "wrong owner scheduling");
        }
        std::cout << "registry handler and scheduling checks passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
