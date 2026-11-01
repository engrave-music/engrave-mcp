#pragma once
#include "api_json.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
namespace music::v2 {
// Engine render returns encoded bytes: engine_call includes draw + encode.
// response is owner completion to session resume, before transport serialization.
struct ExportTiming {
    using Clock = std::chrono::steady_clock;
    Json phases = Json::object();
    std::string mode;
    std::filesystem::path destination;
    Clock::time_point last = Clock::now();
    ExportTiming(std::string m, std::filesystem::path p) : mode(std::move(m)), destination(std::move(p)) {}
    void add(const char* name, Clock::time_point start) {
        auto now = Clock::now();
        phases[name] = phases.value(name, 0.0) + std::chrono::duration<double, std::milli>(now-start).count();
        last = now;
    }
    void finish() {
        add("response", last);
        if (std::getenv("ENGRAVE_MCP_PROFILE_EXPORT")) {
            auto line = Json{{"mode", mode}, {"destination", destination.generic_string()}, {"phases_ms", phases}}.dump();
            std::fprintf(stderr, "EXPORT_TIMING %s\n", line.c_str());
        }
    }
};
}
