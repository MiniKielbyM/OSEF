# OSEF — Operating System Engine Framework

Build your own operating system the way you'd build a game in a game engine.
OSEF supplies the runtime (kernel loop, scheduler, file systems, IPC,
syscalls and a hardware abstraction layer). You supply the parts that make
your OS *yours*.

| Game engine          | OSEF                                               |
|----------------------|----------------------------------------------------|
| Engine runtime       | `Kernel`: owns subsystems, runs the main loop      |
| Game loop / frame    | `Kernel::tick()`: wake, run modules, schedule, reap |
| Game objects         | `Process`: `on_start` / `step` / `on_exit`         |
| Engine plugins       | `Module`: `on_load` / `on_tick` / `on_unload`      |
| Prefabs / assets     | `ProgramRegistry`: named process factories         |
| Platform layer       | `Platform`: time, console, input, randomness       |
| Startup scene        | `KernelConfig::init_program` (pid 1)               |

## Quick start

Every OS starts by declaring the machine it runs on. The declaration must
name the boot firmware (BIOS or UEFI); missing fields are compile errors and
an OS without one does not link (see [engine/include/osef/hardware.h](engine/include/osef/hardware.h)).

```cpp
#include <osef/hardware.h>

OSEF_HARDWARE(osef::Hardware()
    .name("TinyOS")
    .arch(osef::Arch::x86_64)                  // required
    .firmware(osef::Firmware::BIOS)            // required: BIOS or UEFI
    .devices(osef::Hw::Serial | osef::Hw::Framebuffer | osef::Hw::PS2Keyboard)
    .memory_mb(64));

#include <osef/osef.h>

int main() {
    osef::Kernel kernel; // the OS name comes from the hardware declaration
    kernel.add_module<osef::ShellModule>();
    return kernel.run();
}
```

The engine is freestanding: it is written against OSEF's own core library
(`osef::String`, `StringView`, `Vector`, `Map`, `Deque`, `Function`,
`UniquePtr`, `SharedPtr`, `format`, ...; see [engine/include/osef/core](engine/include/osef/core))
instead of the C++ standard library, so the same OS source builds as a
native app and as a bare-metal kernel. The shell's `hw` command prints the
declaration and `mem` shows memory usage.

See [examples/myos/main.cpp](examples/myos/main.cpp) for a tour of every
extension point: a custom module, an IPC daemon, programs, a custom syscall,
a device, a persistent disk, a custom init and a different scheduler.

## Building

Requires a C++20 compiler. There are no other dependencies for hosted builds.

```sh
# Any platform with CMake >= 3.21 (Windows / macOS / Linux / BSD)
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --config Release
ctest --test-dir build-cmake -C Release   # core, kernel and hardware-declaration tests
./build-cmake/myos                        # (Windows: build-cmake\Release\myos.exe)

# No CMake? (macOS / Linux / MinGW)
make && make test && make run        # make test SANITIZE=1 for ASan/UBSan
make run OS=minimal                  # any examples/<name>
make run OS_DIR=path/to/my_os        # any directory with a main.cpp

# WebAssembly (experimental; needs emsdk)
emcmake cmake -S . -B build-web && cmake --build build-web
```

The Makefile owns `build/`, so give CMake its own directory (`build-cmake`
above): `make clean` empties `build/`.

### Bootable ISOs (bare metal)

