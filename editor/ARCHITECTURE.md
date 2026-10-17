# OSEF Editor — architecture

A Unity-style desktop editor for OSEF operating systems, written in C++20 with
[Dear ImGui](https://github.com/ocornut/imgui) (docking branch) on GLFW + OpenGL 3.
It runs on Windows, macOS and Linux.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ File  Edit  Component  Build  Window  Help                                   │  menu bar
├──────────────────────────────────────────────────────────────────────────────┤
│                         [ ▶ Run ]  [ ⛭ Build ISO ]  [ ▶ QEMU ]   x86_64·UEFI │  toolbar
├───────────────┬──────────────────────────────────────────┬───────────────────┤
│ Hierarchy     │ main.cpp ●  keyboard.cpp  ×               │ Inspector         │
│ [+] [search ] │  1  // MyOS: a tour of every ...          │ [■] ☑ Keyboard    │
│ ▾ ▣ MyOS      │  2  #include <osef/hardware.h>            │ PS/2 · IRQ 1  ✔   │
│   ▾ Boot      │  3                                        │ ▾ Hardware Decl.  │
│     Firmware  │  4  OSEF_HARDWARE(osef::Hardware()        │   Enabled   ☑ ⛓  │
│   ▾ Hardware  │  5      .name("MyOS")                     │ ▾ Driver          │
│     CPU       │  …                                        │   IRQ       1     │
│     Memory    │                                           │ ▾ Source Files    │
│     Time      │                                           │   keyboard.cpp ↗ │
│     Keyboard  │                                           │ ▾ Documentation   │
│     Mouse  ◌  │                                           │   …               │
│     USB    ◌  │                                           │                   │
├───────────────┴──────────────────────────────────────────┤                   │
│ Project | Console   [Clear] ⓘ 12 ⚠ 0 ⛔ 0                 │                   │
│ [build] make iso ...                                     │                   │
├──────────────────────────────────────────────────────────┴───────────────────┤
│ Ready                                              main.cpp  Ln 4, Col 9  ●  │  status bar
└──────────────────────────────────────────────────────────────────────────────┘
```

* **Hierarchy** (left): the OS as a tree of components, such as boot firmware, CPU,
  memory, time, serial, display, keyboard, mouse, USB, storage, PCI, network,
  audio, power, the kernel subsystems and userland. Components the engine doesn't
  implement yet appear dimmed with a "planned" tag. Programs and modules
  registered in `main.cpp` appear as live children.
* **Code** (center): a tabbed code editor with C++ syntax highlighting.
  Selecting a component opens its source.
* **Inspector** (right): every detail of the selected component, in collapsible
  Unity-style sections. Properties bound to the `OSEF_HARDWARE` declaration
  (firmware, memory, devices, ...) are marked ⛓. Editing one rewrites the
  declaration in `main.cpp` live, as one undoable edit. Editing the code updates
  the Inspector in turn.
* **Project / Console** (bottom): a file browser of the OS project and the engine,
  and the build/run log streamed from background jobs.
* **Appearance** (menu bar): switch between beginner guidance and the advanced
  experience, or choose the OSEF Dark, Light or High Contrast palette. These
  choices share the persisted editor settings with panel visibility and layout.

## Source layout and ownership

| Path | What | Depends on ImGui? |
|---|---|---|
| `src/main.cpp` | GLFW/OpenGL/ImGui bootstrap, main loop, CLI, screenshot mode | yes |
| `src/app/editor_app.{h,cpp}` | Owns panels, dockspace + default layout, menu bar, toolbar, status bar, shortcuts | yes |
| `src/app/editor_context.{h,cpp}` | Shared state + helpers (`select`, `open_file`, `run_target`) | yes |
| `src/app/log.h` | Thread-safe console log | no |
| `src/app/panel.h` | Panel base class | no |
| `src/app/theme.{h,cpp}` | Unity "dark skin" style + fonts | yes |
| `src/app/icons.{h,cpp}` | Vector icons for every `ComponentKind` (ImDrawList) | yes |
| `src/panels/hierarchy_panel.{h,cpp}` | `HierarchyPanel` | yes |
| `src/panels/inspector_panel.{h,cpp}` | `InspectorPanel` | yes |
| `src/panels/code_panel.{h,cpp}` | `CodePanel`: tabs + find bar + `CodeEditor` | yes |
| `src/panels/console_panel.{h,cpp}` | `ConsolePanel` | yes |
| `src/panels/project_panel.{h,cpp}` | `ProjectPanel`: file browser | yes |
| `src/widgets/code_editor.{h,cpp}` | The code editor widget (rendering + input) | yes |
| `src/text/text_buffer.{h,cpp}` | Editing model: lines, cursor, selection, undo/redo, find | **no** |
| `src/text/cpp_highlighter.{h,cpp}` | C++ lexer → token spans per line | **no** |
| `src/model/*.{h,cpp}` | Component, Property, catalog, HardwareDecl, Document(Manager), Project | **no** |
| `src/services/process_runner.{h,cpp}` | Background shell jobs → Log | **no** |
| `src/util/png_writer.{h,cpp}` | Minimal PNG encoder for screenshots | no |
| `tests/` | Headless unit tests for `model/`, `text/`, `services/` | no |

Rule: `model/`, `text/`, `services/` and `util/` never include ImGui or OpenGL,
so they can be unit tested headlessly. Everything uses the `osef::editor` namespace.

## Component catalog (model/component_catalog.cpp)

Ids are stable and dotted. `⛓` = bound to `OSEF_HARDWARE`. Statuses reflect
the engine today (bare-metal layer in `engine/src/baremetal/`).

| Id | Name | Status | Inspector properties | Sources |
|---|---|---|---|---|
| `os` | *(OS name)* | — | Name ⛓ (String), Project directory, Main file, Declaration status (Info) | `main.cpp` (project) |
| `boot` | Boot | group | | |
| `boot.firmware` | Firmware | Implemented | Firmware ⛓ (Enum radio BIOS/UEFI), Architecture ⛓ (Enum x86_64), Boot menu timeout ⛓ (Int 0–30 s), Bootloader (Info: Limine 11.4.1, Limine protocol, base revision 3), Image (Info: BIOS → El Torito no-emulation ISO / UEFI → EFI system partition ISO) | `engine/src/baremetal/boot.cpp`, `tools/osef-image/main.cpp` |
| `hw` | Hardware | group | | |
| `hw.cpu` | CPU | Implemented | Architecture (Info), Features (Info: SSE enabled at boot), Cores (Info: 1, SMP planned), Descriptor tables (Info: GDT + TSS, IDT 256 vectors) | `gdt.cpp`, `idt.cpp`, `isr.S`, `cpu.h` |
| `hw.memory` | Memory | Implemented | Memory ⛓ (Int 32–65536 MiB, step 32), Allocator (Info: free-list, first-fit, coalescing), Mapping (Info: Limine HHDM) | `heap.cpp`, `runtime.cpp` |
| `hw.interrupts` | Interrupts | Implemented | Controller (Info: 8259 PIC remapped to vectors 32–47), Exceptions (Info: isolated per process) | `pic.cpp`, `idt.cpp`, `protect.cpp` |
| `hw.timer` | Time | Implemented | Source (Info: PIT channel 0), Frequency (Info: 1000 Hz), IRQ (Info: 0) | `pit.cpp` |
| `hw.rtc` | Real-Time Clock | Implemented, toggle ⛓ Hw::RTC | CMOS polling (Info: 0x70/0x71), Try it (Info: `time`) | `rtc.cpp`, `shell.cpp`, `platform_baremetal.cpp` |
| `hw.serial` | Serial Port | Implemented, toggle ⛓ Hw::Serial | Baud rate ⛓ (Enum 115200/57600/38400/19200/9600), Port (Info: COM1 0x3F8), IRQ (Info: 4) | `serial.cpp` |
| `hw.display` | Display | Implemented, toggle ⛓ Hw::Framebuffer | Resolution ⛓ (Resolution, 0×0 = auto), Console (Info: font8x8 text console, ANSI colors) | `fbconsole.cpp` |
| `hw.keyboard` | Keyboard | Implemented, toggle ⛓ Hw::PS2Keyboard | Interface (Info: PS/2, scancode set 1), IRQ (Info: 1), Ports (Info: 0x60 / 0x64), Layout (Enum US, read-only) | `keyboard.cpp` |
| `hw.mouse` | Mouse | Planned | Interface (Info: PS/2 aux port, IRQ 12; USB HID later) | — |
| `hw.usb` | USB | Partial, toggle ⛓ Hw::USB | Host controller (Info: xHCI, polled event ring), Class drivers (Info: HID boot keyboard as console input), Try it (Info: lsusb), Planned (Info: hubs, HID mouse, mass storage, MSI-X, hot plug) | `usb.cpp`, `vmm.cpp` |
| `hw.storage` | Storage | Planned | Controllers (Info: AHCI SATA, NVMe, ATA PIO) | — |
| `hw.pci` | PCI | Implemented, toggle ⛓ Hw::PCI | Access (Info: legacy configuration mechanism #1, 0xCF8/0xCFC), Try it (Info: `lspci`) | `pci.cpp`, `shell.cpp`, `platform_baremetal.cpp` |
| `hw.network` | Network | Planned | Adapters (Info: virtio-net, Intel e1000) | — |
| `hw.audio` | Audio | Planned | Devices (Info: Intel HDA, AC'97) | — |
| `hw.power` | Power | Implemented | Shutdown (Info: ACPI S5 with emulator fallback), ACPI (Info: RSDP, FADT, DSDT `_S5`) | `panic.cpp`, `acpi.cpp` |
| `kernel` | Kernel | group | | |
| `kernel.scheduler` | Scheduler | Implemented | Policies (Info: round-robin (default), priority, lottery), Configured in (Info: `kernel.set_scheduler(...)`) | `engine/src/scheduler.cpp`, `engine/include/osef/scheduler.h` |
| `kernel.processes` | Processes | Implemented | Model (Info: cooperative, step-based), Fault isolation (Info: yes) | `engine/src/kernel.cpp`, `engine/include/osef/process.h` |
| `kernel.ipc` | IPC & Services | Implemented | Mailboxes, service registry (Info) | `engine/src/ipc.cpp`, `engine/include/osef/ipc.h` |
| `kernel.syscalls` | System Calls | Implemented | Built-ins (Info: getpid, getppid, uptime, uname, kill, spawn) | `engine/include/osef/syscall.h` |
| `kernel.vfs` | File Systems | Implemented | Mounts (Info: / ramfs, /dev devfs) | `engine/src/vfs.cpp`, `engine/include/osef/vfs.h` |
| `kernel.devices` | Devices (/dev) | Implemented | Built-ins (Info: null, console, random, uptime) | `engine/src/kernel.cpp` |
| `user` | Userland | group | | |
| `user.shell` | Shell | Implemented | Builtins (Info: help, ls, cat, ps, ...) | `engine/src/shell.cpp` |
| `user.programs` | Programs | Implemented | Count (Info) | `main.cpp` (project) |
| `user.programs.<name>` | *(program)* | Implemented | Kind (Info: class or lambda), Description (Info) | `main.cpp` at its registration line |
| `user.modules` | Modules | Implemented | Count (Info) | `main.cpp` (project) |
| `user.modules.<Type>` | *(module)* | Implemented | Type (Info) | `main.cpp` at its `add_module` line |

Programs and modules are discovered in `main.cpp` by scanning for
`programs.add<T>("name", "description")`, `.add_fn("name", "description", ...)`
and `add_module<T>()`, ignoring comments and strings.

## Data flow

```
main.cpp Document ──parse_hardware_decl──▶ Project ──▶ Component properties ──▶ Inspector
        ▲                                                                         │
        └────────── apply_hardware_decl (one undoable replace_all) ◀── apply_property
```

`Project::sync_from_code()` runs every frame. It is cheap: it re-parses only when
`Document::version()` changed. Inspector edits call `Project::apply_property()`,
which rewrites only the declaration's byte range. Nothing is saved until the user
saves (Cmd/Ctrl+S).

## Build targets (toolbar / Build menu)

`Project::build_targets()` returns shell commands run by `ProcessRunner` from the
OSEF checkout:

| id | label | command |
|---|---|---|
| `run` | Run (hosted) | `make run OS_DIR=<project dir>` |
| `iso` | Build ISO | `make iso OS_DIR=<project dir>` |
| `qemu` | Run in QEMU | `make run-iso OS_DIR=<project dir>` |
| `test` | Run Tests | `make test` |

## Command line

```
osef-editor [<project dir or main.cpp>] [--root <osef checkout>]
            [--screenshot <out.png>] [--size <W>x<H>] [--frames <N>]
            [--select <component id>] [--open <file>[:<line>]]
            [--set <component id>.<property id>=<value>]...
            [--toggle <component id>=on|off]...
```

* With no project argument, the editor opens `examples/myos` in the detected OSEF checkout.
* `--root` defaults to searching upward from the project, then from the executable,
  for `engine/include/osef/hardware.h`.
* `--screenshot` renders headlessly into an offscreen framebuffer of `--size`
  (default 1600x900) for `--frames` frames (default 5), writes a PNG and exits.
  It never saves documents.
* `--set` and `--toggle` drive the Inspector → code binding without UI interaction.
  They're used by tests and screenshots, for example
  `--set boot.firmware.firmware=BIOS --toggle hw.keyboard=off`.

## Look and feel (Unity dark skin)

| Element | Color |
|---|---|
| Window/panel background | `#383838` |
| Title bars, tab bar, inactive tab | `#282828` |
| Active tab, section headers | `#3C3C3C` (section headers `#3E3E3E` with a 1 px `#232323` top border) |
| Separators / borders | `#191919` / `#232323` |
| Text / dimmed text | `#D2D2D2` / `#8A8A8A` |
| Selection (focused) | `#2C5D87`; unfocused selection `#4D4D4D` |
| Hover | `#454545` |
| Input fields | `#2A2A2A`, border `#212121` |
| Buttons | `#585858`, hover `#676767`, active `#464646` |
| Accent / checkmarks | `#4C7EFF` |
| Code background / gutter | `#1E1E1E` / `#1E1E1E` with line numbers `#858585` (current line `#C6C6C6`) |
| Status colors | implemented `#5DB35D`, partial `#D8A23A`, planned `#8A8A8A`, error `#E05252` |

* Syntax colors (VS Code Dark+): keyword `#569CD6`, control keyword `#C586C0`,
  type `#4EC9B0`, function `#DCDCAA`, string `#CE9178`, number `#B5CEA8`,
  comment `#6A9955`, preprocessor `#C586C0`, macro/`OSEF_*` `#4FC1FF`,
  punctuation `#D4D4D4`.
* Fonts: Inter (UI) and JetBrains Mono (code), both under the SIL Open Font License,
  fetched by `tools/bootstrap/editor-deps.sh`. If they're missing, the editor falls
  back to the built-in ImGui font.
* Small rounding (2–3 px), compact spacing, 1 px borders and full-width selection
  rows, as in Unity.

## Extending

* **New component:** add an entry in `model/component_catalog.cpp`. If it binds
  to the hardware declaration, extend `HardwareDecl` and `Project::apply_property`.
* **New panel:** subclass `Panel`, then register it in `EditorApp`. It appears in
  the Window menu automatically.
* **New build target:** add it in `Project::build_targets()`.
* **Planned hardware:** "Create Driver Stub…" in the Inspector or the Hierarchy
  context menu writes `drivers/<id>.cpp` into the OS project, as a starting point.
