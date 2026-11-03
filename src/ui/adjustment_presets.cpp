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
    {"Lighter", {0.25f, 0.0f}},
    {"Darker", {-0.25f, 0.0f}},
    {"Lower Contrast", {0.0f, -0.25f}},
    {"Increase Contrast", {0.0f, 0.25f}},
    {"Saturate", {0.0f, 1.0f}},
    {"Desaturate", {-1.0f, -1.0f}},
    {"Muted Colours 01", {1.0f, -1.0f}},
    {"Muted Colours 02", {-1.0f, 1.0f}},
    {"Subtle Punch", {0.10f, 0.10f}},
    {"Dodge", {0.15f, 0.30f}},
    {"Burn", {-0.15f, 0.15f}},
};

// Levels: p[0] inBlack, p[1] inWhite, p[2] gamma, p[3] outBlack,
// p[4] outWhite.
const AdjustmentPreset kLevels[] = {
    {"(Default)", {0.0f, 1.0f, 1.0f, 0.0f, 1.0f}},
    {"Darken", {0.2018f, 1.0f, 1.0f, 0.0f, 1.0f}},
    {"Lighten", {0.0f, 0.799f, 1.0f, 0.0f, 1.0f}},
    {"Raise Black Level", {0.0f, 1.0f, 1.0f, 0.10f, 1.0f}},
    {"Lower White Level", {0.0f, 1.0f, 1.0f, 0.0f, 0.90f}},
    {"Increase Gamma", {0.0f, 1.0f, 2.0f, 0.0f, 1.0f}},
    {"Decrease Gamma", {0.0f, 1.0f, 0.5f, 0.0f, 1.0f}},
    {"Highlight Burn", {0.40f, 1.0f, 1.0f, 0.0f, 1.0f}},
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
    // Real Affinity factory values (adjustments.propcol, AdjP/RecP fields).
    {"Coarse Sepia", {30.0f, 0.42f, 0.0f}},
    {"Fire", {40.0f, 1.0f, 0.0f}},
    {"Night", {210.0f, 0.25f, 0.0f}},
    {"Purple Hour", {285.0f, 0.20f, 0.0f}},
    {"Blue Hour", {250.0f, 0.55f, 0.0f}},
    {"Wine", {0.0f, 0.50161f, -0.59576f}},
};

// Black & White: p[0..5] R/Y/G/C/B/M weights (50 neutral).
const AdjustmentPreset kBlackWhite[] = {
    {"(Default)", {40.0f, 60.0f, 40.0f, 60.0f, 20.0f, 80.0f}},
    // Affinity stores weights 0..3; Pittore's engine uses -200..300 with 50 neutral.
    {"Cold", {50.0f, 100.0f, 100.0f, 100.0f, 50.0f, 0.0f}},
    {"Warm", {100.0f, 0.0f, -100.0f, -50.0f, 50.0f, 150.0f}},
    {"Crunch", {0.0f, 50.0f, 50.0f, 0.0f, 50.0f, 50.0f}},
    {"Darken Blues/Cyans", {50.0f, 50.0f, -50.0f, -50.0f, 50.0f, 50.0f}},
    {"Red/Blue Contrast", {100.0f, 50.0f, 0.0f, 0.0f, 50.0f, 50.0f}},
};

// Exposure: p[0] is stops.
const AdjustmentPreset kExposure[] = {
    {"(Default)", {0.0f}},
    {"-1 Stop", {-1.0f}}, {"+1 Stop", {1.0f}},
    {"-2 Stops", {-2.0f}}, {"+2 Stops", {2.0f}},
    {"-3 Stops", {-3.0f}}, {"+3 Stops", {3.0f}},
    {"-4 Stops", {-4.0f}}, {"+4 Stops", {4.0f}},
    {"-5 Stops", {-5.0f}}, {"+5 Stops", {5.0f}},
    {"-6 Stops", {-6.0f}}, {"+6 Stops", {6.0f}},
};

// Threshold: p[0] threshold 0..1.
const AdjustmentPreset kThreshold[] = {
    {"(Default)", {0.5f}},
    {"Favour Light", {0.25f}},
    {"Favour Dark", {0.75f}},
    {"S-Curve", {0.5f}},
    {"S-Curve Stronger", {0.5f}},
    {"Inverse S-Curve", {0.5f}},
    {"Inverse S-Curve Stronger", {0.5f}},
    {"Weighted Contrast", {0.5f}},
    {"Matte Black", {0.6f}},
    {"Matte Contrast", {0.45f}},
    {"Reduce Highlights", {0.55f}},
    {"Punchy", {0.35f}},
    {"Lightness Contrast Curve", {0.5f}},
};

