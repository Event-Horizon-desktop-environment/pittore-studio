#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pittore::io {

// IFP — our project file, PSD-style: big-endian header + u32 sections, so
// readers can skip layers and jump to the preview. One file, no sidecars.
// Header: INFIPROJ, v1/v2, w/h, RGBA16, colorMode, dpi, background, meta/layer/
// preview lens. Layers (0 = top): name, kind, flags (visible/locked/isText/
// textSpec/textExtras/vector/adjustment) [+v2 flags2 + indent], opacity/fill,
// blend, transform, pixels (deflate RGBA16; 0x0 = metadata-only), then
// text/extras/vector/adjustment [+v2 mask] blocks. Preview: w/h + pixel
// block. Samples are straight big-endian 16-bit, like PSD.
//
// v2 adds per-layer masks, clipping, nesting depth and the transparency lock.
// v5 adds optional per-layer native CMYK samples. Files that need no newer
// fields retain the oldest compatible version; v1-v4 files decode unchanged.
//
// v7 adds artboards (D1): a header version bump (a v6 reader refuses the file,
// §8.7) plus self-describing TLV records inside the layers section (§8.2/§8.3).
// A board is a layer row whose kind byte is kProjectKindArtboard, immediately
// followed by an ABDT record carrying the board payload. In memory a board is a
// Group row carrying boardId (INV-011); the codec translates, so the runtime
// Kind enum is untouched. ABOR opens the section (board count + id counter),
// ABEN closes the rows (board order), and trailing unknown records are retained
// verbatim and re-emitted in order (§8.2 rules 1/2/5).

// On-disk kind byte for a board row, and the Group kind a board row decodes to
// (the runtime models a board as a Group row carrying boardId, INV-011). Both
// mirror LayerItem::Kind in app_state.h, which stops at Frame=6; Artboard=7
// exists only inside the codec.
inline constexpr std::uint8_t kProjectKindArtboard = 7;
inline constexpr std::uint8_t kProjectKindGroup = 4;

struct ProjectLayerFile {
    std::string name;
    std::uint8_t kind = 0;
    bool visible = true;
    bool locked = false;
    std::uint16_t opacity = 100;
    std::uint16_t fill = 100;
    std::string blend;                  // blend-mode name
    double offsetX = 0;
    double offsetY = 0;
    double scaleX = 1;
    double scaleY = 1;
    std::uint32_t width = 0;            // native pixel width; 0 = metadata-only
    std::uint32_t height = 0;
    std::vector<std::uint16_t> rgba;    // width*height*4 straight samples
    // Optional native C,M,Y,K samples (width*height*4), in device ink space.
    std::vector<std::uint16_t> cmyk;

    // Text: isText = rendered glyphs; hasTextSpec = live editable (fields below).
    bool isText = false;
    bool hasTextSpec = false;
    std::string text;
    std::string textFamily;
    bool textBold = false;
    bool textItalic = false;
    std::uint8_t textAlign = 0;         // 0 left, 1 centre, 2 right
    double textSize = 48;
    double textLineHeight = 1;
    double textTracking = 0;
    double textWrapWidth = 0;           // > 0 = frame text
    double textFrameHeight = 0;
    double textOriginX = 0;
    double textOriginY = 0;
    std::uint32_t textColor = 0xff000000;  // 0xAARRGGBB

    // v6: spacing imported from foreign formats (0 = natural step / none).
    double textEmLeading = 0;     // line advance as an em fraction, when set
    double textParagraphGap = 0;  // distance after every paragraph break, px

    // Char-panel extras (flag-gated so old files still parse).
    bool hasTextExtras = false;
    std::uint8_t textUnderline = 0;   // 0 none, 1 single, 2 double
    std::uint8_t textStrike = 0;
    std::uint32_t textUnderlineColor = 0;   // 0 = inherit the fill
    std::uint32_t textStrikeColor = 0;
    std::uint32_t textBackgroundColor = 0;  // 0 = no highlight
    double textBaselineShift = 0;
    double textHScale = 100;
    double textVScale = 100;
    std::int8_t textSuperSub = 0;
    bool textAllCaps = false;
    bool textKerning = true;
    std::uint32_t textOtFeatures = 0;

    // Kept vector path/paint (opaque ArtNode bytes, via pittore::vector).
    bool hasVector = false;
    std::vector<std::uint8_t> vectorData;

    // Live adjustment (flag-gated). Curve = Curves points, empty otherwise.
    // `adjustmentCurve` is the RGB composite (master); R/G/B hold the
    // per-channel Curves points (v2, empty = identity).
    bool hasAdjustment = false;
    std::uint8_t adjustmentKind = 0;
    double adjustmentParams[16] = {};
    std::vector<std::pair<double, double>> adjustmentCurve;
    std::vector<std::pair<double, double>> adjustmentCurveR;
    std::vector<std::pair<double, double>> adjustmentCurveG;
    std::vector<std::pair<double, double>> adjustmentCurveB;

