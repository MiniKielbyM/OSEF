# OSEF root Makefile (works with GNU make 3.81+).
#
# Hosted (the OS runs as a native app; quick iteration and tests):
#   make                        engine, host tool, tests and every example
#   make test                   core + kernel tests and the hardware-declaration checks
#   make test SANITIZE=1        same, with -fsanitize=address,undefined
#   make run                    build and run the selected OS as a native app
#
# Bare metal (x86_64 kernel booted by Limine, packaged as a bootable ISO whose
# boot path is exactly the firmware declared in OSEF_HARDWARE: BIOS or UEFI):
#   make kernel                 link the selected OS into build/<os>.elf
#   make iso                    <project>/<os>.iso (+ build/<os>.iso) for the selected OS
#                               (every example if none is selected; EXPORT_ISO=0: build/ only)
#   make run-iso                boot the selected OS's ISO in QEMU (window + serial on this terminal)
#   make test-iso               boot every example ISO (and tests/iso/faultos) headless in QEMU and
#                               check the serial output; also the ISO part of tests/build
#   make hardware-check         OSEF_HARDWARE fixtures in tests/hardware (also part of `make test`)
#   make build-check            Makefile regression checks in tests/build (also part of `make test`)
#
#   make clean                  remove build outputs (build/editor is left alone)
#
# Selecting an OS (any directory with a main.cpp that starts with OSEF_HARDWARE):
#   OS=<name>                   examples/<name>          (default for run/kernel/run-iso: OS=myos)
#   OS_DIR=<dir>                any directory; every <dir>/*.cpp is compiled. Spaces in the path are
#                               fine (the OS is built through a symlink in build/links/), but not in
#                               the path of this OSEF directory itself.
#
# Before an OS is compiled, `osef-image check-source` verifies that its main.cpp
# declares its hardware with OSEF_HARDWARE(...), and warns about C/C++ standard
# library use, which builds hosted but not in the freestanding ISO kernel.
# Rebuilds of an OS do not depend on mtimes alone: a checksum of its sources
# catches same-second edits and deleted files (see "source fingerprint").
#
# Bare-metal tools (each defaults to .toolchain/, then PATH; override on the command line):
#   BM_CXX=clang++  OSEF_LD=<ELF linker>  XORRISO=<xorriso>  LIMINE_DIR=<limine binaries>
#   QEMU=<qemu-system-x86_64>  OVMF=<UEFI firmware code .fd>

ROOT     := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ifeq ($(wildcard $(ROOT)/Makefile),)
  $(error the OSEF directory ($(CURDIR)) must not contain spaces in its path: make cannot handle them. Move it, e.g. to ~/OSEF)
