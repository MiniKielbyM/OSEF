# OSEF CMake API.
#
#   osef_add_os(<name> [DIRECTORY <dir>] [SOURCES <file>...])
#
# Adds an operating system built with the OSEF engine. Its main.cpp must start
# with an OSEF_HARDWARE(...) declaration (see engine/include/osef/hardware.h).
#   DIRECTORY  directory of the OS (default: the current source directory).
#              Without SOURCES, every <dir>/*.cpp is compiled (like the Makefile).
#   SOURCES    explicit source list (relative to DIRECTORY, or to the current
#              source directory). The file named main.cpp is the one checked.
#
# In the normal (hosted) build this creates:
#   <name>               the OS as a native executable (links osef::osef)
#   <name>-check-source  `osef-image check-source main.cpp`: fails early, with a
#                        readable message, when the OSEF_HARDWARE declaration is missing
#   <name>-iso           the kernel ELF (from the bare-metal sub-build) and a bootable
#                        ISO for the declared firmware: <build>/images/<name>.{elf,iso},
#                        exported to <dir>/<name>.iso too (OSEF_EXPORT_ISO, default ON)
#   <name>-qemu          boot that ISO in QEMU (window = framebuffer console,
#                        terminal = serial console; OVMF for UEFI OSes)
# In the bare-metal sub-build (cmake/toolchains/x86_64-osef-baremetal.cmake) it
# creates the kernel <name>.elf instead.
#
# Bare-metal sub-build: every OS added in the hosted build is listed in a
# generated manifest. One ExternalProject (`osef_kernels`) configures this same
# source tree with the bare-metal toolchain, reads the manifest and links
# every kernel; the `<name>-iso` targets then run `osef-image iso` on them.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/OSEFBaremetalFlags.cmake")

# ------------------------------------------------------------------ helpers

function(_osef_root out)
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(${out} "${root}" PARENT_SCOPE)
endfunction()

# Warnings for every hosted OSEF target (engine, tools, tests, OSes).
function(osef_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE ${OSEF_WARNING_FLAGS})
    endif()
endfunction()

# Freestanding compile command line (one string) used by the hardware-check
# test; same flags as BM_CXXFLAGS in the root Makefile.
function(osef_baremetal_flags_string out)
    _osef_root(root)
    string(JOIN " " flags -std=c++20 --target=${OSEF_BAREMETAL_TARGET} ${OSEF_BAREMETAL_CXX_FLAGS}
           ${OSEF_WARNING_FLAGS} -O2 "-I${root}/engine/include")
    set(${out} "${flags}" PARENT_SCOPE)
endfunction()