    // Live Tone Blend Group (flag-gated, bit 7): strength, color, contrast,
    // lowPass, contentType (as double). Groups predate tone blend, so old
    // files simply read hasToneBlend = false. (The research brief reserved
    // bit 5, but bits 5–6 went to vector/adjustment first.)
    bool hasToneBlend = false;
    double toneBlend[5] = {1, 1, 0, 1, 0};

    // v2: flags2 bit 0 = hasMask, bit 1 = clipped, bit 2 = lockTransparency;
    // indent is the group nesting depth (0 = top level). The mask is a
    // single-channel coverage grid (65535 = reveal) in layer-native pixels,
    // mapped to the document by (offset, scale) like the live model.
    bool lockTransparency = false;
    bool clipped = false;
    std::int32_t indent = 0;
    bool hasMask = false;
    bool maskEnabled = true;
    double maskOffsetX = 0;
    double maskOffsetY = 0;
    double maskScaleX = 1;
    double maskScaleY = 1;
    double maskDensity = 1;
    double maskFeather = 0;
    std::uint32_t maskWidth = 0;
    std::uint32_t maskHeight = 0;
    std::vector<std::uint16_t> mask;

    // Live filter (v2, flags2 bit 3): recipe id + params + enabled. The
    // native pixels persist untouched alongside; the render re-derives.
    bool hasLiveFilter = false;
    bool liveFilterEnabled = true;
    std::string liveFilterId;
    std::vector<double> liveFilterParams;

    // v7 artboards (D1). On disk a board is a layer row whose kind byte is
    // kProjectKindArtboard (7), immediately followed by an ABDT record (§8.3)
    // carrying the payload below. In memory a board is a Group row carrying
    // boardId (INV-011); the codec translates, so the runtime Kind enum and
    // every Kind switch are untouched. Structure and membership ride the
    // existing `indent`; board opacity reuses `opacity` and visible/locked
    // reuse the layer flags, so each property has a single source of truth.
    // A v6 reader refuses a v7 file outright (§8.7), so these are
    // version-gated, not forward-negotiated.
    bool isBoard = false;
    std::uint64_t boardId = 0;
    double boardX = 0;
    double boardY = 0;
    double boardW = 0;
    double boardH = 0;
    bool boardHasFill = false;
    std::uint32_t boardFillColor = 0xff000000;  // 0xAARRGGBB
    float boardFillOpacity = 1.0f;
    // ABDT `extras` blob (length-delimited, currently empty). Carried verbatim
    // so a future per-board record (guides, INV-034) round-trips unmodelled.
    std::vector<std::uint8_t> boardExtras;

    // Foreign PSD blocks (v4): verbatim TySh/SoLd/..., carried only for
    // layers that are untouched (guarded upstairs, not here). Any layer
    // with blocks forces version 4; older readers reject v4 files.
    struct PsdBlock {
        char sig[4] = {'8', 'B', 'I', 'M'};
        char key[4] = {};
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> padding;
    };
    std::vector<PsdBlock> psdBlocks;
    // Native CMYK samples use the v5 optional per-layer payload.
};

struct ProjectFileDoc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t dpi = 300;
    std::uint16_t colorMode = 3;        // 1 grayscale, 3 RGB
    std::string name;
    std::string colorModeName;          // e.g. "RGB/8"
    std::string background;             // white | black | transparent
    // CMYK destination profile bytes (tag "CMYK/…"); empty otherwise.
    // Trailing u32-length block in the meta section: old readers skip to
    // metaLen and new readers tolerate its absence, so no version bump.
    std::vector<std::uint8_t> icc;
    std::vector<ProjectLayerFile> layers;  // index 0 = top
    // v7: highest board id issued in this document, so a board created after
    // load gets an id above every loaded one (INV-006). 0 = no boards.
    std::uint64_t boardIdCounter = 0;
    // v7: board order from the ABEN record, in sequence. The UI rebuilds order
    // from panel position (they match); carried so the record round-trips and
    // a reader without a tree can still resolve order.
    std::vector<std::uint64_t> boardOrder;
    // v7: trailing unknown records (§8.2 rules 1/2/5), retained verbatim and
    // re-emitted in order on save. "Preserve unknown data on round trip".
    std::vector<std::uint8_t> unknownRecords;
    std::uint32_t previewWidth = 0;
    std::uint32_t previewHeight = 0;
    std::vector<std::uint16_t> preview; // previewW*previewH*4 straight samples
};

// Writes IFP. Nullopt on bad dims or pixel mismatch.
std::optional<std::vector<std::uint8_t>> projectEncode(const ProjectFileDoc& project,
                                                       std::string* error = nullptr);

// Reads IFP. Bounds-checked; nullopt with reason on corrupt data.
std::optional<ProjectFileDoc> projectDecode(const std::vector<std::uint8_t>& bytes,
                                            std::string* error = nullptr);

// Header + preview only, no layer pixels. For start-page thumbnails.
struct ProjectScanInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string name;
    std::string colorModeName;
    std::string background;
    std::uint32_t previewWidth = 0;
    std::uint32_t previewHeight = 0;
    std::vector<std::uint16_t> preview;
};
std::optional<ProjectScanInfo> projectScan(const std::vector<std::uint8_t>& bytes,
                                           std::string* error = nullptr);

}  // namespace pittore::io
