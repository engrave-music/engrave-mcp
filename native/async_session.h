#pragma once
#include <asio/awaitable.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
namespace music {
struct RuntimeOptions;
// Invoke on the session I/O thread. A separate owning OS thread contains the engine.
class AsyncSession {
public:
    AsyncSession(RuntimeOptions options, std::filesystem::path executable);
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