endif
BUILD    := $(ROOT)/build
EXAMPLES := $(notdir $(patsubst %/,%,$(dir $(wildcard $(ROOT)/examples/*/main.cpp))))
TOOLCHAIN:= $(ROOT)/.toolchain

# ---------------------------------------------------------------- selected OS
# Windows sets OS=Windows_NT in the environment; that is not an OSEF OS.
ifeq ($(origin OS):$(OS),environment:Windows_NT)
  OS_SELECTED := $(if $(filter-out undefined,$(origin OS_DIR)),yes,)
  OS := myos
else
  OS_SELECTED := $(if $(filter-out undefined,$(origin OS) $(origin OS_DIR)),yes,)
endif
OS       ?= myos
OS_DIR   ?= $(ROOT)/examples/$(OS)
ifeq ($(strip $(OS_DIR)),)
  $(error OS_DIR is empty: pass OS_DIR=<directory containing main.cpp> or OS=<name> (one of: $(EXAMPLES)))
endif
# make cannot handle spaces in file names, but OS folders like "~/Documents/My OS"
# are common: such an OS is built through a space-free symlink in build/links/.
ifneq ($(words $(OS_DIR)),1)
  OS_DIR_REAL := $(shell cd "$(OS_DIR)" 2>/dev/null && pwd)
  ifeq ($(OS_DIR_REAL),)
    $(error OS_DIR "$(OS_DIR)" does not exist)
  endif
  override OS_DIR := $(shell sh "$(ROOT)/tools/build/link-os-dir.sh" "$(BUILD)" "$(OS_DIR_REAL)")
  ifneq ($(words $(OS_DIR)),1)
    $(error cannot create a symlink without spaces for OS_DIR "$(OS_DIR_REAL)" in $(BUILD)/links)
  endif
endif
OS_ABS   := $(abspath $(OS_DIR))
OS_SOURCE_DIR := $(if $(OS_DIR_REAL),$(OS_DIR_REAL),$(OS_ABS))
OS_NAME  := $(notdir $(OS_ABS))
OS_MAIN  := $(OS_ABS)/main.cpp
OS_SRC   := $(sort $(OS_MAIN) $(wildcard $(OS_ABS)/*.cpp))
# Build directory unique per source directory (two projects may share a name).
OS_KEY   := $(subst /,_,$(patsubst /%,%,$(OS_ABS)))
OS_BUILD := $(BUILD)/os/$(OS_KEY)

# ---------------------------------------------------------------- compiler
CXX      ?= c++
CXXFLAGS ?= -O2
WARN     := -Wall -Wextra -Wpedantic
HOSTED_FLAGS := -std=c++20 $(WARN) -I$(ROOT)/engine/include -MMD -MP

SANITIZE ?= 0
ifeq ($(SANITIZE),1)
  VARIANT  := hosted-asan
  SAN      := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g
else
  VARIANT  := hosted
  SAN      :=
endif

HOSTED_CXX = $(CXX) $(HOSTED_FLAGS) $(CXXFLAGS) $(SAN)

# ---------------------------------------------------------------- engine (hosted)
# core/cstring.cpp (memcpy & co.) is bare-metal only: hosted builds use libc.
CORE_SRC   := $(filter-out %/cstring.cpp,$(wildcard $(ROOT)/engine/src/core/*.cpp))
ENGINE_SRC := $(wildcard $(ROOT)/engine/src/*.cpp) $(CORE_SRC) $(wildcard $(ROOT)/engine/src/hosted/*.cpp)
OBJ        := $(BUILD)/obj/$(VARIANT)
ENGINE_OBJ := $(patsubst $(ROOT)/%.cpp,$(OBJ)/%.o,$(ENGINE_SRC))
CORE_OBJ   := $(patsubst $(ROOT)/%.cpp,$(OBJ)/%.o,$(CORE_SRC))
LIBOSEF    := $(BUILD)/lib/$(VARIANT)/libosef.a

OSEF_IMAGE := $(BUILD)/osef-image
TEST_CORE  := $(BUILD)/tests/$(VARIANT)/test_core
TEST_KERNEL:= $(BUILD)/tests/$(VARIANT)/test_kernel
OS_OBJ     := $(patsubst $(OS_ABS)/%.cpp,$(OS_BUILD)/$(VARIANT)/%.o,$(OS_SRC))
OS_BIN     := $(OS_BUILD)/$(VARIANT)/$(OS_NAME)

# ---------------------------------------------------------------- bare metal
# Freestanding flags from .toolchain/CONTRACT.md. clang is only the compiler;
# the ELF is always linked with an ELF linker ($(OSEF_LD)), never via clang.
BM_CXX    ?= clang++
BM_OPT    ?= -O2
BM_TARGET := --target=x86_64-unknown-none-elf
BM_CXXFLAGS := -std=c++20 $(BM_TARGET) -ffreestanding -nostdinc++ -nostdlib -fno-exceptions -fno-rtti \
               -fno-threadsafe-statics -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel \
               -fno-asynchronous-unwind-tables -DOSEF_BAREMETAL=1 $(WARN) $(BM_OPT) -I$(ROOT)/engine/include
BM_ASFLAGS  := $(BM_TARGET) -Wall -Wextra

BM_ENGINE_SRC := $(wildcard $(ROOT)/engine/src/*.cpp) $(wildcard $(ROOT)/engine/src/core/*.cpp) \
                 $(wildcard $(ROOT)/engine/src/baremetal/*.cpp)
BM_ASM_SRC    := $(wildcard $(ROOT)/engine/src/baremetal/*.S)
BM_OBJDIR     := $(BUILD)/baremetal/obj
BM_ENGINE_OBJ := $(patsubst $(ROOT)/%.cpp,$(BM_OBJDIR)/%.o,$(BM_ENGINE_SRC)) \
                 $(patsubst $(ROOT)/%.S,$(BM_OBJDIR)/%.S.o,$(BM_ASM_SRC))
BM_LDSCRIPT   := $(ROOT)/engine/src/baremetal/linker.ld

BM_OS_DIR := $(OS_BUILD)/baremetal
BM_OS_OBJ := $(patsubst $(OS_ABS)/%.cpp,$(BM_OS_DIR)/%.o,$(OS_SRC))
# Linked/packaged per source directory, then published as build/<os>.elf/.iso.
BM_OS_ELF := $(BM_OS_DIR)/$(OS_NAME).elf
BM_OS_ISO := $(BM_OS_DIR)/$(OS_NAME).iso
OS_ELF    := $(BUILD)/$(OS_NAME).elf
OS_ISO    := $(BUILD)/$(OS_NAME).iso
# The finished ISO is also exported into the OS's own project folder
# (EXPORT_ISO=0 turns that off). OS_ABS has no spaces (see the symlink above).
EXPORT_ISO ?= 1
PROJECT_ISO := $(OS_ABS)/$(OS_NAME).iso

# ---------------------------------------------------------------- source fingerprint
# GNU make 3.81 (macOS's /usr/bin/make) compares whole-second mtimes: an edit
# saved in the same second as the last build counts as up to date, and
# deleting a source leaves nothing newer than the ELF. So every build of an OS
# also compares a checksum of its directory's sources and headers (names and
# contents) with the one recorded for each build variant; if it differs, that
# variant's OS objects and outputs are deleted, so they must be rebuilt.
OS_FINGERPRINT_GOALS := all os run kernel iso iso-one run-iso
ifneq ($(wildcard $(OS_MAIN)),)
ifneq ($(filter $(OS_FINGERPRINT_GOALS),$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)),)
OS_SUM := $(shell find -H "$(OS_ABS)" -maxdepth 4 -type f \( -name '*.cpp' -o -name '*.cc' -o -name '*.cxx' \
             -o -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' -o -name '*.inl' -o -name '*.ipp' \
             -o -name '*.inc' \) -exec cksum {} + 2>/dev/null | LC_ALL=C sort | cksum)
OS_STALE := $(shell for d in "$(OS_BUILD)/$(VARIANT)" "$(BM_OS_DIR)"; do \
                if [ "$$(cat "$$d/sources.cksum" 2>/dev/null)" != "$(OS_SUM)" ]; then \
                    rm -f "$$d"/*.o "$$d"/*.d "$$d/$(OS_NAME)" "$$d/$(OS_NAME).elf" "$$d/$(OS_NAME).iso"; \
                    mkdir -p "$$d" && printf '%s\n' "$(OS_SUM)" >"$$d/sources.cksum"; echo "$$d"; \
                fi; done)
endif
endif

# Tool lookup: project-local .toolchain first, then PATH. These are recursive
# (lazy) variables, so hosted-only builds never run the lookups.
have = $(shell command -v $(1) 2>/dev/null)
OSEF_LD ?= $(firstword $(wildcard $(TOOLCHAIN)/cross/bin/x86_64-elf-ld) $(call have,ld.lld) $(call have,x86_64-elf-ld))
XORRISO ?= $(firstword $(wildcard $(TOOLCHAIN)/xorriso/bin/xorriso) $(call have,xorriso))
LIMINE_DIR ?= $(firstword $(patsubst %/limine-bios.sys,%,$(wildcard $(TOOLCHAIN)/limine/limine-bios.sys)) \
                          $(if $(call have,limine),$(shell limine --print-datadir 2>/dev/null)))
LIMINE_TOOL ?= $(firstword $(wildcard $(LIMINE_DIR)/limine) $(call have,limine))
QEMU ?= $(firstword $(wildcard $(TOOLCHAIN)/qemu/bin/qemu-system-x86_64) $(call have,qemu-system-x86_64))
OVMF ?= $(firstword $(wildcard $(TOOLCHAIN)/qemu/share/qemu/edk2-x86_64-code.fd \
                               /opt/homebrew/share/qemu/edk2-x86_64-code.fd \
                               /usr/local/share/qemu/edk2-x86_64-code.fd \
                               /usr/share/qemu/edk2-x86_64-code.fd \
                               /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
                               /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/edk2/ovmf/OVMF_CODE.fd))
PYTHON ?= python3

.PHONY: all lib tool tests test os examples run clean \
        kernel iso iso-one run-iso test-iso hardware-check build-check bm-engine \
        need-ld need-iso-tools need-qemu
.DEFAULT_GOAL := all

# The selected OS (if one was selected) and every example. Each example is
# built by a sub-make, after the shared engine library and host tool exist, so
# parallel builds (-j) never build the same file twice at the same time.
all: lib tool tests examples $(if $(OS_SELECTED),os)

lib: $(LIBOSEF)
tool: $(OSEF_IMAGE)
tests: $(TEST_CORE) $(TEST_KERNEL)
os: $(OS_BIN)

# The example that is also the selected OS is built by `os`, not twice.
SELECTED_EXAMPLE := $(if $(OS_SELECTED),$(notdir $(filter $(addprefix $(ROOT)/examples/,$(EXAMPLES)),$(OS_ABS))))
examples: lib tool
	@for os in $(filter-out $(SELECTED_EXAMPLE),$(EXAMPLES)); do \
	    $(MAKE) --no-print-directory -f $(ROOT)/Makefile OS=$$os os || exit 1; done

test: tests $(OSEF_IMAGE)
	@echo "== core tests"
	@$(TEST_CORE)
	@echo "== kernel tests"
	@$(TEST_KERNEL)
	@$(MAKE) --no-print-directory -f $(ROOT)/Makefile hardware-check
	@$(MAKE) --no-print-directory -f $(ROOT)/Makefile build-check

# Hosted run of the selected OS. Run it from the OS directory so relative
# paths (like HostFS folders) land next to its sources.
run: $(OS_BIN)
	cd $(OS_ABS) && $(OS_BIN)

clean:
	@if [ -d $(BUILD) ]; then find $(BUILD) -mindepth 1 -maxdepth 1 ! -name editor -exec rm -rf {} +; fi

# ---------------------------------------------------------------- bare-metal targets
bm-engine: $(BM_ENGINE_OBJ)

# build/<os>.elf for the selected OS.
kernel: $(BM_OS_ELF)
	@cmp -s $(BM_OS_ELF) $(OS_ELF) || cp $(BM_OS_ELF) $(OS_ELF)
	@echo "kernel: $(OS_ELF)"

# One ISO for the selected OS, or one per example when no OS is selected.
ifeq ($(OS_SELECTED),yes)
iso: iso-one
else
iso:
	@for os in $(EXAMPLES); do $(MAKE) --no-print-directory -f $(ROOT)/Makefile OS=$$os iso-one || exit 1; done
endif

# Prints one line, "ISO written: ..." when the ISO changed, else
# "ISO up to date: ...", with the same key=value fields either way.
iso-one: $(BM_OS_ISO)
	@cmp -s $(BM_OS_ELF) $(OS_ELF) || cp $(BM_OS_ELF) $(OS_ELF)
	@if cmp -s $(BM_OS_ISO) $(OS_ISO); then state="ISO up to date"; else cp $(BM_OS_ISO) $(OS_ISO) && state="ISO written"; fi; \
	iso="$(OS_ISO)"; \
	if [ "$(EXPORT_ISO)" = 1 ]; then \
	    if ! cmp -s $(BM_OS_ISO) $(PROJECT_ISO); then cp $(BM_OS_ISO) $(PROJECT_ISO) && state="ISO written"; fi; \
	    iso="$(OS_SOURCE_DIR)/$(OS_NAME).iso"; \
	fi; \
	$(OSEF_IMAGE) inspect $(OS_ELF) | awk -F= -v iso="$$iso" -v src="$(OS_SOURCE_DIR)" -v state="$$state" \
	    '{ v[$$1] = $$2 } END { printf "%s: os=%s firmware=%s path=%s (source: %s)\n", state, v["name"], v["firmware"], iso, src }'

# Boot the selected OS's ISO in QEMU with the firmware it declares. The QEMU
# window shows the framebuffer console; the serial console is this terminal.
run-iso: iso-one need-qemu
	$(OSEF_IMAGE) run $(OS_ISO) --kernel $(OS_ELF) --qemu $(QEMU) $(if $(OVMF),--ovmf $(OVMF))

# Automated boot tests: every example ISO, headless, scripted serial input;
# then FaultOS (tests/iso/faultos: CPU faults and stack overflow are isolated,
# escape sequences, run --test exit codes), DevicesOS (tests/iso/devicesos: USB
# enumeration + typing on a USB keyboard, custom interrupts) and the ISO
# build-system checks.
FAULT_OS := $(ROOT)/tests/iso/faultos
DEVICES_OS := $(ROOT)/tests/iso/devicesos
test-iso: need-qemu
	@for os in $(EXAMPLES); do $(MAKE) --no-print-directory -f $(ROOT)/Makefile OS=$$os iso-one || exit 1; done
	@$(MAKE) --no-print-directory -f $(ROOT)/Makefile OS_DIR=$(FAULT_OS) iso-one
	@OSEF_IMAGE="$(OSEF_IMAGE)" QEMU="$(QEMU)" OVMF="$(OVMF)" XORRISO="$(XORRISO)" BUILD="$(BUILD)" \
	    PYTHON="$(PYTHON)" bash $(ROOT)/tests/iso/boot_test.sh $(EXAMPLES)
	@OSEF_IMAGE="$(OSEF_IMAGE)" QEMU="$(QEMU)" BUILD="$(BUILD)" PYTHON="$(PYTHON)" bash $(ROOT)/tests/iso/fault_test.sh
	@$(MAKE) --no-print-directory -f $(ROOT)/Makefile OS_DIR=$(DEVICES_OS) EXPORT_ISO=0 iso-one
	@echo "== devicesos (USB + custom interrupts)"
	@$(PYTHON) $(ROOT)/tests/iso/devices_test.py "$(OSEF_IMAGE)" "$(QEMU)" "$(OVMF)" "$(BUILD)"
	@MAKE="$(MAKE)" ROOT="$(ROOT)" OUT="$(BUILD)/tests/build-iso" bash $(ROOT)/tests/build/run.sh iso

# OSEF_HARDWARE checks: missing declarations, missing firmware and bad devices
# are rejected; valid BIOS / UEFI declarations end up in the kernel object.
hardware-check: $(OSEF_IMAGE)
	@OSEF_IMAGE="$(OSEF_IMAGE)" BM_CXX="$(BM_CXX)" BM_CXXFLAGS="$(BM_CXXFLAGS)" \
	    HOST_CXX="$(CXX)" HOST_CXXFLAGS="-std=c++20 -I$(ROOT)/engine/include" \
	    OUT="$(BUILD)/tests/hardware" bash $(ROOT)/tests/hardware/run.sh

# Makefile behavior on scratch OS directories (hosted part; the ISO part runs
# in test-iso): same-second edits, deleted sources, OS_DIR with spaces.
build-check: lib tool
	@MAKE="$(MAKE)" ROOT="$(ROOT)" OUT="$(BUILD)/tests/build-hosted" bash $(ROOT)/tests/build/run.sh hosted

need-ld:
	@if [ -z "$(OSEF_LD)" ]; then \
	    echo "error: no ELF linker found for the bare-metal kernel." >&2; \
	    echo "  Build the project-local one with: tools/bootstrap/binutils.sh  (-> .toolchain/cross/bin/x86_64-elf-ld)" >&2; \
	    echo "  or install ld.lld / x86_64-elf-ld, or pass OSEF_LD=<path to an ELF linker>." >&2; \
	    exit 1; fi

need-iso-tools:
	@if [ -z "$(XORRISO)" ]; then \
	    echo "error: xorriso not found. Run tools/bootstrap/xorriso.sh or pass XORRISO=<path>." >&2; exit 1; fi
	@if [ -z "$(LIMINE_DIR)" ] || [ ! -f "$(LIMINE_DIR)/limine-bios.sys" ]; then \
	    echo "error: Limine binaries not found. Run tools/bootstrap/limine.sh or pass LIMINE_DIR=<dir>." >&2; exit 1; fi

need-qemu:
	@if [ -z "$(QEMU)" ]; then \
	    echo "error: qemu-system-x86_64 not found. Run tools/bootstrap/qemu.sh or pass QEMU=<path>." >&2; exit 1; fi

# ---------------------------------------------------------------- rules
$(OBJ)/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(HOSTED_CXX) -c $< -o $@

$(LIBOSEF): $(ENGINE_OBJ)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(AR) rcs $@ $^

$(OSEF_IMAGE): $(ROOT)/tools/osef-image/main.cpp $(ROOT)/engine/include/osef/hardware.h
	@mkdir -p $(dir $@)
	$(CXX) -std=c++20 $(WARN) -O2 -I$(ROOT)/engine/include $< -o $@

# Core tests link only the core library (test_core.cpp provides its own
# assert_fail), so they prove the core does not depend on the engine.
$(TEST_CORE): $(OBJ)/tests/test_core.o $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(HOSTED_CXX) $^ -o $@

$(TEST_KERNEL): $(OBJ)/tests/test_kernel.o $(LIBOSEF)
	@mkdir -p $(dir $@)
	$(HOSTED_CXX) $^ -o $@

# The selected OS. main.cpp is checked for OSEF_HARDWARE before compiling.
$(OS_BUILD)/$(VARIANT)/main.o: $(OS_MAIN) $(OSEF_IMAGE)
	@mkdir -p $(dir $@)
	@$(OSEF_IMAGE) check-source $< >/dev/null
	$(HOSTED_CXX) -c $< -o $@

$(OS_BUILD)/$(VARIANT)/%.o: $(OS_ABS)/%.cpp $(OSEF_IMAGE)
	@mkdir -p $(dir $@)
	@$(OSEF_IMAGE) check-source --no-declaration $< >/dev/null
	$(HOSTED_CXX) -c $< -o $@

$(OS_BIN): $(OS_OBJ) $(LIBOSEF)
	$(HOSTED_CXX) $^ -o $@

# ---- bare metal: engine objects (shared by every OS)
$(BM_OBJDIR)/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(BM_CXX) $(BM_CXXFLAGS) -MMD -MP -c $< -o $@

$(BM_OBJDIR)/%.S.o: $(ROOT)/%.S
	@mkdir -p $(dir $@)
	$(BM_CXX) -x assembler-with-cpp $(BM_ASFLAGS) -MMD -MP -c $< -o $@

# ---- bare metal: the selected OS (its sources are compiled freestanding too)
# OS sources that build hosted can still fail here if they use the C/C++
# standard library (hosted builds have it, the kernel does not); such a
# failure gets an explanation after the compiler's own messages.
BM_STDLIB_ERRORS := fatal error: '([a-z_]+|(assert|ctype|errno|fenv|inttypes|locale|math|setjmp|signal|stdio|stdlib|string|strings|time|uchar|unistd|wchar|wctype|pthread)\.h)' file not found|in namespace 'std'|undeclared identifier '(std|printf|puts|putchar|malloc|calloc|free|exit|abort|strcpy|sprintf|snprintf)'
define bm_compile_os
	@echo "$(BM_CXX) $(BM_CXXFLAGS) -MMD -MP -c $< -o $@"
	@$(BM_CXX) $(BM_CXXFLAGS) -MMD -MP -c $< -o $@ 2>$@.log; rc=$$?; cat $@.log >&2; \
	if [ $$rc -ne 0 ] && grep -Eq "$(BM_STDLIB_ERRORS)" $@.log; then \
	    echo "osef: $<: the ISO kernel is freestanding: it has no C/C++ standard library" >&2; \
	    echo "  (<string>, <vector>, <stdio.h>, std::..., printf ...), even though the hosted build (make run) has one." >&2; \
	    echo "  Use osef::String, Vector, Map, format() and c.println() from <osef/osef.h>, or put" >&2; \
	    echo "  hosted-only code inside #if OSEF_HOSTED ... #endif." >&2; \
	fi; rm -f $@.log; exit $$rc
endef

$(BM_OS_DIR)/main.o: $(OS_MAIN) $(OSEF_IMAGE)
	@mkdir -p $(dir $@)
	@$(OSEF_IMAGE) check-source $< >/dev/null
	$(bm_compile_os)

$(BM_OS_DIR)/%.o: $(OS_ABS)/%.cpp $(OSEF_IMAGE)
	@mkdir -p $(dir $@)
	@$(OSEF_IMAGE) check-source --no-declaration $< >/dev/null
	$(bm_compile_os)

$(BM_OS_ELF): $(BM_OS_OBJ) $(BM_ENGINE_OBJ) $(BM_LDSCRIPT) | need-ld
	$(OSEF_LD) -nostdlib -static -z max-page-size=0x1000 -T $(BM_LDSCRIPT) -o $@ $(BM_OS_OBJ) $(BM_ENGINE_OBJ)

$(BM_OS_ISO): $(BM_OS_ELF) $(OSEF_IMAGE) | need-iso-tools
	$(OSEF_IMAGE) iso $(BM_OS_ELF) -o $@ --limine $(LIMINE_DIR) --xorriso $(XORRISO) \
	    $(if $(LIMINE_TOOL),--limine-tool $(LIMINE_TOOL))

# Friendly error when the selected OS directory has no main.cpp.
$(OS_MAIN):
	@echo "error: $(OS_MAIN) not found." >&2
	@echo "Select an OS with OS=<name> (one of: $(EXAMPLES)) or OS_DIR=<directory containing main.cpp>." >&2
	@exit 1

-include $(ENGINE_OBJ:.o=.d) $(OBJ)/tests/test_core.d $(OBJ)/tests/test_kernel.d $(OS_OBJ:.o=.d)
-include $(BM_ENGINE_OBJ:.o=.d) $(BM_OS_OBJ:.o=.d)
