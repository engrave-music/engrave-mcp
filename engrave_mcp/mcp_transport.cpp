#include "mcp_transport.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <asio.hpp>
#if !defined(ASIO_HAS_THREADS)
#error "Asio thread support (ASIO_HAS_THREADS) must be enabled; ensure _MT is defined on Windows MSVC"
#endif
#include <dyn/dyn.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#ifdef ERROR
#undef ERROR
#endif
#ifdef EOF
#undef EOF
#endif

#include "publication_gate.h"
#include "runtime_limits.h"
#include "schema_defs.h"
#include "terminal_diagnostic.h"
#include "test_hooks.h"
#include "tool_result.h"
#include "utf8_text.h"

namespace music {

namespace {

constexpr std::size_t k_max_line_bytes = api::transport_request_limit;
constexpr std::size_t k_max_pending_lines = 64u;
constexpr std::size_t k_max_pending_bytes = 32u * 1024u * 1024u;

constexpr std::size_t k_max_inflight_calls = 64u;
constexpr std::size_t k_drain_batch = 64u;

// A single wire response is capped at 16 MiB (including its newline). The
// output budget covers both serialised-but-not-yet-queued responses and
// messages queued for (or being written by) the writer thread, so the total
// response memory held by the transport is bounded independently of how fast
// the client reads stdout.
constexpr std::size_t k_max_response_bytes = 16u * 1024u * 1024u;
constexpr std::size_t k_output_budget_bytes = 48u * 1024u * 1024u;
constexpr std::size_t k_max_output_messages = 256u;

constexpr int k_read_error = -1;
constexpr int k_read_eof = 0;
constexpr int k_read_stopped = -2;

constexpr std::string_view k_modern_version = "2026-07-28";
constexpr std::string_view k_latest_legacy_version = "2025-11-25";
constexpr std::string_view k_oldest_legacy_version = "2025-06-18";
constexpr std::string_view k_protocol_version_key = "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view k_client_capabilities_key = "io.modelcontextprotocol/clientCapabilities";
constexpr std::string_view k_client_info_key = "io.modelcontextprotocol/clientInfo";
constexpr std::string_view k_log_level_key = "io.modelcontextprotocol/logLevel";

enum class ServerState { UNINITIALIZED, INITIALIZED_PENDING, READY };

enum class WireStatus { OK, TOO_LARGE, FAILED };

// Lets tests shorten the live-export watchdog; production always uses fallback.
std::chrono::milliseconds test_hook_ms(const char *name, std::int64_t minimum, std::chrono::milliseconds fallback) {
    if (!test_hooks_enabled()) return fallback;
    const char *text = std::getenv(name);
    if (!text) return fallback;
    const std::string_view value(text);
    std::int64_t ms = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), ms);
    if (ec != std::errc{} || end != value.data() + value.size() || ms < minimum) return fallback;
    return std::chrono::milliseconds(ms);
}

bool json_nesting_exceeds(std::string_view text, int max_depth) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char ch : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{' || ch == '[') {
            if (++depth > max_depth) return true;
        } else if (ch == '}' || ch == ']') {
            --depth;
        }
    }
    return false;
}

bool is_valid_id(const Json &id) { return id.is_string() || id.holds<std::uint64_t>() || id.holds<std::int64_t>(); }

bool has_modern_meta_key(const Json &meta) {
    return meta.contains(k_protocol_version_key) || meta.contains(k_client_capabilities_key) || meta.contains(k_client_info_key) ||
           meta.contains(k_log_level_key);
}

bool valid_client_info(const Json &info) {
    return info.is_object() && info.contains("name") && info.at("name").is_string() && info.contains("version") &&
           info.at("version").is_string();
}

bool valid_log_level(const Json &level) {
    if (!level.is_string()) return false;
    const std::string_view value = level.get<std::string>();
    return value == "debug" || value == "info" || value == "notice" || value == "warning" || value == "error" || value == "critical" ||
           value == "alert" || value == "emergency";
}

Json supported_versions() { return json_array(k_modern_version, k_latest_legacy_version, k_oldest_legacy_version); }

std::string server_instructions() {
    std::string instructions =
        "Start with score_create or score_open. Keep the document handle and revision, then inspect the returned outline for "
        "measure lengths, staves, signatures and excerpts. Bars, staves and voices start at 1. Use score_write_phrase for "
        "note entry, then score_read_music to check the result in short windows. Compact positions use exact whole-note offsets "
        "relative to the start of the measure: 0, 1/4 and 3/8 are valid offsets (explicit segment addresses use score-wide tick). "
        "A pickup uses its actual duration. "
        "Pitch defaults to concert. Choose written pitch_mode on a lane when copying a transposed part. ";
    instructions += api::phrase_grammar;
    instructions += " ";
    instructions += api::phrase_grace_defaults;
    instructions += " Echo the last revision as expected_revision to guard writes and reads. Phrase writes and default score_edit "
                    "batches are atomic. Save a named checkpoint before experimenting and use restore_checkpoint to return to it. "
                    "Use runtime_info with scope ops for detailed edit operations or instruments to find instrument IDs. "
                    "score_edit numbers resolve against the live score as each op runs, so a later op sees bars and staves an "
                    "earlier one inserted. Address a position as {measure, staff?, voice?, offset?}; set top-level excerpt to "
                    "scope a whole batch to one excerpt. score_read_music lists dynamics, articulations, lyrics, text and other "
                    "markings on each bar's marks: line. Request include_ids on a music window when an edit needs an element ID. "
                    "Export the finished document with score_export by supplying format and path. Use mscz for an editable score. "
                    "Close the document when finished.";
    return instructions;
}

void add_cache_fields(Json &result) {
    result["ttlMs"] = 3600000;
    result["cacheScope"] = "public";
}

struct ReaderEvent {
    enum class Kind { LINE, OVERSIZE, ERROR, EOF };

    Kind kind = Kind::LINE;
    std::string data;
};

struct CallState {
    Json id;
    std::shared_ptr<asio::cancellation_signal> cancel;
    bool modern = false;
    bool cancel_requested = false;
    bool response_started = false;
    bool live_export = false;
    std::shared_ptr<RequestExecution> execution = std::make_shared<RequestExecution>();
    bool tool_finished = false;
    std::shared_ptr<asio::steady_timer> watchdog;
};

using CallStatePtr = std::shared_ptr<CallState>;

struct OutputMessage {
    std::string wire;
};

using OutputWaiter = dyn::SmallBox<dyn::FnOnce<void() noexcept>, 32>;

struct ArmedWaiter {
    OutputWaiter waiter;
    bool invoked = false;

    explicit ArmedWaiter(OutputWaiter w) noexcept : waiter(std::move(w)) {}

    ArmedWaiter(ArmedWaiter &&other) noexcept : waiter(std::move(other.waiter)), invoked(std::exchange(other.invoked, true)) {}

    ArmedWaiter &operator=(ArmedWaiter &&) = delete;

    ~ArmedWaiter() {
        if (!invoked && waiter.has_value()) (*std::move(waiter))();
    }

    void operator()() noexcept {
        invoked = true;
        if (waiter.has_value()) (*std::move(waiter))();
    }
};

bool is_operation_aborted_code(const std::system_error &error) noexcept {
    return error.code().value() == static_cast<int>(asio::error::operation_aborted);
}

bool is_operation_aborted(const std::exception_ptr &error) noexcept {
    if (!error) {
        return false;
    }
    try {
        std::rethrow_exception(error);
    } catch (const std::system_error &e) {
        return is_operation_aborted_code(e);
    } catch (...) {
        return false;
    }
}

