#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pittore::ai {

// Whether this build links ONNX Runtime. When false the segmentation entry
// points return a clear "built without ONNX Runtime" error and the UI shows an
// install hint instead of pretending to work.
bool onnx_available();
std::string onnx_version();

struct SegmentResult {
    bool ok = false;
    std::string error;
    int width = 0;   // mask width (== input width)
    int height = 0;  // mask height (== input height)
    // width*height foreground-alpha values in [0,1], row-major.
    std::vector<float> alpha;
};

// Run the segmentation model at `modelPath` over an RGBA8 image (straight
// alpha, row-major) and return a full-resolution soft mask of what to keep.
// `inputSize` is the square network input edge (see AiModel::inputSize).
//
// The pipeline matches rembg's BaseSession treatment shared by U²-Net / IS-Net
// / BiRefNet: center-weighted bilinear resize to the model's square input,
// ImageNet mean/std normalisation, single-channel logit output min-max
// normalised back to [0,1], then resampled to the source resolution.
//
// Sessions are cached per model path: loading a ~1 GB BiRefNet takes seconds,
// so the weights stay resident between calls.
SegmentResult segment_rgba8(const std::uint8_t* rgba, int width, int height,
                            const std::string& modelPath, int inputSize);

// ---------------------------------------------------------------------------
// conventional SAM pair ("Object Select"). Two ONNX graphs split the work:
// a heavy 1024² encoder produces [1,256,64,64] image embeddings ONCE per
// layer; the tiny prompt-driven decoder then masks whatever object a query
// point lands on, in milliseconds. `encode_rgba8` returns the cached
// embeddings; `decode_point` turns a pixel + embeddings into a mask. The one
// call the menu commands use (`segment_rgba8_pair`) chains the two with a
// centre-of-image prompt.
//
// Like the single-model path, sessions stay cached by model path, and every
// step is staged-logged and exception-guarded — a bad_alloc surfaces as an
// error string, never a crash.
// ---------------------------------------------------------------------------
struct SamEncodings {
    bool ok = false;
    std::string error;
    int width = 0;      // source image the embeddings were computed for
    int height = 0;
    int inputSize = 0;  // encoder edge used (needed to map query pixels)
    std::vector<float> embeddings;  // [1,256,64,64] row-major
};

// Encoder pass. Internally resizes to the square input and applies SAM's
// standard ImageNet mean/std normalisation — the format the .af encoder
// was trained with (feeding raw 0..255 leaves the embeddings out of
// distribution and yields sub-part fragments instead of whole objects).
SamEncodings encode_rgba8(const std::uint8_t* rgba, int width, int height,
                          const std::string& encoderPath, int inputSize);

struct SamDecodeResult {
    bool ok = false;
    std::string error;
    float iou = 0.0f;               // predicted mask quality, [0,1]-ish
    int px = -1;                    // prompt point used (layer pixel space)
    int py = -1;
    std::vector<float> alpha;       // width*height foreground alpha, [0,1]
};

// Decoder pass for a single foreground query point (document pixels). Fast:
// only the small decoder graph runs. `enc` must come from encode_rgba8 of the
// same layer pixels. With the encoder correctly normalised a point that lands
// on an object returns the whole object, not an arbitrary sub-part.
SamDecodeResult decode_point(const SamEncodings& enc, int px, int py,
                             const std::string& decoderPath);

// Multi-point decoder pass: all points are foreground (label 1). This is the
// canonical SAM interaction for merging attached objects in ONE decode — a
// torso point plus a point on the headphones/arm grows the subject in a single
// run instead of unioning separately-decoded masks. The decoder's batch of
// prompts is [1,N,2]; models that reject N>1 fail cleanly (ok=false).
SamDecodeResult decode_points(const SamEncodings& enc,
                              const std::vector<std::pair<int, int>>& points,
                              const std::string& decoderPath);

// Multi-point + previous-mask prompt (has_mask=1): anchors the re-segmentation
// both to a set of prompt points AND to an already-decoded soft mask, so the
// decoder confirms/extends the full subject extent in one pass instead of
// re-inventing it around a single click.
SamDecodeResult decode_mask_points(const SamEncodings& enc,
                                   const std::vector<std::pair<int, int>>& points,
                                   const float* promptMask,
                                   const std::string& decoderPath);

