# OSEF: an operating system engine.
#
# Hosted build (Windows / macOS / Linux; any C++20 compiler: clang, gcc, MSVC):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   ctest --test-dir build -C Release        # core, kernel and hardware-declaration tests
#
# Bootable ISOs (bare metal, x86_64, Limine): enabled automatically when clang,
# an ELF linker (x86_64-elf-ld or ld.lld), xorriso and Limine are found (run
# tools/bootstrap.sh). Then `cmake --build build` also writes
# build/images/<os>.{elf,iso}, and per OS:
#   cmake --build build --target <os>-iso    # kernel ELF + ISO for the declared firmware
#   cmake --build build --target <os>-qemu   # boot it in QEMU
# The kernels come from a bare-metal sub-build of this same tree
# (cmake/toolchains/x86_64-osef-baremetal.cmake; see cmake/OSEF.cmake).
#
# Options:
#   OSEF_BUILD_ISO      AUTO (default: ON when the tools are found) | ON | OFF
#   OSEF_TOOLCHAIN_DIR  project-local tools (default .toolchain, relative to this directory)
#   OSEF_BUILD_EXAMPLES / OSEF_BUILD_TESTS (ON when this is the top-level project)
#   OSEF_TEST_ISO       add the ISO boot test to ctest (OFF; slow, needs QEMU)
#   OSEF_BUILD_EDITOR   add_subdirectory(editor) (OFF)
cmake_minimum_required(VERSION 3.21...4.4)
project(OSEF VERSION 0.1.0 LANGUAGES CXX)

set(OSEF_TOOLCHAIN_DIR ".toolchain" CACHE PATH
    "Directory with the project-local tools from tools/bootstrap (relative paths are relative to the OSEF source tree)")
if(IS_ABSOLUTE "${OSEF_TOOLCHAIN_DIR}")
    set(OSEF_TOOLCHAIN_DIR_ABS "${OSEF_TOOLCHAIN_DIR}")
