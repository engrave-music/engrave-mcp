#pragma once
#include <chrono>
#include <future>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace music {
// Never let an unread diagnostic pipe hold up owner cleanup or terminal exit.
// The detached writer owns its buffer; process exit may truncate this diagnostic.
inline bool terminal_diagnostic(std::string message) noexcept {
    try {
        std::promise<void> done;
        auto ready = done.get_future();
        std::thread([message = std::move(message), done = std::move(done)]() mutable {
#ifdef _WIN32
            DWORD written;
            (void)::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), message.data(),
                              static_cast<DWORD>(message.size()), &written, nullptr);
#else
            (void)::write(STDERR_FILENO, message.data(), message.size());
#endif
            done.set_value();
        }).detach();
        return ready.wait_for(std::chrono::milliseconds(50)) == std::future_status::ready;
    } catch (...) { /* Diagnostics are best effort, including allocation failure. */ }
    return false;
}
}
