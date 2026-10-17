# Freestanding x86_64 flags for OSEF kernels (from .toolchain/CONTRACT.md).
# Shared by the bare-metal toolchain file, cmake/OSEF.cmake (hardware-check
# test) and kept in sync with BM_CXXFLAGS in the root Makefile.
include_guard(GLOBAL)

set(OSEF_BAREMETAL_TARGET "x86_64-unknown-none-elf")

# Compile flags every freestanding OSEF translation unit needs (besides
# -std=c++20 and --target=..., which CMake adds itself).
set(OSEF_BAREMETAL_CXX_FLAGS
    -ffreestanding -nostdinc++ -nostdlib
    -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-stack-protector
    -fno-pic -fno-pie -mno-red-zone -mcmodel=kernel -fno-asynchronous-unwind-tables
    -DOSEF_BAREMETAL=1)

# Link flags for the ELF linker (GNU ld or ld.lld), plus `-T <linker script>`.
set(OSEF_BAREMETAL_LINK_FLAGS -nostdlib -static -z max-page-size=0x1000)

set(OSEF_WARNING_FLAGS -Wall -Wextra -Wpedantic)
