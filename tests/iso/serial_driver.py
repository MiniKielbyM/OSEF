#!/usr/bin/env python3
"""Drive a headless QEMU boot over its serial console (used by boot_test.sh).

    serial_driver.py --log LOG [--screenshot OUT.ppm] [--timeout S]
                     (--commands FILE | --observe S) -- <command...>

The command (normally `osef-image run --test ...`) must have the guest's
serial port on its stdin/stdout. Every argument equal to @QMP@ is replaced by
the path of a QMP unix socket created by this script, so the command can pass
`-qmp unix:@QMP@,server=on,wait=off` to QEMU; it is used for screendumps.

--commands FILE   one shell command per line; each is typed (followed by CR)
                  as soon as the OS shows its next shell prompt. A line
                  "@screendump" saves a framebuffer screenshot instead. After
                  the last line the script waits for the VM to exit.
                  "@raw <text>" types <text> with Python escapes decoded
                  (e.g. "@raw echo a\\x1b[Db" sends an arrow-key sequence).
--prompt TEXT     what the end of a shell prompt looks like (Python escapes),
                  for OSes with a custom Appearance::prompt.
--observe S       type nothing: watch the boot for S seconds (or until the
                  VM exits), take the screenshot, then stop the VM.

All serial output is written to LOG. Exit status: the command's own exit
status, or 124 if it had to be killed (timeout, or the end of --observe).
"""
import argparse
import codecs
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

PROMPT = b"\x1b[0m$ "  # end of the OSEF shell prompt: <os>:<cwd>$ (after the color reset)
# Themed OSes (Appearance colors) follow the reset with their own SGR codes.
PROMPT_RE = re.compile(rb"\x1b\[0m(?:\x1b\[[0-9;]*m)*\$ ")


class Output:
    def __init__(self, stream, log_path, prompt=PROMPT):
        self.prompt = prompt
        self.data = bytearray()
        self.lock = threading.Lock()
        self.log = open(log_path, "wb")
        self.thread = threading.Thread(target=self._pump, args=(stream,), daemon=True)
        self.thread.start()

    def _pump(self, stream):
        fd = stream.fileno()
        while True:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            with self.lock:
                self.data += chunk
            self.log.write(chunk)
            self.log.flush()

    def prompts(self):
        with self.lock:
            if self.prompt == PROMPT:
                return len(PROMPT_RE.findall(self.data))
            return self.data.count(self.prompt)

    def close(self):
        self.thread.join(timeout=5)
        self.log.close()


def qmp_screendump(sock_path, out_path, timeout=10.0):
    deadline = time.time() + timeout
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    while True:
        try:
            s.connect(sock_path)
            break
        except OSError:
            if time.time() > deadline:
                raise RuntimeError("cannot connect to QMP socket " + sock_path)
            time.sleep(0.2)
    s.settimeout(timeout)
    f = s.makefile("rwb")

    def call(cmd):
        f.write(json.dumps(cmd).encode() + b"\n")
        f.flush()
        while True:
            line = f.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            msg = json.loads(line)
            if "return" in msg:
                return msg["return"]
            if "error" in msg:
                raise RuntimeError("QMP error: %s" % msg["error"])

    json.loads(f.readline())  # greeting
    call({"execute": "qmp_capabilities"})
    call({"execute": "screendump", "arguments": {"filename": out_path}})
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--screenshot")
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--step-timeout", type=float, default=30.0)
    ap.add_argument("--prompt", help="text that ends a shell prompt (Python escapes; default: the OSEF shell's)")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--commands")
    group.add_argument("--observe", type=float)
    ap.add_argument("command", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        ap.error("missing command after --")

    tmp = tempfile.mkdtemp(prefix="osef-qmp-", dir="/tmp")
    sock = os.path.join(tmp, "qmp.sock")
    command = [sock if a == "@QMP@" else a.replace("@QMP@", sock) for a in command]

    steps = []
    if args.commands:
        with open(args.commands) as f:
            steps = [line.rstrip("\n") for line in f if line.strip() and not line.startswith("#")]

    start = time.time()
    # Own process group, so a kill also reaches QEMU when the command is osef-image.
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, start_new_session=True)
    prompt = codecs.decode(args.prompt, "unicode_escape").encode("latin-1") if args.prompt else PROMPT
    out = Output(proc.stdout, args.log, prompt)
    killed = False

    def remaining():
        return args.timeout - (time.time() - start)

    def screendump():
        if not args.screenshot:
            return
        time.sleep(0.5)  # let the framebuffer console catch up
        try:
            qmp_screendump(sock, os.path.abspath(args.screenshot))
            sys.stderr.write("serial_driver: screenshot saved to %s\n" % args.screenshot)
        except Exception as e:  # report, keep going: the serial checks still matter
            sys.stderr.write("serial_driver: screendump failed: %s\n" % e)

    try:
        if args.observe is not None:
            try:
                proc.wait(timeout=min(args.observe, max(remaining(), 0)))
            except subprocess.TimeoutExpired:
                screendump()
                killed = True
        else:
            sent = 0
            for step in steps:
                if step == "@screendump":
                    # Wait for the prompt that follows the last command first.
                    target = sent + 1
                    step_deadline = time.time() + args.step_timeout
                    while out.prompts() < target and proc.poll() is None and time.time() < step_deadline:
                        time.sleep(0.05)
                    screendump()
                    continue
                target = sent + 1
                step_deadline = min(time.time() + args.step_timeout, start + args.timeout)
                while out.prompts() < target and proc.poll() is None and time.time() < step_deadline:
                    time.sleep(0.05)
                if proc.poll() is not None:
                    sys.stderr.write("serial_driver: VM exited before '%s' could be typed\n" % step)
                    break
                if out.prompts() < target:
                    sys.stderr.write("serial_driver: no shell prompt before '%s' (waited %.0fs)\n"
                                     % (step, args.step_timeout))
                    killed = True
                    break
                time.sleep(0.1)
                try:
                    if step.startswith("@raw "):
                        data = codecs.decode(step[5:], "unicode_escape").encode("latin-1")
                    else:
                        data = step.encode()
                    proc.stdin.write(data + b"\r")
                    proc.stdin.flush()
                except OSError:
                    sys.stderr.write("serial_driver: VM exited while '%s' was typed\n" % step)
                    break
                sent += 1
            if not killed:
                try:
                    proc.wait(timeout=max(remaining(), 1))
                except subprocess.TimeoutExpired:
                    sys.stderr.write("serial_driver: VM did not exit within %.0fs\n" % args.timeout)
                    killed = True
    finally:
        if proc.poll() is None:
            killed = True
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except OSError:
                proc.kill()
        proc.wait()
        try:
            os.killpg(proc.pid, signal.SIGKILL)  # stragglers (QEMU under a killed osef-image)
        except OSError:
            pass
        try:
            proc.stdin.close()
        except OSError:
            pass
        out.close()
        shutil.rmtree(tmp, ignore_errors=True)

    return 124 if killed else proc.returncode


if __name__ == "__main__":
    sys.exit(main())