struct Session : std::enable_shared_from_this<Session> {
    Session(asio::io_context &ioc, UniqueFn<Json()> list_tools, UniqueFn<bool(std::string_view)> tool_exists, AsyncToolCall call_tool,
            UniqueFn<void()> shutdown)
        : ioc_(ioc), list_tools_(std::move(list_tools)), tool_exists_(std::move(tool_exists)), call_tool_(std::move(call_tool)),
          shutdown_(std::move(shutdown)), work_(asio::make_work_guard(ioc)) {
        inflight_.reserve(k_max_inflight_calls);
#if !defined(_WIN32)
        if (::pipe2(reader_wake_, O_CLOEXEC | O_NONBLOCK) != 0) {
            throw std::system_error(errno, std::generic_category(), "stdin wake pipe");
        }
#endif
    }

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    ~Session() {
        stop_threads();
#if defined(_WIN32)
        close_os_handles();
#else
        ::close(reader_wake_[0]);
        ::close(reader_wake_[1]);
#endif
    }

    void start_threads() {
        auto self = shared_from_this();
        writer_thread_ = std::jthread([self](std::stop_token) { self->writer_main(); });
        reader_thread_ = std::jthread([self](std::stop_token stop) { self->reader_main(stop); });
    }

    void stop_threads() noexcept {
        request_reader_stop();
        request_writer_abort();
        if (reader_thread_.joinable()) {
            reader_thread_.request_stop();
#if defined(_WIN32)
            // CancelSynchronousIo can race the reader entering ReadFile. Retry
            // only during teardown until the thread exits; normal reads block
            // without timers or polling. The thread handle stays valid to join.
            HANDLE thread = reader_thread_.native_handle();
            while (::WaitForSingleObject(thread, 1) == WAIT_TIMEOUT) {
                ::CancelSynchronousIo(thread);
            }
#endif
            reader_thread_.join();
        }
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
    }

    void finalize() noexcept {
        if (!shutdown_called_) {
            shutdown_once();
        }
        work_.reset();
    }

    int exit_code() const noexcept { return failed_.load(std::memory_order_relaxed) ? 1 : 0; }

    // Called on the session/io thread after io_context::run() itself threw.
    void fatal_from_owner(const char *reason) noexcept {
        fatal_shutdown(reason != nullptr ? std::string(reason) : std::string("io_context handler failure"));
    }

private:
    void reader_main(std::stop_token stop) noexcept {
#if defined(_WIN32)
        HANDLE handle = ::OpenThread(THREAD_TERMINATE, FALSE, ::GetCurrentThreadId());
        if (handle != nullptr) {
            reader_handle_.store(handle, std::memory_order_release);
        }
#endif
        try {
            reader_loop(stop);
        } catch (...) {
            stop_reader_.store(true, std::memory_order_release);
            queue_cv_.notify_all();
            post_reader_failure("stdin reader exception");
        }
    }

    void request_reader_stop() noexcept {
        {
            // Pair the state change with the mutex so a reader already inside
            // condition_variable::wait cannot miss the wake-up.
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_reader_.store(true, std::memory_order_release);
        }
        queue_cv_.notify_all();
#if defined(_WIN32)
        HANDLE handle = reader_handle_.load(std::memory_order_acquire);
        if (handle != nullptr) {
            ::CancelSynchronousIo(handle);
        }
#else
        const char wake = 1;
        while (::write(reader_wake_[1], &wake, 1) < 0 && errno == EINTR) {}
#endif
    }

    void reader_loop(std::stop_token stop) {
        std::string current;
        current.reserve(4096);
        bool oversized = false;
        bool saw_eof = false;
        bool saw_error = false;
        std::string read_error;
        char buffer[65536];

        for (;;) {
            if (stop.stop_requested() || stop_reader_.load(std::memory_order_acquire)) {
                return;
            }

            const int n = read_chunk(buffer, sizeof(buffer), stop, read_error);
            if (n == k_read_stopped) {
                return;
            }
            if (n < 0) {
                saw_error = true;
                break;
            }
            if (n == 0) {
                saw_eof = true;
                break;
            }
            if (!consume_bytes(buffer, static_cast<std::size_t>(n), current, oversized)) {
                return;
            }
        }

        if (saw_eof) {
            if (oversized) {
                ReaderEvent event;
                event.kind = ReaderEvent::Kind::OVERSIZE;
                if (!enqueue(std::move(event))) {
                    return;
                }
            } else if (!current.empty()) {
                std::string line;
                line.swap(current);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                ReaderEvent event;
                event.kind = ReaderEvent::Kind::LINE;
                event.data = std::move(line);
                if (!enqueue(std::move(event))) {
                    return;
                }
            }

            ReaderEvent eof;
            eof.kind = ReaderEvent::Kind::EOF;
            std::ignore = enqueue(std::move(eof));
        } else if (saw_error) {
            ReaderEvent event;
            event.kind = ReaderEvent::Kind::ERROR;
            event.data = read_error.empty() ? std::string("unknown stdin read error") : read_error;
            std::ignore = enqueue(std::move(event));
        }
    }

    bool consume_bytes(const char *data, std::size_t size, std::string &current, bool &oversized) {
        std::size_t pos = 0;
        while (pos < size) {
            if (oversized) {
                const void *newline = std::memchr(data + pos, '\n', size - pos);
                if (newline == nullptr) {
                    return true;
                }
                const std::size_t newline_pos = static_cast<std::size_t>(static_cast<const char *>(newline) - data);

                ReaderEvent event;
                event.kind = ReaderEvent::Kind::OVERSIZE;
                if (!enqueue(std::move(event))) {
                    return false;
                }

                oversized = false;
                current.clear();
                pos = newline_pos + 1;
                continue;
            }

            const void *newline = std::memchr(data + pos, '\n', size - pos);
            const std::size_t segment_end = newline ? static_cast<std::size_t>(static_cast<const char *>(newline) - data) : size;
            const std::size_t segment_size = segment_end - pos;
            const std::size_t room = k_max_line_bytes - current.size();

            if (segment_size > room) {
                if (room > 0) {
                    current.append(data + pos, room);
                }
                oversized = true;
                current.clear();
                pos += room;
                continue;
            }

            if (segment_size > 0) {
                current.append(data + pos, segment_size);
            }
            pos = segment_end;

            if (newline != nullptr) {
                std::string line;
                line.swap(current);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }

                ReaderEvent event;
                event.kind = ReaderEvent::Kind::LINE;
                event.data = std::move(line);
                if (!enqueue(std::move(event))) {
                    return false;
                }

                current.reserve(4096);
                ++pos;
            }
        }
        return true;
    }

    int read_chunk(char *buffer, std::size_t capacity, const std::stop_token &stop, std::string &error) {
#if defined(_WIN32)
        const auto raw = _get_osfhandle(_fileno(stdin));
        if (raw == -1) {
            error = "invalid stdin handle";
            return k_read_error;
        }
        if (stop.stop_requested() || stop_reader_.load(std::memory_order_acquire)) {
            return k_read_stopped;
        }
        DWORD count = 0;
        if (::ReadFile(reinterpret_cast<HANDLE>(raw), buffer, static_cast<DWORD>(capacity), &count, nullptr)) {
            return static_cast<int>(count);
        }
        const DWORD code = ::GetLastError();
        if (stop.stop_requested() || stop_reader_.load(std::memory_order_acquire)) {
            return k_read_stopped;
        }
        if (code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED || code == ERROR_HANDLE_EOF) {
            return k_read_eof;
        }
        error = "stdin ReadFile failed with error " + std::to_string(code);
        return k_read_error;
#else
        for (;;) {
            if (stop.stop_requested() || stop_reader_.load(std::memory_order_acquire)) {
                return k_read_stopped;
            }

            struct pollfd descriptors[2] = {{STDIN_FILENO, POLLIN, 0}, {reader_wake_[0], POLLIN, 0}};
            const int ready = ::poll(descriptors, 2, -1);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                error = "stdin poll failed: " + std::error_code(errno, std::generic_category()).message();
                return k_read_error;
            }
            if (descriptors[1].revents != 0) {
                return k_read_stopped;
            }
            if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) {
                error = "stdin poll reported an error";
                return k_read_error;
            }
            if ((descriptors[0].revents & (POLLIN | POLLHUP)) == 0) {
                continue;
            }

            const ssize_t n = ::read(STDIN_FILENO, buffer, capacity);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                error = "stdin read failed: " + std::error_code(errno, std::generic_category()).message();
                return k_read_error;
            }
            if (n == 0) {
                return k_read_eof;
            }
            return static_cast<int>(n);
        }
