#pragma once
// Inter-process communication: per-process mailboxes plus a name registry
// for services (microkernel style: "time", "net", "fs", ...).

#include <osef/process.h>

namespace osef {

struct Message {
    Pid from = kNoPid;
    uint32_t type = 0;
    String data;
};

class IPC {
public:
    void post(Pid to, Message msg) { boxes_[to].push_back(osef::move(msg)); }
    bool take(Pid self, Message& out);
    size_t pending(Pid pid) const;

    bool register_service(StringView name, Pid pid); // false if taken
    Pid lookup(StringView name) const;
    const Map<String, Pid>& services() const { return services_; }

    // Called by the kernel when a process dies.
    void release(Pid pid);

private:
    Map<Pid, Deque<Message>> boxes_;
    Map<String, Pid> services_;
};

} // namespace osef
