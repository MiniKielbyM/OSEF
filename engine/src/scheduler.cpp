#include <osef/scheduler.h>

namespace osef {

Process* RoundRobinScheduler::pick(const Vector<Process*>& ready) {
    Process* best = ready.front();
    for (Process* p : ready)
        if (p->info().last_run < best->info().last_run) best = p;
    return best;
}

Process* PriorityScheduler::pick(const Vector<Process*>& ready) {
    Process* best = ready.front();
    for (Process* p : ready) {
        const auto& a = p->info();
        const auto& b = best->info();
        if (a.priority > b.priority || (a.priority == b.priority && a.last_run < b.last_run))
            best = p;
    }
    return best;
}

Process* LotteryScheduler::pick(const Vector<Process*>& ready) {
    auto tickets = [](const Process* p) -> uint64_t {
        int t = p->info().priority + 1;
        return t < 1 ? 1 : static_cast<uint64_t>(t);
    };
    uint64_t total = 0;
    for (Process* p : ready) total += tickets(p);
    uint64_t draw = rng_.below(total);
    for (Process* p : ready) {
        uint64_t t = tickets(p);
        if (draw < t) return p;
        draw -= t;
    }
    return ready.back();
}

} // namespace osef