const AdjustmentPreset kToneCompression[] = {
    {"(Default)", {0.2f, 0.5f}},
    {"Balanced", {0.25f, 0.5f}},
    {"Vibrant Sunset", {0.30f, 0.55f}},
    {"Colourful", {0.35f, 0.5f}},
    {"Punchy 3D Render", {0.40f, 0.45f}},
    {"Maximum Range", {0.55f, 0.5f}},
    {"Colour & Contrast", {0.30f, 0.4f}},
    {"Dark & Moody", {0.45f, 0.35f}},
    {"Bright & Muted", {0.25f, 0.65f}},
};

const AdjustmentPreset kToneStretch[] = {
    {"(Default)", {0.2f, 0.5f}},
    {"Soft", {0.15f, 0.5f}},
    {"Strong", {0.45f, 0.5f}},
    {"Inspection", {0.70f, 0.5f}},
};

const AdjustmentPreset kSelectiveColour[] = {
    {"(Default)", {0,0,0,0,0,0, 0,0,0,0,0,0}},
    {"Golden Reds", {0.35f,0,0,0,0,0, 0.10f,0,0,0,0,0}},
    {"Golden Yellows", {0,0.40f,0,0,0,0, 0,0.12f,0,0,0,0}},
    {"Punchy Cyans", {0,0,0,0.50f,0,0, 0,0,0,0.10f,0,0}},
    {"Punchy Blues", {0,0,0,0,0.50f,0, 0,0,0,0,0.10f,0}},
    {"Autumnal", {0.20f,0.25f,-0.30f,0,0,0, 0.08f,0.10f,-0.10f,0,0,0}},
    {"Blue Cast", {0,0,0,0.15f,0.25f,0, 0,0,0,0,0.08f,0}},
    {"Redden Foliage", {0.20f,0,-0.20f,0,0,0, 0.05f,0,-0.10f,0,0,0}},
    {"Neon Dreams", {0.35f,0.35f,0.35f,0.35f,0.35f,0.35f, 0.10f,0.10f,0.10f,0.10f,0.10f,0.10f}},
};

const AdjustmentPreset kSoftProof[] = {
    {"(Default)", {1,0,0, 0,1,0, 0,0,1, 0}},
    {"Coated FOGRA27", {0.95f,0.03f,0.0f, 0.02f,0.94f,0.02f, 0.0f,0.03f,0.90f, 1}},
    {"Euroscale Coated v2", {0.92f,0.04f,0.02f, 0.03f,0.90f,0.05f, 0.02f,0.06f,0.88f, 1}},
    {"Greyscale D50", {0.30f,0.59f,0.11f, 0.30f,0.59f,0.11f, 0.30f,0.59f,0.11f, 1}},
};

const AdjustmentPreset kLut[] = {
    {"(Default)", {1,0,0, 0,1,0, 0,0,1, 1}},
    {"Deuteranope (Simulate)", {0.625f,0.375f,0, 0.70f,0.30f,0, 0,0.30f,0.70f, 1}},
    {"Protanope (Simulate)", {0.567f,0.433f,0, 0.558f,0.442f,0, 0,0.242f,0.758f, 1}},
    {"Tritanope (Simulate)", {0.95f,0.05f,0, 0,0.433f,0.567f, 0,0.475f,0.525f, 1}},
    {"Deuteranope (Daltonize)", {1.0f,0.25f,0, 0.20f,1.10f,0, 0,0.20f,1.05f, 1}},
    {"Protanope (Daltonize)", {1.10f,0.15f,0, 0.25f,0.95f,0, 0,0.15f,1.10f, 1}},
    {"Tritanope (Daltonize)", {1.05f,0,0.10f, 0,1.0f,0.15f, 0.10f,0.15f,1.0f, 1}},
};

