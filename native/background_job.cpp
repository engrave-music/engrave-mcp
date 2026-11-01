#include "background_job.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace music {
namespace {
constexpr std::size_t log_limit = 64 * 1024;
#ifdef _WIN32
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE value) : value(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE next = nullptr) noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
};
[[noreturn]] void windows_error(const char* what) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
}
std::wstring quote_arg(const std::wstring& arg) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') ++slashes;
        else {
            result.append(slashes * (c == L'"' ? 2 : 1) + (c == L'"' ? 1 : 0), L'\\');
            result.push_back(c);
            slashes = 0;
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}
#else
struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) ::close(value); }
    Fd() = default;
    explicit Fd(int value) : value(value) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
void spawn_check(int error, const char* message) {
    if (error) throw std::system_error(error, std::generic_category(), message);
}
#endif

// Live worker children, used to interrupt a synchronous export at host
// shutdown (the engine-owner thread is blocked waiting for the child).
struct ActiveWorkers {
    std::mutex mutex;
    std::atomic<bool> shutdown{false};
    std::uint64_t next_token = 1;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> callbacks;
};

ActiveWorkers& active_workers() {
    static ActiveWorkers state;
    return state;
}

std::uint64_t add_active_worker(std::function<void()> terminate) {
    ActiveWorkers& state = active_workers();
    std::lock_guard lock(state.mutex);
    const std::uint64_t token = state.next_token++;
    state.callbacks.emplace_back(token, std::move(terminate));
    return token;
}

void remove_active_worker(std::uint64_t token) noexcept {
    ActiveWorkers& state = active_workers();
    std::lock_guard lock(state.mutex);
    std::erase_if(state.callbacks, [token](const auto& entry) { return entry.first == token; });
}
}

std::uint64_t current_process_id() noexcept {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}
void enforce_worker_parent(std::uint64_t expected_parent) {
#ifndef _WIN32
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) < 0)
        throw std::system_error(errno, std::generic_category(), "Cannot configure worker parent-death signal");
    // Parent may have died between spawn and prctl. Check after installing the signal.
    if (expected_parent != static_cast<std::uint64_t>(::getppid()))
        throw std::runtime_error("Worker parent has exited");
#else
    (void)expected_parent; // Parent creates the process suspended and attaches a kill-on-close JobObject.
#endif
}
std::filesystem::path current_executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!count) windows_error("GetModuleFileNameW failed");
        if (count < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), count));
        if (buffer.size() >= 65536) throw std::runtime_error("Executable path too long");
        buffer.resize(buffer.size() * 2);
    }
#else
    std::vector<char> buffer(512);
    for (;;) {
        const auto count = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (count < 0) throw std::system_error(errno, std::generic_category(), "readlink failed");
        if (static_cast<std::size_t>(count) < buffer.size()) return std::filesystem::path(std::string(buffer.data(), count));
        if (buffer.size() >= 65536) throw std::runtime_error("Executable path too long");
        buffer.resize(buffer.size() * 2);
    }
#endif
}

struct WorkerProcess::Impl {
#ifdef _WIN32
    Handle process, job, log_read;
#else
    pid_t pid = -1;
    Fd log_read;
#endif
    std::optional<int> exit_code;
    std::jthread log_reader;
    std::uint64_t registry_token = 0;

