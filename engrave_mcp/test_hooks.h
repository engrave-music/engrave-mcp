#pragma once

#include <atomic>

namespace music {

inline std::atomic<bool> g_test_hooks_enabled{false};

inline bool test_hooks_enabled() noexcept { return g_test_hooks_enabled.load(std::memory_order_relaxed); }

inline void enable_test_hooks() noexcept { g_test_hooks_enabled.store(true, std::memory_order_relaxed); }

} // namespace music
