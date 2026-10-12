#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pittore::io {

// TIFF via libtiff (needs PITTORE_TIFF).
// Decode gives 8-bit RGBA (x257 to 16-bit); 16-bit sources lose depth, like Qt.
// Encode writes 8-bit LZW scanlines, keeping dpi.
//
// Large-image notes: tiffDecodeRgba16() holds a full uint32 raster (W*H*4)
// plus the RGBA16 output (W*H*8) at once — ~5GB transient for a 416MP file.
// Use tiffProbeFile() + tiffDecodeFileBanded() for big files: the banded path
// reads via TIFFReadScanline row-by-row straight into the caller's sink with
// only one row in flight, and opens by filename so the 1GB compressed file
// is never fully buffered in RAM.

// Reads a TIFF buffer into RGBA16. False if not decodable.
bool tiffDecodeRgba16(const std::vector<std::uint8_t>& data,
                      std::uint32_t& width, std::uint32_t& height,
                      std::vector<std::uint16_t>& rgba,
                      int& dpi);

// Writes RGBA16 (>>8) as LZW TIFF. Sets dpi tags when dpi > 0. Replaces `out`.
bool tiffEncodeRgba16(std::uint32_t width, std::uint32_t height, int dpi,
                      const std::uint16_t* rgba,
                      std::vector<std::uint8_t>& out);

// Export encoder options: bit depth (8/16), monochrome gray, and packbits-free
// compression (0 = none, 1 = LZW, 2 = ZIP/deflate). Input is RGBA16
// straight-alpha (the >>8 convention above); gray averages the RGB channels
// and alpha is flattened over white first by the caller when the target has
// no alpha channel. `cmyk` separates the appearance into ink planes
// (PHOTOMETRIC_SEPARATED, ink coverage — not Adobe-inverted) at write time:
// profiled from `icc` when it is a usable CMYK profile, naive full-GCR
// otherwise; `icc` is also embedded as TIFFTAG_ICCPROFILE whenever set.
struct TiffEncodeOptions {
    int bits = 8;          // 8 or 16
    bool grayscale = false;
    bool withAlpha = true;   // extra alpha sample for RGB(A)/gray (8-bit only)
    int compression = 1;     // 0 none, 1 LZW, 2 ZIP
    bool cmyk = false;                    // separate to CMYK ink planes
    std::vector<std::uint8_t> icc;        // profile to embed (any space)
};

// Full export encoder. 16-bit + alpha writes RGBA64 (the one combination
// libtiff widely reads); gray+alpha is flattened by the caller instead.
// Replaces `out`.
bool tiffEncodeExport(std::uint32_t width, std::uint32_t height, int dpi,
                      const std::uint16_t* rgba, const TiffEncodeOptions& opt,
                      std::vector<std::uint8_t>& out);

// Image dimensions + resolution without decoding any pixels. Opens by filename
// so huge files are never buffered. False when the file is not a TIFF.
bool tiffProbeFile(const char* path, std::uint32_t& width,
                   std::uint32_t& height, int& dpi);

// Read original 8/16-bit separated ink samples without converting them
// through the display RGB preview. Supports stripped or tiled, contiguous or
// planar-separate CMYK/CMYKA in all eight TIFF orientations; samples use
// top-left display order.
bool tiffDecodeCmykFile(const char* path, std::uint32_t& width,
                        std::uint32_t& height,
                        std::vector<std::uint16_t>& cmyk,
                        std::vector<std::uint8_t>* icc = nullptr,
                        int* bitDepth = nullptr);

// Streaming decode for large files: reads row-by-row via TIFFReadScanline and
// calls sink(y, rgba8Row) with W*4 straight-alpha bytes per row. Only one row
// is in flight; the caller owns the destination (e.g. QImage scanlines).
// Supports 8/16-bit contiguous grey/RGB/RGBA (MINISBLACK/MINISWHITE/RGB,
// TOPLEFT orientation only); palette/YCbCr/CMYK/separate-planar fall back to
// tiffDecodeRgba16() (returns false here). Returns false on any failure;
// a sink returning false aborts the decode (treated as failure).
bool tiffDecodeFileBanded(const char* path,
                          std::function<bool(std::uint32_t, const std::uint8_t*)>& sink);

// Same as above but over an in-memory buffer (for tests). The buffer must
// stay alive for the call.
bool tiffDecodeBufferBanded(const std::vector<std::uint8_t>& data,
                            std::function<bool(std::uint32_t, const std::uint8_t*)>& sink);

// Subsampled decode for huge files: box-averages each factor×factor source
// block (edge blocks shrink) into one output pixel, emitting hOut =
// ceil(h/factor) rows of wOut = ceil(w/factor) RGBA8 pixels. Same format
// support as the banded path; factor <= 1 behaves like the banded decode.
// Reports the subsampled dimensions in wOut/hOut (0 on entry, set on success)
// and the file dpi. Only one converted source row plus one accumulator row
// (~wOut*16 bytes) is in flight, so a 416MP file decodes to an 11MP proxy
// with < 1MB of transient state.
bool tiffDecodeFileSubsampled(const char* path, int factor,
                              std::uint32_t& wOut, std::uint32_t& hOut,
                              int& dpi,
                              std::function<bool(std::uint32_t, const std::uint8_t*)>& sink);

// Buffer twin of the above (for tests).
bool tiffDecodeBufferSubsampled(const std::vector<std::uint8_t>& data,
                                int factor, std::uint32_t& wOut,
                                std::uint32_t& hOut, int& dpi,
                                std::function<bool(std::uint32_t, const std::uint8_t*)>& sink);

// Embedded ICC profile description from the ICCPROFILE tag, or "" when the
// file carries none or it cannot be read (first directory only). Used by the
// import profile-mismatch policy.
std::string tiffIccProfileName(const char* path);
// Buffer twin (for tests). The buffer must stay alive for the call.
std::string tiffIccProfileData(const std::vector<std::uint8_t>& data);

}  // namespace pittore::io
