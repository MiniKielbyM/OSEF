# OSEF Editor (Dear ImGui docking + GLFW + OpenGL 3.2 core).
#
#   make                  build build/editor/osef-editor (fetches third_party/ first if missing)
#   make run ARGS="..."   build and run it, e.g. ARGS="--screenshot /tmp/x.png"
#   make test             build + run the headless unit tests (no ImGui/GLFW/OpenGL)
#   make test SANITIZE=1  same, with -fsanitize=address,undefined (separate objects/binaries)
#   make deps             run tools/bootstrap/editor-deps.sh if third_party/ is incomplete
#   make clean            remove build/editor
#
# Editor sources are found by wildcard (src/*.cpp src/*/*.cpp), so new files
# are picked up automatically. V=1 shows full command lines.

EDITOR_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT       := $(patsubst %/,%,$(dir $(EDITOR_DIR)))
SRC_DIR    := $(EDITOR_DIR)/src
TESTS_DIR  := $(EDITOR_DIR)/tests
TP         := $(EDITOR_DIR)/third_party
IMGUI_DIR  := $(TP)/imgui
GLFW_DIR   := $(TP)/glfw
FONTS_DIR  := $(TP)/fonts
BOOTSTRAP  := $(ROOT)/tools/bootstrap/editor-deps.sh
BUILD      := $(ROOT)/build/editor

SANITIZE ?= 0
ifeq ($(SANITIZE),1)
  OBJ        := $(BUILD)/obj-asan
  BIN        := $(BUILD)/osef-editor-asan
  TEST_BIN   := $(BUILD)/osef_editor_tests-asan
  SAN_FLAGS  := -fsanitize=address,undefined -fno-omit-frame-pointer
else
  OBJ        := $(BUILD)/obj
  BIN        := $(BUILD)/osef-editor
  TEST_BIN   := $(BUILD)/osef_editor_tests
  SAN_FLAGS  :=
endif

UNAME_S := $(shell uname -s)

ifeq ($(origin CC),default)
  CC := cc
endif
ifeq ($(origin CXX),default)
  CXX := c++
endif

ifeq ($(V),1)
  Q :=
else
  Q := @
endif

# ---- Flags ---------------------------------------------------------------------

OPT        := -O2 -g
COMMON     := $(OPT) $(SAN_FLAGS) -MMD -MP
IMGUI_DEFS := -DIMGUI_DISABLE_OBSOLETE_FUNCTIONS
WARN       := -Wall -Wextra -Wpedantic

# Third-party headers are -isystem so their warnings never leak into editor builds.
TP_INCLUDES := -isystem $(IMGUI_DIR) -isystem $(IMGUI_DIR)/backends -isystem $(IMGUI_DIR)/misc/cpp \
               -isystem $(GLFW_DIR)/include

EDITOR_CXXFLAGS := -std=c++20 $(COMMON) $(WARN) -I$(SRC_DIR) $(TP_INCLUDES) $(IMGUI_DEFS) \
                   -DOSEF_EDITOR_FONTS_DIR=\"$(FONTS_DIR)\" -DOSEF_ROOT_DIR=\"$(ROOT)\"
IMGUI_CXXFLAGS  := -std=c++20 $(COMMON) -w -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends \
                   -I$(GLFW_DIR)/include $(IMGUI_DEFS)
GLFW_CFLAGS     := $(COMMON) -w -I$(GLFW_DIR)/include -I$(GLFW_DIR)/src
# Tests: model/ text/ services/ util/ only; no ImGui include path on purpose.
TEST_CXXFLAGS   := -std=c++20 $(COMMON) $(WARN) -I$(SRC_DIR) -I$(TESTS_DIR) \
                   -DOSEF_ROOT_DIR=\"$(ROOT)\"

LDFLAGS += $(SAN_FLAGS)

# ---- Sources -------------------------------------------------------------------

EDITOR_SRCS := $(sort $(wildcard $(SRC_DIR)/*.cpp $(SRC_DIR)/*/*.cpp))
EDITOR_NATIVE_OBJS :=
ifeq ($(UNAME_S),Darwin)
  EDITOR_NATIVE_OBJS := $(OBJ)/editor/app/window_icon_macos.m.o
endif

IMGUI_SRCS := $(addprefix $(IMGUI_DIR)/, imgui.cpp imgui_draw.cpp imgui_tables.cpp \
                imgui_widgets.cpp imgui_demo.cpp misc/cpp/imgui_stdlib.cpp \
                backends/imgui_impl_glfw.cpp backends/imgui_impl_opengl3.cpp)

GLFW_COMMON := context.c init.c input.c monitor.c platform.c vulkan.c window.c \
               egl_context.c osmesa_context.c null_init.c null_monitor.c null_window.c \
               null_joystick.c

ifeq ($(UNAME_S),Darwin)
  GLFW_PLATFORM := macos_time.c posix_module.c posix_thread.c \
                   cocoa_init.m cocoa_joystick.m cocoa_monitor.m cocoa_window.m nsgl_context.m
  GLFW_CFLAGS   += -D_GLFW_COCOA
  PLATFORM_DEFS := -DGL_SILENCE_DEPRECATION
  PLATFORM_LIBS := -framework Cocoa -framework IOKit -framework CoreFoundation \
                   -framework CoreVideo -framework OpenGL -framework QuartzCore
