#pragma once
#include <QColor>
#include <QDateTime>
#include <QDialog>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/core/image.h"
#include "engine/vector/vector_art.h"

class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMimeData;
class QSpinBox;
class QStackedLayout;

namespace pittore::ui {

class AppState;

// ─────────────────────────────────────────────────────────────────────────────
// Own-format projects
// ─────────────────────────────────────────────────────────────────────────────
// A project is a SINGLE file under the project library:
//
//   ~/Pictures/Pittore Studio/Projects/<Name>.psc
//
// The binary layout (pittore::io project codec) is modeled on PSD: a fixed
// big-endian header (magic, version, size, channels, depth, color mode, dpi,
// background) followed by length-prefixed metadata, layer, and preview
// sections. Every pixel layer's straight 16-bit RGBA lives inside the file as
// a deflated block — there are no sidecar layer PNGs, so a save cannot fail
// midway through a folder of images. Preview thumbnails are kept inline for
// the start page, and a light scan pass decodes them without touching layers.
//
// Layers are stored in document order with index 0 = top, matching the Layers
// panel and DocumentItem::layers. Non-pixel layers (text/shape/group/…) persist
// as metadata only in v1; their pixel content is lossless once supported.

// One layer as it lives in the project file.
struct ProjectLayerMeta {
    QString name = QStringLiteral("Layer 1");
    int kind = 0;             // int(LayerItem::Kind)
    bool visible = true;
    bool locked = false;
    int opacity = 100;
    int fill = 100;
    QString blendMode = QStringLiteral("Normal");
    QPointF offset{0, 0};
    double scaleX = 1.0;
    double scaleY = 1.0;
    // 16-bit straight RGBA native pixels (Format_RGBA64, straight values written
    // into the SCANLINES — the codec stores them verbatim, big-endian, so no
    // premultiplication ever happens). Null image = metadata-only layer
    // (text/shape/group/…).
    QImage pixels;
    std::vector<std::uint16_t> cmyk;
    // v2 layer state: transparency lock, clip-to-below, group nesting depth,
    // and the user mask (straight RGBA64 opaque-grey coverage in R,
    // layer-native, mapped by maskOffset/maskScale like the live model).
    bool lockTransparency = false;
    bool clipped = false;
    int indent = 0;
    bool hasMask = false;
    bool maskEnabled = true;
    QPointF maskOffset{0, 0};
    double maskScaleX = 1.0;
    double maskScaleY = 1.0;
    float maskDensity = 1.0f;
    float maskFeather = 0.0f;
    QImage mask;

    // v7 artboards (D1): a board is a Group row carrying a nonzero boardId
    // (INV-011), so `kind` stays Group and structure rides `indent`; only the
    // payload is extra here. Global document coords, fill opacity separate
    // from `opacity`.
    bool isBoard = false;
    std::uint64_t boardId = 0;
    QRectF boardRect;
    bool boardHasFill = false;
    QColor boardFill;
    double boardFillOpacity = 1.0;

    // Live adjustment layers: kind is an AdjustmentKind value (0 = legacy
    // stand-in with no parameters); params are the kind's floats and the
    // curve holds Curves control points (normalized). `adjustmentCurve` is
    // the RGB composite (master); R/G/B hold per-channel curves, empty =
    // identity. The 768-entry LUT is derived, never persisted.
    bool hasAdjustment = false;
    int adjustmentKind = 0;
    float adjustmentParams[16] = {};
    QVector<QPointF> adjustmentCurve;
    QVector<QPointF> adjustmentCurveR;
    QVector<QPointF> adjustmentCurveG;
    QVector<QPointF> adjustmentCurveB;

    // Live Tone Blend Group (group rows only): set when the header blends.
    // Order matches the file block: strength, color, contrast, lowPass,
    // contentType.
    bool hasToneBlend = false;
    float toneBlendStrength = 1.0f;
    float toneBlendColor = 1.0f;
    float toneBlendContrast = 0.0f;
    float toneBlendLowPass = 1.0f;
    int toneBlendContentType = 0;

    // Text layers. `isText` marks rendered glyphs (the Layers panel fits the
    // whole run inside the thumbnail); `liveText` marks an editable text layer,
    // whose spec fields below are persisted alongside the rendered pixels.
    bool isText = false;
    bool liveText = false;
    QString text;
    QString textFamily = QStringLiteral("sans-serif");
    bool textBold = false;
    bool textItalic = false;
    int textAlign = 0;              // 0 left, 1 centre, 2 right
    double textSize = 48;
    double textLineHeight = 1;
    // Recorded spacing from imported formats (0 = natural step / none).
    double textEmLeading = 0;
    double textParagraphGap = 0;
    double textTracking = 0;
    double textWrapWidth = 0;
    double textFrameHeight = 0;
    QPointF textOrigin{0, 0};
    QColor textColor = QColor(0, 0, 0);

