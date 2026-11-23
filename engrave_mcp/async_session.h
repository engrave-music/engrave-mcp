#pragma once

#include <memory>
#include <string>

#include <asio/awaitable.hpp>
#include <engrave/engine.h>

#include "json.h"
#include "publication_gate.h"

namespace music {

// Invoke on the session I/O thread. A separate owning OS thread contains the engine.
struct AsyncSession {
    explicit AsyncSession(engrave::EngineOptions options);
    ~AsyncSession();
    AsyncSession(const AsyncSession &) = delete;
    AsyncSession &operator=(const AsyncSession &) = delete;
    asio::awaitable<Json> call(std::string name, Json arguments, std::shared_ptr<RequestExecution> execution = {});
    void shutdown() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace music
