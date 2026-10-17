// Host platform for Windows, macOS, Linux/BSD and Emscripten, plus the
// hosted runtime hooks (osef::assert_fail, osef::create_default_platform).
// Time uses <chrono>; only console input needs per-OS code.

#include <osef/platform.h>

#include "../escape_filter.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <conio.h>
#elif defined(__EMSCRIPTEN__)
#  include <emscripten.h>
#else
#  include <sys/select.h>
#  include <unistd.h>
#endif

#if defined(__APPLE__)
#  include <malloc/malloc.h>
#  define OSEF_HOST_HEAP_STATS 1
#elif defined(__EMSCRIPTEN__) || (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33)))
#  include <malloc.h>
#  define OSEF_HOST_HEAP_STATS 1
#else
#  define OSEF_HOST_HEAP_STATS 0
#endif

namespace osef {

[[noreturn]] void assert_fail(const char* file, int line, const char* expr) {
    std::fflush(stdout);
    std::fprintf(stderr, "\nosef: assertion failed: %s (%s:%d)\n", expr, file, line);
    std::abort();
}

namespace {

// Bytes currently allocated from the C/C++ heap by this process (0 if the
// host has no cheap way to tell).
uint64_t heap_in_use() {
#if defined(__APPLE__)
    malloc_statistics_t st{};
    malloc_zone_statistics(nullptr, &st);  // all zones
    return st.size_in_use;
#elif defined(__EMSCRIPTEN__)
    struct mallinfo mi = mallinfo();
    return static_cast<uint64_t>(mi.uordblks);
#elif OSEF_HOST_HEAP_STATS
    struct mallinfo2 mi = mallinfo2();
    return static_cast<uint64_t>(mi.uordblks) + static_cast<uint64_t>(mi.hblkhd);
#else
    return 0;
#endif
}

class HostPlatform final : public Platform {
public:
    const char* name() const override {
#if defined(_WIN32)
        return "windows";
#elif defined(__EMSCRIPTEN__)
        return "web";
#elif defined(__ANDROID__)
        return "android";
#elif defined(__APPLE__)
        return "macos";
#elif defined(__linux__)
        return "linux";
#elif defined(__FreeBSD__)
        return "freebsd";
#else
        return "posix";
#endif
    }

    void init(const HardwareSpec& hw) override {
        memory_mb_ = hw.memory_mb;
        start_ = Clock::now();
        rng_.seed(std::random_device{}());
#if defined(_WIN32)
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(out, &mode))
            SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleOutputCP(CP_UTF8);
        in_ = GetStdHandle(STD_INPUT_HANDLE);
        DWORD in_mode = 0;
        console_in_ = GetConsoleMode(in_, &in_mode) != 0;
        interactive_ = console_in_;
#elif defined(__EMSCRIPTEN__)
        interactive_ = false; // input arrives via Kernel::push_input
#else
        interactive_ = isatty(STDIN_FILENO) != 0;
#endif
    }

