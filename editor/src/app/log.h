#pragma once
// Thread-safe log shown in the Console panel. Background jobs (builds, QEMU)
// append from worker threads; the UI thread reads it every frame.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace osef::editor {

enum class LogLevel { Info = 0, Warning, Error, Build };

struct LogLine {
    LogLevel level = LogLevel::Info;
    std::string text;
    uint64_t serial = 0; // unique, increasing
};

class Log {
public:
    void append(LogLevel level, std::string text) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(LogLine{level, std::move(text), next_serial_++});
        ++counts_[static_cast<int>(level)];
        if (lines_.size() > kMaxLines) {
            --counts_[static_cast<int>(lines_.front().level)];
            lines_.pop_front();
        }
        ++version_;
    }
    void info(std::string text) { append(LogLevel::Info, std::move(text)); }
    void warn(std::string text) { append(LogLevel::Warning, std::move(text)); }
    void error(std::string text) { append(LogLevel::Error, std::move(text)); }
    void build(std::string text) { append(LogLevel::Build, std::move(text)); }

    // Calls fn(const LogLine&) for every line, oldest first, under the lock.
    // Keep fn cheap (it runs on the UI thread while workers wait).
    template <class Fn>
    void visit(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const LogLine& line : lines_) fn(line);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
        for (auto& c : counts_) c = 0;
        ++version_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.size();
    }
    size_t count(LogLevel level) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return counts_[static_cast<int>(level)];
    }
    // Increments on every change; panels use it to auto-scroll.
    uint64_t version() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return version_;
    }
    LogLine last() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.empty() ? LogLine{} : lines_.back();
    }

    static constexpr size_t kMaxLines = 20000;

private:
    mutable std::mutex mutex_;
    std::deque<LogLine> lines_;
    size_t counts_[4] = {};
    uint64_t version_ = 0;
    uint64_t next_serial_ = 1;
};

} // namespace osef::editor
