#pragma once
// Refine Lab — every tunable of the segmentation + refine pipeline, as one
// plain struct with JSON round-trip. Defaults reproduce the shipped Refine
// dialog / Select Subject behaviour exactly, so an untouched LabParams runs
// the same pipeline the app does. Everything here is meant to be dragged in
// realtime; save/load a preset to carry a good setting back into the code.
//
// The knobs are grouped the way the pipeline runs:
//   Segmentation  — the AI matte that seeds the refine (Select Subject /
//                   Remove Background path, single-file model).
//   Refine        — the classical stages, in execution order: speckles,
//                   decay, AI refine, smooth, snap, fill-holes, then the
//                   dialog's feather/shift/ramp user adjustments. AI runs
//                   before the edge solve so the colour-honest snap trims the
//                   guided skirt while keeping strands.
//   AI refine     — guided_refine_alpha (the SAM edge-aware refine + level).
//   Decontam      — decontaminateStraightRgba (background unmixing).
//   View / proxy  — how the lab renders and at what preview resolution.

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QVector>

namespace pittore::lab {

// View / preview modes for the result pane.
enum class ViewMode {
    Original,        // untouched source
    Matte,           // refined alpha, grayscale
    Rubylith,        // red overlay on the source (classic selection view)
    CompositeAlpha,  // cutout over a transparency checkerboard
    CompositeSolid,  // cutout over a flat color (fringe-hunting background)
    BlackWhite,      // hard 50% threshold of the matte
    FringeMap,       // partial-alpha pixels tinted by color contamination
    Count
};
inline const char* viewModeName(ViewMode m) {
    switch (m) {
        case ViewMode::Original: return "Original";
        case ViewMode::Matte: return "Matte (alpha)";
        case ViewMode::Rubylith: return "Rubylith";
        case ViewMode::CompositeAlpha: return "Cutout / transparency";
        case ViewMode::CompositeSolid: return "Cutout / flat color";
        case ViewMode::BlackWhite: return "Black & White";
        case ViewMode::FringeMap: return "Fringe map";
        default: return "?";
    }
}

struct LabParams {
    // --- Segmentation (initial matte) --------------------------------------
    QString modelPath;         // chosen .onnx on disk (empty = no segment yet)
    int inputSize = 1024;      // square network input edge
    double segmentLevelLo = 0.0;   // clamp the raw model alpha below this -> 0
    double segmentLevelHi = 1.0;   // clamp above this -> 1 (band around 0.5)

    // --- Refine: runPipeline() stages, in execution order ------------------
    bool useSpeckles = true;
    int speckleMinPx = 32;     // drop detached islands smaller than this

    bool useDecay = true;
    int decaySupportPx = 32;   // confident support radius for wash decay
    double decayTexTol = 8.0;  // texture energy below which wash is decayed

    bool useSnap = true;
    bool smartRadius = true;   // SnapSmart (narrow+wide) vs plain SnapToEdges
    int snapBandPx = 0;        // 0 = derive from borderPct like the dialog
    int borderPct = 10;        // dialog Border slider (%), used if band==0
    int snapNarrowDiv = 2;     // narrow = band / this (smart radius)
    int snapPasses = 3;

    bool useSmooth = true;
    int smoothSlider = 6;      // dialog Smooth slider (passes = (v+1)/3)

    bool useFillHoles = true;
    int fillHolesMaxPx = 16;   // fill enclosed bg islands below this size

    bool useFeather = false;
    int featherPx = 0;

    bool useShift = false;
    int shiftPct = 0;          // dialog Shift slider; px = pct/100*24

    bool useRamp = false;
    int rampPct = 0;           // dialog Ramp slider; harden(+) / grow(−)

    // --- AI refine: guided_refine_alpha ------------------------------------
    bool useRefineAi = false;
    int refineRadius = 10;
    double refineEps = 1e-3;
    double refineLevelLo = 0.15;
    double refineLevelHi = 0.85;

    // --- Decontamination: decontaminateStraightRgba ------------------------
    bool decontam = false;
    int decontamRadius = 12;   // foreground-colour diffusion reach
    double decontamAmount = 0.85;  // blend toward the replacement colour

    // --- View / preview ----------------------------------------------------
    ViewMode view = ViewMode::CompositeAlpha;
    double proxyCap = 1400.0;  // live-preview longest side (0 = full res)
    int solidBg = 0;           // flat-color background index for fringe view

