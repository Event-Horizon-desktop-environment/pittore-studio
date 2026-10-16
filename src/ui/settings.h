#pragma once
#include <QString>
#include <QStringList>

#include "ui/theme.h"

namespace pittore::ui {

// Saved preferences, stored as TOML in ~/.config/PittoreStudio/Settings.toml.
// Plain values on purpose so the file stays hand-editable. Example:
//
//   theme = "Dark Gray"
//   [compute]
//   gpu_enabled = false
//   ram_limit_mb = 42383   # default: 90% of physical RAM; 0 = unlimited
//
// `reference_mask` holds ';'-separated ground-truth masks used to conform
// AI selections for testing. Empty = normal AI output.

// Default per-document working-set budget: 90% of physical RAM in MB.
// Returns 0 when total RAM is undetectable (legacy unlimited behaviour).
int defaultRamLimitMb();

// Total physical RAM in MB, or 0 when undetectable. Backs the Preferences
// memory slider (range + live % readout).
int totalSystemRamMb();

struct AppSettings {
    UiTheme theme = UiTheme::DarkGray;

    // Compute:
    bool gpuEnabled = false;
    QString gpuDevice;   // preferred GPU device name; empty = first available
    QString cpuDevice;   // preferred CPU device name; empty = default
    int ramLimitMb = defaultRamLimitMb();  // working-set budget per document; 0 = unlimited

    // AI background removal:
    QString bgModel = QStringLiteral("birefnet-portrait");  // id from allAiModels()
    // AI edge enhance (Enhance Edges on the task bar / in Refine Selection):
    // portrait-matting model id from allAiModels(). Uninstalled ids fall back
    // to the best installed hair model automatically.
    QString enhanceModel = QStringLiteral("birefnet-hr-matting");
    // Optional reference mask(s), ';'-separated (see above).
    QString referenceMask;
    // Split `referenceMask` on ';' (empty = no refs).
    QStringList referenceMasks() const {
        return referenceMask.split(u';', Qt::SkipEmptyParts);
    }

    // Crash safety: 0 interval means disabled (avoids a busy timer).
    bool autosaveEnabled = true;
    int autosaveIntervalMinutes = 5;
    // Reopen file-backed documents on startup (General tab).
    bool reopenDocuments = false;
    bool zoomWithScroll = false;
    double gridSpacing = 64.0;
    bool snapEnabled = true;
    // Snap targets as a bitmask matching AppState::SnapTarget bits:
    // Guides=1, Grid=2, DocumentBounds=4, Slices=8, Layers=16 (31 = all).
    // Persisted so View > Snap To survives restarts; session state in
    // AppState mirrors this on load/apply.
    int snapTargets = 31;
    // Canvas "Show" defaults (View menu + Canvas tab): applied to the canvas
    // at startup and whenever Settings closes with OK; the View menu edits
    // the live canvas and writes these back, so both stay in sync.
    bool showRulers = true;
    bool showGuides = true;
    bool showGrid = false;
    bool showSelectionEdges = true;
    bool showSmartGuides = true;
    bool showPixelGrid = false;
    bool showExtras = true;
    // Arrow-key nudge distances in document pixels (Shift-held second).
    double nudgeStepPx = 1.0;
    double nudgeShiftStepPx = 10.0;
    // Grid subdivisions between major lines (1 = off).
    int gridSubdivisions = 4;
    // Slice number tags on the canvas (rectangles always draw).
    bool showSliceNumbers = true;
    // Guide + grid ink (Canvas tab). Stored as #rrggbbaa.
    QColor guideColor = QColor(0x3d, 0xa5, 0xff);
    QColor gridColor = QColor(255, 255, 255, 40);
    // Transparency checkerboard: view-space cell size + inks.
    int transparencyCellPx = 8;
    QColor transparencyLight = QColor(0xff, 0xff, 0xff);
    QColor transparencyDark = QColor(0xbf, 0xbf, 0xbf);
    // Tablet mode: enlarged tool strip + persona bar targets for pen taps.
    bool tabletMode = false;
    // Display scaling, applied at startup before the UI toolkit starts
    // (changing either needs a restart): hidpiEnabled off runs the whole
    // interface unscaled for small screens where it would otherwise be too
    // big; hidpiFractional keeps non-integer scales like 125/150% exact
    // instead of rounding them to whole pixels.
    bool hidpiEnabled = true;
    bool hidpiFractional = true;
    // Font preview size in family combos (Interface tab): 0 = None (plain
    // list), 1 = Small, 2 = Medium, 3 = Large. See familyPreviewPx().
    int fontPreviewSize = 2;
    // Brush cursor (clean-room take on the cursor+outline split): the OS
    // cursor shape and the canvas-drawn tip outline are independent.
    // cursorShape: 0 = outline only (OS cursor hidden), 1 = arrow,
    // 2 = crosshair. outlineShape: 0 = none, 1 = circle, 2 = tip preview
    // (actual nib ellipse/square), 3 = tilt (circle + tilt tick).
    int cursorShape = 0;
    int outlineShape = 2;
    // Show the outline mid-stroke; when false it returns on release.
    bool showOutlineWhilePainting = true;
    // Size the ring by the maximum diameter instead of the live
    // pressure/velocity width, so it stops pulsing under the hand.
    bool outlineEffectiveSize = false;

