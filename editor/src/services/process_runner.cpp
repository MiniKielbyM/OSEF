// Background shell jobs. One job at a time: its combined stdout/stderr is
// split into lines on a reader thread and appended to the Log as Build lines.
//
// POSIX: fork + setpgid + exec /bin/sh -c, so cancel() can signal the whole
// process group (make -> compiler -> qemu ...): SIGTERM, then SIGKILL after 2 s.
// Windows: CreateProcessW("cmd.exe /d /s /c ...") inside a Job object.

#include "services/process_runner.h"

#include "app/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace osef::editor {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxLineBytes = 16 * 1024;
constexpr auto kKillGrace = std::chrono::seconds(2);

std::string format_seconds(double s) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f s", s);
    return buf;
}

// Splits a byte stream into lines. "\r\n" and "\n" end a line; a bare "\r"
// (progress output such as "[ 42%]\r") restarts the line, so only the final
// state of an overwritten line is logged. ANSI escape sequences are dropped.
class LineSplitter {
public:
    explicit LineSplitter(Log& log) : log_(log) {}

    void feed(const char* data, size_t n) {
        for (size_t i = 0; i < n; ++i) feed_char(data[i]);
    }

    void flush() {
        if (!line_.empty()) emit();
        pending_cr_ = false;
    }

private:
    void feed_char(char ch) {
        // ANSI CSI/OSC sequences: ESC '[' params final-byte, ESC ']' ... BEL.
        if (esc_ == Esc::Start) {
            esc_ = ch == '[' ? Esc::Csi : ch == ']' ? Esc::Osc : Esc::None;
            return;
        }
        if (esc_ == Esc::Csi) {
            if (ch >= 0x40 && ch <= 0x7E) esc_ = Esc::None;
            return;
        }
        if (esc_ == Esc::Osc) {
            if (ch == '\a' || ch == '\x1b') esc_ = Esc::None;
            return;
        }
        if (ch == '\x1b') {
            esc_ = Esc::Start;
            return;
        }
        if (ch == '\n') {
            pending_cr_ = false;
            emit();
            return;
        }
        if (pending_cr_) {
            line_.clear();  // a bare '\r' overwrote the line
            pending_cr_ = false;
        }
        if (ch == '\r') {
            pending_cr_ = true;
            return;
        }
        if (ch == '\t') {
            line_.append(4 - line_.size() % 4, ' ');
        } else if (static_cast<unsigned char>(ch) >= 0x20 || ch < 0) {
            line_.push_back(ch);  // printable ASCII or UTF-8 bytes; other controls dropped
        }
        if (line_.size() >= kMaxLineBytes) emit();
    }

    void emit() {
        log_.build(std::move(line_));
        line_.clear();
    }

    enum class Esc { None, Start, Csi, Osc };
    Log& log_;
    std::string line_;
    bool pending_cr_ = false;
    Esc esc_ = Esc::None;
};

} // namespace

struct ProcessRunner::Impl {
    mutable std::mutex mutex;
    std::mutex start_mutex;  // serializes start(); never held by the worker
    std::thread worker;      // guarded by `mutex`; moved out before joining

    void join_worker() {
        std::thread t;
        {
            std::lock_guard<std::mutex> lock(mutex);
            t = std::move(worker);
        }
        if (t.joinable()) t.join();
    }

    // Guarded by `mutex`.
    bool running = false;
    bool report_pending = false;  // finished, poll() hasn't logged it yet
    bool cancel_requested = false;
    int exit_code = -1;
    std::string label;
    Clock::time_point started{};
    double duration = 0.0;  // of the last finished job
    Log* log = nullptr;

#if defined(_WIN32)
    HANDLE process = nullptr;
    HANDLE job = nullptr;
#else
    pid_t pid = -1;  // also the process group id
    Clock::time_point kill_deadline{};
    bool killed = false;
#endif

