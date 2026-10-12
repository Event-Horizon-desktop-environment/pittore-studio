#pragma once
// Refine Lab engine — runs the EXACT production pipeline (the real
// selection_mask.cpp solvers and the real AI segmentation / guided refine)
// under a LabParams, and hands back every intermediate matte plus the final
// cutout so the UI can show what each stage did. No re-implemented math: this
// links the same objects the app ships with.

#include <QImage>
#include <QPair>
#include <QString>
#include <QVector>

#include <vector>

#include "lab_params.h"

namespace pittore::lab {

// One captured pipeline stage (name + the matte right after it).
using Stage = QPair<QString, QImage>;

struct RunResult {
    bool ok = false;
    QString error;
    QImage matte;    // refined alpha, Grayscale8
    QImage cutout;   // straight RGBA, alpha = matte, RGB decontaminated
    QImage original; // the source this run started from (same scale as matte)
    QVector<Stage> stages;  // intermediate mattes, in execution order
    double ms = 0.0;
};

class LabEngine {
  public:
    // Initial AI matte (Select Subject / Remove Background), Grayscale8, at
    // the scale of `img`. Empty result + error string on failure. When
    // `rawAlpha` is non-null it receives the model's raw [0,1] alpha so the
    // caller can re-apply a level band live without re-running inference.
    QImage segment(const QImage& img, const LabParams& p, QString* error,
                   std::vector<float>* rawAlpha = nullptr) const;

    // Full refine over the classical stages + optional AI refine + optional
    // decontamination. `seed` is the initial matte (Grayscale8, same size as
    // img). Captures a stage matte after every step.
    RunResult refine(const QImage& img, const QImage& seed,
                     const LabParams& p) const;
};

// Flat backgrounds the fringe view / solid composite cycle through.
inline QString solidBgName(int i) {
    switch (i) {
        case 0: return "Magenta";
        case 1: return "Green";
        case 2: return "Gray 50%";
        case 3: return "Black";
        default: return "Magenta";
    }
}
inline QRgb solidBgColor(int i) {
    switch (i) {
        case 0: return qRgba(255, 0, 255, 255);
        case 1: return qRgba(0, 255, 0, 255);
        case 2: return qRgba(128, 128, 128, 255);
        case 3: return qRgba(0, 0, 0, 255);
        default: return qRgba(255, 0, 255, 255);
    }
}

}  // namespace pittore::lab