    // Derived quantities, matching the Refine dialog exactly.
    int smoothPasses() const { return (smoothSlider + 1) / 3; }
    int shiftPx() const { return int(shiftPct / 100.0 * 24); }

    // --- JSON --------------------------------------------------------------
    QJsonObject toJson() const {
        QJsonObject o;
        o["modelPath"] = modelPath;
        o["inputSize"] = inputSize;
        o["segmentLevelLo"] = segmentLevelLo;
        o["segmentLevelHi"] = segmentLevelHi;
        o["useSpeckles"] = useSpeckles;
        o["speckleMinPx"] = speckleMinPx;
        o["useDecay"] = useDecay;
        o["decaySupportPx"] = decaySupportPx;
        o["decayTexTol"] = decayTexTol;
        o["useSnap"] = useSnap;
        o["smartRadius"] = smartRadius;
        o["snapBandPx"] = snapBandPx;
        o["borderPct"] = borderPct;
        o["snapNarrowDiv"] = snapNarrowDiv;
        o["snapPasses"] = snapPasses;
        o["useSmooth"] = useSmooth;
        o["smoothSlider"] = smoothSlider;
        o["useFillHoles"] = useFillHoles;
        o["fillHolesMaxPx"] = fillHolesMaxPx;
        o["useFeather"] = useFeather;
        o["featherPx"] = featherPx;
        o["useShift"] = useShift;
        o["shiftPct"] = shiftPct;
        o["useRamp"] = useRamp;
        o["rampPct"] = rampPct;
        o["useRefineAi"] = useRefineAi;
        o["refineRadius"] = refineRadius;
        o["refineEps"] = refineEps;
        o["refineLevelLo"] = refineLevelLo;
        o["refineLevelHi"] = refineLevelHi;
        o["decontam"] = decontam;
        o["decontamRadius"] = decontamRadius;
        o["decontamAmount"] = decontamAmount;
        o["view"] = int(view);
        o["proxyCap"] = proxyCap;
        o["solidBg"] = solidBg;
        return o;
    }
    void fromJson(const QJsonObject& o) {
        auto num = [&](const char* k, double def) {
            return o.contains(k) ? o[k].toDouble(def) : def;
        };
        auto flag = [&](const char* k, bool def) {
            return o.contains(k) ? o[k].toBool(def) : def;
        };
        modelPath = o["modelPath"].toString(modelPath);
        inputSize = int(num("inputSize", inputSize));
        segmentLevelLo = num("segmentLevelLo", segmentLevelLo);
        segmentLevelHi = num("segmentLevelHi", segmentLevelHi);
        useSpeckles = flag("useSpeckles", useSpeckles);
        speckleMinPx = int(num("speckleMinPx", speckleMinPx));
        useDecay = flag("useDecay", useDecay);
        decaySupportPx = int(num("decaySupportPx", decaySupportPx));
        decayTexTol = num("decayTexTol", decayTexTol);
        useSnap = flag("useSnap", useSnap);
        smartRadius = flag("smartRadius", smartRadius);
        snapBandPx = int(num("snapBandPx", snapBandPx));
        borderPct = int(num("borderPct", borderPct));
        snapNarrowDiv = int(num("snapNarrowDiv", snapNarrowDiv));
        snapPasses = int(num("snapPasses", snapPasses));
        useSmooth = flag("useSmooth", useSmooth);
        smoothSlider = int(num("smoothSlider", smoothSlider));
        useFillHoles = flag("useFillHoles", useFillHoles);
        fillHolesMaxPx = int(num("fillHolesMaxPx", fillHolesMaxPx));
        useFeather = flag("useFeather", useFeather);
        featherPx = int(num("featherPx", featherPx));
        useShift = flag("useShift", useShift);
        shiftPct = int(num("shiftPct", shiftPct));
        useRamp = flag("useRamp", useRamp);
        rampPct = int(num("rampPct", rampPct));
        useRefineAi = flag("useRefineAi", useRefineAi);
        refineRadius = int(num("refineRadius", refineRadius));
        refineEps = num("refineEps", refineEps);
        refineLevelLo = num("refineLevelLo", refineLevelLo);
        refineLevelHi = num("refineLevelHi", refineLevelHi);
        decontam = flag("decontam", decontam);
        decontamRadius = int(num("decontamRadius", decontamRadius));
        decontamAmount = num("decontamAmount", decontamAmount);
        view = ViewMode(int(num("view", int(view))));
        proxyCap = num("proxyCap", proxyCap);
        solidBg = int(num("solidBg", solidBg));
    }
};

}  // namespace pittore::lab
