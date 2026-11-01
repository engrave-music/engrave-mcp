#pragma once
#include <asio/awaitable.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include <engrave/engine.h>
namespace music {
// Invoke on the session I/O thread. A separate owning OS thread contains the engine.
class AsyncSession {
public:
    explicit AsyncSession(engrave::EngineOptions options, bool live_export = false);
    ~AsyncSession();
    AsyncSession(const AsyncSession&) = delete;
    AsyncSession& operator=(const AsyncSession&) = delete;
    asio::awaitable<nlohmann::json> call(std::string name, nlohmann::json arguments);
    void shutdown() noexcept;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