else
  GLFW_PLATFORM := posix_time.c posix_module.c posix_thread.c posix_poll.c \
                   x11_init.c x11_monitor.c x11_window.c xkb_unicode.c glx_context.c \
                   linux_joystick.c
  GLFW_CFLAGS   += -D_GLFW_X11 -D_DEFAULT_SOURCE
  PLATFORM_DEFS :=
  PLATFORM_LIBS := -lX11 -lGL -lpthread -ldl -lm
endif

EDITOR_CXXFLAGS += $(PLATFORM_DEFS)
IMGUI_CXXFLAGS  += $(PLATFORM_DEFS)
GLFW_CFLAGS     += $(PLATFORM_DEFS)

GLFW_SRCS := $(addprefix $(GLFW_DIR)/src/, $(GLFW_COMMON) $(GLFW_PLATFORM))

TEST_SRCS := $(sort $(wildcard $(TESTS_DIR)/*.cpp)) \
             $(sort $(wildcard $(SRC_DIR)/model/*.cpp $(SRC_DIR)/text/*.cpp \
                               $(SRC_DIR)/services/*.cpp $(SRC_DIR)/util/*.cpp))

# Object paths mirror the source tree under $(OBJ)/{editor,imgui,glfw,tests/tests,tests/src}.
EDITOR_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ)/editor/%.o,$(EDITOR_SRCS))
IMGUI_OBJS  := $(patsubst $(IMGUI_DIR)/%.cpp,$(OBJ)/imgui/%.o,$(IMGUI_SRCS))
GLFW_OBJS   := $(patsubst $(GLFW_DIR)/src/%,$(OBJ)/glfw/%.o,$(GLFW_SRCS))
TEST_OBJS   := $(patsubst $(TESTS_DIR)/%.cpp,$(OBJ)/tests/tests/%.o,$(filter $(TESTS_DIR)/%,$(TEST_SRCS))) \
               $(patsubst $(SRC_DIR)/%.cpp,$(OBJ)/tests/src/%.o,$(filter $(SRC_DIR)/%,$(TEST_SRCS)))

ALL_OBJS := $(EDITOR_OBJS) $(EDITOR_NATIVE_OBJS) $(IMGUI_OBJS) $(GLFW_OBJS) $(TEST_OBJS)

# ---- Targets -------------------------------------------------------------------

.PHONY: all run test deps clean app install uninstall icons
.DEFAULT_GOAL := all

all: deps $(BIN)

# Re-runs the (idempotent) bootstrap only when a dependency is missing.
deps:
	@if [ ! -f "$(IMGUI_DIR)/.osef-version" ] || [ ! -f "$(GLFW_DIR)/.osef-version" ] || \
	    [ ! -f "$(FONTS_DIR)/.osef-version" ]; then \
	    echo "  DEPS  $(BOOTSTRAP)"; bash "$(BOOTSTRAP)"; fi

$(BIN): $(EDITOR_OBJS) $(EDITOR_NATIVE_OBJS) $(IMGUI_OBJS) $(GLFW_OBJS)
	@mkdir -p $(dir $@)
	@echo "  LINK  $@"
	$(Q)$(CXX) $^ $(LDFLAGS) $(PLATFORM_LIBS) -o $@

run: all
	$(BIN) $(ARGS)

test: $(TEST_BIN)
	$(TEST_BIN) $(TEST_ARGS)

$(TEST_BIN): $(TEST_OBJS)
	@mkdir -p $(dir $@)
	@echo "  LINK  $@"
	$(Q)$(CXX) $^ $(LDFLAGS) -pthread -o $@

clean:
	rm -rf $(BUILD)

# ---- macOS app bundle -----------------------------------------------------------
#   make -C editor app        build/editor/OSEF Editor.app (double-click it, or drag it to Applications)
#   make -C editor install    copy it to /Applications (or ~/Applications if that isn't writable)
#   make -C editor uninstall  remove the installed copy
# The bundle keeps using this OSEF folder (compiled in) for the engine and the
# example projects, so build it from where the OSEF checkout will stay.
APP_VERSION ?= 0.1.0
APP         := $(BUILD)/OSEF Editor.app
APP_STAMP   := $(BUILD)/.app-stamp
ICONSET     := $(BUILD)/OSEF.iconset
ICNS        := $(BUILD)/OSEF.icns
INSTALL_DIR ?= $(shell if [ -w /Applications ]; then echo /Applications; else echo "$$HOME/Applications"; fi)

ifeq ($(UNAME_S),Darwin)
# App icon: editor/assets/icon.png is the single source for every platform.
# `make -C editor icons` regenerates the derived files (committed, so other
# systems need no image tools): assets/icon.ico (Windows), assets/OSEF.icns
# (macOS) and src/app/icon_data.inc (window/Dock icon at runtime).
ICON_PNG := $(EDITOR_DIR)/assets/icon.png
$(ICNS): $(EDITOR_DIR)/assets/OSEF.icns
	@echo "  ICON  $@"
	$(Q)cp "$<" "$@"

icons: all
	@echo "  ICONS from $(ICON_PNG)"
	$(Q)rm -rf "$(EDITOR_DIR)/assets/OSEF.iconset"
	$(Q)$(BIN) --make-icons "$(ICON_PNG)" "$(EDITOR_DIR)/assets" "$(SRC_DIR)/app/icon_data.inc"
	$(Q)iconutil -c icns "$(EDITOR_DIR)/assets/OSEF.iconset" -o "$(EDITOR_DIR)/assets/OSEF.icns"
	$(Q)rm -rf "$(EDITOR_DIR)/assets/OSEF.iconset"
	$(Q)$(MAKE) --no-print-directory -f "$(EDITOR_DIR)/Makefile" all
	@echo "Updated assets/icon.ico, assets/OSEF.icns and src/app/icon_data.inc; rebuild the app with: make -C editor app"

app: all $(ICNS) $(EDITOR_DIR)/macos/Info.plist.in
	@echo "  APP   $(APP)"
	$(Q)rm -rf "$(APP)"
	$(Q)mkdir -p "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources/fonts"
	$(Q)cp "$(BIN)" "$(APP)/Contents/MacOS/osef-editor"
	$(Q)cp "$(FONTS_DIR)"/*.ttf "$(FONTS_DIR)"/*.txt "$(APP)/Contents/Resources/fonts/"
	$(Q)cp "$(ICNS)" "$(APP)/Contents/Resources/OSEF.icns"
	$(Q)sed 's/@VERSION@/$(APP_VERSION)/g' "$(EDITOR_DIR)/macos/Info.plist.in" > "$(APP)/Contents/Info.plist"
	$(Q)printf 'APPL????' > "$(APP)/Contents/PkgInfo"
	$(Q)codesign --force --deep --sign - "$(APP)" 2>/dev/null || echo "  (codesign unavailable: the app is unsigned)"
	@echo "Built $(APP)"
	@echo "Install it with: make -C editor install   (or drag it into Applications in Finder)"

install: app
	@mkdir -p "$(INSTALL_DIR)"
	@rm -rf "$(INSTALL_DIR)/OSEF Editor.app"
	@ditto "$(APP)" "$(INSTALL_DIR)/OSEF Editor.app"
	@echo "Installed $(INSTALL_DIR)/OSEF Editor.app (open it from Launchpad, Spotlight or Finder)"

uninstall:
	rm -rf "$(INSTALL_DIR)/OSEF Editor.app"
else
app install uninstall:
	@echo "The app bundle is macOS only; on this system run $(BIN) directly."

icons: all
	@echo "  ICONS from $(EDITOR_DIR)/assets/icon.png (the macOS .icns needs iconutil: run this on a Mac)"
	$(Q)rm -rf "$(EDITOR_DIR)/assets/OSEF.iconset"
	$(Q)$(BIN) --make-icons "$(EDITOR_DIR)/assets/icon.png" "$(EDITOR_DIR)/assets" "$(SRC_DIR)/app/icon_data.inc"
	$(Q)rm -rf "$(EDITOR_DIR)/assets/OSEF.iconset"
	$(Q)$(MAKE) --no-print-directory -f "$(EDITOR_DIR)/Makefile" all
endif

# Objects wait for the bootstrap (order-only: it never forces a rebuild).
$(ALL_OBJS): | deps

$(OBJ)/editor/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   src/$*.cpp"
	$(Q)$(CXX) $(EDITOR_CXXFLAGS) $(CXXFLAGS) -c $< -o $@

$(OBJ)/editor/%.m.o: $(SRC_DIR)/%.m
	@mkdir -p $(dir $@)
	@echo "  OBJC  src/$*.m"
	$(Q)$(CC) $(COMMON) $(PLATFORM_DEFS) -c $< -o $@

$(OBJ)/imgui/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   imgui/$*.cpp"
	$(Q)$(CXX) $(IMGUI_CXXFLAGS) $(CXXFLAGS) -c $< -o $@

$(OBJ)/glfw/%.c.o: $(GLFW_DIR)/src/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    glfw/$*.c"
	$(Q)$(CC) -std=c99 $(GLFW_CFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ)/glfw/%.m.o: $(GLFW_DIR)/src/%.m
	@mkdir -p $(dir $@)
	@echo "  OBJC  glfw/$*.m"
	$(Q)$(CC) -x objective-c $(GLFW_CFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ)/tests/tests/%.o: $(TESTS_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   tests/$*.cpp"
	$(Q)$(CXX) $(TEST_CXXFLAGS) $(CXXFLAGS) -c $< -o $@

$(OBJ)/tests/src/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   src/$*.cpp (tests)"
	$(Q)$(CXX) $(TEST_CXXFLAGS) $(CXXFLAGS) -c $< -o $@

-include $(ALL_OBJS:.o=.d)
