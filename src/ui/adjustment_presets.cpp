// Preset tables behind adjustment_presets.h. Entries follow the parameter
// semantics documented in engine/compute/adjust.h; index 0 is the reset state.

#include "ui/adjustment_presets.h"

#include "engine/compute/adjust.h"

namespace pittore::ui {
namespace {

using pittore::compute::AdjustmentKind;

// Brightness/Contrast: p[0] brightness [-1,1], p[1] contrast [-1,1].
const AdjustmentPreset kBrightnessContrast[] = {
    {"(Default)", {0.0f, 0.0f}},
    {"Lighter", {0.12f, 0.0f}},
    {"Darker", {-0.12f, 0.0f}},
    {"Lower Contrast", {0.0f, -0.30f}},
    {"Increase Contrast", {0.0f, 0.30f}},
    // Brightness/Contrast has no saturation control; these two are contrast
    // fits so the names still resolve to something usable.
    {"Saturate", {0.0f, 0.16f}},
    {"Desaturate", {0.0f, -0.16f}},
    {"Muted Colours 01", {0.04f, -0.12f}},
    {"Muted Colours 02", {0.08f, -0.20f}},
    {"Subtle Punch", {0.0f, 0.18f}},
};

// Levels: p[0] inBlack, p[1] inWhite, p[2] gamma, p[3] outBlack,
// p[4] outWhite.
const AdjustmentPreset kLevels[] = {
    {"(Default)", {0.0f, 1.0f, 1.0f, 0.0f, 1.0f}},
    {"Darken", {0.0f, 1.0f, 0.80f, 0.0f, 1.0f}},
    {"Lighten", {0.0f, 1.0f, 1.25f, 0.0f, 1.0f}},
    {"Raise Black Level", {0.10f, 1.0f, 1.0f, 0.0f, 1.0f}},
    {"Lower White Level", {0.0f, 0.90f, 1.0f, 0.0f, 1.0f}},
    {"Increase Gamma", {0.0f, 1.0f, 1.30f, 0.0f, 1.0f}},
    {"Decrease Gamma", {0.0f, 1.0f, 0.75f, 0.0f, 1.0f}},
    {"Highlight Burn", {0.0f, 1.0f, 1.0f, 0.0f, 0.88f}},
};

// White Balance: p[0] temperature Kelvin, p[1] tint (- green, + magenta).
const AdjustmentPreset kWhiteBalance[] = {
    {"(Default)", {6500.0f, 0.0f}},
    {"Warm", {8000.0f, 0.0f}},
    {"Cool", {5000.0f, 0.0f}},
    {"Magenta Tint", {6500.0f, 0.25f}},
    {"Green Tint", {6500.0f, -0.25f}},
    {"Cool Magenta", {5200.0f, 0.18f}},
    {"Warm Green", {7800.0f, -0.18f}},
    {"Warm Crush", {8800.0f, 0.12f}},
    {"Cool Crush", {4400.0f, -0.12f}},
};

// Hue/Saturation: p[0] hue degrees, p[1] saturation [-1,1],
// p[2] lightness [-1,1]. The per-range entries are global fits: this kind has
// one hue shift, not Affinity's six hue ranges.
const AdjustmentPreset kHueSaturation[] = {
    {"(Default)", {0.0f, 0.0f, 0.0f}},
    {"Saturate", {0.0f, 0.35f, 0.0f}},
    {"Desaturate", {0.0f, -0.35f, 0.0f}},
    {"Invert Hue", {180.0f, 0.0f, 0.0f}},
    {"Red Saturation", {0.0f, 0.12f, 0.0f}},
    {"Yellow Saturation", {0.0f, 0.13f, 0.0f}},
    {"Green Saturation", {0.0f, 0.14f, 0.0f}},
    {"Cyan Saturation", {0.0f, 0.15f, 0.0f}},
    {"Blue Saturation", {0.0f, 0.16f, 0.0f}},
    {"Magenta Saturation", {0.0f, 0.17f, 0.0f}},
    {"Brighten Reds", {0.0f, 0.04f, 0.06f}},
    {"Brighten Yellows", {0.0f, 0.04f, 0.07f}},
    {"Brighten Greens", {0.0f, 0.04f, 0.08f}},
    {"Brighten Cyans", {0.0f, 0.04f, 0.09f}},
    {"Brighten Blues", {0.0f, 0.04f, 0.10f}},
    {"Brighten Magentas", {0.0f, 0.04f, 0.11f}},
    {"Autumnal", {-12.0f, 0.10f, 0.0f}},
    {"Brighten Sky Detail", {0.0f, 0.06f, 0.05f}},
};

// Recolour: p[0] hue degrees, p[1] saturation [0,1], p[2] lightness [-1,1].
const AdjustmentPreset kRecolour[] = {
    {"(Default)", {0.0f, 1.0f, 0.0f}},
    {"Coarse Sepia", {32.0f, 0.55f, -0.02f}},
    {"Fire", {18.0f, 0.95f, 0.02f}},
    {"Night", {225.0f, 0.55f, -0.12f}},
    {"Purple Hour", {285.0f, 0.55f, 0.0f}},
    {"Blue Hour", {215.0f, 0.65f, -0.05f}},
    {"Wine", {345.0f, 0.70f, -0.08f}},
};

// Black & White: p[0..5] R/Y/G/C/B/M weights (50 neutral).
const AdjustmentPreset kBlackWhite[] = {
    {"(Default)", {40.0f, 60.0f, 40.0f, 60.0f, 20.0f, 80.0f}},
    {"Cold", {30.0f, 50.0f, 45.0f, 60.0f, 70.0f, 50.0f}},
    {"Warm", {70.0f, 65.0f, 45.0f, 40.0f, 30.0f, 50.0f}},
    {"Crunch", {55.0f, 60.0f, 45.0f, 50.0f, 35.0f, 60.0f}},
    {"Darken Blues/Cyans", {40.0f, 60.0f, 40.0f, 25.0f, 10.0f, 60.0f}},
    {"Red/Blue Contrast", {85.0f, 60.0f, 40.0f, 45.0f, 15.0f, 70.0f}},
};

// Posterize: p[0] level count.
const AdjustmentPreset kPosterize[] = {
    {"(Default)", {4.0f}},
    {"Harsh", {2.0f}},
    {"Soft", {8.0f}},
    {"Muted", {5.0f}},
};

// Vibrance: p[0] amount [-1,1].
const AdjustmentPreset kVibrance[] = {
    {"(Default)", {0.0f}},
    {"Full Vibrance", {0.80f}},
    {"No Vibrance", {-1.0f}},
    {"Half Vibrance", {0.40f}},
};

}  // namespace

const AdjustmentPreset* adjustmentPresets(int kind, int* count) {
    const AdjustmentKind k = static_cast<AdjustmentKind>(kind);
    switch (k) {
        case AdjustmentKind::BrightnessContrast:
            if (count) *count = int(sizeof(kBrightnessContrast) /
                                    sizeof(kBrightnessContrast[0]));
            return kBrightnessContrast;
        case AdjustmentKind::Levels:
            if (count) *count = int(sizeof(kLevels) / sizeof(kLevels[0]));
            return kLevels;
        case AdjustmentKind::WhiteBalance:
            if (count) *count = int(sizeof(kWhiteBalance) / sizeof(kWhiteBalance[0]));
            return kWhiteBalance;
        case AdjustmentKind::HueSaturation:
            if (count) *count = int(sizeof(kHueSaturation) /
                                    sizeof(kHueSaturation[0]));
            return kHueSaturation;
        case AdjustmentKind::Recolour:
            if (count) *count = int(sizeof(kRecolour) / sizeof(kRecolour[0]));
            return kRecolour;
        case AdjustmentKind::BlackWhite:
            if (count) *count = int(sizeof(kBlackWhite) / sizeof(kBlackWhite[0]));
            return kBlackWhite;
        case AdjustmentKind::Posterize:
            if (count) *count = int(sizeof(kPosterize) / sizeof(kPosterize[0]));
            return kPosterize;
        case AdjustmentKind::Vibrance:
            if (count) *count = int(sizeof(kVibrance) / sizeof(kVibrance[0]));
            return kVibrance;
        default:
            break;
    }
    if (count) *count = 0;
    return nullptr;
}

}  // namespace pittore::ui
