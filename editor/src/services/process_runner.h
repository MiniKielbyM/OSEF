#pragma once
// Runs one shell command at a time in the background (builds, QEMU), streaming
// its combined stdout/stderr into the Log as LogLevel::Build lines.

#include <filesystem>
#include <memory>
#include <string>

namespace osef::editor {

class Log;

class ProcessRunner {
public:
    ProcessRunner();
    ~ProcessRunner();  // cancels a running job and joins its threads
    ProcessRunner(const ProcessRunner&) = delete;
    ProcessRunner& operator=(const ProcessRunner&) = delete;

    // Starts `command` via the platform shell in `cwd`. Returns false (and
    // logs why) if a job is already running or it cannot be started.
    bool start(const std::string& label, const std::string& command,
               const std::filesystem::path& cwd, Log& log);
    bool running() const;
    void cancel();               // terminates the job's whole process group
    void poll();                 // UI thread, every frame: reports finished jobs
    int last_exit_code() const;  // -1 while running or before the first job
    std::string current_label() const;
    double elapsed_seconds() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace osef::editor
