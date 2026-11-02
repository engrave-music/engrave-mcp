#pragma once
#include <mutex>
#include <cstddef>
#include <condition_variable>

namespace music {
// Process-wide: exit must hold this lock and observe no publisher. Owner
// completion uses the same lock so an old watchdog cannot kill a later task.
struct PublicationGate {
    std::mutex mutex;
    std::size_t publishers = 0;
    std::size_t owner_tasks = 0;
    bool output_failed = false;
    bool writing = false;
    std::size_t cleanup_failures = 0;
    std::condition_variable changed;
};
inline PublicationGate publication_gate;
}