else()
    get_filename_component(OSEF_TOOLCHAIN_DIR_ABS "${OSEF_TOOLCHAIN_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

option(OSEF_BUILD_EXAMPLES "Build the example operating systems" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# OSEF does not use C++20 modules; don't make Ninja builds require clang-scan-deps.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/OSEF.cmake")

# ================================================================ bare-metal mode
# Configured with cmake/toolchains/x86_64-osef-baremetal.cmake: build kernels only.
if(OSEF_BAREMETAL_TOOLCHAIN)
    enable_language(ASM)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    osef_add_kernel_engine()
    if(OSEF_OS_MANIFEST)
        # Sub-build of the hosted build: exactly the OSes it added.
        include("${OSEF_OS_MANIFEST}")
    elseif(OSEF_BUILD_EXAMPLES)
        osef_add_os(minimal DIRECTORY examples/minimal)
        osef_add_os(myos DIRECTORY examples/myos)
    endif()
    return()
endif()

# ================================================================ hosted mode
set(OSEF_BUILD_ISO AUTO CACHE STRING
    "Build bootable ISOs: AUTO (when clang, an ELF linker, xorriso and Limine are found), ON or OFF")
set_property(CACHE OSEF_BUILD_ISO PROPERTY STRINGS AUTO ON OFF)
set(OSEF_KERNEL_BUILD_TYPE Release CACHE STRING "CMAKE_BUILD_TYPE of the bare-metal kernel sub-build")
option(OSEF_BUILD_TESTS "Build the engine tests" ${PROJECT_IS_TOP_LEVEL})
option(OSEF_TEST_ISO "Add the ISO boot test (tests/iso/boot_test.sh) to ctest" OFF)
option(OSEF_BUILD_EDITOR "Build the OSEF editor (editor/)" OFF)
# osef_configure_iso_pipeline() and osef_add_os() read the absolute path.
set(OSEF_TOOLCHAIN_DIR "${OSEF_TOOLCHAIN_DIR_ABS}")

# ---------------------------------------------------------------- engine (hosted)
# engine/src/core/cstring.cpp (memcpy & co.) is bare-metal only: hosted builds use libc.
file(GLOB OSEF_CORE_SOURCES CONFIGURE_DEPENDS engine/src/core/*.cpp)
list(FILTER OSEF_CORE_SOURCES EXCLUDE REGEX "/cstring\\.cpp$")
file(GLOB OSEF_ENGINE_SOURCES CONFIGURE_DEPENDS engine/src/*.cpp engine/src/hosted/*.cpp)

add_library(osef STATIC ${OSEF_ENGINE_SOURCES} ${OSEF_CORE_SOURCES})
add_library(osef::osef ALIAS osef)
target_include_directories(osef PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/engine/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(osef PUBLIC cxx_std_20)
osef_target_warnings(osef)
# std::filesystem (HostFS) needs an extra library on older GCC.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
    target_link_libraries(osef PUBLIC stdc++fs)
endif()

# ---------------------------------------------------------------- osef-image (host tool)
# check-source / inspect / iso / run. Not built when cross-compiling the hosted
# build (it has to run on the build machine).
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(osef-image tools/osef-image/main.cpp)
    target_include_directories(osef-image PRIVATE engine/include)
    target_compile_features(osef-image PRIVATE cxx_std_20)
    osef_target_warnings(osef-image)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
        target_link_libraries(osef-image PRIVATE stdc++fs)
    endif()
endif()

# ---------------------------------------------------------------- ISO pipeline detection
osef_configure_iso_pipeline()

# ---------------------------------------------------------------- tests
if(OSEF_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    set_property(GLOBAL PROPERTY OSEF_TESTS_ENABLED TRUE)

    # Core tests link only the core library (test_core.cpp provides its own
    # assert_fail), which proves the core does not depend on the engine.
    add_executable(osef_core_tests tests/test_core.cpp ${OSEF_CORE_SOURCES})
    target_include_directories(osef_core_tests PRIVATE engine/include)
    osef_target_warnings(osef_core_tests)
    add_test(NAME osef_core_tests COMMAND osef_core_tests)

    add_executable(osef_tests tests/test_kernel.cpp)
    target_link_libraries(osef_tests PRIVATE osef::osef)
    osef_target_warnings(osef_tests)
    add_test(NAME osef_tests COMMAND osef_tests)

    # OSEF_HARDWARE fixtures (tests/hardware/run.sh): bad declarations are
    # rejected, good ones compile freestanding and reach the object file.
    # Needs bash and a clang that targets x86_64 ELF.
    find_program(OSEF_BASH bash)
    get_property(bm_cxx_ok GLOBAL PROPERTY OSEF_BAREMETAL_CXX_OK)
    if(TARGET osef-image AND OSEF_BASH AND bm_cxx_ok AND NOT WIN32)
        osef_baremetal_flags_string(OSEF_HW_CHECK_FLAGS)
        add_test(NAME osef_hardware_check
            COMMAND "${CMAKE_COMMAND}" -E env
                "OSEF_IMAGE=$<TARGET_FILE:osef-image>" "BM_CXX=${OSEF_BAREMETAL_CXX}"
                "BM_CXXFLAGS=${OSEF_HW_CHECK_FLAGS}" "OUT=${CMAKE_CURRENT_BINARY_DIR}/tests/hardware"
                "HOST_CXX=${CMAKE_CXX_COMPILER}" "HOST_CXXFLAGS=-std=c++20 -I${CMAKE_CURRENT_SOURCE_DIR}/engine/include"
                "${OSEF_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/hardware/run.sh")
        set_tests_properties(osef_hardware_check PROPERTIES LABELS "hardware")
    else()
        message(STATUS "OSEF: hardware-check test skipped (needs bash and a clang++ that targets x86_64 ELF; not run on Windows)")
    endif()
endif()

# ---------------------------------------------------------------- example operating systems
if(OSEF_BUILD_EXAMPLES)
    osef_add_os(minimal DIRECTORY examples/minimal)
    osef_add_os(myos DIRECTORY examples/myos)
endif()

# ---------------------------------------------------------------- editor (other team; optional)
if(OSEF_BUILD_EDITOR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/editor/CMakeLists.txt")
        add_subdirectory(editor)
    else()
        message(WARNING "OSEF_BUILD_EDITOR=ON, but editor/CMakeLists.txt does not exist")
    endif()
endif()

# ---------------------------------------------------------------- install
install(TARGETS osef EXPORT osefTargets ARCHIVE DESTINATION lib)
if(TARGET osef-image)
    install(TARGETS osef-image RUNTIME DESTINATION bin)
endif()
install(DIRECTORY engine/include/ DESTINATION include)
install(EXPORT osefTargets NAMESPACE osef:: DESTINATION lib/cmake/osef)
