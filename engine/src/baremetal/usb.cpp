// xHCI USB host controller driver (polling, no MSI): takes the controller
// from the firmware, sets up the command and event rings, enumerates every
// device on the root hub ports (descriptors + strings), and drives USB HID
// boot-protocol keyboards as an input device. Reference: xHCI 1.2, USB 2.0
// ch. 9, USB HID 1.11 (boot protocol, appendix B).
//
// Hubs, mass storage and isochronous transfers are not implemented yet.

#include "arch.h"
#include "cpu.h"
#include "internal.h"

namespace osef::bm {

namespace {

// ---------------------------------------------------------------- helpers

constexpr uint32_t kRingTrbs = 256;  // one 4 KiB page per ring

struct Trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};
static_assert(sizeof(Trb) == 16, "TRB layout");

enum TrbType : uint32_t {
    kNormal = 1, kSetup = 2, kData = 3, kStatus = 4, kLink = 6,
    kEnableSlot = 9, kAddressDevice = 11, kConfigureEndpoint = 12, kEvaluateContext = 13,
    kTransferEvent = 32, kCommandCompletion = 33, kPortStatusChange = 34,
};
constexpr uint32_t kCycle = 1u << 0;
constexpr uint32_t kIoc = 1u << 5;
constexpr uint32_t kIdt = 1u << 6;
constexpr uint32_t kSuccess = 1, kShortPacket = 13;

uint32_t trb_type(const Trb& t) { return (t.control >> 10) & 0x3F; }

uint64_t phys_of(const void* p) { return reinterpret_cast<uint64_t>(p) - hhdm_offset(); }

void* dma_alloc(size_t size) {
    size = (size + 4095) & ~size_t(4095);
    auto* p = static_cast<uint8_t*>(heap_alloc(size, 4096));
    for (size_t i = 0; i < size; ++i) p[i] = 0;
    return p;
}

inline uint32_t rd32(volatile uint8_t* base, uint32_t off) {
    return *reinterpret_cast<volatile uint32_t*>(base + off);
}
inline void wr32(volatile uint8_t* base, uint32_t off, uint32_t v) {
    *reinterpret_cast<volatile uint32_t*>(base + off) = v;
}
inline void wr64(volatile uint8_t* base, uint32_t off, uint64_t v) {
    wr32(base, off, static_cast<uint32_t>(v));
    wr32(base, off + 4, static_cast<uint32_t>(v >> 32));
}

// Busy-waits (interrupts are on, so ticks_ms advances) until cond() or timeout.
template <class Cond>
bool wait_for(Cond cond, uint64_t timeout_ms) {
    const uint64_t deadline = ticks_ms() + timeout_ms;
    while (!cond()) {
        if (ticks_ms() >= deadline) return cond();
        cpu::pause();
    }
    return true;
}

// A producer ring (command or transfer) with a link TRB back to the start.
struct TrbRing {
    Trb* trbs = nullptr;
    uint32_t index = 0;
    uint32_t cycle = 1;

