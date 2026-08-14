#pragma once

namespace cc
{

// Property kind hints for the workbench's property editor. The host picks the
// widget that matches the kind; plugins declare one per property in the
// factory's property_schema(), and the same enum is reused by a value type's
// inline editor (control kind for editing a pin value in place).
enum class property_kind
{
    text,      // single-line InputText
    multiline, // multi-line InputTextMultiline
    path,      // InputText + "..." Browse button (opens ImFileDialog)
    integer,   // InputInt
    boolean,   // Checkbox
};

} // namespace cc
