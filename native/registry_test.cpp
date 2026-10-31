#include "registry.h"
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace music::v2;

template<std::size_t I> Json mock_handler(void*, const Json&) { return I; }
template<std::size_t... I>
constexpr HandlerTable handlers(std::index_sequence<I...>) { return {&mock_handler<I>...}; }

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class F> void rejects(F&& function) {
    try { function(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("invalid handler table accepted");
}
}

int main() {
    try {
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
        require(find_tool("score_export")->scheduling == Scheduling::ExportJob, "wrong export scheduling");
        require(find_tool("runtime_job")->scheduling == Scheduling::JobManager, "wrong job scheduling");
        require(find_tool("unknown") == nullptr, "unknown tool resolved");
        for (const auto& spec : registry()) {
            require(find_tool(spec.name) == &spec, "registry lookup failed");
            if (spec.handler != HandlerId::Export && spec.handler != HandlerId::Job)
                require(spec.scheduling == Scheduling::Owner, "wrong owner scheduling");
        }
        std::cout << "registry handler and scheduling checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