    // Character-panel attributes (persisted in the text extras block).
    int textUnderline = 0;
    int textStrike = 0;
    QColor textUnderlineColor;   // transparent/invalid = inherit the fill
    QColor textStrikeColor;
    QColor textBackgroundColor;  // transparent = no highlight
    double textBaselineShift = 0;
    double textHScale = 100;
    double textVScale = 100;
    int textSuperSub = 0;
    bool textAllCaps = false;
    bool textKerning = true;
    unsigned textOtFeatures = 0;

    // True vector geometry retained from an SVG import, in the layer's source
    // space. Shared so a snapshot costs a refcount; null for raster-only layers.
    std::shared_ptr<const pittore::vector::ArtNode> art;
    // Live filter recipe (v2): id + params + enabled. Native pixels persist
    // untouched alongside; the render re-derives on load.
    bool hasLiveFilter = false;
    bool liveFilterEnabled = true;
    QString liveFilterId;
    QVector<double> liveFilterParams;

    // Foreign PSD blocks (v4): verbatim TySh/SoLd/... kept only for fresh
    // layers (see psdRawBlocksFresh); stale ones are dropped at save, so
    // what reloads always matches the saved pixels.
    struct PsdRawBlock {
        char sig[4] = {'8', 'B', 'I', 'M'};
        char key[4] = {};
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> padding;
    };
    std::vector<PsdRawBlock> psdRawBlocks;
};

// Everything the project file persists.
struct ProjectFileData {
    QString name;
    QSize size{1920, 1080};
    int dpi = 300;
    QString colorMode = QStringLiteral("RGB/8");
    // CMYK destination profile bytes, persisted with the tag so a reopened
    // project re-exports through the same separation (empty for RGB/Gray).
    QByteArray iccProfile;
    QString background = QStringLiteral("white");  // white | black | transparent
    QVector<ProjectLayerMeta> layers;              // index 0 = top
    QImage preview;                                // flattened preview (saved scaled)
    // v7: highest board id issued, so a board created after load never
    // collides (INV-006). 0 = no boards.
    std::uint64_t boardIdCounter = 0;
};

// The project library root: ~/Pictures/Pittore Studio/Projects (override with
// $PITTORE_PROJECTS_DIR for tests/portable installs).
QString projectsRootDir();
// Absolute path of the project file that `name` owns (<root>/<name>.psc).
QString projectPathForName(const QString& name);
// True for a suffix holding a native project: "psc" is current, "ifp" is the
// legacy Pittore Studio extension — still opened everywhere new projects are.
bool isNativeProjectSuffix(const QString& suffix);

// Write `data` into the single project file `path`, creating the library
// directory as needed. Returns false with *error set on any failure.
bool saveProjectFile(const QString& path, const ProjectFileData& data, QString* error = nullptr);
// Read `path` back into `out`. Returns false with *error set when the file is
// not a project or is corrupt.
bool loadProjectFile(const QString& path, ProjectFileData* out, QString* error = nullptr);

// Straight-alpha bridge between the engine's RGBAf image and the 16-bit
// RGBA64 QImage the layer files store. Values are placed into the scanlines
// verbatim — the IFP codec round-trips them big-endian (verified in tests), so
// no premultiplication ever happens on either side.
QImage imageToStraightRgba64(const pittore::Image& img);
std::shared_ptr<pittore::Image> straightRgba64ToImage(const QImage& q);

// Copy a straight-RGBA QImage (any native format) into the codec's 16-bit
// sample vector (Format_RGBA64 layout, straight values written verbatim), and
// the inverse. Exported so the image import/export helpers and the tests can
// use them directly.
std::vector<std::uint16_t> rgba16FromQImage(const QImage& q);
QImage qimageFromRgba16(const std::vector<std::uint16_t>& samples, int w, int h);

// Decode an image file for import (flattened composite). IFP is NOT handled
// here — use loadProjectFile/openProject for the native format. PSD/PSB and
// XCF go through the built-in codecs, KRA through the built-in ZIP reader
// (its mergedimage.png), and every other suffix through Qt's image plugins,
// i.e. anything QImageReader supports at runtime (PNG/JPEG/TIFF/WebP/GIF/BMP
// plus whatever system plugins add: AVIF/HEIF/JXL/EXR/…). Returns a null image
// with *error set on failure; *dpiOut (when non-null) receives the file's
// stored resolution, 0 when it stores none.
//
// The capped overload decodes huge images as a box-averaged proxy instead of
// refusing them: when the full dimensions hold more than maxPixels, it opens
// at 1/factor scale with factor = ceil(sqrt(pixels/maxPixels)) and reports
// the factor in *factorOut (1 = full-res) and the true dimensions in
// *fullSizeOut. Pass maxPixels = 0 for the legacy dense-only behaviour.
// TIFF proxies stream row-by-row; other formats use QImageReader::setScaledSize
// with a full-decode fallback. Proxy rows are rejected the same way dense
// failures are (null image + *error).
QImage qimageFromFile(const QString& path, QString* error = nullptr,
                      int* dpiOut = nullptr);
QImage qimageFromFileCapped(const QString& path, std::uint64_t maxPixels,
                            QString* error = nullptr, int* dpiOut = nullptr,
                            int* factorOut = nullptr, QSize* fullSizeOut = nullptr);
// Fast dimensions-only probe (no pixel decode): TIFF via libtiff, everything
// else via QImageReader::size. False when the size is unknown.
bool probeImageSize(const QString& path, QSize& sizeOut, int& dpiOut);
// Smallest integer subsample factor whose output fits in maxPixels
// (ceil(sqrt(pixels/maxPixels)), min 1). Pure arithmetic for tests and the
// import path.
int proxyFactorForPixels(std::uint64_t pixels, std::uint64_t maxPixels);
// True for a suffix the app can open (IFP, built-in PSD/PSB/XCF/KRA/SVG, WebP/
// TIFF when compiled in, or any suffix the Qt image plugins can read).
bool suffixIsOpenable(const QString& suffix);
// True when any of the dropped mime-data URLs is a local openable file. Shared
// by the main-window drop target and the project-manager start page.
bool dropHasOpenableFiles(const QMimeData* mime);
// File-dialog filter for every importable format (IFP project, built-in
// PSD/PSB/XCF/KRA, WebP/TIFF when compiled in, plus every suffix
// QImageReader supports at runtime), and the matching export filter
// (PSD/PSB/XCF + built-ins + every suffix QImageWriter supports).
QString imageOpenFilter();
QString imageExportFilter();
// Write a flattened straight-RGBA document to `path`, choosing the writer by
// file extension: .psd/.psb (16-bit PSD), .xcf (XCF), or any suffix
// QImageWriter supports (PNG/JPEG/TIFF/WebP/BMP/…). Returns false with *error
// set on any failure.
//
// `jpegQuality` (1-100) applies to the writers that expose a quality knob
// (JPEG/WebP/AVIF/HEIF); -1 keeps the format default (92 for lossy formats).
// `progressive` enables a progressive/interlaced scan where the writer
// supports it (ProgressiveScanWrite: JPEG, WebP). Both are ignored by the
// built-in PSD/XCF/PNG/TIFF paths. The Export As dialog passes them through.
bool saveImageFile(const QImage& flat, const QString& path,
                   QString* error = nullptr, int jpegQuality = -1,
                   bool progressive = false);

// A row on the start page.
struct ProjectEntry {
    QString name;
    QString path;
    QDateTime modified;
    QImage thumb;   // inline preview, pre-scaled for card display
};
// Scan the library for existing projects (newest last-modified first).
QVector<ProjectEntry> scanProjects();

// ─────────────────────────────────────────────────────────────────────────────
// The project manager / start page
// ─────────────────────────────────────────────────────────────────────────────
// Two stacked pages: a thumbnail grid of existing projects (open via
// double-click / Open) and the New Project form (name, auto-computed path, size
// presets, DPI, color mode, background). Performs AppState mutations itself and
// exits with Accepted once a project is open or created.
class ProjectManagerDialog : public QDialog {
    Q_OBJECT

  public:
    explicit ProjectManagerDialog(AppState* state, QWidget* parent = nullptr);

    void refresh();   // re-scan the library and rebuild the start page

  protected:
    // Dragging a supported file (PSD/SVG/PNG/IFP/…) onto the start page opens
    // it, so a drop works even while the modal dialog covers the window.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private slots:
    void newClicked();
    void openClicked();
    void openItem(QListWidgetItem* item);
    void nameEdited();
    void presetChanged(int index);
    void createClicked();

  private:
    QWidget* buildStartPage();
    QWidget* buildNewProjectPage();
    void switchToStart();
    void switchToNew();
    void openProjectPath(const QString& path);

    AppState* state_;
    QStackedLayout* stack_ = nullptr;
    QListWidget* recents_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLabel* pathLabel_ = nullptr;
    QComboBox* preset_ = nullptr;
    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QSpinBox* dpi_ = nullptr;
    QComboBox* mode_ = nullptr;
    QComboBox* background_ = nullptr;
};

}  // namespace pittore::ui
