# OSEF Editor (Dear ImGui docking + GLFW + OpenGL 3.2 core).
#
# Standalone:      cmake -S editor -B build-editor && cmake --build build-editor
#                  ctest --test-dir build-editor
# From the root:   add_subdirectory(editor)
#
# Dependencies come from editor/third_party/ (tools/bootstrap/editor-deps.sh)
# when present, otherwise FetchContent downloads the same pinned archives.
# Keep the URLs + hashes below in sync with tools/bootstrap/editor-deps.sh.
#
# Targets: osef_editor (binary "osef-editor"), osef_editor_tests (+ ctest).
cmake_minimum_required(VERSION 3.16...4.4)
project(osef_editor VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC)
endif()

option(OSEF_EDITOR_BUILD_TESTS "Build the OSEF Editor unit tests" ON)
option(OSEF_EDITOR_SANITIZE "Build with -fsanitize=address,undefined (GCC/Clang)" OFF)

set(OSEF_EDITOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(OSEF_EDITOR_ROOT "${OSEF_EDITOR_DIR}/.." ABSOLUTE)
set(OSEF_EDITOR_TP "${OSEF_EDITOR_DIR}/third_party")

# ---- Pinned dependencies -------------------------------------------------------

set(OSEF_IMGUI_VERSION "1.93.0-docking")
set(OSEF_IMGUI_URL "https://github.com/ocornut/imgui/archive/refs/tags/v${OSEF_IMGUI_VERSION}.tar.gz")
set(OSEF_IMGUI_SHA256 "746c265540bd3aa109570f29492ef4d8f5b421dd9fd0049c35ca35f21496f1a5")

set(OSEF_GLFW_VERSION "3.5.1")
set(OSEF_GLFW_URL "https://github.com/glfw/glfw/releases/download/${OSEF_GLFW_VERSION}/glfw-${OSEF_GLFW_VERSION}.zip")
set(OSEF_GLFW_SHA256 "ea79bc5feffc254c87291980c2d0bce9acebb68c4983b79f961dcd2cb8a611a0")

set(OSEF_INTER_VERSION "4.1")
set(OSEF_INTER_URL "https://github.com/rsms/inter/releases/download/v${OSEF_INTER_VERSION}/Inter-${OSEF_INTER_VERSION}.zip")
set(OSEF_INTER_SHA256 "9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e")

set(OSEF_JBMONO_VERSION "2.304")
set(OSEF_JBMONO_URL "https://github.com/JetBrains/JetBrainsMono/releases/download/v${OSEF_JBMONO_VERSION}/JetBrainsMono-${OSEF_JBMONO_VERSION}.zip")
set(OSEF_JBMONO_SHA256 "6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf")

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)  # extracted files get the extraction time
endif()

