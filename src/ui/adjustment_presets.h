#pragma once
// Named parameter presets for adjustment layers, mirroring the preset lists
// Affinity shows in its Adjustments panel.
//
// The names are functional labels read off the product UI. The numbers are our
// own fits to the observed control semantics — they are not extracted from
// Affinity, and no Affinity code or constants are used here.
//
// Every table starts with the reset state ("(Default)"), so index 0 always
// matches what AppState::setAdjustmentDefaults writes for that kind.

namespace pittore::ui {

struct AdjustmentPreset {
    const char* name;
    float params[16];
};

// Preset table for one compute::AdjustmentKind value (the int form stored on
// LayerItem). Stores the entry count in *count and returns the table, or
// nullptr when the kind has no presets.
const AdjustmentPreset* adjustmentPresets(int kind, int* count);

}  // namespace pittore::ui