// Gradient Map: four stop positions, then four RGB stop colours.
const AdjustmentPreset kGradientMap[] = {
    {"(Default)", {0, 0.333f, 0.666f, 1, 0,0,0, 0.333f,0.333f,0.333f, 0.666f,0.666f,0.666f, 1,1,1}},
    {"Ultra Violet", {0, 0.333f, 0.666f, 1, 0.05f,0,0.10f, 0.30f,0,0.50f, 0.80f,0.20f,0.90f, 1.0f,0.80f,1.0f}},
    {"Nuclear", {0, 0.333f, 0.666f, 1, 0.01f,0.02f,0.0f, 0.05f,0.35f,0.05f, 0.55f,0.85f,0.05f, 1.0f,0.95f,0.30f}},
    {"Tritone Posterisation", {0, 0.333f, 0.666f, 1, 0.03f,0.05f,0.15f, 0.55f,0.20f,0.10f, 0.90f,0.60f,0.25f, 1.0f,0.92f,0.75f}},
    {"Duotone Posterisation", {0, 0.333f, 0.666f, 1, 0.02f,0.02f,0.02f, 0.20f,0.10f,0.05f, 0.65f,0.35f,0.15f, 0.95f,0.75f,0.45f}},
    {"Pastel", {0, 0.333f, 0.666f, 1, 0.60f,0.75f,0.95f, 0.85f,0.75f,0.90f, 0.95f,0.80f,0.75f, 1.0f,0.95f,0.85f}},
    {"OOC Infrared", {0, 0.333f, 0.666f, 1, 0.10f,0.0f,0.05f, 0.55f,0.0f,0.25f, 0.95f,0.20f,0.45f, 1.0f,0.85f,0.85f}},
    {"Dreamy Haze", {0, 0.333f, 0.666f, 1, 0.05f,0.10f,0.25f, 0.35f,0.30f,0.55f, 0.75f,0.60f,0.65f, 1.0f,0.88f,0.75f}},
    {"Smooth", {0, 0.333f, 0.666f, 1, 0.0f,0.0f,0.0f, 0.25f,0.25f,0.25f, 0.70f,0.70f,0.70f, 1.0f,1.0f,1.0f}},
    {"Murky", {0, 0.333f, 0.666f, 1, 0.08f,0.07f,0.05f, 0.25f,0.24f,0.16f, 0.50f,0.50f,0.42f, 0.75f,0.75f,0.70f}},
    {"Bleached", {0, 0.333f, 0.666f, 1, 0.05f,0.04f,0.03f, 0.35f,0.30f,0.22f, 0.75f,0.70f,0.55f, 1.0f,0.98f,0.92f}},
};

// Channel Mixer: 3x3 output matrix, three constants, monochrome flag.
const AdjustmentPreset kChannelMixer[] = {
    {"(Default)", {1,0,0, 0,1,0, 0,0,1, 0,0,0, 0}},
    {"Infrared RB Swap", {0,0,1, 0,1,0, 1,0,0, 0,0,0, 0}},
    {"Infrared RGB Swap", {0,1,0, 1,0,0, 0,0,1, 0,0,0, 0}},
    {"Infrared Emulation", {0.4f,0.4f,0.2f, 0.2f,0.5f,0.3f, 0.1f,0.2f,0.7f, 0,0,0, 0}},
    {"LAB Gold Punch", {1.1f,0.1f,0.0f, 0.0f,1.0f,0.1f, 0.0f,0.1f,0.9f, 0,0,0, 0}},
    {"Two Strip", {1,0,0, 0,1,0, 0,1,0, 0,0,0, 0}},
    {"Three Strip", {1,0,0, 0,1,0, 0,0,1, 0,0,0, 0}},
    {"Weighted Greyscale", {0.3f,0.59f,0.11f, 0.3f,0.59f,0.11f, 0.3f,0.59f,0.11f, 0,0,0, 0}},
    {"A to B", {0,0,1, 0,1,0, 1,0,0, 0,0,0, 0}},
    {"B to A", {0,1,0, 1,0,0, 0,0,1, 0,0,0, 0}},
    {"A+B 50:50", {0.5f,0,0.5f, 0.5f,0.5f,0, 0,0.5f,0.5f, 0,0,0, 0}},
    {"A Lightness", {0.6f,0.3f,0.1f, 0.3f,0.6f,0.1f, 0.1f,0.3f,0.6f, 0,0,0, 0}},
    {"B Lightness", {0.2f,0.6f,0.2f, 0.2f,0.2f,0.6f, 0.6f,0.2f,0.2f, 0,0,0, 0}},
    {"Hazy Saturation", {0.8f,0.15f,0.05f, 0.05f,0.8f,0.15f, 0.15f,0.05f,0.8f, 0.05f,0.05f,0.05f, 0}},
    {"Earthy Tones", {1.05f,0.1f,-0.05f, 0.0f,0.95f,0.05f, -0.05f,0.1f,0.85f, 0.02f,0.01f,0.0f, 0}},
    {"Urban Crush", {0.9f,0.1f,0.1f, 0.1f,0.85f,0.1f, 0.1f,0.1f,0.9f, -0.02f,-0.02f,-0.02f, 0}},
    {"Night Vision", {0.0f,0.4f,0.0f, 0.0f,0.8f,0.0f, 0.0f,0.4f,0.0f, 0,0,0, 0}},
};