    // New-document defaults (seed both File > New dialogs; still editable per
    // document). Background: white | black | transparent.
    int newDocWidth = 1920;
    int newDocHeight = 1080;
    int newDocDpi = 300;
    QString newDocColorMode = QStringLiteral("RGB/8");
    QString newDocBackground = QStringLiteral("white");
    // Recent-projects cap (Documents tab). 0 keeps none.
    int recentMax = 12;
    // Working-space profile new documents and imports assume
    // ("sRGB IEC61966-2.1" etc., matching the Export color-space names).
    QString workingProfile = QStringLiteral("sRGB IEC61966-2.1");
    // Profile-mismatch policy (Documents tab): 0 = ask on mismatch,
    // 1 = convert silently, 2 = keep embedded silently,
    // 3 = always ask about any embedded profile (conventional behaviour).
    // Default is AlwaysAsk so an embedded profile is never silently
    // glossed over; pick Ask in Settings for conventional silence
    // on family matches.
    int colorMismatchPolicy = 3;

    // Export defaults (seed the Export dialog; per-export edits don't write
    // back). Format is a lowercase suffix from availableExportFormats();
    // location: 0 = ask each time, 1 = same folder, 2 = choose folder.
    QString exportFormat = QStringLiteral("png");
    int exportQuality = 92;
    int exportLocation = 0;
    bool exportEmbedIcc = true;
    // Layered-PSD channel compression: 0 = RLE (compatible), 1 = ZIP
    // (smaller). Untouched ZIP-origin layers keep ZIP either way.
    int psdCompression = 0;

    // Undo depth (Performance tab). 1..1000 like conventional history states.
    int undoLimit = 100;

    // Soft-proof setup (View > Proof Setup; the Proof Colors / Gamut toggles
    // themselves are session view state). Profile is an
    // absolute .icc/.icm path, empty = unset. Intent follows ProofIntent
    // (0 perceptual … 3 absolute).
    QString proofProfile;
    int proofIntent = 1;
    bool proofBpc = true;

    // CMYK destination (Image > Mode > CMYK; CMYK export separates through
    // it). Absolute .icc/.icm path; empty = probe system defaults, and a
    // document with no resolvable profile converts/exports naively.
    QString cmykProfile;

    QStringList recentProjects;

    bool operator==(const AppSettings&) const = default;
};

// Path of the settings file (created on save).
QString settingsPath();

// Canonical profile family for import mismatch comparison: RGB working
// spaces compare by family ("sRGB" == "sRGB IEC61966-2.1",
// "Adobe RGB" == "Adobe RGB (1998)"), while print profiles (SWOP, FOGRA,
// GRACoL, ...) compare exactly — different CMYK profiles genuinely differ
// even though the naive import cannot tell them apart yet.
QString canonicalProfileName(const QString& name);

// Load from disk. Missing file or bad TOML leaves `out` alone, returns false.
bool loadSettings(AppSettings& out);

// Save to disk, making folders as needed. False on I/O failure.
bool saveSettings(const AppSettings& in);

}  // namespace pittore::ui