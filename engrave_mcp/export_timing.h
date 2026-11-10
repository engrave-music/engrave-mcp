#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "api_json.h"

namespace music::api {

// Engine render returns encoded bytes: engine_call includes draw + encode.
// response runs through the synchronous report callback (including transport
// serialization/write) and session resume.
struct ExportTiming {
    using Clock = std::chrono::steady_clock;
    Json phases = make_object<Json>();
    std::string mode;
    std::filesystem::path destination;
    Clock::time_point last = Clock::now();

    ExportTiming(std::string m, std::filesystem::path p) : mode(std::move(m)), destination(std::move(p)) {}

    void add(const char *name, Clock::time_point start) {
        auto now = Clock::now();
        phases[name] = json_value<double>(phases, name, 0.0) + std::chrono::duration<double, std::milli>(now - start).count();
        last = now;
    }

    void finish() {
        add("response", last);
        if (std::getenv("ENGRAVE_MCP_PROFILE_EXPORT")) {
            auto line = dump_json(Json{{"mode", mode}, {"destination", destination.generic_string()}, {"phases_ms", phases}});
            std::fprintf(stderr, "EXPORT_TIMING %s\n", line.c_str());
        }
    }
};

} // namespace music::api