    uint64_t now_ms() override {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start_).count());
    }

    void sleep_ms(uint32_t ms) override { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

    void write(StringView text) override {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    }

    bool poll_line(String& out) override {
        pump();
        if (lines_.empty()) return false;
        out = osef::move(lines_.front());
        lines_.pop_front();
        return true;
    }

    bool input_closed() const override { return eof_ && lines_.empty(); }
    bool interactive() const override { return interactive_; }
    uint64_t random_u64() override { return rng_(); }

    bool read_wall_clock(WallClock& out) override {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        if (localtime_s(&local, &now) != 0) return false;
#else
        if (!localtime_r(&now, &local)) return false;
#endif
        out = {static_cast<uint16_t>(local.tm_year + 1900), static_cast<uint8_t>(local.tm_mon + 1),
               static_cast<uint8_t>(local.tm_mday), static_cast<uint8_t>(local.tm_hour),
               static_cast<uint8_t>(local.tm_min), static_cast<uint8_t>(local.tm_sec)};
        return true;
    }

    // The declared machine size (.memory_mb()) as the total, and what this
    // process has allocated from the host heap as used, so `mem` shows
    // numbers comparable to the bare-metal kernel heap.
    MemoryStats memory() const override {
        MemoryStats m;
        const uint64_t used = heap_in_use();
        if (used == 0 || memory_mb_ == 0) return m;  // not reported
        m.total = static_cast<uint64_t>(memory_mb_) * 1024u * 1024u;
        m.used = used;
        return m;
    }

    // Crash isolation: a process "faults" by throwing (any std::exception,
    // or ProcessFault from fault()). Without exceptions there is no
    // isolation, and fault() stops the program.
    bool run_protected(void (*fn)(void*), void* arg, String& fault_message) override {
#if OSEF_HAS_EXCEPTIONS
        try {
            fn(arg);
            return true;
        } catch (const ProcessFault& f) {
            fault_message = f.message();
        } catch (const std::exception& e) {
            fault_message = e.what();
        } catch (...) {
            fault_message = "unknown exception";
        }
        return false;
#else
        (void)fault_message;
        fn(arg);
        return true;
#endif
    }

    [[noreturn]] void fault(StringView message) override {
#if OSEF_HAS_EXCEPTIONS
        throw ProcessFault(message);
#else
        write("\nprocess fault (no fault isolation without C++ exceptions): ");
        write(message);
        write("\n");
        std::abort();
#endif
    }

private:
    using Clock = std::chrono::steady_clock;

    void feed(const char* data, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            char ch = data[i];
            if (escapes_.swallow(ch)) continue;
            if (ch == '\r') continue;
            if (ch == '\n') {
                lines_.push_back(osef::move(partial_));
                partial_.clear();
            } else {
                partial_ += ch;
            }
        }
    }

    void close_input() {
        eof_ = true;
        if (!partial_.empty()) {
            lines_.push_back(osef::move(partial_));
            partial_.clear();
        }
    }

    void pump() {
        if (eof_) return;
#if defined(__EMSCRIPTEN__)
        return;
#elif defined(_WIN32)
        if (console_in_) {
            // Console has no non-blocking line mode, so do our own editing.
            while (_kbhit()) {
                int c = _getch();
                if (c == 0 || c == 0xE0) { _getch(); continue; } // arrows, F-keys
                if (c == '\r') {
                    write("\r\n");
                    lines_.push_back(osef::move(partial_));
                    partial_.clear();
                } else if (c == '\b') {
                    if (!partial_.empty()) { partial_.pop_back(); write("\b \b"); }
                } else if (c == 26) { // Ctrl-Z
                    close_input();
                    return;
                } else if (c >= 32 || c == '\t') {
                    char ch = static_cast<char>(c);
                    partial_ += ch;
                    write(StringView(&ch, 1));
                }
            }
            return;
        }
        // Redirected stdin: a pipe or a file.
        for (;;) {
            char buf[512];
            DWORD n = 0;
            if (GetFileType(in_) == FILE_TYPE_PIPE) {
                DWORD avail = 0;
                if (!PeekNamedPipe(in_, nullptr, 0, nullptr, &avail, nullptr)) { close_input(); return; }
                if (avail == 0) return;
                DWORD want = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
                if (!ReadFile(in_, buf, want, &n, nullptr) || n == 0) { close_input(); return; }
            } else {
                if (!ReadFile(in_, buf, sizeof(buf), &n, nullptr) || n == 0) { close_input(); return; }
            }
            feed(buf, n);
        }
#else
        for (;;) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(STDIN_FILENO, &set);
            timeval tv{0, 0};
            if (select(STDIN_FILENO + 1, &set, nullptr, nullptr, &tv) <= 0) return;
            char buf[512];
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) { close_input(); return; }
            feed(buf, static_cast<size_t>(n));
        }
#endif
    }

    Clock::time_point start_ = Clock::now();
    std::mt19937_64 rng_;
    uint32_t memory_mb_ = 0;
    detail::EscapeFilter escapes_;  // arrow keys etc. in a cooked-mode terminal
    Deque<String> lines_;
    String partial_;
    bool eof_ = false;
    bool interactive_ = true;
#if defined(_WIN32)
    HANDLE in_ = nullptr;
    bool console_in_ = false;
#endif
};

} // namespace

UniquePtr<Platform> create_default_platform() { return make_unique<HostPlatform>(); }

} // namespace osef