# osef_editor_fetch(<name> <url> <sha256>) -> <name>_SOURCE_DIR
macro(osef_editor_fetch name url sha)
    FetchContent_Declare(${name} URL "${url}" URL_HASH "SHA256=${sha}")
    FetchContent_GetProperties(${name})
    if(NOT ${name}_POPULATED)
        message(STATUS "osef-editor: fetching ${url}")
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.30)
            FetchContent_Populate(${name} URL "${url}" URL_HASH "SHA256=${sha}"
                                  SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/${name}-src"
                                  SUBBUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/${name}-subbuild"
                                  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/${name}-build")
        else()
            FetchContent_Populate(${name})
        endif()
    endif()
endmacro()

# Dear ImGui
if(EXISTS "${OSEF_EDITOR_TP}/imgui/imgui.h")
    set(OSEF_IMGUI_DIR "${OSEF_EDITOR_TP}/imgui")
else()
    osef_editor_fetch(osef_imgui "${OSEF_IMGUI_URL}" "${OSEF_IMGUI_SHA256}")
    set(OSEF_IMGUI_DIR "${osef_imgui_SOURCE_DIR}")
endif()

# GLFW (its own CMake handles Win32 / Cocoa / X11 / Wayland)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
if(EXISTS "${OSEF_EDITOR_TP}/glfw/CMakeLists.txt")
    set(OSEF_GLFW_DIR "${OSEF_EDITOR_TP}/glfw")
else()
    osef_editor_fetch(osef_glfw "${OSEF_GLFW_URL}" "${OSEF_GLFW_SHA256}")
    set(OSEF_GLFW_DIR "${osef_glfw_SOURCE_DIR}")
endif()
# Force a static GLFW regardless of the parent project's BUILD_SHARED_LIBS.
set(OSEF_EDITOR_SAVED_SHARED "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${OSEF_GLFW_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/glfw" EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS "${OSEF_EDITOR_SAVED_SHARED}")

# Fonts: third_party/fonts, or staged from the release zips into the build dir.
if(EXISTS "${OSEF_EDITOR_TP}/fonts/Inter-Regular.ttf")
    set(OSEF_EDITOR_FONTS_DIR "${OSEF_EDITOR_TP}/fonts")
else()
    osef_editor_fetch(osef_inter "${OSEF_INTER_URL}" "${OSEF_INTER_SHA256}")
    osef_editor_fetch(osef_jbmono "${OSEF_JBMONO_URL}" "${OSEF_JBMONO_SHA256}")
    set(OSEF_EDITOR_FONTS_DIR "${CMAKE_CURRENT_BINARY_DIR}/fonts")
    file(MAKE_DIRECTORY "${OSEF_EDITOR_FONTS_DIR}")
    foreach(f Inter-Regular.ttf Inter-SemiBold.ttf)
        file(COPY "${osef_inter_SOURCE_DIR}/extras/ttf/${f}" DESTINATION "${OSEF_EDITOR_FONTS_DIR}")
    endforeach()
    configure_file("${osef_inter_SOURCE_DIR}/LICENSE.txt" "${OSEF_EDITOR_FONTS_DIR}/Inter-OFL.txt" COPYONLY)
    file(COPY "${osef_jbmono_SOURCE_DIR}/fonts/ttf/JetBrainsMono-Regular.ttf" DESTINATION "${OSEF_EDITOR_FONTS_DIR}")
    configure_file("${osef_jbmono_SOURCE_DIR}/OFL.txt" "${OSEF_EDITOR_FONTS_DIR}/JetBrainsMono-OFL.txt" COPYONLY)
endif()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# ---- Shared settings -----------------------------------------------------------

set(OSEF_EDITOR_WARNINGS)
if(MSVC)
    set(OSEF_EDITOR_WARNINGS /W4 /utf-8 /permissive-)
else()
    set(OSEF_EDITOR_WARNINGS -Wall -Wextra -Wpedantic)
endif()

set(OSEF_EDITOR_SAN)
if(OSEF_EDITOR_SANITIZE AND NOT MSVC)
    set(OSEF_EDITOR_SAN -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

function(osef_editor_target_setup target)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_definitions(${target} PRIVATE OSEF_ROOT_DIR="${OSEF_EDITOR_ROOT}")
    if(APPLE)
        target_compile_definitions(${target} PRIVATE GL_SILENCE_DEPRECATION)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    endif()
    if(OSEF_EDITOR_SAN)
        target_compile_options(${target} PRIVATE ${OSEF_EDITOR_SAN})
        target_link_options(${target} PRIVATE ${OSEF_EDITOR_SAN})
    endif()
endfunction()

# ---- Dear ImGui (static library, warnings off) -----------------------------------

add_library(osef_editor_imgui STATIC
    "${OSEF_IMGUI_DIR}/imgui.cpp"
    "${OSEF_IMGUI_DIR}/imgui_draw.cpp"
    "${OSEF_IMGUI_DIR}/imgui_tables.cpp"
    "${OSEF_IMGUI_DIR}/imgui_widgets.cpp"
    "${OSEF_IMGUI_DIR}/imgui_demo.cpp"
    "${OSEF_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp"
    "${OSEF_IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${OSEF_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp")
# SYSTEM: third-party warnings never leak into editor sources.
target_include_directories(osef_editor_imgui SYSTEM PUBLIC
    "${OSEF_IMGUI_DIR}" "${OSEF_IMGUI_DIR}/backends" "${OSEF_IMGUI_DIR}/misc/cpp")
target_compile_definitions(osef_editor_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(osef_editor_imgui PUBLIC glfw OpenGL::GL)
osef_editor_target_setup(osef_editor_imgui)
if(MSVC)
    target_compile_options(osef_editor_imgui PRIVATE /w)
else()
    target_compile_options(osef_editor_imgui PRIVATE -w)
endif()

# ---- Editor --------------------------------------------------------------------

# CONFIGURE_DEPENDS: new files under src/ are picked up on the next build.
file(GLOB OSEF_EDITOR_SOURCES CONFIGURE_DEPENDS
    "${OSEF_EDITOR_DIR}/src/*.cpp" "${OSEF_EDITOR_DIR}/src/*/*.cpp")

add_executable(osef_editor ${OSEF_EDITOR_SOURCES})
if(APPLE)
    target_sources(osef_editor PRIVATE "${OSEF_EDITOR_DIR}/src/app/window_icon_macos.m")
endif()
set_target_properties(osef_editor PROPERTIES OUTPUT_NAME osef-editor)
# The app icon (Explorer / taskbar) from assets/icon.ico (see `make -C editor icons`).
if(WIN32)
    target_sources(osef_editor PRIVATE "${OSEF_EDITOR_DIR}/assets/icon.rc")
endif()
osef_editor_target_setup(osef_editor)
target_include_directories(osef_editor PRIVATE "${OSEF_EDITOR_DIR}/src")
target_compile_definitions(osef_editor PRIVATE OSEF_EDITOR_FONTS_DIR="${OSEF_EDITOR_FONTS_DIR}")
target_compile_options(osef_editor PRIVATE ${OSEF_EDITOR_WARNINGS})
target_link_libraries(osef_editor PRIVATE osef_editor_imgui glfw OpenGL::GL Threads::Threads)
if(APPLE)
    target_link_libraries(osef_editor PRIVATE
        "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation"
        "-framework CoreVideo" "-framework QuartzCore")
endif()
# Fonts next to the executable, so a copied build dir keeps working.
add_custom_command(TARGET osef_editor POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:osef_editor>/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${OSEF_EDITOR_FONTS_DIR}/Inter-Regular.ttf" "${OSEF_EDITOR_FONTS_DIR}/Inter-SemiBold.ttf"
            "${OSEF_EDITOR_FONTS_DIR}/JetBrainsMono-Regular.ttf" "${OSEF_EDITOR_FONTS_DIR}/Inter-OFL.txt"
            "${OSEF_EDITOR_FONTS_DIR}/JetBrainsMono-OFL.txt" "$<TARGET_FILE_DIR:osef_editor>/fonts"
    VERBATIM)

# ---- Tests (headless: model/ text/ services/ util/, no ImGui/GLFW/OpenGL) ---------

if(OSEF_EDITOR_BUILD_TESTS)
    file(GLOB OSEF_EDITOR_TEST_SOURCES CONFIGURE_DEPENDS
        "${OSEF_EDITOR_DIR}/tests/*.cpp"
        "${OSEF_EDITOR_DIR}/src/model/*.cpp" "${OSEF_EDITOR_DIR}/src/text/*.cpp"
        "${OSEF_EDITOR_DIR}/src/services/*.cpp" "${OSEF_EDITOR_DIR}/src/util/*.cpp")
    add_executable(osef_editor_tests ${OSEF_EDITOR_TEST_SOURCES})
    osef_editor_target_setup(osef_editor_tests)
    target_include_directories(osef_editor_tests PRIVATE "${OSEF_EDITOR_DIR}/src" "${OSEF_EDITOR_DIR}/tests")
    target_compile_options(osef_editor_tests PRIVATE ${OSEF_EDITOR_WARNINGS})
    target_link_libraries(osef_editor_tests PRIVATE Threads::Threads)

    enable_testing()
    add_test(NAME osef_editor_tests COMMAND osef_editor_tests)
endif()
