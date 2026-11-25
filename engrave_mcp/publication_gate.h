#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "json.h"
#include "types.h"
#include "unique_fn.h"

namespace music {

// The thread publishing an export (the owner, or the audio lane for audio)
// holds this lock from first publication through the report write. Recursive
// only so staging cleanup can record failures in that same scope.
struct PublicationGate {
    std::recursive_mutex mutex;
    // Requests running on the owner thread or the audio lane. An audio export
    // counts once, from its owner-thread setup until its lane task ends.
    usize owner_tasks = 0;
    std::atomic<bool> closing{false};
    usize cleanup_failures = 0;
    std::condition_variable_any changed;
};

inline PublicationGate publication_gate;

enum class ExecutionState : i32 {
    QUEUED = 0,
    EXECUTING = 1,
    CANCELLED_QUEUED = 2,
    FINISHED = 3,
    PUBLISHING = 4,
    WATCHDOG_TERMINATING = 5,
};

struct RequestExecution {
    std::atomic<ExecutionState> state{ExecutionState::QUEUED};
    UniqueFn<const char *(Json)> report;
    UniqueFn<void(const char *)> abort_transport;
    mutable std::mutex response_mutex;
    bool report_sent = false;
    bool cancel_before_publication = false;

    void mark_report_sent() {
        std::lock_guard lock(response_mutex);
        report_sent = true;
    }

    bool was_reported() const {
        std::lock_guard lock(response_mutex);
        return report_sent;
    }
};

} // namespace music