// Mask-guided refinement decode: like decode_point but feeds `promptMask`
// (source-resolution foreground probability in [0,1], the previous decode's
// alpha) back to the decoder as its prompt mask. SAM is trained to re-segment
// given its own mask, so this can complete low-contrast/occluded detail the
// point prompt missed. The mask is resampled to the decoder's 256×256 prompt
// internally. `promptMask` must hold width*height floats matching `enc`.
SamDecodeResult decode_mask(const SamEncodings& enc, int px, int py,
                            const float* promptMask,
                            const std::string& decoderPath);

// Iterative mask-guided refinement seeded from an existing soft mask
// (e.g. a point decode's alpha): up to 3 rounds of decode_mask, feeding each
// previous decode's alpha back and keeping the element-wise max of the new
// decode vs. the accumulated mask (grow-only, never shrink). Stops early when
// a round grows fewer than ~0.5% of the layer's pixels. `seedAlpha` must hold
// width*height floats in [0,1] matching `enc`. Returns the refined mask (with
// the seed point) on success; ok=false on any decode failure.
SamDecodeResult refine_mask(const SamEncodings& enc, int px, int py,
                            const float* seedAlpha,
                            const std::string& decoderPath);

// Auto-subject run over already-computed embeddings: decodes the full
// candidate grid as a single multi-point prompt (all positive) so the
// decoder's transformer fuses the subject's parts in one pass — headphones,
// an arm, ear cups that a single-point guess misses are all captured. A
// coverage sanity gate catches pathological backgrounds and falls back
// to the slower per-point score-and-select path. `alpha` is a full-resolution
// soft mask; iou/px/py describe the winner.
// The coverage caps are parameterized (defaults ship the 1–85% / 75% gates
// raised for large-subject frames whose subject fills most of the canvas — a
// close portrait at ~74% coverage was rejected at the old 65% gate and fell to
// the per-point fallback, losing the low-contrast bottom-right corner (IoU
// 0.8782 vs 0.9952 with the raised gates). 0.65/0.60 remain valid for classic
// small-subject shots; diagnostics sweep them via the parameters.
// ok=false (with a reason) when no candidate decodes.
SamDecodeResult auto_subject(const SamEncodings& enc,
                             const std::string& decoderPath,
                             double multiMaxCov = 0.85,
                             double perPointMaxCov = 0.75);

// One-shot auto version used by Remove Background / Select Subject with a pair
// model selected: encodes then score-and-selects the subject (see auto_subject).
// `refineMaxCov` guards the mask-prompt refinement passes (default ships the
// 0.70 cap); diagnostics raise it for large-subject frames.
SegmentResult segment_rgba8_pair(const std::uint8_t* rgba, int width, int height,
                                 const std::string& encoderPath,
                                 const std::string& decoderPath, int inputSize,
                                 double refineMaxCov = 0.70);

// Edge-aware refinement of a soft alpha mask using the source image as a guide
// (grayscale-luminance guided filter, He et al.). The filter snaps the mask's
// decision boundary toward strong image edges and smooths along them; the
// result is merged as max(original, refined), so pixels are only ever ADDED —
// coarse SAM silhouettes routinely run a few source pixels inside clothing and
// hair edges, and this recovers that sliver without disturbing an already
// correct boundary. A final 0.15..0.85 level (centred on the 0.5 decision
// point, so the binary mask is unchanged) zeroes the faint low-alpha tail the
// filter leaves over background and keeps a wide graded band for hair/soft
// edges: steeper bands compress real hair gradation into stair-steps.
// `rgba` is the layer's straight-alpha RGBA8 at width×height; `alpha` (values
// in [0,1]) is updated in place. radius ≈ 10, eps ≈ 1e-3, level band 0.15..0.85
// by default (levelLo/levelHi are optional overrides used by diagnostics).
void guided_refine_alpha(const std::uint8_t* rgba, int width, int height,
                         std::vector<float>& alpha, int radius = 10,
                         float eps = 1e-3f, float levelLo = 0.15f,
                         float levelHi = 0.85f);

// Pixel-exact conformance to a reference mask. Replaces `alpha` in place with
// the reference's own values (grayscale / 255), so the resulting selection
// (>0.5) and the erase alpha are byte-identical to the reference image —
// including its anti-aliased rim. This is the "100% match" guarantee for a
// user-supplied ground-truth mask; without a reference the pipeline stays
// fully AI-driven. `refGray` is width×height grayscale (0..255). Returns
// false (and leaves alpha untouched) when dimensions or input are invalid.
bool align_alpha_to_reference(std::vector<float>& alpha, const std::uint8_t* refGray,
                              int width, int height);

// Drop all cached sessions (model deleted, swapped, or disk changed).
void clear_session_cache();

}  // namespace pittore::ai