    // Joins a finished worker and logs its result. Call without `mutex` held.
    void report_finished() {
        std::string message;
        bool failed = false, cancelled = false;
        Log* target = nullptr;
        std::thread finished_worker;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!report_pending) return;
            report_pending = false;
            finished_worker = std::move(worker);
            target = log;
            cancelled = cancel_requested;
            failed = exit_code != 0;
            message = label + (cancelled ? " cancelled (exit " : " finished (exit ") + std::to_string(exit_code) +
                      ", " + format_seconds(duration) + ")";
        }
        if (finished_worker.joinable()) finished_worker.join();
        if (!target) return;
        if (cancelled)
            target->warn(std::move(message));
        else if (failed)
            target->error(std::move(message));
        else
            target->info(std::move(message));
    }

    // Worker: called once the child has exited and its output is drained.
    void finish(int code) {
        std::lock_guard<std::mutex> lock(mutex);
        exit_code = code;
        duration = std::chrono::duration<double>(Clock::now() - started).count();
        running = false;
        report_pending = true;
#if defined(_WIN32)
        if (process) CloseHandle(process);
        if (job) CloseHandle(job);
        process = job = nullptr;
#else
        pid = -1;
#endif
    }

#if defined(_WIN32)
    bool spawn(const std::string& command, const std::filesystem::path& cwd, std::string* error);
    void read_loop(HANDLE out);
#else
    bool spawn(const std::string& command, const std::filesystem::path& cwd, std::string* error);
    void read_loop(int fd, pid_t child);
#endif
};

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string last_error_text() {
    char buf[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, GetLastError(), 0, buf,
                   sizeof(buf), nullptr);
    std::string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

} // namespace

bool ProcessRunner::Impl::spawn(const std::string& command, const std::filesystem::path& cwd, std::string* error) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_end = nullptr, write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        *error = "CreatePipe failed: " + last_error_text();
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                                 nullptr);

    HANDLE job_handle = CreateJobObjectW(nullptr, nullptr);
    if (job_handle) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &info, sizeof(info));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = null_in;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"cmd.exe /d /s /c \"" + widen(command) + L"\"";
    const std::wstring wcwd = cwd.empty() ? std::wstring() : cwd.wstring();
    const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                   wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(write_end);
    if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (!ok) {
        *error = "CreateProcess failed: " + last_error_text();
        CloseHandle(read_end);
        if (job_handle) CloseHandle(job_handle);
        return false;
    }
    if (job_handle) AssignProcessToJobObject(job_handle, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process = pi.hProcess;
    job = job_handle;
    worker = std::thread([this, read_end] { read_loop(read_end); });
    return true;
}

void ProcessRunner::Impl::read_loop(HANDLE out) {
    LineSplitter splitter(*log);
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(out, buf, sizeof(buf), &n, nullptr) && n > 0) splitter.feed(buf, n);
    splitter.flush();
    CloseHandle(out);
    HANDLE proc = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        proc = process;
    }
    DWORD code = 1;
    if (proc) {
        WaitForSingleObject(proc, INFINITE);
        GetExitCodeProcess(proc, &code);
    }
    finish(static_cast<int>(code));
}

#else // POSIX

