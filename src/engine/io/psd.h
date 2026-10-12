#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pittore::io {

// PSD flattened to straight RGBA16. `depth` is source depth (8/16) for info;
// gray/CMYK/Lab are promoted to RGBA.
struct PsdImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int depth = 8;      // source bit depth: 8 or 16
    int colorMode = 0;  // original PSD color mode id (1 gray, 3 RGB, 4 CMYK, 9 Lab)
    std::vector<std::uint16_t> rgba;  // width*height*4, straight alpha
    // Native C,M,Y,K device values for original CMYK data; inks are not
    // complemented here. Empty for other source modes.
    std::vector<std::uint16_t> cmyk;  // width*height*4
};

// Reads PSD composite (raw/RLE; 8/16-bit RGB, gray, CMYK, Lab). Nullopt on failure.
std::optional<PsdImage> psdDecode(const std::vector<std::uint8_t>& data,
                                  std::string* error = nullptr,
                                  const std::vector<std::uint8_t>* embeddedIcc = nullptr);

// Embedded ICC profile description, or "" when the file carries none or it
// cannot be read. Walks the image-resource section for resource id 0x040F
// (spec; 0x0422 accepted as legacy) and reads the profile's 'desc' tag
// (textDescriptionType, mluc with enUS preferred, or plain 'text' type).
// Fully bounds-checked, never throws; a hostile/truncated file simply
// yields "". Used by the import profile-
// mismatch policy (the conventional "Embedded Profile Mismatch" dialog).
std::string psdIccProfileName(const std::vector<std::uint8_t>& data);

// Raw embedded ICC profile bytes, or empty when absent/unreadable (same walk
// as above). Feeds the profiled CMYK import path; the name above feeds the
// mismatch dialog.
std::vector<std::uint8_t> psdEmbeddedIcc(const std::vector<std::uint8_t>& data);

// Writes RGBA as PSD (8BPS v1, RLE). `colorMode` 3 (default) writes RGB;
// 4 separates the appearance into CMYK (profiled from `icc` when it is a
// usable CMYK profile, naive full-GCR otherwise) and embeds the profile as
// resource 0x040F. Nullopt on bad input.
std::optional<std::vector<std::uint8_t>> psdEncodeRgba(
    std::uint32_t width, std::uint32_t height, int depth,
    const std::uint16_t* rgba, int colorMode = 3,
    const std::uint8_t* icc = nullptr, std::size_t iccLen = 0);

// One layer from the layer & mask section.
struct PsdLayerFile {
    std::uint32_t left = 0, top = 0;      // document-space origin
    std::uint32_t width = 0, height = 0;  // layer extent
    std::uint16_t opacity = 255;          // 0..255 (PSD byte convention)
    bool visible = true;
    bool clipped = false;
    int indent = 0;             // group nesting depth (0 = top level)
    bool isGroup = false;       // folder layer (lsct section type 1/2)
    bool groupExpanded = true;  // open folder (1) vs closed (2)
    std::string name;
    std::string blend = "Normal";  // Pittore blend-mode name
    // width*height*4 RGBA16 (empty for groups).
    std::vector<std::uint16_t> rgba;
    // Native C,M,Y,K device values (not complemented), retained when the
    // source layer was decoded from a CMYK PSD/PSB.
    std::vector<std::uint16_t> cmyk;
    // Original channel compression of the first pixel channel seen at
    // decode (0/1/2/3, -1 when authored here): untouched layers re-emit
    // with it (see preferZip), so opening and saving doesn't churn methods.
    int channelMethod = -1;
    // Force ZIP for this layer's channel blocks even when the document
    // encodes RLE (set for untouched ZIP-origin layers; mixed methods are
    // legal PSD).
    bool preferZip = false;
    // User mask (channel -2), 0..65535 coverage, 65535 = show. Coords are
    // document-space unless `maskRelative` (then layer-relative).
    bool hasMask = false;
    bool maskEnabled = true;
    bool maskRelative = false;
    std::int32_t maskLeft = 0, maskTop = 0;
    std::uint32_t maskWidth = 0, maskHeight = 0;
    std::vector<std::uint16_t> mask;
    // Live adjustment layer (no pixels). Kind 0 = unsupported, kept as no-op
    // so the row stays visible. `adjustmentCurve` is the RGB composite
    // (master); R/G/B hold per-channel Curves points. All 0..1, empty =
    // identity.
    bool isAdjustment = false;
    int adjustmentKind = 0;
    float adjustmentParams[16] = {};
    std::vector<std::pair<double, double>> adjustmentCurve;
    std::vector<std::pair<double, double>> adjustmentCurveR;
    std::vector<std::pair<double, double>> adjustmentCurveG;
    std::vector<std::pair<double, double>> adjustmentCurveB;
    // Uninterpreted additional-info blocks, preserved verbatim so a
    // decode→encode round trip doesn't shed data we don't understand
    // (type, smart objects, effects, vector masks, ...). Blocks we
    // regenerate ourselves ('luni', 'lsct', masks, adjustments) are never
    // stored here. `sig` is the stored signature (8BIM, or 8B64 from PSB);
    // `padding` is the exact pad bytes that followed the data.
    struct RawBlock {
        char sig[4] = {'8', 'B', 'I', 'M'};
        char key[4] = {};
        std::vector<std::uint8_t> data;
        std::vector<std::uint8_t> padding;
    };
    std::vector<RawBlock> rawBlocks;
};

// Layered doc in FILE order (bottom -> top). UI reverses for its panel.
struct PsdLayersDoc {
    std::uint32_t width = 0, height = 0;
    int depth = 8;  // 8 or 16
    // PSD color mode id: 1 gray, 3 RGB, 4 CMYK, 9 Lab. Layer `rgba` planes
    // stay appearance RGBA16 either way — mode 4 separates them (and the
    // composite) to inks at write time. The writer only branches on 4 and
    // always emits 3/4, so other ids are import metadata.
    int colorMode = 3;
    // Destination CMYK profile bytes for mode 4: profiled separation when
    // usable, naive full-GCR when empty/garbage. Also embedded as resource
    // 0x040F so round trips separate through the same profile.
    std::vector<std::uint8_t> icc;
    std::vector<PsdLayerFile> layers;
};

// Reads layer & mask section. For previews use psdDecode instead.
std::optional<PsdLayersDoc> psdDecodeLayers(const std::vector<std::uint8_t>& data,
                                            std::string* error = nullptr,
                                            const std::vector<std::uint8_t>* embeddedIcc = nullptr);

// Writes layered PSD (8/16-bit; RLE channels by default, ZIP on request).
// Groups get lsct markers;
// masks ride as channel -2; live adjustments ride as their native
// additional-info payloads; foreign blocks are preserved verbatim.
// Composite is source-over with masks + clipping + adjustments. Stand-in
// adjustments (kind 0) are skipped: they have no faithful representation,
// and blank pixels would corrupt the canvas on reimport.
enum class PsdLayerCompression { Rle, Zip };
std::optional<std::vector<std::uint8_t>> psdEncodeLayers(
    const PsdLayersDoc& doc, std::string* error = nullptr,
    PsdLayerCompression compression = PsdLayerCompression::Rle);

}  // namespace pittore::io
