#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>

#include "json.h"

namespace music {

// The owner holds this lock from first publication through the report write.
// Recursive only so staging cleanup can record failures in that same scope.
struct PublicationGate {
    std::recursive_mutex mutex;
    std::size_t owner_tasks = 0;
    bool closing = false;
    std::size_t cleanup_failures = 0;
    std::condition_variable_any changed;
};

inline PublicationGate publication_gate;

// Execution states cover queued/running/cancelled/finished/publishing/watchdog.
// Report delivery is synchronous on the owner; it has no execution state.
struct RequestExecution : std::atomic<int> {
    RequestExecution() : std::atomic<int>(0) {}

    std::function<void(Json)> report;

    void mark_report_sent() {
        std::lock_guard lock(response_mutex);
        report_sent = true;
    }

    bool was_reported() const {
        std::lock_guard lock(response_mutex);
        return report_sent;
    }
private:
    mutable std::mutex response_mutex;
    bool report_sent = false;
};

} // namespace music