    void start_log(const std::filesystem::path& path) {
        log_reader = std::jthread([this, path] {
            // Keep the first 64 KiB for diagnostics, then drain/discard so the child never blocks on logs.
#ifdef _WIN32
            FILE* output = _wfopen(path.c_str(), L"wb");
#else
            FILE* output = std::fopen(path.c_str(), "wb");
#endif
            struct CloseLog { FILE* file; ~CloseLog() { if (file) std::fclose(file); } } close_log{output};
            std::size_t retained = 0;
            std::array<char, 8192> buffer{};
            for (;;) {
#ifdef _WIN32
                DWORD count = 0;
                if (!ReadFile(log_read.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) || !count) break;
#else
                const auto count = ::read(log_read.value, buffer.data(), buffer.size());
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) break;
#endif
                const auto keep = std::min<std::size_t>(count, log_limit - retained);
                if (keep && output) (void)std::fwrite(buffer.data(), 1, keep, output);
                retained += keep;
            }
        });
    }

    Impl(const std::filesystem::path& executable, const std::filesystem::path& request,
         const std::filesystem::path& log) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (input.value == INVALID_HANDLE_VALUE) windows_error("Cannot open worker stdin");
        Handle log_write;
        if (!CreatePipe(&log_read.value, &log_write.value, &security, 0)) windows_error("Cannot create worker log pipe");
        if (!SetHandleInformation(log_read.value, HANDLE_FLAG_INHERIT, 0)) windows_error("Cannot protect worker log reader");
        job.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job.value) windows_error("Cannot create worker job object");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            windows_error("Cannot configure worker job object");
        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        std::vector<unsigned char> attributes(attribute_size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &attribute_size)) windows_error("Cannot initialize worker handle list");
        struct AttributeCleanup { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeCleanup() { DeleteProcThreadAttributeList(value); } } cleanup{list};
        std::array<HANDLE, 2> inherited{input.value, log_write.value};
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), sizeof(inherited), nullptr, nullptr))
            windows_error("Cannot constrain worker handles");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdInput = input.value;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log_write.value;
        startup.lpAttributeList = list;
        auto command = quote_arg(executable.wstring()) + L" --worker " + quote_arg(request.wstring());
        PROCESS_INFORMATION created{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                            nullptr, nullptr, &startup.StartupInfo, &created)) windows_error("Cannot start worker");
        process.reset(created.hProcess);
        Handle thread(created.hThread);
        try {
            if (!AssignProcessToJobObject(job.value, process.value)) windows_error("Cannot contain worker process");
            if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) windows_error("Cannot resume worker");
            start_log(log);
        } catch (...) {
            TerminateProcess(process.value, 1);
            WaitForSingleObject(process.value, INFINITE);
            throw;
        }
#else
        int pipes[2];
        if (::pipe2(pipes, O_CLOEXEC) != 0) throw std::system_error(errno, std::generic_category(), "Cannot create worker log pipe");
        log_read.value = pipes[0];
        Fd log_write(pipes[1]);
        posix_spawn_file_actions_t actions;
        spawn_check(posix_spawn_file_actions_init(&actions), "Cannot initialize spawn actions");
        struct ActionsCleanup { posix_spawn_file_actions_t* actions; ~ActionsCleanup() { posix_spawn_file_actions_destroy(actions); } } cleanup{&actions};
        spawn_check(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0), "Cannot redirect worker stdin");
        spawn_check(posix_spawn_file_actions_adddup2(&actions, log_write.value, STDOUT_FILENO), "Cannot redirect worker stdout");
        spawn_check(posix_spawn_file_actions_adddup2(&actions, log_write.value, STDERR_FILENO), "Cannot redirect worker stderr");
        spawn_check(posix_spawn_file_actions_addclose(&actions, log_read.value), "Cannot close worker log reader");
        spawn_check(posix_spawn_file_actions_addclose(&actions, log_write.value), "Cannot close worker log writer");
        auto exe = executable.string(), request_text = request.string();
        char mode[] = "--worker";
        char* argv[] = {exe.data(), mode, request_text.data(), nullptr};
        spawn_check(posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv, environ), "Cannot start worker");
        try { start_log(log); }
        catch (...) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            pid = -1;
            throw;
        }