// Photo Filter / Lens Filter: p[0..2] filter colour, p[3] density, p[4] preserve.
const AdjustmentPreset kPhotoFilter[] = {
    {"(Default)", {0,0,0, 0,0}},
    {"Cool", {0.35f,0.55f,1.0f, 0.25f, 0}},
    {"Warm", {1.0f,0.65f,0.30f, 0.25f, 0}},
    {"Moody", {0.35f,0.45f,0.75f, 0.35f, 0}},
    {"Menacing", {0.75f,0.25f,0.20f, 0.35f, 0}},
    {"Red/Blue Split", {0.9f,0.2f,0.5f, 0.25f, 0}},
    {"Blue/Red Split", {0.2f,0.4f,0.9f, 0.25f, 0}},
    {"Golden Brown", {0.85f,0.55f,0.20f, 0.30f, 0}},
    {"Retro Tint", {0.90f,0.70f,0.40f, 0.30f, 0}},
    {"Soft Cool Tint", {0.55f,0.75f,0.95f, 0.15f, 0}},
    {"Soft Warm Tint", {0.95f,0.80f,0.55f, 0.15f, 0}},
    {"Acid Crunch", {0.70f,0.95f,0.20f, 0.30f, 0}},
};

// Colour Balance: shadows/midtones/highlights × CR/MG/YB.
const AdjustmentPreset kColorBalance[] = {
    {"(Default)", {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {"Subtle Blue Cast", {0.0f, 0.0f, 20.0f, 0.0f, 0.0f, 20.0f, 0.0f, 0.0f, 0.0f}},
    {"Subtle Red Cast", {5.0f, 0.0f, 0.0f, 40.19f, 0.0f, 0.0f, 5.0f, 0.0f, 0.0f}},
    {"Tinted Lift", {100.0f, 0.0f, 100.0f, 0.0f, 100.0f, -100.0f, 0.0f, 0.0f, 100.0f}},
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
        case AdjustmentKind::Exposure:
            if (count) *count = int(sizeof(kExposure) / sizeof(kExposure[0]));
            return kExposure;
        case AdjustmentKind::Threshold:
            if (count) *count = int(sizeof(kThreshold) / sizeof(kThreshold[0]));
            return kThreshold;
        case AdjustmentKind::ColorBalance:
            if (count) *count = int(sizeof(kColorBalance) / sizeof(kColorBalance[0]));
            return kColorBalance;
        case AdjustmentKind::ChannelMixer:
            if (count) *count = int(sizeof(kChannelMixer) / sizeof(kChannelMixer[0]));
            return kChannelMixer;
        case AdjustmentKind::PhotoFilter:
            if (count) *count = int(sizeof(kPhotoFilter) / sizeof(kPhotoFilter[0]));
            return kPhotoFilter;
        case AdjustmentKind::GradientMap:
            if (count) *count = int(sizeof(kGradientMap) / sizeof(kGradientMap[0]));
            return kGradientMap;
        case AdjustmentKind::ToneCompression:
            if (count) *count = int(sizeof(kToneCompression) / sizeof(kToneCompression[0]));
            return kToneCompression;
        case AdjustmentKind::ToneStretch:
            if (count) *count = int(sizeof(kToneStretch) / sizeof(kToneStretch[0]));
            return kToneStretch;
        case AdjustmentKind::SelectiveColour:
            if (count) *count = int(sizeof(kSelectiveColour) / sizeof(kSelectiveColour[0]));
            return kSelectiveColour;
        case AdjustmentKind::LUT:
            if (count) *count = int(sizeof(kLut) / sizeof(kLut[0]));
            return kLut;
        case AdjustmentKind::SoftProof:
            if (count) *count = int(sizeof(kSoftProof) / sizeof(kSoftProof[0]));
            return kSoftProof;
        default:
            break;
    }
    if (count) *count = 0;
    return nullptr;
}

}  // namespace pittore::ui