#endif
    }

    bool enqueue(ReaderEvent event) noexcept {
        bool schedule = false;
        try {
            const std::size_t line_bytes = event.kind == ReaderEvent::Kind::LINE ? event.data.size() : 0;

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [&] {
                    if (stop_reader_.load(std::memory_order_acquire) || fatal_.load(std::memory_order_acquire)) {
                        return true;
                    }
                    if (queue_.size() >= k_max_pending_lines) {
                        return false;
                    }
                    if (line_bytes != 0 && queued_bytes_ + line_bytes > k_max_pending_bytes) {
                        return false;
                    }
                    return true;
                });

                if (stop_reader_.load(std::memory_order_acquire) || fatal_.load(std::memory_order_acquire)) {
                    return false;
                }

                queue_.push_back(std::move(event));
                queued_bytes_ += line_bytes;
                if (!drain_scheduled_) {
                    drain_scheduled_ = true;
                    schedule = true;
                }
            }

            if (schedule) {
                auto self = shared_from_this();
                asio::post(ioc_.get_executor(), [self]() { self->start_drain(); });
            }
            return true;
        } catch (...) {
            stop_reader_.store(true, std::memory_order_release);
            queue_cv_.notify_all();
            post_reader_failure("stdin reader queue failure");
            return false;
        }
    }

    void post_reader_failure(const char *reason) noexcept {
        try {
            auto self = shared_from_this();
            const std::string text = reason != nullptr ? std::string(reason) : std::string("stdin reader failure");
            asio::post(ioc_.get_executor(), [self, text]() { self->fatal_shutdown(text); });
        } catch (...) {}
    }

    void writer_main() noexcept {
#if defined(_WIN32)
        HANDLE handle = ::OpenThread(THREAD_TERMINATE, FALSE, ::GetCurrentThreadId());
        if (handle != nullptr) {
            writer_handle_.store(handle, std::memory_order_release);
        }
#else
        original_stdout_flags_ = ::fcntl(STDOUT_FILENO, F_GETFL, 0);
        if (original_stdout_flags_ >= 0) {
            std::ignore = ::fcntl(STDOUT_FILENO, F_SETFL, original_stdout_flags_ | O_NONBLOCK);
        }
#endif
        try {
            writer_loop();
        } catch (...) {
            post_writer_failure("stdout writer exception");
        }
#if !defined(_WIN32)
        if (original_stdout_flags_ >= 0) {
            std::ignore = ::fcntl(STDOUT_FILENO, F_SETFL, original_stdout_flags_);
        }
#endif
        post_writer_exited();
    }

    void request_writer_abort() noexcept {
        writer_abort_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(out_mutex_);
            out_abort_ = true;
        }
        out_cv_.notify_all();
#if defined(_WIN32)
        HANDLE handle = writer_handle_.load(std::memory_order_acquire);
        if (handle != nullptr) {
            ::CancelSynchronousIo(handle);
        }