bool ProcessRunner::Impl::spawn(const std::string& command, const std::filesystem::path& cwd, std::string* error) {
    int fds[2];
    if (::pipe(fds) != 0) {
        *error = std::string("pipe failed: ") + std::strerror(errno);
        return false;
    }
    for (int fd : fds) ::fcntl(fd, F_SETFD, FD_CLOEXEC);

    // Everything the child needs is prepared before fork(): after it, only
    // async-signal-safe calls are allowed (the editor is multithreaded).
    const std::string dir = cwd.empty() ? std::string() : cwd.string();
    static const char kCdFailed[] = "error: cannot change to the job's working directory\n";
    static const char kExecFailed[] = "error: cannot run /bin/sh\n";

    const pid_t child = ::fork();
    if (child < 0) {
        *error = std::string("fork failed: ") + std::strerror(errno);
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (child == 0) {
        ::setpgid(0, 0);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::signal(SIGPIPE, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        const int null_in = ::open("/dev/null", O_RDONLY);
        if (null_in >= 0) ::dup2(null_in, STDIN_FILENO);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        if (!dir.empty() && ::chdir(dir.c_str()) != 0) {
            ssize_t ignored = ::write(STDERR_FILENO, kCdFailed, sizeof(kCdFailed) - 1);
            (void)ignored;
            ::_exit(127);
        }
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ssize_t ignored = ::write(STDERR_FILENO, kExecFailed, sizeof(kExecFailed) - 1);
        (void)ignored;
        ::_exit(127);
    }
    ::setpgid(child, child);  // also in the parent, so cancel() can't race the child's own call
    ::close(fds[1]);
    pid = child;
    worker = std::thread([this, fd = fds[0], child] { read_loop(fd, child); });
    return true;
}

void ProcessRunner::Impl::read_loop(int fd, pid_t child) {
    LineSplitter splitter(*log);
    char buf[4096];
    bool reaped = false;
    int status = 0;
    Clock::time_point reaped_at{};
    bool eof = false;

    // Escalates a pending cancel to SIGKILL once the grace period is over.
    auto check_kill = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        if (cancel_requested && !killed && Clock::now() >= kill_deadline) {
            ::kill(-child, SIGKILL);
            killed = true;
        }
    };

    while (!eof) {
        pollfd p{fd, POLLIN, 0};
        const int r = ::poll(&p, 1, 100);
        if (r > 0) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                splitter.feed(buf, static_cast<size_t>(n));
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                eof = true;
            }
        } else if (r < 0 && errno != EINTR) {
            eof = true;
        }
        if (!reaped) {
            const pid_t w = ::waitpid(child, &status, WNOHANG);
            if (w == child || (w < 0 && errno == ECHILD)) {
                reaped = true;
                reaped_at = Clock::now();
            }
        } else if (r == 0 && Clock::now() - reaped_at > std::chrono::milliseconds(250)) {
            // The shell is gone but a background grandchild still holds the
            // pipe open: stop following it rather than hang the job.
            break;
        }
        check_kill();
    }
    splitter.flush();
    ::close(fd);

    while (!reaped) {
        const pid_t w = ::waitpid(child, &status, WNOHANG);
        if (w == child || (w < 0 && errno != EINTR)) break;
        check_kill();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    int code = -1;
    if (WIFEXITED(status))
        code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        code = 128 + WTERMSIG(status);
    finish(code);
}

#endif

ProcessRunner::ProcessRunner() : impl_(std::make_unique<Impl>()) {}

ProcessRunner::~ProcessRunner() {
    cancel();
    impl_->join_worker();
}

bool ProcessRunner::start(const std::string& label, const std::string& command, const std::filesystem::path& cwd,
                          Log& log) {
    std::lock_guard<std::mutex> serialize(impl_->start_mutex);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->running) {
            log.error("Cannot start " + label + ": " + impl_->label + " is still running");
            return false;
        }
    }
    // Nothing is running, so the previous job's state is final: report it
    // (if poll() hasn't yet) and join its worker.
    impl_->report_finished();
    impl_->join_worker();

    std::error_code ec;
    if (!cwd.empty() && !std::filesystem::is_directory(cwd, ec)) {
        log.error("Cannot start " + label + ": working directory " + cwd.string() + " does not exist");
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->label = label;
    impl_->log = &log;
    impl_->exit_code = -1;
    impl_->cancel_requested = false;
    impl_->report_pending = false;
    impl_->duration = 0.0;
    impl_->started = Clock::now();
#if !defined(_WIN32)
    impl_->killed = false;
#endif
    log.info(label + ": " + command);
    // `running` is set before the worker exists, so finish() can't be overtaken.
    impl_->running = true;
    std::string error;
    if (!impl_->spawn(command, cwd, &error)) {
        impl_->running = false;
        log.error("Cannot start " + label + ": " + error);
        return false;
    }
    return true;
}

bool ProcessRunner::running() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running;
}

void ProcessRunner::cancel() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running || impl_->cancel_requested) return;
    impl_->cancel_requested = true;
#if defined(_WIN32)
    if (impl_->job)
        TerminateJobObject(impl_->job, 1);
    else if (impl_->process)
        TerminateProcess(impl_->process, 1);
#else
    if (impl_->pid > 0) {
        ::kill(-impl_->pid, SIGTERM);
        ::kill(impl_->pid, SIGTERM);  // in case the group wasn't set up yet
        impl_->kill_deadline = Clock::now() + kKillGrace;
    }
#endif
}

void ProcessRunner::poll() { impl_->report_finished(); }

int ProcessRunner::last_exit_code() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running ? -1 : impl_->exit_code;
}

std::string ProcessRunner::current_label() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->running ? impl_->label : std::string();
}

double ProcessRunner::elapsed_seconds() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->running) return std::chrono::duration<double>(Clock::now() - impl_->started).count();
    return impl_->duration;
}

} // namespace osef::editor
