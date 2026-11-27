#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "json.h"
#include "types.h"
#include "unique_fn.h"

namespace music {

// Decouples stdout wire serialization, short-lived task/publication state, and
// lock-free staging cleanup counters under an acyclic lock order:
//   stdout_mutex -> state_mutex -> RequestExecution::response_mutex.
// state_mutex is never held across disk flush, stdout writes, or sleeps.
struct PublicationGate {
    std::mutex stdout_mutex;
    std::mutex state_mutex;
    std::condition_variable state_cv;
    // Requests running on the owner thread or the audio lane. An audio export
    // counts once, from its owner-thread setup until its lane task ends.
    usize active_tasks = 0;
    // Exports committed from begin_publication() until their report finishes.
    usize active_publications = 0;
    // Threads currently inside write_report_bytes().
    usize active_stdout_writes = 0;
    bool exit74_claimed = false;
    std::atomic<bool> closing{false};
    std::atomic<usize> cleanup_failures{0};

    void mark_closing() noexcept {
        {
            std::lock_guard lock(state_mutex);
            closing.store(true, std::memory_order_release);
        }
        state_cv.notify_all();
    }

    void notify_state_change() noexcept {
        { std::lock_guard lock(state_mutex); }
        state_cv.notify_all();
    }
};

inline PublicationGate publication_gate;

// Releases an admitted publication token when the export's publication and
// synchronous report delivery finish or unwind.
class PublicationScope {
public:
    PublicationScope() noexcept = default;

    ~PublicationScope() noexcept { release(); }

    PublicationScope(const PublicationScope &) = delete;
    PublicationScope &operator=(const PublicationScope &) = delete;

    void adopt() noexcept { active_ = true; }

    bool active() const noexcept { return active_; }

    void release() noexcept {
        if (!active_) return;
        active_ = false;
        {
            std::lock_guard lock(publication_gate.state_mutex);
            --publication_gate.active_publications;
        }
        publication_gate.state_cv.notify_all();
    }

private:
    bool active_ = false;
};

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