    void init() {
        trbs = static_cast<Trb*>(dma_alloc(sizeof(Trb) * kRingTrbs));
        Trb& link = trbs[kRingTrbs - 1];
        link.param = phys_of(trbs);
        link.control = (kLink << 10) | (1u << 1);  // toggle cycle
        index = 0;
        cycle = 1;
    }
    // Returns the physical address of the enqueued TRB.
    uint64_t push(uint64_t param, uint32_t status, uint32_t control) {
        Trb& t = trbs[index];
        t.param = param;
        t.status = status;
        asm volatile("" ::: "memory");
        t.control = (control & ~kCycle) | cycle;  // the cycle bit hands the TRB to the controller
        const uint64_t at = phys_of(&t);
        if (++index == kRingTrbs - 1) {
            Trb& link = trbs[kRingTrbs - 1];
            link.control = (link.control & ~kCycle) | cycle;
            index = 0;
            cycle ^= 1;
        }
        return at;
    }
};

// ---------------------------------------------------------------- controller state

struct Endpoint {
    TrbRing ring;
    uint8_t dci = 0;
    uint8_t* buffer = nullptr;
    uint16_t max_packet = 0;
};

struct Device {
    bool used = false;
    uint8_t slot = 0, port = 0, speed = 0;
    uint8_t* out_ctx = nullptr;   // device context (owned by the controller)
    uint8_t* in_ctx = nullptr;    // input context (ours)
    TrbRing ep0;
    uint8_t* xfer = nullptr;      // 4 KiB buffer for control transfers
    UsbInfo info{};
    // HID boot keyboard
    bool keyboard = false;
    Endpoint kbd;
    uint8_t last_report[8] = {};
};

constexpr uint32_t kMaxDevices = 16;

struct Controller {
    bool ready = false;
    volatile uint8_t* cap = nullptr;
    volatile uint8_t* op = nullptr;
    volatile uint8_t* rt = nullptr;
    volatile uint32_t* db = nullptr;
    uint32_t ctx_size = 32;
    uint32_t max_slots = 0, max_ports = 0;
    uint64_t* dcbaa = nullptr;
    TrbRing cmd;
    Trb* events = nullptr;
    uint32_t event_index = 0;
    uint32_t event_cycle = 1;
    Device devices[kMaxDevices];
    uint32_t device_count = 0;
    // Result of the last completed command / control transfer we waited for.
    bool cmd_done = false;
    uint64_t cmd_trb = 0;
    uint32_t cmd_code = 0;
    uint8_t cmd_slot = 0;
    bool xfer_done = false;
    uint8_t xfer_slot = 0, xfer_dci = 0;
    uint32_t xfer_code = 0, xfer_residue = 0;
};

Controller g;
Ring<128> g_chars;  // decoded USB keyboard input

uint32_t op_portsc(uint32_t port) { return 0x400 + 0x10 * (port - 1); }

uint8_t* ctx_at(uint8_t* base, uint32_t index) { return base + index * g.ctx_size; }
uint32_t* dw(uint8_t* ctx) { return reinterpret_cast<uint32_t*>(ctx); }

Device* device_for_slot(uint8_t slot) {
    for (Device& d : g.devices)
        if (d.used && d.slot == slot) return &d;
    return nullptr;
}

void handle_keyboard_report(Device& d);

// ---------------------------------------------------------------- events

void process_events() {
    bool any = false;
    for (;;) {
        Trb& e = g.events[g.event_index];
        if ((e.control & kCycle) != g.event_cycle) break;
        any = true;
        const uint32_t type = trb_type(e);
        const uint32_t code = e.status >> 24;
        if (type == kCommandCompletion) {
            g.cmd_done = true;
            g.cmd_trb = e.param;
            g.cmd_code = code;
            g.cmd_slot = static_cast<uint8_t>(e.control >> 24);
        } else if (type == kTransferEvent) {
            const uint8_t slot = static_cast<uint8_t>(e.control >> 24);
            const uint8_t dci = static_cast<uint8_t>((e.control >> 16) & 0x1F);
            Device* d = device_for_slot(slot);
            if (d && d->keyboard && dci == d->kbd.dci) {
                if (code == kSuccess || code == kShortPacket) handle_keyboard_report(*d);
                // Re-arm the interrupt endpoint for the next report.
                d->kbd.ring.push(phys_of(d->kbd.buffer), 8, (kNormal << 10) | kIoc | (1u << 2));
                g.db[slot] = dci;
            } else {
                g.xfer_done = true;
                g.xfer_slot = slot;
                g.xfer_dci = dci;
                g.xfer_code = code;
                g.xfer_residue = e.status & 0xFFFFFF;
            }
        }
        // Port status change events need no action: ports are scanned at init.
        if (++g.event_index == kRingTrbs) {
            g.event_index = 0;
            g.event_cycle ^= 1;
        }
    }
    if (any) wr64(g.rt, 0x20 + 0x18, phys_of(&g.events[g.event_index]) | (1u << 3));  // ERDP, clear EHB
}

bool command(uint64_t param, uint32_t status, uint32_t control, uint8_t* slot_out = nullptr) {
    g.cmd_done = false;
    const uint64_t at = g.cmd.push(param, status, control);
    g.db[0] = 0;
    const bool ok = wait_for(
        [&] {
            process_events();
            return g.cmd_done && g.cmd_trb == at;
        },
        1000);
    if (!ok || g.cmd_code != kSuccess) return false;
    if (slot_out) *slot_out = g.cmd_slot;
    return true;
}

// A control transfer on EP0. `data` points into d.xfer (IN) or is copied there (OUT).
bool control(Device& d, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, uint16_t length,
             uint32_t* actual = nullptr) {
    const bool in = (request_type & 0x80) != 0;
    const uint64_t setup = request_type | (uint64_t(request) << 8) | (uint64_t(value) << 16) |
                           (uint64_t(index) << 32) | (uint64_t(length) << 48);
    const uint32_t trt = length == 0 ? 0 : in ? 3 : 2;
    g.xfer_done = false;
    d.ep0.push(setup, 8, (kSetup << 10) | kIdt | (trt << 16));
    if (length) d.ep0.push(phys_of(d.xfer), length, (kData << 10) | (in ? (1u << 16) : 0));
    d.ep0.push(0, 0, (kStatus << 10) | kIoc | ((length && in) ? 0 : (1u << 16)));
    g.db[d.slot] = 1;  // EP0 = DCI 1
    const bool ok = wait_for(
        [&] {
            process_events();
            return g.xfer_done && g.xfer_slot == d.slot && g.xfer_dci == 1;
        },
        1000);
    if (!ok || (g.xfer_code != kSuccess && g.xfer_code != kShortPacket)) return false;
    if (actual) *actual = length >= g.xfer_residue ? length - g.xfer_residue : 0;
    return true;
}

bool get_descriptor(Device& d, uint8_t type, uint8_t index, uint16_t lang, uint16_t length, uint32_t* actual = nullptr) {
    return control(d, 0x80, 6, static_cast<uint16_t>((type << 8) | index), lang, length, actual);
}

void read_string(Device& d, uint8_t index, char* out, size_t cap) {
    out[0] = '\0';
    if (!index || cap == 0) return;
    if (!get_descriptor(d, 3, 0, 0, 4) || d.xfer[0] < 4) return;  // language IDs
    const uint16_t lang = static_cast<uint16_t>(d.xfer[2] | (d.xfer[3] << 8));
    uint32_t got = 0;
    if (!get_descriptor(d, 3, index, lang, 255, &got) || got < 2) return;
    const uint32_t len = d.xfer[0] < got ? d.xfer[0] : got;
    size_t n = 0;
    for (uint32_t i = 2; i + 1 < len && n + 1 < cap; i += 2) {
        const uint16_t ch = static_cast<uint16_t>(d.xfer[i] | (d.xfer[i + 1] << 8));
        out[n++] = ch >= 0x20 && ch < 0x7F ? static_cast<char>(ch) : '?';
    }
    out[n] = '\0';
}

// ---------------------------------------------------------------- HID boot keyboard

char usage_to_ascii(uint8_t usage, bool shift) {
    if (usage >= 4 && usage <= 29) return static_cast<char>((shift ? 'A' : 'a') + (usage - 4));
    static const char digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";
    if (usage >= 30 && usage <= 39) return (shift ? shifted_digits : digits)[usage - 30];
    switch (usage) {
    case 40: return '\n';
    case 42: return '\b';
    case 43: return '\t';
    case 44: return ' ';
    case 45: return shift ? '_' : '-';
    case 46: return shift ? '+' : '=';
    case 47: return shift ? '{' : '[';
    case 48: return shift ? '}' : ']';
    case 49: return shift ? '|' : '\\';
    case 51: return shift ? ':' : ';';
    case 52: return shift ? '"' : '\'';
    case 53: return shift ? '~' : '`';
    case 54: return shift ? '<' : ',';
    case 55: return shift ? '>' : '.';
    case 56: return shift ? '?' : '/';
    case 88: return '\n';  // keypad Enter
    default: return 0;
    }
}

void handle_keyboard_report(Device& d) {
    const uint8_t* r = d.kbd.buffer;
    const bool shift = (r[0] & 0x22) != 0;
    for (int i = 2; i < 8; ++i) {
        const uint8_t usage = r[i];
        if (usage < 4) continue;  // none / rollover error
        bool held = false;
        for (int j = 2; j < 8; ++j) held |= d.last_report[j] == usage;
        if (held) continue;  // only new key presses
        if (const char c = usage_to_ascii(usage, shift)) g_chars.push(c);
    }
    for (int i = 0; i < 8; ++i) d.last_report[i] = r[i];
}

// Sets up the boot-protocol keyboard's interrupt IN endpoint.
bool start_keyboard(Device& d, uint8_t config_value, uint8_t interface, uint8_t ep_address, uint16_t max_packet,
                    uint8_t interval) {
    if (!control(d, 0x00, 9, config_value, 0, 0)) return false;            // SET_CONFIGURATION
    control(d, 0x21, 0x0B, 0, interface, 0);                                // SET_PROTOCOL(boot)
    control(d, 0x21, 0x0A, 0, interface, 0);                                // SET_IDLE(0)

    const uint8_t dci = static_cast<uint8_t>((ep_address & 0x0F) * 2 + 1);
    d.kbd.dci = dci;
    d.kbd.max_packet = max_packet;
    d.kbd.ring.init();
    d.kbd.buffer = static_cast<uint8_t*>(dma_alloc(64));

    // xHCI interval is 2^n * 125 us. FS/LS bInterval is in 1 ms frames.
    uint32_t xi = 0;
    if (d.speed == 3 || d.speed >= 4) {
        xi = interval ? interval - 1u : 0u;
    } else {
        uint32_t frames = interval ? interval : 1, log2 = 0;
        while ((1u << (log2 + 1)) <= frames) ++log2;
        xi = log2 + 3;
    }
    if (xi > 15) xi = 15;

    for (uint32_t i = 0; i < 33 * g.ctx_size; ++i) d.in_ctx[i] = 0;
    dw(ctx_at(d.in_ctx, 0))[1] = 1u | (1u << dci);                          // add slot + endpoint
    uint8_t* slot = ctx_at(d.in_ctx, 1);
    const uint32_t* out_slot = dw(ctx_at(d.out_ctx, 0));
    for (int i = 0; i < 4; ++i) dw(slot)[i] = out_slot[i];
    dw(slot)[0] = (dw(slot)[0] & ~(0x1Fu << 27)) | (uint32_t(dci) << 27);  // context entries
    uint32_t* ep = dw(ctx_at(d.in_ctx, 1 + dci));
    ep[0] = xi << 16;
    ep[1] = (3u << 1) | (7u << 3) | (uint32_t(max_packet) << 16);          // CErr 3, interrupt IN
    const uint64_t deq = phys_of(d.kbd.ring.trbs) | 1;
    ep[2] = static_cast<uint32_t>(deq);
    ep[3] = static_cast<uint32_t>(deq >> 32);
    ep[4] = 8u | (uint32_t(max_packet) << 16);                               // avg TRB len, max ESIT payload
    if (!command(phys_of(d.in_ctx), 0, (kConfigureEndpoint << 10) | (uint32_t(d.slot) << 24))) return false;

    d.keyboard = true;
    d.kbd.ring.push(phys_of(d.kbd.buffer), 8, (kNormal << 10) | kIoc | (1u << 2));
    g.db[d.slot] = dci;
    return true;
}

// ---------------------------------------------------------------- enumeration

const char* speed_name(uint8_t s) {
    switch (s) {
    case 1: return "full-speed";
    case 2: return "low-speed";
    case 3: return "high-speed";
    case 4: return "super-speed";
    default: return "super-speed+";
    }
}

void enumerate_port(uint32_t port) {
    uint32_t sc = rd32(g.op, op_portsc(port));
    if (!(sc & 1)) return;  // nothing connected
    constexpr uint32_t kChange = 0x00FE0000u;  // RW1C change bits 17-23
    if (!(sc & (1u << 1))) {  // not enabled yet (USB2 ports need a reset)
        wr32(g.op, op_portsc(port), (sc & ~(kChange | (1u << 1))) | (1u << 4));
        if (!wait_for([&] { return (rd32(g.op, op_portsc(port)) & (1u << 21)) != 0; }, 500)) return;
        sc = rd32(g.op, op_portsc(port));
        if (!(sc & (1u << 1))) return;
    }
    wr32(g.op, op_portsc(port), (sc & ~(1u << 1)) | kChange);  // acknowledge changes
    if (g.device_count >= kMaxDevices) return;
    Device& d = g.devices[g.device_count];
    d = Device{};
    d.port = static_cast<uint8_t>(port);
    d.speed = static_cast<uint8_t>((sc >> 10) & 0xF);

    uint8_t slot = 0;
    if (!command(0, 0, kEnableSlot << 10, &slot) || slot == 0 || slot > g.max_slots) return;
    d.used = true;
    d.slot = slot;
    ++g.device_count;
    d.out_ctx = static_cast<uint8_t*>(dma_alloc(32 * g.ctx_size));
    d.in_ctx = static_cast<uint8_t*>(dma_alloc(33 * g.ctx_size));
    d.xfer = static_cast<uint8_t*>(dma_alloc(4096));
    d.ep0.init();
    g.dcbaa[slot] = phys_of(d.out_ctx);

    const uint16_t mps0 = d.speed >= 4 ? 512 : d.speed == 3 ? 64 : 8;
    dw(ctx_at(d.in_ctx, 0))[1] = 0x3;                                         // add slot + EP0
    uint32_t* sl = dw(ctx_at(d.in_ctx, 1));
    sl[0] = (uint32_t(d.speed) << 20) | (1u << 27);                          // speed, 1 context entry
    sl[1] = uint32_t(port) << 16;                                            // root hub port
    uint32_t* ep0 = dw(ctx_at(d.in_ctx, 2));
    ep0[1] = (3u << 1) | (4u << 3) | (uint32_t(mps0) << 16);                 // CErr 3, control
    const uint64_t deq = phys_of(d.ep0.trbs) | 1;
    ep0[2] = static_cast<uint32_t>(deq);
    ep0[3] = static_cast<uint32_t>(deq >> 32);
    ep0[4] = 8;
    if (!command(phys_of(d.in_ctx), 0, (kAddressDevice << 10) | (uint32_t(slot) << 24))) {
        log("USB: port %u: Address Device failed", static_cast<unsigned>(port));
        return;
    }

    // Device descriptor: first 8 bytes for bMaxPacketSize0, then all 18.
    if (!get_descriptor(d, 1, 0, 0, 8)) return;
    const uint16_t real_mps = d.speed >= 4 ? static_cast<uint16_t>(1u << d.xfer[7]) : d.xfer[7];
    if (real_mps && real_mps != mps0) {
        dw(ctx_at(d.in_ctx, 0))[0] = 0;
        dw(ctx_at(d.in_ctx, 0))[1] = 0x2;  // evaluate EP0
        ep0[1] = (ep0[1] & 0xFFFFu) | (uint32_t(real_mps) << 16);
        command(phys_of(d.in_ctx), 0, (kEvaluateContext << 10) | (uint32_t(slot) << 24));
    }
    if (!get_descriptor(d, 1, 0, 0, 18)) return;
    UsbInfo& info = d.info;
    info.port = d.port;
    info.slot = d.slot;
    info.speed = d.speed;
    info.vendor = static_cast<uint16_t>(d.xfer[8] | (d.xfer[9] << 8));
    info.product = static_cast<uint16_t>(d.xfer[10] | (d.xfer[11] << 8));
    info.device_class = d.xfer[4];
    info.device_subclass = d.xfer[5];
    info.device_protocol = d.xfer[6];
    const uint8_t i_manufacturer = d.xfer[14], i_product = d.xfer[15];

    // Configuration 1: find the first interface (class info) and a HID boot keyboard.
    uint8_t config_value = 1, kbd_interface = 0, kbd_ep = 0, kbd_interval = 10;
    uint16_t kbd_mps = 8;
    bool kbd = false;
    if (get_descriptor(d, 2, 0, 0, 9)) {
        const uint16_t total = static_cast<uint16_t>(d.xfer[2] | (d.xfer[3] << 8));
        uint32_t got = 0;
        if (get_descriptor(d, 2, 0, 0, total > 4096 ? 4096 : total, &got)) {
            config_value = d.xfer[5];
            bool first_if = true, in_kbd_if = false;
            for (uint32_t off = 0; off + 2 <= got && d.xfer[off] >= 2; off += d.xfer[off]) {
                const uint8_t* desc = d.xfer + off;
                if (desc[1] == 4 && desc[0] >= 9) {  // interface
                    if (first_if && info.device_class == 0) {
                        info.device_class = desc[5];
                        info.device_subclass = desc[6];
                        info.device_protocol = desc[7];
                    }
                    first_if = false;
                    in_kbd_if = desc[5] == 3 && desc[6] == 1 && desc[7] == 1 && !kbd;
                    if (in_kbd_if) kbd_interface = desc[2];
                } else if (desc[1] == 5 && desc[0] >= 7 && in_kbd_if && !kbd) {  // endpoint
                    if ((desc[2] & 0x80) && (desc[3] & 3) == 3) {                 // interrupt IN
                        kbd = true;
                        kbd_ep = desc[2];
                        kbd_mps = static_cast<uint16_t>((desc[4] | (desc[5] << 8)) & 0x7FF);
                        kbd_interval = desc[6];
                    }
                }
            }
        }
    }
    read_string(d, i_manufacturer, info.manufacturer, sizeof(info.manufacturer));
    read_string(d, i_product, info.name, sizeof(info.name));
    if (kbd) {
        info.keyboard = start_keyboard(d, config_value, kbd_interface, kbd_ep, kbd_mps, kbd_interval);
    } else {
        control(d, 0x00, 9, config_value, 0, 0);  // configure it anyway
    }
    log("USB: port %u: %04x:%04x %s %s (%s)%s", static_cast<unsigned>(port), static_cast<unsigned>(info.vendor),
        static_cast<unsigned>(info.product), info.manufacturer[0] ? info.manufacturer : "?",
        info.name[0] ? info.name : "device", speed_name(d.speed), info.keyboard ? ", keyboard input enabled" : "");
}

bool init_controller(const PciInfo& pci) {
    // Memory decoding + bus mastering.
    pci_write32(pci, 0x04, pci_read32(pci, 0x04) | 0x6);
    const uint32_t bar0 = pci_read32(pci, 0x10);
    uint64_t base = bar0 & ~0xFull;
    if (((bar0 >> 1) & 3) == 2) base |= uint64_t(pci_read32(pci, 0x14)) << 32;
    pci_write32(pci, 0x10, 0xFFFFFFFF);
    const uint32_t mask = pci_read32(pci, 0x10) & ~0xFu;
    pci_write32(pci, 0x10, bar0);
    const uint64_t size = mask ? uint64_t(~mask) + 1 : 0x10000;
    if (!base) return false;
    g.cap = static_cast<volatile uint8_t*>(map_mmio(base, size < 0x10000 ? 0x10000 : size));

    const uint32_t caplength = g.cap[0];
    const uint32_t hcs1 = rd32(g.cap, 0x04), hcs2 = rd32(g.cap, 0x08), hcc1 = rd32(g.cap, 0x10);
    g.op = g.cap + caplength;
    g.rt = g.cap + (rd32(g.cap, 0x18) & ~0x1Fu);
    g.db = reinterpret_cast<volatile uint32_t*>(g.cap + (rd32(g.cap, 0x14) & ~0x3u));
    g.max_slots = hcs1 & 0xFF;
    g.max_ports = hcs1 >> 24;
    g.ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;
    if (g.max_slots > kMaxDevices) g.max_slots = kMaxDevices;

    // Take ownership from the firmware (USB legacy support capability).
    for (uint32_t off = (hcc1 >> 16) << 2; off;) {
        const uint32_t v = rd32(g.cap, off);
        if ((v & 0xFF) == 1) {
            wr32(g.cap, off, v | (1u << 24));
            wait_for([&] { return (rd32(g.cap, off) & (1u << 16)) == 0; }, 1000);
            wr32(g.cap, off + 4, rd32(g.cap, off + 4) & ~0x0000E01Fu);  // no SMIs
        }
        const uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + (next << 2) : 0;
    }

    // Stop, reset.
    wr32(g.op, 0x00, rd32(g.op, 0x00) & ~1u);
    if (!wait_for([&] { return (rd32(g.op, 0x04) & 1u) != 0; }, 100)) return false;
    wr32(g.op, 0x00, 1u << 1);
    if (!wait_for([&] { return (rd32(g.op, 0x00) & (1u << 1)) == 0 && (rd32(g.op, 0x04) & (1u << 11)) == 0; }, 1000))
        return false;

    wr32(g.op, 0x38, g.max_slots);
    g.dcbaa = static_cast<uint64_t*>(dma_alloc(8 * 256));
    const uint32_t scratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (scratch) {
        auto* array = static_cast<uint64_t*>(dma_alloc(8 * scratch));
        for (uint32_t i = 0; i < scratch; ++i) array[i] = phys_of(dma_alloc(4096));
        g.dcbaa[0] = phys_of(array);
    }
    wr64(g.op, 0x30, phys_of(g.dcbaa));

    g.cmd.init();
    wr64(g.op, 0x18, phys_of(g.cmd.trbs) | 1);

    g.events = static_cast<Trb*>(dma_alloc(sizeof(Trb) * kRingTrbs));
    auto* erst = static_cast<uint64_t*>(dma_alloc(64));
    erst[0] = phys_of(g.events);
    erst[1] = kRingTrbs;
    wr32(g.rt, 0x20 + 0x08, 1);                                   // ERSTSZ
    wr64(g.rt, 0x20 + 0x18, phys_of(g.events));                   // ERDP
    wr64(g.rt, 0x20 + 0x10, phys_of(erst));                       // ERSTBA
    wr32(g.rt, 0x20 + 0x00, rd32(g.rt, 0x20) & ~2u);              // IMAN.IE = 0: we poll

    wr32(g.op, 0x00, 1u);                                         // run
    if (!wait_for([&] { return (rd32(g.op, 0x04) & 1u) == 0; }, 100)) return false;
    g.ready = true;

    // Let connections settle (ports report devices shortly after power-up).
    const uint64_t settle = ticks_ms() + 100;
    while (ticks_ms() < settle) cpu::pause();
    for (uint32_t port = 1; port <= g.max_ports; ++port) enumerate_port(port);
    return true;
}

} // namespace

void usb_init() {
    PciInfo pci{};
    if (!pci_find_class(0x0C, 0x03, 0x30, pci)) {
        log("USB: no xHCI controller found (QEMU: -device qemu-xhci)");
        return;
    }
    if (!init_controller(pci)) {
        log("USB: xHCI controller %04x:%04x did not initialize", static_cast<unsigned>(pci.vendor_id),
            static_cast<unsigned>(pci.device_id));
        return;
    }
    log("USB: xHCI %04x:%04x ready, %u port(s), %u device(s)", static_cast<unsigned>(pci.vendor_id),
        static_cast<unsigned>(pci.device_id), static_cast<unsigned>(g.max_ports),
        static_cast<unsigned>(g.device_count));
}

void usb_poll() {
    if (g.ready) process_events();
}

bool usb_getc(char& c) {
    usb_poll();
    return g_chars.pop(c);
}

size_t usb_list(UsbInfo* out, size_t capacity) {
    size_t n = 0;
    for (const Device& d : g.devices)
        if (d.used && d.info.vendor) {
            if (out && n < capacity) out[n] = d.info;
            ++n;
        }
    return n;
}

} // namespace osef::bm