#endif
    }

    // Tracks an in-flight stdout byte write under stdout_mutex without holding
    // state_mutex across the OS write call.
    struct StdoutWriteScope {
        bool active = false;
        bool failed = false;

        bool enter() noexcept {
            std::lock_guard lock(publication_gate.state_mutex);
            if (publication_gate.closing.load(std::memory_order_acquire)) return false;
            ++publication_gate.active_stdout_writes;
            active = true;
            return true;
        }

        ~StdoutWriteScope() noexcept {
            if (!active) return;
            {
                std::lock_guard lock(publication_gate.state_mutex);
                --publication_gate.active_stdout_writes;
                if (failed) publication_gate.closing.store(true, std::memory_order_release);
            }
            publication_gate.state_cv.notify_all();
        }
    };

    void writer_loop() {
        for (;;) {
            OutputMessage output;
            {
                std::unique_lock<std::mutex> lock(out_mutex_);
                out_cv_.wait(lock, [&] {
                    return writer_abort_.load(std::memory_order_acquire) || out_abort_ || !out_messages_.empty() || out_graceful_stop_;
                });

                if (writer_abort_.load(std::memory_order_acquire) || out_abort_) {
                    return;
                }
                if (out_messages_.empty()) {
                    if (out_graceful_stop_) {
                        return;
                    }
                    continue;
                }

                output = std::move(out_messages_.front());
                out_messages_.pop_front();
            }

            const auto &message = output.wire;
            bool written = false;
            {
                std::lock_guard stdout_lock(publication_gate.stdout_mutex);
                StdoutWriteScope write_scope;
                if (!write_scope.enter()) return;
                written = write_report_bytes(message);
                if (!written) write_scope.failed = true;
            }

            {
                std::lock_guard<std::mutex> lock(out_mutex_);
                if (out_budget_used_ >= message.size()) {
                    out_budget_used_ -= message.size();
                } else {
                    out_budget_used_ = 0;
                }
                if (out_message_slots_ > 0) {
                    --out_message_slots_;
                }
            }

            if (!written) {
                if (!writer_abort_.load(std::memory_order_acquire)) {
                    post_writer_failure("stdout write failed");
                }
                return;
            }

            schedule_output_progress();
        }
    }

    bool write_report_bytes(const std::string &message) {
        if (test_hooks_enabled()) {
            if (const auto *barrier = std::getenv("ENGRAVE_MCP_TEST_WRITE_BARRIER")) {
                const std::filesystem::path arm = std::string(barrier) + ".arm";
                if (std::filesystem::exists(arm)) {
                    std::filesystem::remove(arm);
                    std::fprintf(stderr, "WRITE_REGISTERED\n");
                    std::fflush(stderr);
                    while (!std::filesystem::exists(barrier)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
        return write_all(message.data(), message.size());
    }

    // Called synchronously on the owner or audio lane while a PublicationScope
    // holds active_publications. Serializes JSON unlocked, then acquires only
    // stdout_mutex for the raw write. On terminal failure, fences closing and
    // returns the reason so the caller can release its publication token and
    // task count before calling post_writer_failure(reason).
    const char *publish_report(const Json &id, Json payload, bool modern) noexcept {
        auto fence_failure = [this](const char *reason) noexcept {
            publication_gate.mark_closing();
            {
                std::lock_guard output_lock(out_mutex_);
                out_abort_ = true;
                out_messages_.clear();
            }
            out_cv_.notify_all();
            return reason;
        };
        try {
            if (payload.is_null()) return fence_failure("published report unavailable");
            Json result;
            try {
                result = make_tool_result(payload, true);
            } catch (...) {
                Json paths = make_array<Json>();
                if (payload.contains("artifacts")) {
                    for (const auto &a : payload.at("artifacts").get_array()) json_push(paths, a.at("path"));
                } else {
                    paths = json_value<Json>(payload.at("error"), "published_artifacts", make_array<Json>());
                }
                result = make_tool_result(Json{{"ok", false},
                                               {"error",
                                                {{"code", "EngineFailure"},
                                                 {"message", "Completed export wrapper failed"},
                                                 {"published_artifacts", std::move(paths)}}}});
            }
            if (modern) add_modern_result_fields(result);
            auto wire = dump_json(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
            wire.push_back('\n');
            if (wire.size() > k_max_response_bytes) return fence_failure("published report exceeds limit");
            bool written = false;
            {
                std::lock_guard stdout_lock(publication_gate.stdout_mutex);
                StdoutWriteScope write_scope;
                if (write_scope.enter()) {
                    written = write_report_bytes(wire);
                    if (!written) write_scope.failed = true;
                }
            }
            if (!written) return fence_failure("stdout write failed");
            return nullptr;
        } catch (...) {
            return fence_failure("cannot construct published report");
        }
    }

    bool write_all(const char *data, std::size_t size) noexcept {
        std::size_t offset = 0;
#if defined(_WIN32)
        const auto raw = _get_osfhandle(_fileno(stdout));
        if (raw < 0) {
            return false;
        }
        HANDLE handle = reinterpret_cast<HANDLE>(raw);
        while (offset < size) {
            if (writer_abort_.load(std::memory_order_acquire)) {
                return false;
            }
            const std::size_t remaining = size - offset;
            const DWORD chunk = static_cast<DWORD>(remaining < 0x7fffffffu ? remaining : 0x7fffffffu);
            DWORD written = 0;
            if (!::WriteFile(handle, data + offset, chunk, &written, nullptr)) {
                // Broken pipe, closed reader and cancellation are all fatal;
                // the owner decides how to unwind pending work.
                return false;
            }
            if (written == 0) {
                return false;
            }
            offset += written;
        }
        return true;
#else
        while (offset < size) {
            if (writer_abort_.load(std::memory_order_acquire)) {
                return false;
            }
            const std::size_t remaining = size - offset;
            const ssize_t n = ::write(STDOUT_FILENO, data + offset, remaining);
            if (n > 0) {
                offset += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    struct pollfd descriptor;
                    descriptor.fd = STDOUT_FILENO;
                    descriptor.events = static_cast<short>(POLLOUT);
                    descriptor.revents = 0;
                    const int ready = ::poll(&descriptor, 1, 5);
                    if (ready < 0 && errno == EINTR) {
                        continue;
                    }
                    if (ready < 0) {
                        return false;
                    }
                    continue;
                }
                return false;
            }
            return false;
        }
        return true;
#endif
    }

    void schedule_output_progress() noexcept {
        if (output_progress_pending_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            auto self = shared_from_this();
            asio::post(ioc_.get_executor(), [self]() {
                self->output_progress_pending_.store(false, std::memory_order_release);
                self->resume_output_waiters();
                self->maybe_finish_graceful();
            });
        } catch (...) {
            output_progress_pending_.store(false, std::memory_order_release);
        }
    }

    [[noreturn]] void post_writer_failure(const char *reason) noexcept {
        {
            std::unique_lock lock(publication_gate.state_mutex);
            publication_gate.closing.store(true, std::memory_order_release); // fences both admission and queued owner starts
            const bool primary = !publication_gate.exit74_claimed;
            if (primary) publication_gate.exit74_claimed = true;
            lock.unlock();
            publication_gate.state_cv.notify_all();
            if (!primary) {
                lock.lock();
                publication_gate.state_cv.wait(lock, [] { return false; });
            }
        }
        if (test_hooks_enabled()) {
            if (const auto *barrier = std::getenv("ENGRAVE_MCP_TEST_CLOSING_BARRIER")) {
                std::fprintf(stderr, "CLOSING_REGISTERED\n");
                std::fflush(stderr);
                while (!std::filesystem::exists(barrier)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
        {
            std::lock_guard output_lock(out_mutex_);
            out_abort_ = true;
            out_messages_.clear();
        }
        out_cv_.notify_all();
        // Active owner and audio-lane tasks clean up their staging; active exports
        // also cancel on `closing`. Never wait indefinitely for engine/filesystem
        // work; exit also reports possibly unfinished cleanup.
        bool cleaned = false;
        {
            std::unique_lock lock(publication_gate.state_mutex);
            cleaned =
                publication_gate.state_cv.wait_for(lock, std::chrono::milliseconds(500), [] { return publication_gate.active_tasks == 0; });
        }
        try {
            terminal_diagnostic(std::string("mcp transport terminal: ") + reason +
                                "; exit 74 after publication cleanup; cleanup failures=" +
                                std::to_string(publication_gate.cleanup_failures.load(std::memory_order_acquire)) +
                                (cleaned ? "; owners cleaned; directories may remain\n" : "; cleanup timed out; staging may remain\n"));
        } catch (...) {}
        std::_Exit(74);
    }

    void post_writer_exited() noexcept {
        try {
            auto self = shared_from_this();
            asio::post(ioc_.get_executor(), [self]() {
                self->resume_output_waiters();
                // No response producer can legitimately still be waiting:
                // graceful stop is only requested with no in-flight calls,
                // and fatal stop aborts all waiters. The guard is released
                // only here, after the writer thread can no longer post.
                self->work_.reset();
            });
        } catch (...) {}
    }

    bool output_closed() noexcept {
        std::lock_guard<std::mutex> lock(out_mutex_);
        return out_abort_ || out_graceful_stop_ || writer_abort_.load(std::memory_order_acquire);
    }

    bool try_reserve_output(std::size_t bytes) noexcept {
        std::lock_guard<std::mutex> lock(out_mutex_);
        if (out_abort_ || out_graceful_stop_ || writer_abort_.load(std::memory_order_acquire)) {
            return false;
        }
        if (out_message_slots_ >= k_max_output_messages) {
            return false;
        }
        if (out_budget_used_ + bytes > k_output_budget_bytes) {
            return false;
        }
        out_budget_used_ += bytes;
        ++out_message_slots_;
        return true;
    }

    void release_output_reservation(std::size_t reserved) noexcept {
        {
            std::lock_guard<std::mutex> lock(out_mutex_);
            if (out_budget_used_ >= reserved) {
                out_budget_used_ -= reserved;
            } else {
                out_budget_used_ = 0;
            }
            if (out_message_slots_ > 0) {
                --out_message_slots_;
            }
        }
        resume_output_waiters();
    }

    bool commit_output(std::size_t reserved, std::string &message) noexcept {
        const std::size_t actual = message.size();
        try {
            std::lock_guard<std::mutex> lock(out_mutex_);
            if (out_abort_ || writer_abort_.load(std::memory_order_acquire)) {
                if (out_budget_used_ >= reserved) {
                    out_budget_used_ -= reserved;
                } else {
                    out_budget_used_ = 0;
                }
                if (out_message_slots_ > 0) {
                    --out_message_slots_;
                }
                return false;
            }
            try {
                out_messages_.push_back({std::move(message)});
            } catch (...) {
                if (out_budget_used_ >= reserved) {
                    out_budget_used_ -= reserved;
                } else {
                    out_budget_used_ = 0;
                }
                if (out_message_slots_ > 0) {
                    --out_message_slots_;
                }
                return false;
            }
            if (out_budget_used_ >= reserved) {
                out_budget_used_ -= reserved;
            } else {
                out_budget_used_ = 0;
            }
            out_budget_used_ += actual;
        } catch (...) {
            return false;
        }
        out_cv_.notify_one();
        return true;
    }

    asio::awaitable<void> wait_for_output_room() {
        if (fatal_.load(std::memory_order_acquire)) {
            co_return;
        }
        auto token = asio::use_awaitable;
        co_await asio::async_initiate<decltype(token), void()>(
            [this](auto handler) {
                output_waiters_.push_back(OutputWaiter::from([h = std::move(handler)]() mutable noexcept {
                    try {
                        auto fn = std::move(h);
                        fn();
                    } catch (...) {}
                }));
            },
            token);
    }

    void resume_output_waiters() noexcept {
        while (!output_waiters_.empty()) {
            ArmedWaiter armed(std::move(output_waiters_.front()));
            output_waiters_.pop_front();
            try {
                asio::post(ioc_.get_executor(), std::move(armed));
            } catch (...) {
                // ArmedWaiter's destructor resumes the suspended coroutine inline if post threw before moving.
            }
        }
    }

    asio::awaitable<WireStatus> send_wire(const Json &value) {
        if (fatal_.load(std::memory_order_acquire)) {
            co_return WireStatus::FAILED;
        }

        for (;;) {
            if (fatal_.load(std::memory_order_acquire)) {
                co_return WireStatus::FAILED;
            }
            if (try_reserve_output(k_max_response_bytes)) {
                break;
            }
            if (output_closed()) {
                co_return WireStatus::FAILED;
            }
            co_await wait_for_output_room();
        }

        std::string wire;
        try {
            wire = dump_json(value);
            wire.push_back('\n');
        } catch (...) {
            release_output_reservation(k_max_response_bytes);
            fatal_shutdown("cannot serialise MCP response");
            co_return WireStatus::FAILED;
        }

        if (wire.size() > k_max_response_bytes) {
            release_output_reservation(k_max_response_bytes);
            co_return WireStatus::TOO_LARGE;
        }

        if (!commit_output(k_max_response_bytes, wire)) {
            fatal_shutdown("cannot queue MCP response");
            co_return WireStatus::FAILED;
        }
        co_return WireStatus::OK;
    }

    asio::awaitable<WireStatus> send_result_status(const Json &id, Json result, bool modern = false) {
        try {
            if (modern) add_modern_result_fields(result);
            Json response = make_object<Json>();
            response["jsonrpc"] = "2.0";
            response["id"] = id;
            response["result"] = std::move(result);
            co_return co_await send_wire(response);
        } catch (...) {
            fatal_shutdown("cannot build MCP result response");
            co_return WireStatus::FAILED;
        }
    }

    asio::awaitable<bool> send_error(const Json &id, int code, const std::string &message, Json data = nullptr) {
        try {
            Json error = make_object<Json>();
            error["jsonrpc"] = "2.0";
            error["id"] = is_valid_id(id) ? id : Json(nullptr);
            error["error"] = Json{{"code", code}, {"message", message}};
            if (!data.is_null()) error["error"]["data"] = std::move(data);
            const WireStatus status = co_await send_wire(error);
            if (status == WireStatus::TOO_LARGE) {
                fatal_shutdown("MCP error response exceeded the 16 MiB limit");
            }
            co_return status == WireStatus::OK;
        } catch (...) {
            fatal_shutdown("cannot build MCP error response");
            co_return false;
        }
    }

    // Generic protocol results: if the response is over budget, replace it
    // with a bounded protocol resource error rather than silently dropping it.
    asio::awaitable<bool> send_result(const Json &id, Json result) {
        const WireStatus status = co_await send_result_status(id, std::move(result));
        if (status == WireStatus::OK) {
            co_return true;
        }
        if (status == WireStatus::TOO_LARGE && !fatal_.load(std::memory_order_acquire)) {
            co_return co_await send_error(id, -32603, "Internal error: response exceeds the 16 MiB transport limit");
        }
        co_return false;
    }

    void start_drain() noexcept {
        if (fatal_.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            drain_scheduled_ = false;
            return;
        }
        try {
            auto self = shared_from_this();
            asio::co_spawn(ioc_.get_executor(), [self]() -> asio::awaitable<void> { co_await self->drain_loop(); }, asio::detached);
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                drain_scheduled_ = false;
            }
            fatal_shutdown("cannot start MCP input dispatcher");
        }
    }

    asio::awaitable<void> drain_loop() {
        try {
            std::size_t processed = 0;
            for (;;) {
                ReaderEvent event;
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    if (fatal_.load(std::memory_order_acquire) || queue_.empty()) {
                        drain_scheduled_ = false;
                        break;
                    }

                    event = std::move(queue_.front());
                    queue_.pop_front();
                    if (event.kind == ReaderEvent::Kind::LINE) {
                        queued_bytes_ -= event.data.size();
                    }
                }
                queue_cv_.notify_all();

                if (!co_await process_event(event)) {
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex_);
                        drain_scheduled_ = false;
                    }
                    break;
                }

                if (++processed >= k_drain_batch) {
                    processed = 0;
                    // Yield so queued owner completions and timers run even
                    // while input is continuously available.
                    co_await asio::post(ioc_.get_executor(), asio::use_awaitable);
                }
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                drain_scheduled_ = false;
            }
            fatal_shutdown("MCP input dispatch failed");
        }

        maybe_finish_graceful();
    }

    asio::awaitable<bool> process_event(ReaderEvent &event) {
        switch (event.kind) {
            case ReaderEvent::Kind::LINE: co_return co_await handle_line(event.data);

            case ReaderEvent::Kind::OVERSIZE: co_return co_await send_error(nullptr, -32700, "Parse error: line exceeds 4 MiB limit");

            case ReaderEvent::Kind::ERROR:
                std::fprintf(stderr, "stdin read error: %s\n", event.data.c_str());
                std::fflush(stderr);
                fatal_shutdown("stdin read error: " + event.data);
                co_return false;

            case ReaderEvent::Kind::EOF:
                eof_ = true;
                // This cancels queued engine work, but running committed
                // operations still complete and their responses are drained.
                shutdown_once();
                co_return false;
        }
        co_return false;
    }

    asio::awaitable<bool> handle_line(const std::string &line) {
        bool only_ws = true;
        for (unsigned char ch : line) {
            if (!std::isspace(ch)) {
                only_ws = false;
                break;
            }
        }
        if (only_ws) {
            co_return true;
        }
        co_return co_await process_line(line);
    }

    asio::awaitable<bool> process_line(const std::string &line) {
        // The protocol caps nesting at 64 levels; glaze's own parser limit is
        // higher, so enforce the wire contract before parsing.
        if (json_nesting_exceeds(line, 64)) {
            co_return co_await send_error(nullptr, -32600, "Invalid Request: JSON nesting exceeds 64 levels");
        }
        auto parsed = parse_json<Json>(line);
        if (!parsed) {
            co_return co_await send_error(nullptr, -32700, "Parse error");
        }
        Json req = std::move(*parsed);

        if (req.is_array()) {
            co_return co_await send_error(nullptr, -32600, "Batch requests are not supported");
        }
        if (!req.is_object()) {
            co_return co_await send_error(nullptr, -32600, "Invalid Request");
        }

        // Deferred protocol error, delivered after the try/catch because a
        // coroutine handler must not suspend.
        int protocol_error_code = 0;
        std::string protocol_error_message;
        Json protocol_error_id = nullptr;
        try {
            if (!req.contains("jsonrpc") || !req["jsonrpc"].is_string() || req["jsonrpc"].get<std::string>() != "2.0") {
                Json fallback_id = nullptr;
                if (req.contains("id") && is_valid_id(req["id"])) {
                    fallback_id = req["id"];
                }
                co_return co_await send_error(fallback_id, -32600, "Invalid Request: invalid or missing jsonrpc version");
            }

            if (!req.contains("method") || !req["method"].is_string()) {
                Json fallback_id = nullptr;
                if (req.contains("id") && is_valid_id(req["id"])) {
                    fallback_id = req["id"];
                }
                co_return co_await send_error(fallback_id, -32600, "Invalid Request: method must be a string");
            }

            std::string method = req["method"].get<std::string>();

            if (!req.contains("id")) {
                if (method == "notifications/initialized") {
                    if (state_ == ServerState::INITIALIZED_PENDING) {
                        state_ = ServerState::READY;
                    }
                } else if (method == "notifications/cancelled") {
                    handle_cancelled(req);
                }
                co_return true;
            }

            const Json &id = req["id"];
            if (!is_valid_id(id)) {
                co_return co_await send_error(nullptr, -32600, "Invalid Request: id must be a string or integer");
            }

            for (const auto &call : inflight_) {
                if (call->id == id) {
                    // The original request still owns this id. Returning the
                    // duplicate with a null id avoids two responses that
                    // claim the same request id.
                    co_return co_await send_error(nullptr, -32600, "Invalid Request: request id is already in flight");
                }
            }

            Json params = make_object<Json>();
            if (req.contains("params")) {
                if (!req["params"].is_object()) {
                    co_return co_await send_error(id, -32602, "Invalid params: must be an object");
                }
                params = req["params"];
            }

            if (params.contains("_meta") && !params.at("_meta").is_object()) {
                co_return co_await send_error(id, -32602, "Invalid params: _meta must be an object");
            }
            const bool has_meta = params.contains("_meta");
            const bool modern =
                method == "server/discover" || (method != "initialize" && has_meta && has_modern_meta_key(params.at("_meta")));
            if (modern) {
                if (!has_meta) {
                    co_return co_await send_error(id, -32602, "Invalid params: _meta object required");
                }
                const auto &meta = params.at("_meta");
                if (!meta.contains(k_protocol_version_key) || !meta.at(k_protocol_version_key).is_string()) {
                    co_return co_await send_error(id, -32602, "Invalid params: _meta protocol version string required");
                }
                const auto &requested = meta.at(k_protocol_version_key).get<std::string>();
                if (requested != k_modern_version) {
                    Json data = {{"supported", supported_versions()}, {"requested", requested}};
                    co_return co_await send_error(id, -32022, "Unsupported protocol version", std::move(data));
                }
                if (!meta.contains(k_client_capabilities_key) || !meta.at(k_client_capabilities_key).is_object()) {
                    co_return co_await send_error(id, -32602, "Invalid params: _meta client capabilities object required");
                }
                if (meta.contains(k_client_info_key) && !valid_client_info(meta.at(k_client_info_key))) {
                    co_return co_await send_error(id, -32602, "Invalid params: _meta client info requires name and version strings");
                }
                if (meta.contains(k_log_level_key) && !valid_log_level(meta.at(k_log_level_key))) {
                    co_return co_await send_error(id, -32602, "Invalid params: _meta logLevel must be a valid logging level");
                }
            }

            if (method == "server/discover") {
                Json result = {{"supportedVersions", supported_versions()},
                               {"capabilities", {{"tools", make_object<Json>()}}},
                               {"instructions", server_instructions()}};
                add_modern_result_fields(result);
                add_cache_fields(result);
                co_return co_await send_result(id, std::move(result));
            }

            if (method == "initialize") {
                if (state_ != ServerState::UNINITIALIZED) {
                    co_return co_await send_error(id, -32600, "Server already initialized");
                }
                if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string()) {
                    co_return co_await send_error(id, -32602, "Invalid params: protocolVersion string required");
                }
                if (!params.contains("capabilities") || !params["capabilities"].is_object()) {
                    co_return co_await send_error(id, -32602, "Invalid params: capabilities object required");
                }
                if (!params.contains("clientInfo") || !params["clientInfo"].is_object()) {
                    co_return co_await send_error(id, -32602, "Invalid params: clientInfo object required");
                }
                if (!valid_client_info(params.at("clientInfo"))) {
                    co_return co_await send_error(id, -32602, "Invalid params: clientInfo name and version strings required");
                }

                std::string_view version = params.at("protocolVersion").get<std::string>();
                if (version != k_oldest_legacy_version) version = k_latest_legacy_version;
                Json result = {{"protocolVersion", version},
                               {"capabilities", {{"tools", make_object<Json>()}}},
                               {"serverInfo", server_info()},
                               {"instructions", server_instructions()}};
                if (!co_await send_result(id, std::move(result))) {
                    co_return false;
                }
                state_ = ServerState::INITIALIZED_PENDING;
                co_return true;
            }

            if (method == "ping") {
                Json result = make_object<Json>();
                if (modern) add_modern_result_fields(result);
                co_return co_await send_result(id, std::move(result));
            }

            if (method == "tools/list") {
                if (!modern && state_ != ServerState::READY) {
                    co_return co_await send_error(id, -32600, "Tools available only after initialized notification");
                }

                Json tools_raw;
                bool list_tools_failed = false;
                std::string list_tools_error;
                try {
                    tools_raw = list_tools_();
                } catch (const std::exception &e) {
                    list_tools_failed = true;
                    list_tools_error = std::string("Tool list callback error: ") + e.what();
                } catch (...) {
                    list_tools_failed = true;
                    list_tools_error = "Unknown tool list callback error";
                }
                if (list_tools_failed) {
                    co_return co_await send_error(id, -32603, list_tools_error);
                }

                Json tools_list = make_array<Json>();
                if (tools_raw.is_array()) {
                    tools_list = tools_raw;
                } else if (tools_raw.is_object() && tools_raw.contains("tools") && tools_raw["tools"].is_array()) {
                    tools_list = tools_raw["tools"];
                }

                Json result = {{"tools", std::move(tools_list)}};
                if (modern) {
                    add_modern_result_fields(result);
                    add_cache_fields(result);
                }
                co_return co_await send_result(id, std::move(result));
            }

            if (method == "tools/call") {
                if (!modern && state_ != ServerState::READY) {
                    co_return co_await send_error(id, -32600, "Tools available only after initialized notification");
                }
                if (!params.contains("name") || !params["name"].is_string()) {
                    co_return co_await send_error(id, -32602, "Invalid params: tool name required and must be a string");
                }

                std::string tool_name = params["name"].get<std::string>();
                Json arguments = make_object<Json>();
                if (params.contains("arguments")) {
                    if (!params["arguments"].is_object()) {
                        co_return co_await send_error(id, -32602, "Invalid params: arguments must be an object");
                    }
                    arguments = params["arguments"];
                }

                if (inflight_.size() >= k_max_inflight_calls) {
                    co_return co_await send_error(id, -32000, "Server overload: 64 outstanding tool calls already in flight");
                }

                if (!tool_exists_(tool_name)) {
                    co_return co_await send_error(id, -32602, "Unknown tool: " + tool_name);
                }

                co_return co_await start_tool_call(id, std::move(tool_name), std::move(arguments), modern);
            }

            co_return co_await send_error(id, -32601, "Method not found: " + method);
        } catch (const std::bad_variant_access &e) {
            protocol_error_code = -32600;
            protocol_error_message = std::string("Malformed request: ") + e.what();
            if (req.is_object() && req.contains("id") && is_valid_id(req["id"])) {
                protocol_error_id = req["id"];
            }
        } catch (...) {
            protocol_error_code = -32603;
            protocol_error_message = "Internal error";
            if (req.is_object() && req.contains("id") && is_valid_id(req["id"])) {
                protocol_error_id = req["id"];
            }
        }

        co_return co_await send_error(protocol_error_id, protocol_error_code, protocol_error_message);
    }

    void handle_cancelled(const Json &req) noexcept {
        try {
            if (!req.contains("params") || !req["params"].is_object()) {
                return;
            }
            const Json &params = req["params"];
            if (!params.contains("requestId") || !is_valid_id(params["requestId"])) {
                return;
            }

            // Terminal cancellation is cooperative: it only takes effect
            // before the tool callback yields its result. Once run_tool_call
            // has a payload it disables incoming cancellation, so a late
            // cancellation can neither discard the payload nor replace it
            // with an error.
            const Json &request_id = params["requestId"];
            CallStatePtr target;
            for (const auto &call : inflight_) {
                if (call->id == request_id) {
                    target = call;
                    break;
                }
            }

            if (target && target->cancel) {
                const bool first = !target->cancel_requested;
                target->cancel_requested = true;
                {
                    std::lock_guard lock(target->execution->response_mutex);
                    target->execution->cancel_before_publication = true;
                }
                publication_gate.notify_state_change();
                target->cancel->emit(asio::cancellation_type::terminal);
                if (first && target->live_export && !target->tool_finished &&
                    target->execution->state.load(std::memory_order_acquire) == ExecutionState::EXECUTING) {
                    auto timer = std::make_shared<asio::steady_timer>(
                        ioc_, test_hook_ms("ENGRAVE_MCP_TEST_WATCHDOG_MS", 1, std::chrono::seconds(5)));
                    target->watchdog = timer;
                    auto self = shared_from_this();
                    timer->async_wait([self, target, timer](const asio::error_code &error) {
                        if (error || target->tool_finished || target->response_started ||
                            target->execution->state.load(std::memory_order_acquire) != ExecutionState::EXECUTING) {
                            return;
                        }
                        auto running = ExecutionState::EXECUTING;
                        if (!target->execution->state.compare_exchange_strong(running, ExecutionState::WATCHDOG_TERMINATING,
                                                                              std::memory_order_acq_rel)) {
                            return;
                        }
                        publication_gate.notify_state_change();
                        if (test_hooks_enabled()) {
                            std::fprintf(stderr, "WATCHDOG_CLAIMED\n");
                            std::fflush(stderr);
                        }
                        target->response_started = true;
                        // Decoupled exit waits for all committed reports. The
                        // identity check still prevents an old exit killing new work.
                        const char *barrier = test_hooks_enabled() ? std::getenv("ENGRAVE_MCP_TEST_WATCHDOG_BARRIER") : nullptr;
                        std::thread([execution = target->execution, barrier = std::string(barrier ? barrier : ""),
                                     settle = test_hook_ms("ENGRAVE_MCP_TEST_WATCHDOG_SETTLE_MS", 0, std::chrono::milliseconds(250))] {
                            // The barrier replaces the settle delay so tests can order the
                            // fence check after another request's publication starts.
                            if (barrier.empty()) {
                                std::this_thread::sleep_for(settle);
                            } else {
                                while (!std::filesystem::exists(std::filesystem::u8path(barrier))) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                }
                            }
                            std::unique_lock lock(publication_gate.state_mutex);
                            if (!publication_gate.closing.load(std::memory_order_acquire) &&
                                (publication_gate.active_publications > 0 || publication_gate.active_stdout_writes > 0)) {
                                if (test_hooks_enabled()) {
                                    std::fprintf(stderr, "WATCHDOG_LOCK_WAITING\n");
                                    std::fflush(stderr);
                                }
                                publication_gate.state_cv.wait(lock, [] {
                                    return publication_gate.closing.load(std::memory_order_acquire) ||
                                           (publication_gate.active_publications == 0 && publication_gate.active_stdout_writes == 0);
                                });
                            }
                            if (!publication_gate.closing.load(std::memory_order_acquire) &&
                                execution->state.load(std::memory_order_acquire) == ExecutionState::WATCHDOG_TERMINATING) {
                                std::_Exit(70);
                            }
                        }).detach();
                        if (!target->modern) {
                            asio::co_spawn(
                                self->ioc_,
                                [self, target]() -> asio::awaitable<void> {
                                    std::ignore =
                                        co_await self->send_error(target->id, -32603,
                                                                  "Live export cancellation watchdog expired after 5 seconds; session "
                                                                  "terminates if export remains blocked");
                                },
                                asio::detached);
                        }
                    });
                }
                if (test_hooks_enabled()) {
                    std::fprintf(stderr, "CANCEL_HANDLED\n");
                    std::fflush(stderr);
                }
            }
        } catch (...) {}
    }

    asio::awaitable<bool> start_tool_call(const Json &id, std::string name, Json arguments, bool modern) {
        CallStatePtr call;
        bool allocation_failed = false;
        try {
            call = std::make_shared<CallState>();
            call->id = id;
            call->modern = modern;
            call->live_export = name == "score_export";
            call->cancel = std::make_shared<asio::cancellation_signal>();
            if (call->live_export) {
                call->execution->report = [self = shared_from_this(), id, modern](Json payload) -> const char * {
                    return self->publish_report(id, std::move(payload), modern);
                };
                call->execution->abort_transport = [self = shared_from_this()](const char *reason) { self->post_writer_failure(reason); };
            }
            inflight_.push_back(call);
        } catch (...) {
            allocation_failed = true;
        }
        if (allocation_failed) {
            co_return co_await send_error(id, -32603, "Internal error: unable to start tool call");
        }

        bool spawn_failed = false;
        try {
            auto self = shared_from_this();
            asio::co_spawn(
                ioc_.get_executor(),
                [self, call, name = std::move(name), arguments = std::move(arguments)]() mutable -> asio::awaitable<void> {
                    co_await self->run_tool_call(call, std::move(name), std::move(arguments));
                },
                asio::bind_cancellation_slot(call->cancel->slot(),
                                             [self, call](std::exception_ptr error) { self->on_call_completion(call, std::move(error)); }));
        } catch (...) {
            spawn_failed = true;
        }
        if (spawn_failed) {
            erase_inflight(call);
            if (call->response_started || call->execution->was_reported()) {
                co_return true;
            }
            co_return co_await send_error(id, -32603, "Internal error: unable to start tool call");
        }
        co_return true;
    }

    void on_call_completion(CallStatePtr call, std::exception_ptr error) noexcept {
        try {
            auto self = shared_from_this();
            asio::co_spawn(
                ioc_.get_executor(), [self, call, error]() -> asio::awaitable<void> { co_await self->finish_call(call, error); },
                asio::detached);
        } catch (...) {
            erase_inflight(call);
            maybe_finish_graceful();
        }
    }

    asio::awaitable<void> run_tool_call(CallStatePtr call, std::string name, Json arguments) {
        enum class FailureDelivery { NONE, CANCELLED, TOOL_ERROR, GENERIC };
        FailureDelivery delivery = FailureDelivery::NONE;
        std::string error_code;
        std::string error_message;

        try {
            Json payload = co_await call_tool_(std::move(name), std::move(arguments), call->execution);
            call->tool_finished = true;
            if (call->watchdog) call->watchdog->cancel();

            // The callback returned success. From here on cancellation must
            // not turn a committed operation into an error; late
            // notifications/cancelled are ignored by this coroutine.
            co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());

            if (fatal_.load(std::memory_order_acquire)) {
                call->response_started = true;
                erase_inflight(call);
                maybe_finish_graceful();
                co_return;
            }
            if (call->execution->was_reported()) {
                call->response_started = true;
                erase_inflight(call);
                maybe_finish_graceful();
                co_return;
            }
            if (!payload.is_object() || !payload.contains("ok") || !payload["ok"].is_boolean()) {
                co_await deliver_tool_error(call, "EngineFailure", "Tool result must be an envelope with a boolean ok field");
                co_return;
            }

            Json result = make_tool_result(std::move(payload));
            co_await deliver_tool_result(call, std::move(result));
            co_return;
        } catch (const std::system_error &error) {
            if (call->cancel_requested && is_operation_aborted_code(error)) {
                delivery = FailureDelivery::CANCELLED;
            } else if (is_operation_aborted_code(error)) {
                delivery = FailureDelivery::TOOL_ERROR;
                error_code = "Cancelled";
                error_message = "Session is shutting down";
            } else {
                delivery = FailureDelivery::GENERIC;
                error_message = error.what();
            }
        } catch (const std::exception &error) {
            delivery = FailureDelivery::GENERIC;
            error_message = error.what();
        } catch (...) {
            delivery = FailureDelivery::GENERIC;
            error_message = "Unknown application error";
        }

        call->tool_finished = true;
        if (call->watchdog) call->watchdog->cancel();
        co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());

        switch (delivery) {
            case FailureDelivery::CANCELLED:
                call->response_started = true; // The completion handler must also leave this request silent.
                erase_inflight(call);
                maybe_finish_graceful();
                break;
            case FailureDelivery::TOOL_ERROR: co_await deliver_tool_error(call, std::move(error_code), std::move(error_message)); break;
            case FailureDelivery::GENERIC: co_await deliver_generic_error(call, std::move(error_message)); break;
            case FailureDelivery::NONE: break;
        }
        co_return;
    }

    Json make_tool_error_result(const std::string &code, const std::string &message) const {
        std::string bounded_message = message;
        bounded_message.resize(utf8_prefix(bounded_message, 16384));
        auto result = make_tool_result(Json{{"ok", false}, {"error", {{"code", code}, {"message", bounded_message}}}});
        result["content"][0]["text"] = std::move(bounded_message);
        return result;
    }

    asio::awaitable<void> deliver_tool_result(CallStatePtr call, Json result) {
        if (call->response_started || call->execution->was_reported() || (call->modern && call->cancel_requested) ||
            fatal_.load(std::memory_order_acquire)) {
            call->response_started = true;
            erase_inflight(call);
            maybe_finish_graceful();
            co_return;
        }
        call->response_started = true;

        const WireStatus status = co_await send_result_status(call->id, std::move(result), call->modern);
        if (status == WireStatus::TOO_LARGE && !fatal_.load(std::memory_order_acquire)) {
            Json fallback = make_tool_error_result("ResourceLimit", "Tool response exceeds the 16 MiB transport limit; "
                                                                    "the requested operation may already have completed.");
            const WireStatus fallback_status = co_await send_result_status(call->id, std::move(fallback), call->modern);
            if (fallback_status == WireStatus::TOO_LARGE) {
                fatal_shutdown("tool resource-limit response exceeded 16 MiB");
            }
        } else if (status == WireStatus::FAILED) {
            // The connection is already failing; stop producing output.
        }

        erase_inflight(call);
        maybe_finish_graceful();
        co_return;
    }

    asio::awaitable<void> deliver_tool_error(const CallStatePtr &call, std::string code, std::string message) {
        if (call->response_started || call->execution->was_reported() || (call->modern && call->cancel_requested) ||
            fatal_.load(std::memory_order_acquire)) {
            call->response_started = true;
            erase_inflight(call);
            maybe_finish_graceful();
            co_return;
        }
        call->response_started = true;

        Json result = make_tool_error_result(code, message);
        const WireStatus status = co_await send_result_status(call->id, std::move(result), call->modern);
        if (status == WireStatus::TOO_LARGE && !fatal_.load(std::memory_order_acquire)) {
            // Fall back to a tiny bounded resource error.
            Json fallback = make_tool_error_result("ResourceLimit", "Tool error exceeds the 16 MiB transport limit.");
            if (co_await send_result_status(call->id, std::move(fallback), call->modern) == WireStatus::TOO_LARGE) {
                fatal_shutdown("tool error response exceeded 16 MiB");
            }
        }

        erase_inflight(call);
        maybe_finish_graceful();
        co_return;
    }

    asio::awaitable<void> deliver_generic_error(const CallStatePtr &call, std::string message) {
        if (call->response_started || call->execution->was_reported() || (call->modern && call->cancel_requested) ||
            fatal_.load(std::memory_order_acquire)) {
            call->response_started = true;
            erase_inflight(call);
            maybe_finish_graceful();
            co_return;
        }
        call->response_started = true;

        if (message.empty()) {
            message = "Unknown application error";
        }
        Json result = make_tool_error_result("EngineFailure", message);
        const WireStatus status = co_await send_result_status(call->id, std::move(result), call->modern);
        if (status == WireStatus::TOO_LARGE) {
            fatal_shutdown("generic tool error response exceeded 16 MiB");
        }

        erase_inflight(call);
        maybe_finish_graceful();
        co_return;
    }

    asio::awaitable<void> finish_call(CallStatePtr call, std::exception_ptr error) {
        if (call->response_started || call->execution->was_reported() || (call->modern && call->cancel_requested)) {
            erase_inflight(call);
            maybe_finish_graceful();
            co_return;
        }
        call->response_started = true;

        if (fatal_.load(std::memory_order_acquire)) {
            erase_inflight(call);
            maybe_finish_graceful();
            co_return;
        }

        if (!(error && call->cancel_requested && is_operation_aborted(error))) {
            std::ignore = co_await send_error(call->id, -32603, "Internal error");
        }

        erase_inflight(call);
        maybe_finish_graceful();
        co_return;
    }

    void erase_inflight(const CallStatePtr &call) noexcept {
        const auto it = std::find(inflight_.begin(), inflight_.end(), call);
        if (it != inflight_.end()) {
            inflight_.erase(it);
        }
    }

    void maybe_finish_graceful() noexcept {
        if (fatal_.load(std::memory_order_acquire) || !eof_ || !inflight_.empty() || writer_stop_requested_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (drain_scheduled_ || !queue_.empty()) {
                return;
            }
        }

        writer_stop_requested_ = true;
        {
            std::lock_guard<std::mutex> lock(out_mutex_);
            out_graceful_stop_ = true;
        }
        out_cv_.notify_all();
    }

    void shutdown_once() noexcept {
        if (shutdown_called_) {
            return;
        }
        shutdown_called_ = true;
        if (shutdown_) {
            try {
                shutdown_();
            } catch (...) {}
        }
    }

    void fatal_shutdown(std::string reason) noexcept {
        if (fatal_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        failed_.store(true, std::memory_order_relaxed);
        if (!reason.empty()) {
            try {
                terminal_diagnostic("mcp transport failure: " + reason + "\n");
            } catch (...) {}
        }

        // Cancel queued engine work and stop both input and output. Crucially
        // do NOT call ioc_.stop(): coroutine frames owned by the separate
        // Engine-owner thread may still post their completion handlers here.
        // Their own work guards keep the io_context alive until those frames
        // have unwound; work_ is released after the writer thread has exited.
        shutdown_once();
        request_reader_stop();
        request_writer_abort();
        resume_output_waiters();
    }

#if defined(_WIN32)
    void close_os_handles() noexcept {
        HANDLE reader = reader_handle_.exchange(nullptr, std::memory_order_acq_rel);
        if (reader != nullptr) {
            ::CloseHandle(reader);
        }
        HANDLE writer = writer_handle_.exchange(nullptr, std::memory_order_acq_rel);
        if (writer != nullptr) {
            ::CloseHandle(writer);
        }
    }
#endif

    asio::io_context &ioc_;
    UniqueFn<Json()> list_tools_;
    UniqueFn<bool(std::string_view)> tool_exists_;
    AsyncToolCall call_tool_;
    UniqueFn<void()> shutdown_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;

    ServerState state_ = ServerState::UNINITIALIZED;
    std::vector<CallStatePtr> inflight_;

    // Reader admission queue. Only the reader thread waits on queue_cv_; the
    // io/session thread merely locks briefly to pop or enqueue.
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<ReaderEvent> queue_;
    usize queued_bytes_ = 0;
    bool drain_scheduled_ = false;
    std::atomic<bool> stop_reader_{false};

    // Output admission and writer exchange. The byte budget covers reserved,
    // queued and currently-writing messages.
    std::mutex out_mutex_;
    std::condition_variable out_cv_;
    std::deque<OutputMessage> out_messages_;
    usize out_budget_used_ = 0;
    usize out_message_slots_ = 0;
    bool out_abort_ = false;
    bool out_graceful_stop_ = false;
    bool writer_stop_requested_ = false;
    std::atomic<bool> writer_abort_{false};
    std::atomic<bool> output_progress_pending_{false};
    std::deque<OutputWaiter> output_waiters_; // session thread only

    std::atomic<bool> fatal_{false};
    std::atomic<bool> failed_{false};
    bool eof_ = false;
    bool shutdown_called_ = false;

    std::jthread reader_thread_;
    std::jthread writer_thread_;
    std::atomic<void *> reader_handle_{nullptr};
    std::atomic<void *> writer_handle_{nullptr};
#if !defined(_WIN32)
    int reader_wake_[2] = {-1, -1};
    int original_stdout_flags_ = -1;
#endif
};

} // namespace

int serve_stdio_async(UniqueFn<Json()> list_tools, UniqueFn<bool(std::string_view)> tool_exists, AsyncToolCall call_tool,
                      UniqueFn<void()> shutdown) {
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif

    try {
        asio::io_context ioc;
        std::shared_ptr<Session> session;
        try {
            session =
                std::make_shared<Session>(ioc, std::move(list_tools), std::move(tool_exists), std::move(call_tool), std::move(shutdown));
            session->start_threads();
        } catch (...) {
            if (session) {
                session->fatal_from_owner("cannot start MCP transport threads");
                // Join any thread that did start and release the work guard
                // before the io_context (and the handler storage it owns)
                // goes out of scope.
                session->stop_threads();
                session->finalize();
            }
            return 1;
        }

        for (;;) {
            try {
                ioc.run();
                break;
            } catch (const std::exception &error) {
                // A handler exception must not abandon completion handlers
                // posted later by the Engine-owner thread. Fail the
                // connection and keep running the context until it drains.
                session->fatal_from_owner(error.what());
            } catch (...) {
                session->fatal_from_owner("unknown io_context handler failure");
            }
        }

        session->stop_threads();
        session->finalize();
        return session->exit_code();
    } catch (...) {
        return 1;
    }
}

} // namespace music
