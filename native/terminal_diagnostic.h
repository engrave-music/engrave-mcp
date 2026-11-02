#pragma once
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif
namespace music {
// One synchronous nonblocking attempt, no threads or retained messages.
// Pipes that cannot be made nonblocking are deliberately skipped.
inline bool terminal_diagnostic(std::string_view message) noexcept {
    message = message.substr(0, 1024);
#ifdef _WIN32
    HANDLE handle = ::GetStdHandle(STD_ERROR_HANDLE);
    if (::GetFileType(handle) != FILE_TYPE_PIPE) return false;
    DWORD mode = PIPE_NOWAIT;
    if (!::SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) return false;
    DWORD written = 0;
    return ::WriteFile(handle, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)
        && written == message.size();
#else
    const int flags = ::fcntl(STDERR_FILENO, F_GETFL);
    if (flags < 0 || ::fcntl(STDERR_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    return ::write(STDERR_FILENO, message.data(), message.size()) == static_cast<ssize_t>(message.size());
#endif
}
}