#endif
        registry_token = add_active_worker([this] { terminate(); });
    }
    ~Impl() noexcept {
        remove_active_worker(registry_token);
        registry_token = 0;
        terminate();
#ifdef _WIN32
        if (process.value) WaitForSingleObject(process.value, INFINITE);
        // Closing job also closes any descendants' inherited log writers.
        job.reset();
#else
        if (pid > 0) {
            while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            pid = -1;
        }
#endif
        if (log_reader.joinable()) log_reader.join();
    }
    std::optional<int> poll() {
        if (exit_code) return exit_code;
#ifdef _WIN32
        const auto status = WaitForSingleObject(process.value, 0);
        if (status == WAIT_TIMEOUT) return std::nullopt;
        if (status != WAIT_OBJECT_0) windows_error("Cannot inspect worker status");
        DWORD code;
        if (!GetExitCodeProcess(process.value, &code)) windows_error("Cannot get worker exit code");
        exit_code = static_cast<int>(code);
#else
        int status = 0;
        pid_t result;
        do { result = ::waitpid(pid, &status, WNOHANG); } while (result < 0 && errno == EINTR);
        if (!result) return std::nullopt;
        if (result < 0) {
            const int error = errno;
            if (error == ECHILD) pid = -1;
            throw std::system_error(error, std::generic_category(), "Cannot inspect worker status");
        }
        pid = -1;
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
        return exit_code;
    }
    void terminate() noexcept {
#ifdef _WIN32
        if (process.value && !exit_code) TerminateProcess(process.value, 1);
#else
        if (pid > 0) ::kill(pid, SIGKILL);
#endif
    }
};
WorkerProcess::WorkerProcess(const std::filesystem::path& executable, const std::filesystem::path& request,
                             const std::filesystem::path& log) : impl_(std::make_unique<Impl>(executable, request, log)) {}
WorkerProcess::~WorkerProcess() noexcept = default;
WorkerProcess::WorkerProcess(WorkerProcess&&) noexcept = default;
WorkerProcess& WorkerProcess::operator=(WorkerProcess&&) noexcept = default;
std::optional<int> WorkerProcess::poll() { return impl_ ? impl_->poll() : std::optional<int>{}; }
void WorkerProcess::terminate() { if (impl_) impl_->terminate(); }

void request_worker_shutdown() noexcept {
    ActiveWorkers& state = active_workers();
    std::lock_guard lock(state.mutex);
    state.shutdown.store(true, std::memory_order_release);
    // Callbacks only terminate the OS process; the owning threads observe the
    // exit and report it as a cancelled export.
    for (auto& entry : state.callbacks) {
        try { entry.second(); } catch (...) {}
    }
}

bool worker_shutdown_requested() noexcept {
    return active_workers().shutdown.load(std::memory_order_acquire);
}

bool same_output_path(const std::filesystem::path& first, const std::filesystem::path& second) {
#ifdef _WIN32
    const auto a = first.lexically_normal().wstring(), b = second.lexically_normal().wstring();
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
#else
    return first.lexically_normal() == second.lexically_normal();
#endif
}

std::filesystem::path create_private_directory(const std::filesystem::path& parent) {
    std::random_device random;
    for (int i = 0; i != 16; ++i) {
        auto path = parent / (".musescore-job-" + std::to_string(random()) + "-" + std::to_string(random()));
        std::error_code error;
        if (std::filesystem::create_directory(path, error)) {
#ifndef _WIN32
            std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
            if (error) {
                std::filesystem::remove(path);
                throw std::system_error(error, "Cannot restrict job directory permissions");
            }
#endif
            return path;
        }
        if (error && error != std::errc::file_exists)
            throw std::system_error(error, "Cannot create job directory");
    }
    throw std::runtime_error("Cannot allocate unique job directory");
}

std::filesystem::path normalize_output_path(const std::filesystem::path& path) {
    if (path.empty()) throw std::invalid_argument("Export path is required");
    const auto filename = path.filename();
    if (filename.empty() || filename == "." || filename == "..")
        throw std::invalid_argument("Export path must name a file");
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error)
        return std::filesystem::canonical(path);
    const auto absolute = std::filesystem::absolute(path);
    if (!std::filesystem::is_directory(absolute.parent_path(), error) || error)
        throw std::invalid_argument("Export parent directory must exist");
    return std::filesystem::canonical(absolute.parent_path()) / absolute.filename();
}