The same `main.cpp` also builds as an x86_64 kernel booted by
[Limine](https://limine-bootloader.org) and packaged as an ISO whose boot path
is exactly the firmware in `OSEF_HARDWARE`: a `.firmware(osef::Firmware::BIOS)`
OS gets a BIOS-only ISO, a `.firmware(osef::Firmware::UEFI)` OS a UEFI-only ISO.
Change `.firmware(...)` and rebuild to switch.

```sh
tools/bootstrap.sh --with-qemu       # one-time: builds only the tools you don't have, into .toolchain/

make iso OS=minimal                  # -> examples/minimal/minimal.iso (BIOS), copy in build/
make iso OS_DIR=path/to/my_os        # -> path/to/my_os/my_os.iso, firmware as declared
                                     #    (EXPORT_ISO=0: only build/my_os.iso)
make iso                             # every example
make run-iso OS=myos                 # boot it in QEMU (window = framebuffer console,
                                     #  this terminal = serial console; OVMF for UEFI)
make test-iso                        # headless boot tests of every example ISO
```

`tools/bootstrap.sh` uses the system's `ld.lld` / `x86_64-elf-ld`, `xorriso`,
`cmake` and (with `--with-qemu`) QEMU + OVMF when they are installed, and
otherwise runs the matching script in [tools/bootstrap](tools/bootstrap)
(`binutils.sh`, `xorriso.sh`, `limine.sh`, `cmake.sh`, `qemu.sh`;
`--with-editor` adds `editor-deps.sh`). It ends with a table of every tool and
where it comes from; `--check` only prints the table.

The kernel is compiled freestanding with clang (`--target=x86_64-unknown-none-elf`)
and linked with an ELF linker (`.toolchain/cross/bin/x86_64-elf-ld`, else `ld.lld`
or `x86_64-elf-ld` from PATH). Every tool can be overridden: `BM_CXX`, `OSEF_LD`,
`XORRISO`, `LIMINE_DIR`, `QEMU`, `OVMF`. UEFI VMs get at least 128 MiB of RAM
(the firmware and Limine need it), whatever `.memory_mb()` says.

`make test-iso` boots each ISO with its declared firmware, types `hw`, `uname`,
`mem`, `ps`, `echo ok` (and `crash` for myos) on the serial console, checks the
output, saves `build/<os>-serial.log` and a framebuffer screenshot
`build/<os>-screen.png`, and expects `shutdown 7` to end QEMU with exit code 7.
It also boots each ISO with the *other* firmware and checks that the OS does
not start. `make test` includes `make hardware-check`, which checks that bad
`OSEF_HARDWARE` declarations in [tests/hardware](tests/hardware) are rejected.

#### With CMake

When clang, an ELF linker, xorriso and Limine are found, the CMake build makes
ISOs too (`OSEF_BUILD_ISO=AUTO`; the configure output says what is missing
otherwise; `-DOSEF_BUILD_ISO=ON` turns a missing tool into an error):

```sh
cmake -S . -B build-cmake && cmake --build build-cmake   # + build-cmake/images/<os>.{elf,iso}
cmake --build build-cmake --target minimal-iso           # one OS's kernel + ISO
cmake --build build-cmake --target myos-qemu             # boot it in QEMU
cmake -S . -B build-cmake -DOSEF_TEST_ISO=ON && ctest --test-dir build-cmake -L iso   # boot tests
```

Your own OS, in your own CMake project:

```cmake
add_subdirectory(path/to/OSEF osef)
osef_add_os(my_os DIRECTORY my_os)   # every my_os/*.cpp; or SOURCES main.cpp ...
# targets: my_os (native app), my_os-check-source, my_os-iso, my_os-qemu
```

The kernels come from a sub-build of the same source tree with
[cmake/toolchains/x86_64-osef-baremetal.cmake](cmake/toolchains/x86_64-osef-baremetal.cmake)
(clang compiles, the ELF linker is called directly with the OSEF linker script),
so they match the Makefile's kernels. The toolchain file also works on its own:
`cmake -S . -B build-kernel --toolchain cmake/toolchains/x86_64-osef-baremetal.cmake`.
On Linux and Windows, install LLVM (clang + lld) for the kernels and
xorriso; the hosted part also builds with gcc or MSVC. Tool paths can be set
with `-DOSEF_BAREMETAL_CXX=`, `-DOSEF_LD=`, `-DOSEF_XORRISO=`, `-DOSEF_LIMINE_DIR=`,
`-DOSEF_QEMU=`, `-DOSEF_OVMF=`, and `-DOSEF_TOOLCHAIN_DIR=` moves `.toolchain`.
`-DOSEF_BUILD_EDITOR=ON` adds the editor (`editor/`).

CI ([.github/workflows/build.yml](.github/workflows/build.yml)) builds and tests
the hosted engine on Linux, macOS and Windows. A bare-metal job on Linux
(clang, lld, xorriso, QEMU, OVMF) builds the ISOs with CMake and with the
Makefile and boots them in QEMU. It also builds the WebAssembly target
(experimental) and the editor on all three systems.

**Releases:** every push to `main` that passes all of these jobs publishes a
GitHub release (`v<version>-build.<run>`). It contains the editor for macOS
(`OSEF Editor.app`, zipped), Linux and Windows (experimental), plus the example
ISOs. A downloaded editor finds the OSEF folder through `--root`, the
`OSEF_ROOT` environment variable, or a checkout at `~/OSEF` or `~/Documents/OSEF`.

## Building the app on each operating system

The OSEF editor (the Unity-style app in [editor/](editor)) is a native C++20
program using Dear ImGui, GLFW and OpenGL 3.2. Its **Run hosted**, **Boot in
QEMU** and **Build ISO image** buttons call `make` in this folder, so to build
ISOs from the app you need the ISO tools as well. Steps for each system:

### macOS (Intel or Apple Silicon)

1. Install Apple's command line tools (clang, make, git):
   ```sh
   xcode-select --install
   ```
2. Build the ISO tools and the editor's third-party sources. This needs no
   Homebrew and no sudo; everything goes into `.toolchain/`. It takes about
   15 minutes the first time, mostly QEMU.
   ```sh
   tools/bootstrap.sh --with-qemu --with-editor
   ```
3. Build and start the app:
   ```sh
   make -C editor                     # -> build/editor/osef-editor
   build/editor/osef-editor           # opens examples/myos; File > New OS… (Cmd+N) for your own
   ```
4. Optional: make it a regular Mac app you can open from Launchpad, Spotlight
   or the Dock:
   ```sh
   make -C editor app                 # -> build/editor/OSEF Editor.app (drag it into Applications)
   make -C editor install             # or copy it there: /Applications, else ~/Applications
   make -C editor uninstall           # remove it again
   ```
   The bundle contains the app, its fonts and its icon, and it is ad-hoc
   signed. It still uses this OSEF folder for the engine and the build tools, so
   keep the folder where it is, or rebuild the app after moving it. Because the
   app isn't notarized, a copy downloaded to another Mac needs right-click >
   Open the first time.

If you already have Homebrew, `brew install lld xorriso qemu` can replace most
of step 2. `tools/bootstrap.sh` uses tools it finds on your PATH and builds only
what is missing.

### Linux

1. Install a compiler, the ISO tools and GLFW's window-system headers.

   Debian / Ubuntu:
   ```sh
   sudo apt install build-essential clang lld cmake git curl python3 xorriso \
       qemu-system-x86 ovmf libx11-dev libxrandr-dev libxinerama-dev \
       libxcursor-dev libxi-dev libgl-dev libwayland-dev libxkbcommon-dev wayland-protocols
   ```
   Fedora:
   ```sh
   sudo dnf install gcc-c++ make clang lld cmake git curl python3 xorriso \
       qemu-system-x86 edk2-ovmf libX11-devel libXrandr-devel libXinerama-devel \
       libXcursor-devel libXi-devel mesa-libGL-devel wayland-devel libxkbcommon-devel
   ```
   Arch:
   ```sh
   sudo pacman -S base-devel clang lld cmake git curl python xorriso qemu-desktop \
       edk2-ovmf libx11 libxrandr libxinerama libxcursor libxi mesa wayland libxkbcommon
   ```
2. Fetch Limine and the editor's third-party sources:
   ```sh
   tools/bootstrap.sh --with-editor
   ```
3. Build and start the app:
   ```sh
   make -C editor && build/editor/osef-editor
   ```
   Or use CMake (GLFW's CMake build supports both X11 and Wayland):
   ```sh
   cmake -S editor -B build-editor -DCMAKE_BUILD_TYPE=Release
   cmake --build build-editor && build-editor/osef-editor
   ```

### Windows 10 / 11

1. Install **Visual Studio 2022** with the "Desktop development with C++"
   workload. It includes CMake. Also install Git.
2. Build the app from a *Developer PowerShell for VS 2022*. CMake downloads the
   pinned ImGui, GLFW and font archives itself:
   ```powershell
   cmake -S editor -B build-editor
   cmake --build build-editor --config Release
   .\build-editor\Release\osef-editor.exe
   ```
3. To use the app's Run / Build ISO / QEMU buttons, `make`, clang, lld,
   xorriso and QEMU must be on your PATH. The simplest way is
   [MSYS2](https://www.msys2.org) (UCRT64 shell):
   ```sh
   pacman -S make mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-lld \
       mingw-w64-ucrt-x86_64-xorriso mingw-w64-ucrt-x86_64-qemu python git
   tools/bootstrap.sh
   ```
   Alternatively, build ISOs inside **WSL 2** (Ubuntu) by following the Linux
   steps; the hosted engine and the app itself also build natively with MSVC.

> Windows support is written but has not been tested yet. Builds on macOS
> (Intel) are tested; CI builds the hosted engine on Windows, macOS and Linux.

### Check that everything works

On any system:

```sh
make test               # engine, hardware-declaration and build-system tests
make -C editor test     # editor tests (headless)
make test-iso           # boots every example ISO in QEMU, BIOS and UEFI
```

The CMake equivalents are `ctest --test-dir build-cmake` and
`ctest --test-dir build-editor`.

## Writing an OS

### Processes: cooperative and step-based

The kernel calls `step()` once per quantum. Do a little work and return.
"Blocking" calls don't block the host. They park the process, and the kernel
calls `step()` again once the condition is met:

```cpp
class Counter : public osef::Process {
    int n = 0;
    void step(osef::Context& c) override {
        c.println(osef::format("tick %d", ++n));
        if (n == 10) c.exit(0);
        else c.sleep(500);            // other processes run meanwhile
    }
};
kernel.programs().add<Counter>("counter", "counts to ten");
```

| Call                              | Parks the process until…        |
|-----------------------------------|---------------------------------|
| `c.read_line(line)` → `false`     | a line of input arrives         |
| `c.receive(msg)` → `false`        | an IPC message arrives          |
| `c.sleep(ms)`                     | the time has passed             |
| `c.wait(pid)`                     | that process exits              |

Because no threads, assembly or context switching are involved, the same
code runs unchanged on every platform. A process that crashes (calls
`c.abort("reason")`, throws in a hosted build, or faults on bare metal) is
killed on its own, and the kernel keeps running.

### The `Context` API

`print/println`, `read_line`, `sleep`, `exit`, `abort`, `spawn(program, args, foreground)`,
`wait`, `send/receive`, `register_service/lookup`, `syscall(name, args)`,
`fs()`, `cwd/chdir/resolve`, `read_file/write_file`.

### Modules

```cpp
class NetModule : public osef::Module {
    const char* name() const override { return "net"; }
    void on_load(osef::Kernel& k) override { /* register programs, syscalls, devices, mounts */ }
    void on_tick(osef::Kernel& k) override { /* kernel-level work every tick */ }
};
kernel.add_module<NetModule>();
NetModule* net = kernel.find_module<NetModule>(); // type tags, no RTTI needed
```

### Schedulers

Built in: `RoundRobinScheduler` (default), `PriorityScheduler`, `LotteryScheduler`.
To write your own, implement one method:

```cpp
struct MyScheduler : osef::Scheduler {
    const char* name() const override { return "mine"; }
    osef::Process* pick(const osef::Vector<osef::Process*>& ready) override;
};
kernel.set_scheduler(osef::make_unique<MyScheduler>());
```

### File systems and devices

The VFS mounts `FileSystem` drivers at paths. Built-in drivers are `RamFS`
(mounted at `/` by default), `DevFS` (mounted at `/dev`) and, in hosted
builds only, `HostFS`, which maps to a real folder for persistent storage:

```cpp
#if OSEF_HOSTED
kernel.vfs().mount("/disk", osef::make_shared<osef::HostFS>("my_disk"));
#endif
kernel.register_device("led", {"status LED", nullptr,
    [](osef::StringView v) { /* toggle */ return true; }});
```

From the shell, run `echo 1 > /dev/led`.

### Syscalls and IPC

```cpp
kernel.syscalls().define("answer", "returns 42",
    [](osef::Context&, const osef::SyscallArgs&) { return osef::SyscallResult{42, ""}; });
kernel.syscalls().set_tracer(/* strace-style hook */);
```

Processes have mailboxes (`send`/`receive`), and services register a name
(`register_service("time")`) so clients can `lookup("time")`.

### Porting to a new target

Implement `osef::Platform` (time, console write, non-blocking line input,
random, and optionally memory stats and `run_protected`/`fault` for crash
isolation) and pass it to the kernel:

```cpp
osef::Kernel kernel(config, osef::make_unique<MyWindowPlatform>());
```

[tests/test_kernel.cpp](tests/test_kernel.cpp) does exactly this with a scripted
platform and a virtual clock. To embed OSEF in another app (a GUI, a game, an
editor), call `kernel.tick()` from your own loop and feed input with
`kernel.push_input()`.

## Layout

```
engine/include/osef/   public API headers (freestanding)
engine/include/osef/core/  core library: strings, containers, format, ...
engine/src/            kernel, scheduler, vfs, ipc/syscalls, shell, hardware
engine/src/core/       core library sources
engine/src/hosted/     host platform (Windows, macOS, Linux, Web) and HostFS
engine/src/baremetal/  x86_64 bare-metal platform (Limine boot)
tools/osef-image/      host tool: checks OSEF_HARDWARE, builds BIOS/UEFI ISOs
tools/bootstrap.sh     gets the bare-metal tools (runs tools/bootstrap/*.sh as needed)
cmake/                 osef_add_os() (OSEF.cmake) and the bare-metal toolchain file
examples/minimal/      the smallest possible OS
examples/myos/         a tour of every extension point
tests/                 core tests and engine tests (scripted platform, virtual clock)
tests/hardware/        OSEF_HARDWARE fixtures (make hardware-check)
tests/iso/             QEMU boot tests for the ISOs (make test-iso)
```

## Current scope

OSEF v0.1 runs your OS two ways from the same source: **hosted**, as an
application on Windows, macOS, Linux or the web (much like a fantasy console),
and on **bare metal** x86_64, as a bootable BIOS or UEFI ISO (serial,
framebuffer text console, PS/2 keyboard, fault isolation for CPU exceptions).
Natural next steps:

- **File descriptors and streams**: open/read/write/seek, and pipes between processes
- **Graphics**: a framebuffer device with SDL / Web Canvas platforms, then a window manager
- **Scripting**: Lua or WASM programs, so OS authors don't need to recompile
- **Project files**: describe an OS (modules, programs, mounts) in a manifest
- **Editor**: a visual tool to compose and live-run an OS, the "Unity" layer
- **More bare-metal targets**: the engine is freestanding; x86_64 (BIOS/UEFI via
  Limine) is the first bare-metal platform, ARM would be next.

## Roadmap

OSEF's goal is to be the game engine for operating systems: an engine that
handles the hard parts and an editor that makes OS development approachable.
This roadmap shows what is done and where the project is heading. Priorities
may change based on feedback.

## ✅ Done (v0.1)

**Engine**
- Kernel loop, scheduler, processes, modules and a program registry
- VFS, IPC, syscalls and custom interrupt handlers (`on_irq` / `on_vector`)
- Freestanding core library, with no C++ standard library required
- `OSEF_HARDWARE` declarations that are checked at compile time

**Bare metal (x86_64)**
- Limine boot over BIOS or UEFI, enforced by the declared firmware
- GDT/IDT, PIC, PIT, paging, heap, and page mapping for MMIO and ACPI
- Drivers: serial, framebuffer console, PS/2 keyboard, RTC, PCI, and USB
  (xHCI with HID keyboards)
- ACPI soft power-off

**Tooling**
- Makefile and CMake builds, with `osef_add_os()` for your own projects
- `tools/bootstrap.sh`, which builds the toolchain without sudo
- CI on Linux, macOS and Windows, plus automated QEMU boot tests

**Editor**
- A Unity-style editor with a hierarchy, an inspector and a code editor
- One-click Run, Build ISO and Boot in QEMU
- An OS Settings panel and a New OS wizard for people who don't program
- A native macOS app bundle

## 🚧 Next (v0.2): Input & storage

- [ ] PS/2 and USB HID mouse drivers (`Hw::Mouse`)
- [ ] USB keyboards with full HID reports, not only boot protocol
- [ ] Disk drivers: AHCI (SATA), then NVMe
- [ ] A FAT32 file system on real disks, so files persist across reboots
- [ ] Load OSEF programs from disk instead of linking them all into the kernel
- [ ] A driver template in the editor (Component → New Device) that generates
      a working skeleton
- [ ] Packaged editor builds for Windows and Linux

## 🔜 Soon (v0.3): A real multitasking kernel

- [ ] Local APIC and IOAPIC timers and interrupts (replacing PIC/PIT on
      modern hardware)
- [ ] User mode (ring 3) with per-process address spaces
- [ ] Preemptive scheduling alongside the current cooperative model
- [ ] SMP support so processes run on multiple CPU cores
- [ ] Crash isolation, so a faulting process is killed instead of panicking
      the whole kernel

## 🌐 Later (v0.4): Connectivity & graphics

- [ ] Networking: a virtio-net / e1000 driver with a small TCP/IP stack
- [ ] USB mass storage
- [ ] A simple windowing and graphics API on the framebuffer, an "OSEF UI"
- [ ] Sound (Intel HD Audio)
- [ ] Stable WebAssembly builds so OSEF OSes can run in the browser

## 🔭 Future: Beyond x86_64

- [ ] ARM64 (AArch64) support, starting with QEMU `virt` and then Raspberry Pi
- [ ] RISC-V support
- [ ] An editor scene view for laying out processes, modules and IPC visually
- [ ] Live debugging from the editor (QEMU GDB stub, breakpoints, memory
      viewer)
- [ ] An asset library for sharing modules, drivers, programs and themes
- [ ] Tutorials and docs: "Build your first OS in 30 minutes"

## Contributing

Pick any unchecked item, open an issue to discuss it, and send a PR. Driver
and documentation contributions are especially welcome.
