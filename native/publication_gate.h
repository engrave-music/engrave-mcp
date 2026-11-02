#pragma once
#include <mutex>
#include <cstddef>

namespace music {
// Process-wide: exit must hold this lock and observe no publisher. Owner
// completion uses the same lock so an old watchdog cannot kill a later task.
struct PublicationGate {
    std::mutex mutex;
    std::size_t publishers = 0;
};
inline PublicationGate publication_gate;
}
