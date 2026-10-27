#pragma once
#include <memory>
#include <string>

namespace music {
class MusicCore;

// Keep the host interface independent of Qt while the implementation is migrated.
class Engine {
public:
    Engine(int& argc, char** argv, const std::string& soundfont, const std::string& resources = "");
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    static std::string tools_json();
    std::string call(const std::string& tool, const std::string& arguments_json);
    MusicCore& core();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
