#pragma once
// The catalog of known OS components (see ARCHITECTURE.md, "Component catalog"):
// boot firmware, hardware devices, kernel subsystems and userland, each with
// its Inspector properties, documentation and engine source files.

#include "model/component.h"

#include <memory>
#include <string>

namespace osef::editor {

// Builds the full component tree. The root (id "os", kind Os) is named
// `os_name`. user.programs and user.modules start empty: Project fills them
// from main.cpp. Bound properties start at the defaults a new declaration
// gets (x86_64, UEFI, all devices, 128 MiB); Project::sync_from_code()
// overwrites them from the code.
//
// Property ids bound to OSEF_HARDWARE (Property::bound == true):
//   os.name                   String      .name("...")
//   boot.firmware.firmware    Enum radio  1 = BIOS, 2 = UEFI (0 = not declared)
//   boot.firmware.arch        Enum        1 = x86_64 (0 = not declared)
//   boot.firmware.boot_timeout Int 0-30 s .boot_timeout(s)
//   hw.memory.memory_mb       Int 32-65536 MiB, step 32
//   hw.serial.baud            Enum        value = baud rate
//   hw.display.resolution     Resolution  i x i2, 0 x 0 = firmware default
// Toggles bound to .devices(...): hw.serial, hw.display, hw.keyboard.
std::unique_ptr<Component> build_component_catalog(const std::string& os_name);

} // namespace osef::editor
