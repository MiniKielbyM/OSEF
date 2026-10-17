#pragma once
// Inspector properties: typed, UI-agnostic values that belong to a Component.
// The Inspector renders them; Project applies edits back to source code.

#include <cstdint>
#include <string>
#include <vector>

namespace osef::editor {

enum class PropertyType {
    Bool,        // checkbox
    Int,         // numeric field with min/max/step and an optional unit
    Enum,        // combo box, or radio buttons when `radio` is set
    String,      // single-line text field
    Info,        // read-only value text (e.g. "IRQ 1")
    Resolution,  // width x height (i = width, i2 = height), 0 x 0 = automatic
};

struct EnumOption {
    std::string label;    // shown in the UI
    int64_t value = 0;
    std::string tooltip;
};

struct Property {
    std::string id;       // stable key within its component, e.g. "memory_mb"
    std::string label;    // "Memory"
    std::string tooltip;
    std::string section;  // Inspector section title, e.g. "Hardware Declaration"
    PropertyType type = PropertyType::Info;
    bool read_only = false;
    bool radio = false;   // Enum only
    // True when the value is bound to the OSEF_HARDWARE declaration in
    // main.cpp: editing it rewrites the code (the Inspector shows a link mark).
    bool bound = false;

    // Current value. The field used depends on `type`.
    bool b = false;
    int64_t i = 0;        // Int value, Enum selected value, Resolution width
    int64_t i2 = 0;       // Resolution height
    std::string s;        // String / Info text

    // Int constraints and display.
    int64_t min = 0;
    int64_t max = 0;
    int64_t step = 1;
    std::string unit;     // "MiB", "Hz", "baud", "s"

    std::vector<EnumOption> options; // Enum
};

} // namespace osef::editor
