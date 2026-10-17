#!/usr/bin/env python3
"""USB + custom-interrupt boot test (run by tests/iso/devices_test.sh).

Boots build/devicesos.iso headless, types commands *on the emulated USB
keyboard* through QEMU's QMP send-key (the OS has no PS/2 keyboard), and
checks the serial log. Exit status 0 = all checks passed.
"""
import json, os, socket, subprocess, sys, time

image, qemu, ovmf, build = sys.argv[1:5]
sock = os.path.join(build, "devicesos-qmp.sock")
log_path = os.path.join(build, "devicesos-serial.log")
if os.path.exists(sock):
    os.remove(sock)
log = open(log_path, "wb")
cmd = [image, "run", os.path.join(build, "devicesos.iso"), "--kernel", os.path.join(build, "devicesos.elf"),
       "--test", "--timeout", "90", "--qemu", qemu]
if ovmf:
    cmd += ["--ovmf", ovmf]
cmd += ["--", "-qmp", "unix:%s,server=on,wait=off" % sock]
proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.DEVNULL)

def text():
    data = open(log_path, "rb").read().decode("latin-1")
    out, i = [], 0
    while i < len(data):  # strip ANSI escape sequences
        if data[i] == "\x1b" and i + 1 < len(data) and data[i + 1] == "[":
            i += 2
            while i < len(data) and not data[i].isalpha():
                i += 1
            i += 1
        else:
            out.append(data[i]); i += 1
    return "".join(out).replace("\r", "")

def wait_for(needle, seconds):
    end = time.time() + seconds
    while time.time() < end and proc.poll() is None:
        if needle in text():
            return True
        time.sleep(0.2)
    return needle in text()

ok = wait_for("DevicesOS:/$ ", 60)
qmp = None
if ok:
    for _ in range(50):
        try:
            qmp = socket.socket(socket.AF_UNIX); qmp.connect(sock); break
        except OSError:
            time.sleep(0.1)
    f = qmp.makefile("rw")
    f.readline()
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); f.flush(); f.readline()
    names = {" ": "spc", "\n": "ret", "-": "minus", ".": "dot", "0": "0"}
    def type_line(s):
        for ch in s + "\n":
            f.write(json.dumps({"execute": "send-key",
                                "arguments": {"keys": [{"type": "qcode", "data": names.get(ch, ch)}]}}) + "\n")
            f.flush(); f.readline(); time.sleep(0.08)
    prompts = text().count("DevicesOS:/$ ")
    for line in ["lsusb", "int 0x80", "int 0x80", "irq", "ticks", "shutdown 4"]:
        type_line(line)
        end = time.time() + 20
        while time.time() < end and proc.poll() is None and text().count("DevicesOS:/$ ") <= prompts:
            time.sleep(0.2)
        prompts = text().count("DevicesOS:/$ ")
try:
    proc.wait(timeout=30)
except subprocess.TimeoutExpired:
    proc.kill(); proc.wait()
t = text()

checks = [
    ("xHCI controller started", "USB: xHCI 1b36:000d ready" in t),
    ("USB keyboard enumerated with its product string", "QEMU USB Keyboard" in t and "keyboard input enabled" in t),
    ("USB tablet enumerated", "QEMU USB Tablet" in t),
    ("commands typed on the USB keyboard reach the shell", "DevicesOS:/$ lsusb" in t),
    ("lsusb lists the keyboard", "ID 0627:0001 QEMU QEMU USB Keyboard  [HID keyboard" in t),
    ("int 0x80 runs the custom vector handler", "custom vector 0x80 x1" in t),
    ("irq lists the soft vector with its count", "0x80  soft         2  test-vector" in t),
    ("irq lists the hardware IRQ 0 hook", "0x20  IRQ 0" in t and "timer-hook" in t),
    ("the IRQ 0 hook sees timer interrupts", "timer hook: counting" in t),
    ("QEMU exit code is 4 from 'shutdown 4'", proc.returncode == 4),
]
failed = 0
for what, good in checks:
    print(("  ok   " if good else "  FAIL ") + what)
    failed += 0 if good else 1
print("devicesos: %s" % ("all checks passed" if not failed else "%d check(s) failed (log: %s)" % (failed, log_path)))
sys.exit(1 if failed else 0)