# ------------------------------------------------------------------ tool detection (hosted build)
#
# Finds clang++ (freestanding x86_64 ELF), an ELF linker, xorriso and Limine,
# decides whether the ISO pipeline is enabled and, if not, why. Also finds QEMU
# + UEFI firmware for the <name>-qemu targets. Results: cache variables
# OSEF_BAREMETAL_CXX OSEF_LD OSEF_XORRISO OSEF_LIMINE_DIR OSEF_LIMINE_TOOL
# OSEF_QEMU OSEF_OVMF, and global properties OSEF_ISO_ENABLED / OSEF_ISO_MISSING.
function(osef_configure_iso_pipeline)
    _osef_root(root)
    set(tc "${OSEF_TOOLCHAIN_DIR}")
    set(missing "")

    if(CMAKE_CROSSCOMPILING)
        set_property(GLOBAL PROPERTY OSEF_ISO_ENABLED FALSE)
        set_property(GLOBAL PROPERTY OSEF_ISO_MISSING "a native host build (this build is cross-compiling, so osef-image cannot run)")
        message(STATUS "OSEF: bootable ISOs disabled: cross-compiling the hosted build")
        return()
    endif()

    # clang that can emit freestanding x86_64 ELF objects (the hosted compiler may be anything).
    find_program(OSEF_BAREMETAL_CXX
        NAMES clang++ clang++-21 clang++-20 clang++-19 clang++-18 clang++-17 clang++-16 clang++-15
        DOC "clang++ used to compile freestanding x86_64 ELF objects (OSEF kernels)")
    set(probe_ok FALSE)
    if(OSEF_BAREMETAL_CXX)
        set(probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/osef-probe")
        file(MAKE_DIRECTORY "${probe_dir}")
        file(WRITE "${probe_dir}/probe.cpp"
             "#include <stdint.h>\nextern \"C\" uint64_t osef_probe(uint64_t x) { return x * 3; }\n")
        execute_process(
            COMMAND "${OSEF_BAREMETAL_CXX}" -std=c++20 --target=${OSEF_BAREMETAL_TARGET} ${OSEF_BAREMETAL_CXX_FLAGS}
                    -c "${probe_dir}/probe.cpp" -o "${probe_dir}/probe.o"
            RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
        if(rc EQUAL 0)
            set(probe_ok TRUE)
        else()
            list(APPEND missing "a clang++ that can compile for ${OSEF_BAREMETAL_TARGET} (${OSEF_BAREMETAL_CXX} cannot)")
        endif()
    else()
        list(APPEND missing "clang++ (compiles the freestanding kernel; install clang/LLVM)")
    endif()
    set_property(GLOBAL PROPERTY OSEF_BAREMETAL_CXX_OK ${probe_ok})

    find_program(OSEF_LD NAMES x86_64-elf-ld ld.lld HINTS "${tc}/cross/bin"
        DOC "ELF linker for OSEF kernels (x86_64-elf-ld or ld.lld)")
    if(OSEF_LD AND NOT EXISTS "${OSEF_LD}")
        list(APPEND missing "OSEF_LD=${OSEF_LD} (no such file)")
    elseif(NOT OSEF_LD)
        list(APPEND missing "an ELF linker (x86_64-elf-ld: tools/bootstrap/binutils.sh, or ld.lld from LLVM)")
    endif()

    find_program(OSEF_XORRISO NAMES xorriso HINTS "${tc}/xorriso/bin" DOC "xorriso (builds the ISO image)")
    if(OSEF_XORRISO AND NOT EXISTS "${OSEF_XORRISO}")
        list(APPEND missing "OSEF_XORRISO=${OSEF_XORRISO} (no such file)")
    elseif(NOT OSEF_XORRISO)
        list(APPEND missing "xorriso (tools/bootstrap/xorriso.sh, or your package manager)")
    endif()

    find_path(OSEF_LIMINE_DIR NAMES limine-bios.sys HINTS "${tc}/limine"
        PATHS /usr/share/limine /usr/local/share/limine /opt/homebrew/share/limine
        DOC "Directory with the Limine binaries (limine-bios.sys, limine-bios-cd.bin, limine-uefi-cd.bin, BOOTX64.EFI)")
    set(limine_ok FALSE)
    if(OSEF_LIMINE_DIR)
        set(limine_ok TRUE)
        foreach(f limine-bios-cd.bin limine-uefi-cd.bin BOOTX64.EFI)
            if(NOT EXISTS "${OSEF_LIMINE_DIR}/${f}")
                set(limine_ok FALSE)
            endif()
        endforeach()
    endif()
    if(limine_ok)
        find_program(OSEF_LIMINE_TOOL NAMES limine HINTS "${OSEF_LIMINE_DIR}"
            DOC "Limine host utility (`limine bios-install`)")
    endif()
    if(NOT limine_ok)
        list(APPEND missing "the Limine binaries (tools/bootstrap/limine.sh, or -DOSEF_LIMINE_DIR=<dir>)")
    elseif(NOT OSEF_LIMINE_TOOL)
        list(APPEND missing "the Limine host utility `limine` (tools/bootstrap/limine.sh builds it)")
    endif()

    # The sub-build needs a single-configuration generator.
    get_property(multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    set(sub_generator "")
    set(sub_make "")
    if(NOT multi AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        set(sub_generator "${CMAKE_GENERATOR}")
        set(sub_make "${CMAKE_MAKE_PROGRAM}")
    elseif(CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
        set(sub_generator "Ninja")
        set(sub_make "${CMAKE_MAKE_PROGRAM}")
    else()
        find_program(OSEF_NINJA NAMES ninja ninja-build DOC "Ninja, for the bare-metal kernel sub-build")
        if(OSEF_NINJA)
            set(sub_generator "Ninja")
            set(sub_make "${OSEF_NINJA}")
        else()
            list(APPEND missing "Ninja (the ${CMAKE_GENERATOR} generator cannot build the bare-metal sub-build)")
        endif()
    endif()
    set_property(GLOBAL PROPERTY OSEF_SUB_GENERATOR "${sub_generator}")
    set_property(GLOBAL PROPERTY OSEF_SUB_MAKE_PROGRAM "${sub_make}")

    # QEMU is optional: only the <name>-qemu targets and the boot tests need it.
    find_program(OSEF_QEMU NAMES qemu-system-x86_64 HINTS "${tc}/qemu/bin"
        PATHS "C:/Program Files/qemu" DOC "qemu-system-x86_64 (for <os>-qemu targets)")
    set(ovmf_paths "${tc}/qemu/share/qemu")
    if(OSEF_QEMU)
        get_filename_component(qemu_bin "${OSEF_QEMU}" DIRECTORY)
        list(APPEND ovmf_paths "${qemu_bin}/../share/qemu" "${qemu_bin}/share")
    endif()
    find_file(OSEF_OVMF
        NAMES edk2-x86_64-code.fd OVMF_CODE_4M.fd OVMF_CODE.fd OVMF_CODE.4m.fd
        PATHS ${ovmf_paths} /opt/homebrew/share/qemu /usr/local/share/qemu /usr/share/qemu
              /usr/share/OVMF /usr/share/edk2/x64 /usr/share/edk2/ovmf
        NO_DEFAULT_PATH DOC "UEFI firmware code image (OVMF) for UEFI OSes")

    # Decide.
    string(TOUPPER "${OSEF_BUILD_ISO}" mode)
    list(JOIN missing "; " missing_text)
    if(mode STREQUAL "AUTO")
        if(missing)
            set(enabled FALSE)
            message(STATUS "OSEF: bootable ISOs disabled (OSEF_BUILD_ISO=AUTO). Missing: ${missing_text}. "
                           "Run tools/bootstrap.sh, then re-run CMake.")
        else()
            set(enabled TRUE)
        endif()
    elseif(OSEF_BUILD_ISO)
        if(missing)
            message(FATAL_ERROR "OSEF_BUILD_ISO=ON, but the bare-metal tools are incomplete. Missing: ${missing_text}. "
                                "Run tools/bootstrap.sh, or point CMake at the tools with -DOSEF_BAREMETAL_CXX=, "
                                "-DOSEF_LD=, -DOSEF_XORRISO=, -DOSEF_LIMINE_DIR=.")
        endif()
        set(enabled TRUE)
    else()
        set(enabled FALSE)
        set(missing_text "OSEF_BUILD_ISO=ON (it is OFF)")
        message(STATUS "OSEF: bootable ISOs disabled (OSEF_BUILD_ISO=OFF)")
    endif()
    if(enabled)
        message(STATUS "OSEF: bootable ISOs enabled: clang ${OSEF_BAREMETAL_CXX}, linker ${OSEF_LD}, "
                       "xorriso ${OSEF_XORRISO}, limine ${OSEF_LIMINE_DIR}")
        if(OSEF_QEMU)
            message(STATUS "OSEF: QEMU ${OSEF_QEMU}, UEFI firmware ${OSEF_OVMF}")
        else()
            message(STATUS "OSEF: QEMU not found: <os>-qemu targets will fail (tools/bootstrap.sh --with-qemu)")
        endif()
    endif()
    set_property(GLOBAL PROPERTY OSEF_ISO_ENABLED ${enabled})
    set_property(GLOBAL PROPERTY OSEF_ISO_MISSING "${missing_text}")
    set_property(GLOBAL PROPERTY OSEF_KERNEL_DIR "${CMAKE_BINARY_DIR}/osef-baremetal/kernels")
    set_property(GLOBAL PROPERTY OSEF_IMAGE_DIR "${CMAKE_BINARY_DIR}/images")
endfunction()

# ------------------------------------------------------------------ bare-metal engine

# The engine compiled freestanding: engine/src/*.cpp, core (with cstring.cpp),
# and the bare-metal layer (C++ and .S). An object library, so every kernel
# links exactly the same objects the Makefile links.
function(osef_add_kernel_engine)
    if(TARGET osef_kernel_engine)
        return()
    endif()
    _osef_root(root)
    # Same order as the Makefile: engine, core, bare-metal C++, assembly.
    set(engine_src "")
    foreach(pattern engine/src/*.cpp engine/src/core/*.cpp engine/src/baremetal/*.cpp engine/src/baremetal/*.S)
        file(GLOB files CONFIGURE_DEPENDS "${root}/${pattern}")
        list(SORT files)
        list(APPEND engine_src ${files})
    endforeach()
    add_library(osef_kernel_engine OBJECT ${engine_src})
    set_target_properties(osef_kernel_engine PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    target_include_directories(osef_kernel_engine PUBLIC "${root}/engine/include")
    target_compile_features(osef_kernel_engine PUBLIC cxx_std_20)
    target_compile_options(osef_kernel_engine PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${OSEF_WARNING_FLAGS}>" "$<$<COMPILE_LANGUAGE:ASM>:-Wall;-Wextra>")
endfunction()

# ------------------------------------------------------------------ osef_add_os

function(osef_add_os name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "DIRECTORY" "SOURCES")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "osef_add_os(${name}): unexpected arguments: ${ARG_UNPARSED_ARGUMENTS} "
                            "(usage: osef_add_os(<name> [DIRECTORY <dir>] [SOURCES <file>...]))")
    endif()
    _osef_root(root)

    # Resolve the OS directory and its sources.
    if(ARG_DIRECTORY)
        get_filename_component(dir "${ARG_DIRECTORY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        set(dir "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    set(sources "")
    if(ARG_SOURCES)
        foreach(src IN LISTS ARG_SOURCES)
            get_filename_component(abs "${src}" ABSOLUTE BASE_DIR "${dir}")
            list(APPEND sources "${abs}")
        endforeach()
    else()
        if(NOT EXISTS "${dir}/main.cpp")
            message(FATAL_ERROR "osef_add_os(${name}): ${dir}/main.cpp not found. An OSEF OS directory needs a "
                                "main.cpp that starts with OSEF_HARDWARE(...).")
        endif()
        file(GLOB sources CONFIGURE_DEPENDS "${dir}/*.cpp")
        list(SORT sources)
    endif()
    set(main "")
    foreach(src IN LISTS sources)
        get_filename_component(fname "${src}" NAME)
        if(fname STREQUAL "main.cpp" AND NOT main)
            set(main "${src}")
        endif()
    endforeach()
    if(NOT main)
        list(GET sources 0 main)
    endif()

    if(OSEF_BAREMETAL_TOOLCHAIN)
        # ---- bare-metal sub-build: the kernel ELF
        osef_add_kernel_engine()
        if(NOT OSEF_KERNEL_OUTPUT_DIR)
            set(OSEF_KERNEL_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
        endif()
        # The OS's own objects first, then the engine: the same link order as
        # the Makefile with every generator, so both produce the same kernel.
        add_library(${name}_os_objects OBJECT ${sources})
        target_include_directories(${name}_os_objects PRIVATE "${root}/engine/include")
        target_compile_features(${name}_os_objects PRIVATE cxx_std_20)
        target_compile_options(${name}_os_objects PRIVATE ${OSEF_WARNING_FLAGS})
        set_target_properties(${name}_os_objects PROPERTIES CXX_SCAN_FOR_MODULES OFF)
        add_executable(${name} $<TARGET_OBJECTS:${name}_os_objects> $<TARGET_OBJECTS:osef_kernel_engine>)
        set_target_properties(${name} PROPERTIES
            LINKER_LANGUAGE CXX
            SUFFIX ".elf"
            RUNTIME_OUTPUT_DIRECTORY "${OSEF_KERNEL_OUTPUT_DIR}"
            LINK_DEPENDS "${OSEF_LINKER_SCRIPT}")
        return()
    endif()

    # ---- hosted: native executable
    add_executable(${name} ${sources})
    set_target_properties(${name} PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    target_link_libraries(${name} PRIVATE osef::osef)
    osef_target_warnings(${name})
    if(EMSCRIPTEN)
        set_target_properties(${name} PROPERTIES SUFFIX ".html")
    endif()

    # check-source runs before anything of this OS is compiled.
    if(TARGET osef-image AND NOT CMAKE_CROSSCOMPILING)
        set(stamp "${CMAKE_CURRENT_BINARY_DIR}/osef/${name}.source-checked")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/osef")
        add_custom_command(OUTPUT "${stamp}"
            COMMAND $<TARGET_FILE:osef-image> check-source "${main}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
            DEPENDS "${main}" osef-image
            COMMENT "osef-image check-source ${main}"
            VERBATIM)
        add_custom_target(${name}-check-source DEPENDS "${stamp}")
        add_dependencies(${name} ${name}-check-source)
    endif()

    set_property(GLOBAL APPEND PROPERTY OSEF_OS_LIST ${name})
    set_property(GLOBAL PROPERTY OSEF_OS_${name}_DIR "${dir}")
    set_property(GLOBAL PROPERTY OSEF_OS_${name}_SOURCES "${sources}")

    # ---- kernel ELF + ISO + QEMU
    get_property(iso_enabled GLOBAL PROPERTY OSEF_ISO_ENABLED)
    get_property(kernel_dir GLOBAL PROPERTY OSEF_KERNEL_DIR)
    get_property(image_dir GLOBAL PROPERTY OSEF_IMAGE_DIR)
    if(iso_enabled AND TARGET osef-image)
        set(kernel "${kernel_dir}/${name}.elf")
        set(elf "${image_dir}/${name}.elf")
        set(iso "${image_dir}/${name}.iso")
        set(limine_tool_args "")
        if(OSEF_LIMINE_TOOL)
            set(limine_tool_args --limine-tool "${OSEF_LIMINE_TOOL}")
        endif()
        add_custom_command(OUTPUT "${iso}" "${elf}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${image_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy "${kernel}" "${elf}"
            COMMAND $<TARGET_FILE:osef-image> iso "${elf}" -o "${iso}"
                    --limine "${OSEF_LIMINE_DIR}" --xorriso "${OSEF_XORRISO}" ${limine_tool_args}
            # Export a copy into the OS's own project folder (OSEF_EXPORT_ISO).
            COMMAND "$<IF:$<BOOL:${OSEF_EXPORT_ISO}>,${CMAKE_COMMAND};-E;copy;${iso};${dir}/${name}.iso,${CMAKE_COMMAND};-E;true>"
            COMMAND_EXPAND_LISTS
            DEPENDS "${kernel}" osef-image
            COMMENT "Building bootable ISO ${iso}"
            VERBATIM)
        add_custom_target(${name}-iso ALL DEPENDS "${iso}")
        add_dependencies(${name}-iso osef_kernels)
        set_property(GLOBAL APPEND PROPERTY OSEF_KERNEL_BYPRODUCTS "${kernel}")

        if(OSEF_QEMU)
            set(ovmf_args "")
            if(OSEF_OVMF)
                set(ovmf_args --ovmf "${OSEF_OVMF}")
            endif()
            add_custom_target(${name}-qemu
                COMMAND $<TARGET_FILE:osef-image> run "${iso}" --kernel "${elf}" --qemu "${OSEF_QEMU}" ${ovmf_args}
                USES_TERMINAL
                COMMENT "Booting ${iso} in QEMU (serial console on this terminal)"
                VERBATIM)
        else()
            add_custom_target(${name}-qemu
                COMMAND "${CMAKE_COMMAND}" -E echo "error: qemu-system-x86_64 not found: run tools/bootstrap.sh --with-qemu, or pass -DOSEF_QEMU=<path>"
                COMMAND "${CMAKE_COMMAND}" -E false
                VERBATIM)
        endif()
        add_dependencies(${name}-qemu ${name}-iso)

        # The sub-build is created once, after every osef_add_os() call.
        get_property(scheduled GLOBAL PROPERTY OSEF_FINALIZE_SCHEDULED)
        if(NOT scheduled)
            set_property(GLOBAL PROPERTY OSEF_FINALIZE_SCHEDULED TRUE)
            cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _osef_finalize)
        endif()
    else()
        get_property(why GLOBAL PROPERTY OSEF_ISO_MISSING)
        if(NOT why)
            set(why "the bare-metal pipeline is not configured (include OSEF's root CMakeLists.txt)")
        endif()
        foreach(t iso qemu)
            add_custom_target(${name}-${t}
                COMMAND "${CMAKE_COMMAND}" -E echo "error: ${name}-${t}: bootable ISOs are disabled. Needed: ${why}. Run tools/bootstrap.sh and re-run CMake."
                COMMAND "${CMAKE_COMMAND}" -E false
                VERBATIM)
        endforeach()
    endif()
endfunction()

# ------------------------------------------------------------------ the bare-metal sub-build

function(_osef_finalize)
    _osef_root(root)
    get_property(oses GLOBAL PROPERTY OSEF_OS_LIST)
    get_property(kernel_dir GLOBAL PROPERTY OSEF_KERNEL_DIR)
    get_property(image_dir GLOBAL PROPERTY OSEF_IMAGE_DIR)
    get_property(byproducts GLOBAL PROPERTY OSEF_KERNEL_BYPRODUCTS)
    get_property(generator GLOBAL PROPERTY OSEF_SUB_GENERATOR)
    get_property(make_program GLOBAL PROPERTY OSEF_SUB_MAKE_PROGRAM)
    set(base "${CMAKE_BINARY_DIR}/osef-baremetal")

    # Manifest: one osef_add_os() call per OS, read by the sub-build. Rewritten
    # only when it changes, so the sub-build reconfigures only when needed.
    set(manifest "# Generated by cmake/OSEF.cmake: the OSes of the hosted build.\n")
    foreach(os IN LISTS oses)
        get_property(dir GLOBAL PROPERTY OSEF_OS_${os}_DIR)
        get_property(sources GLOBAL PROPERTY OSEF_OS_${os}_SOURCES)
        string(APPEND manifest "osef_add_os([==[${os}]==] DIRECTORY [==[${dir}]==] SOURCES")
        foreach(src IN LISTS sources)
            string(APPEND manifest "\n    [==[${src}]==]")
        endforeach()
        string(APPEND manifest ")\n")
    endforeach()
    file(WRITE "${base}/os-manifest.cmake.new" "${manifest}")
    configure_file("${base}/os-manifest.cmake.new" "${base}/os-manifest.cmake" COPYONLY)

    set(cache_args
        "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${root}/cmake/toolchains/x86_64-osef-baremetal.cmake"
        "-DCMAKE_BUILD_TYPE:STRING=${OSEF_KERNEL_BUILD_TYPE}"
        "-DCMAKE_CXX_COMPILER:FILEPATH=${OSEF_BAREMETAL_CXX}"
        "-DOSEF_LD:FILEPATH=${OSEF_LD}"
        "-DOSEF_TOOLCHAIN_DIR:PATH=${OSEF_TOOLCHAIN_DIR}"
        "-DOSEF_OS_MANIFEST:FILEPATH=${base}/os-manifest.cmake"
        "-DOSEF_KERNEL_OUTPUT_DIR:PATH=${kernel_dir}")
    if(make_program)
        list(APPEND cache_args "-DCMAKE_MAKE_PROGRAM:FILEPATH=${make_program}")
    endif()

    include(ExternalProject)
    ExternalProject_Add(osef_kernels
        SOURCE_DIR "${root}"
        PREFIX "${base}"
        BINARY_DIR "${base}/build"
        CMAKE_GENERATOR "${generator}"
        CMAKE_CACHE_ARGS ${cache_args}
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
        BUILD_BYPRODUCTS ${byproducts}
        EXCLUDE_FROM_ALL TRUE)
    # Each OS's OSEF_HARDWARE declaration is checked before its kernel is compiled.
    foreach(os IN LISTS oses)
        if(TARGET ${os}-check-source)
            add_dependencies(osef_kernels ${os}-check-source)
        endif()
    endforeach()

    # Optional ctest boot test of every OS's ISO (slow: about 30 s per OS).
    if(OSEF_TEST_ISO)
        get_property(fixtures_ok GLOBAL PROPERTY OSEF_TESTS_ENABLED)
        find_package(Python3 COMPONENTS Interpreter)
        find_program(OSEF_BASH bash)
        if(NOT OSEF_QEMU OR NOT Python3_Interpreter_FOUND OR NOT OSEF_BASH)
            message(WARNING "OSEF_TEST_ISO=ON needs QEMU, python3 and bash; the ISO boot test is not added")
        elseif(fixtures_ok)
            add_test(NAME osef_iso_boot
                COMMAND "${CMAKE_COMMAND}" -E env
                    "OSEF_IMAGE=$<TARGET_FILE:osef-image>" "QEMU=${OSEF_QEMU}" "OVMF=${OSEF_OVMF}"
                    "XORRISO=${OSEF_XORRISO}" "BUILD=${image_dir}" "PYTHON=${Python3_EXECUTABLE}"
                    "${OSEF_BASH}" "${root}/tests/iso/boot_test.sh" ${oses})
            set_tests_properties(osef_iso_boot PROPERTIES LABELS "iso" TIMEOUT 900)
        endif()
    endif()
endfunction()
