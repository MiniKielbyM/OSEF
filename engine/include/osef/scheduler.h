#pragma once
// Scheduling policy is pluggable: implement Scheduler::pick to design your own.

#include <osef/process.h>

namespace osef {

class Scheduler {
public:
    virtual ~Scheduler() = default;
    virtual const char* name() const = 0;
    // Choose which ready process gets the next quantum. `ready` is never empty.
    virtual Process* pick(const Vector<Process*>& ready) = 0;
};

// Every ready process gets a turn, in order of whoever waited longest.
class RoundRobinScheduler final : public Scheduler {
public:
    const char* name() const override { return "round-robin"; }
    Process* pick(const Vector<Process*>& ready) override;
};

// Highest priority wins; ties fall back to round-robin. Low-priority
// processes can starve. Adding aging is a good first exercise.
class PriorityScheduler final : public Scheduler {
public:
    const char* name() const override { return "priority"; }
    Process* pick(const Vector<Process*>& ready) override;
};

// Each process holds (priority + 1) tickets (minimum 1); a random ticket wins.
class LotteryScheduler final : public Scheduler {
public:
    explicit LotteryScheduler(uint64_t seed = 0x05EF) : rng_(seed) {}
    const char* name() const override { return "lottery"; }
    Process* pick(const Vector<Process*>& ready) override;

private:
    Random rng_;
};

} // namespace osef
