# CMake toolchain file for OSEF bare-metal x86_64 kernels.
#
# Compiles freestanding x86_64 ELF objects with clang
# (--target=x86_64-unknown-none-elf plus the flags from .toolchain/CONTRACT.md)
# and links kernels by calling an ELF linker directly (x86_64-elf-ld or
# ld.lld) with engine/src/baremetal/linker.ld. clang is never used as the link
# driver: Apple clang cannot link ELF at all.
#
# Normally you don't use this file yourself: the hosted build (root
# CMakeLists.txt) runs a bare-metal sub-build of the same source tree with it
# for the `<os>-iso` targets. It also works standalone:
#
#   cmake -S . -B build-kernel --toolchain cmake/toolchains/x86_64-osef-baremetal.cmake
#   cmake --build build-kernel          # -> build-kernel/<os>.elf for every example
#
# Cache variables (all optional):
#   CMAKE_CXX_COMPILER   clang++ (any clang >= 15 that can emit x86_64 ELF; Apple clang works)
#   OSEF_LD              ELF linker; default: <toolchain>/cross/bin/x86_64-elf-ld, then
#                        x86_64-elf-ld / ld.lld from PATH
#   OSEF_TOOLCHAIN_DIR   project-local tools (default: <repo>/.toolchain)
#   OSEF_LINKER_SCRIPT   default: <repo>/engine/src/baremetal/linker.ld
#
# Windows / Linux hosts: install LLVM (clang + lld) and this file finds
# clang++ and ld.lld on PATH; nothing else is needed to link kernels.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(OSEF_BAREMETAL_TOOLCHAIN TRUE)

get_filename_component(_osef_tc_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/../OSEFBaremetalFlags.cmake")

if(NOT OSEF_TOOLCHAIN_DIR)
    set(OSEF_TOOLCHAIN_DIR "${_osef_tc_root}/.toolchain")
elseif(NOT IS_ABSOLUTE "${OSEF_TOOLCHAIN_DIR}")
    get_filename_component(OSEF_TOOLCHAIN_DIR "${OSEF_TOOLCHAIN_DIR}" ABSOLUTE BASE_DIR "${_osef_tc_root}")
endif()

# ---- compiler: clang for C++ and for the .S files
if(NOT CMAKE_CXX_COMPILER)
    find_program(OSEF_BAREMETAL_CXX
        NAMES clang++ clang++-21 clang++-20 clang++-19 clang++-18 clang++-17 clang++-16 clang++-15
        DOC "clang++ used to compile freestanding x86_64 ELF objects")
    if(NOT OSEF_BAREMETAL_CXX)
        message(FATAL_ERROR "OSEF bare-metal toolchain: clang++ not found. Install clang/LLVM "
                            "or pass -DCMAKE_CXX_COMPILER=<path to clang++>.")
    endif()
    set(CMAKE_CXX_COMPILER "${OSEF_BAREMETAL_CXX}")
endif()
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER "${CMAKE_CXX_COMPILER}")
endif()
set(CMAKE_CXX_COMPILER_TARGET "${OSEF_BAREMETAL_TARGET}")
set(CMAKE_ASM_COMPILER_TARGET "${OSEF_BAREMETAL_TARGET}")

# Compiler checks only compile (there is no C library to link against).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

string(JOIN " " _osef_cxx_flags ${OSEF_BAREMETAL_CXX_FLAGS})
set(CMAKE_CXX_FLAGS_INIT "${_osef_cxx_flags}")
# Per-configuration flags as cache entries, so CMake's defaults (-O3 -DNDEBUG)
# don't get appended: Release is -O2, like the Makefile's BM_OPT.
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g" CACHE STRING "C++ flags for Debug kernels")
set(CMAKE_CXX_FLAGS_RELEASE "-O2" CACHE STRING "C++ flags for Release kernels")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g" CACHE STRING "C++ flags for RelWithDebInfo kernels")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os" CACHE STRING "C++ flags for MinSizeRel kernels")
foreach(_osef_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_ASM_FLAGS_${_osef_cfg} "" CACHE STRING "Assembler flags for ${_osef_cfg} kernels")
endforeach()

# ---- archiver (only used by CMake's own compiler checks)
find_program(OSEF_BAREMETAL_AR NAMES x86_64-elf-ar llvm-ar
    HINTS "${OSEF_TOOLCHAIN_DIR}/cross/bin" DOC "archiver for ELF objects")
find_program(OSEF_BAREMETAL_RANLIB NAMES x86_64-elf-ranlib llvm-ranlib
    HINTS "${OSEF_TOOLCHAIN_DIR}/cross/bin" DOC "ranlib for ELF archives")
if(OSEF_BAREMETAL_AR AND OSEF_BAREMETAL_RANLIB)
    set(CMAKE_AR "${OSEF_BAREMETAL_AR}" CACHE FILEPATH "Archiver" FORCE)
    set(CMAKE_RANLIB "${OSEF_BAREMETAL_RANLIB}" CACHE FILEPATH "Ranlib" FORCE)
endif()

# ---- linker: an ELF linker called directly, with the OSEF linker script
if(NOT OSEF_LD)
    find_program(OSEF_LD NAMES x86_64-elf-ld ld.lld
        HINTS "${OSEF_TOOLCHAIN_DIR}/cross/bin"
        DOC "ELF linker for OSEF kernels (GNU ld for x86_64-elf, or ld.lld)")
endif()
if(NOT OSEF_LINKER_SCRIPT)
    set(OSEF_LINKER_SCRIPT "${_osef_tc_root}/engine/src/baremetal/linker.ld")
endif()
set(OSEF_LINKER_SCRIPT "${OSEF_LINKER_SCRIPT}" CACHE FILEPATH "Linker script for OSEF kernels")

if(OSEF_LD)
    set(CMAKE_LINKER "${OSEF_LD}" CACHE FILEPATH "ELF linker" FORCE)
    string(JOIN " " _osef_ld_flags ${OSEF_BAREMETAL_LINK_FLAGS})
    set(CMAKE_CXX_LINK_EXECUTABLE
        "\"${OSEF_LD}\" ${_osef_ld_flags} -T \"${OSEF_LINKER_SCRIPT}\" <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
else()
    # Configuring still works (objects compile); linking a kernel fails with this message.
    set(CMAKE_CXX_LINK_EXECUTABLE
        "\"${CMAKE_COMMAND}\" -E echo \"error: no ELF linker for OSEF kernels: run tools/bootstrap.sh (builds .toolchain/cross/bin/x86_64-elf-ld), install ld.lld, or pass -DOSEF_LD=<linker>\""
        "\"${CMAKE_COMMAND}\" -E false")
endif()

# Variables CMake's compiler checks need to see again inside try_compile projects.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES OSEF_TOOLCHAIN_DIR OSEF_LD OSEF_LINKER_SCRIPT)