namespace {
std::mutex s_reserved_mutex;
struct Reservation { std::filesystem::path path; bool directory; };
std::vector<Reservation> s_reserved_paths;

bool contains_path(const std::filesystem::path& directory, const std::filesystem::path& path) {
    const auto parent = directory.lexically_normal(), child = path.lexically_normal();
    auto a = parent.begin(), b = child.begin();
    for (; a != parent.end(); ++a, ++b) {
        if (b == child.end() || !same_output_path(*a, *b)) return false;
    }
    return true;
}
bool conflicts(const Reservation& existing, const std::filesystem::path& path, bool directory) {
    return same_output_path(existing.path, path)
        || (existing.directory && contains_path(existing.path, path))
        || (directory && contains_path(path, existing.path));
}
}

void reserve_output_path(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::lock_guard lock(s_reserved_mutex);
    s_reserved_paths.push_back({path, false});
}

bool try_reserve_output_path(const std::filesystem::path& path, bool directory) {
    if (path.empty()) return false;
    std::lock_guard lock(s_reserved_mutex);
    for (const auto& reserved : s_reserved_paths)
        if (conflicts(reserved, path, directory)) return false;
    s_reserved_paths.push_back({path, directory});
    return true;
}

void release_output_path(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::lock_guard lock(s_reserved_mutex);
    auto it = std::find_if(s_reserved_paths.begin(), s_reserved_paths.end(),
        [&path](const auto& p) { return same_output_path(p.path, path); });
    if (it != s_reserved_paths.end()) s_reserved_paths.erase(it);
}

bool is_output_path_reserved(const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::lock_guard lock(s_reserved_mutex);
    for (const auto& reserved : s_reserved_paths) {
        if (conflicts(reserved, path, false)) return true;
    }
    return false;
}
void publish_job_output(const std::filesystem::path& staged, const std::filesystem::path& destination) {
#ifdef MUSIC_ASYNC_TEST_HOOKS
    if (std::getenv("ENGRAVE_MCP_TEST_FAIL_FILE_FLUSH"))
        throw PublicationError(EIO, "Injected staged file flush failure", false);
#endif
#ifdef _WIN32
    // FlushFileBuffers makes staged contents durable before the name is visible
    // (subject to the filesystem/device honoring flushes). WRITE_THROUGH is kept,
    // but its documented copy/delete guarantee does not establish directory-entry
    // power-loss durability for this same-volume rename. No such promise is made.
    Handle file(CreateFileW(staged.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) windows_error("Cannot open staged output for flush");
    if (!FlushFileBuffers(file.value)) windows_error("Cannot flush staged output");
    file.reset(); // Close before rename; a failed flush never replaces destination.
    if (!MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        windows_error("Cannot publish job output");
#else
    struct Descriptor {
        int fd;
        ~Descriptor() { if (fd >= 0) ::close(fd); }
    };
    auto parent = destination.parent_path();
    if (parent.empty()) parent = ".";
    Descriptor directory{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory.fd < 0)
        throw PublicationError(errno, "Cannot open publication directory", false);
    Descriptor file{::open(staged.c_str(), O_RDONLY | O_CLOEXEC)};
    if (file.fd < 0)
        throw PublicationError(errno, "Cannot open staged output", false);
    auto sync = [](int fd, const char* message, bool visible) {
        int result;
        do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
        if (result < 0) throw PublicationError(errno, message, visible);
    };
    sync(file.fd, "Cannot sync staged output", false);
    if (::rename(staged.c_str(), destination.c_str()) != 0)
        throw PublicationError(errno, "Cannot publish job output", false);
    sync(directory.fd, "Cannot sync publication directory", true);
#endif
}
}
