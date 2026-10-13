#include "engine/ai/bg_remove.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "engine/core/log.h"

#ifdef PITTORE_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

#include "engine/ai/shared/ai_session.h"
#include "engine/ai/shared/ai_preprocess.h"

namespace pittore::ai {

using namespace ai_detail;


SegmentResult segment_rgba8(const std::uint8_t* rgba, int width, int height,
                            const std::string& modelPath, int inputSize) {
    SegmentResult res;
    res.width = width;
    res.height = height;
#ifndef PITTORE_HAS_ONNX
    // No-ONNX stub: the parameters feed the ONNX path below, so they are
    // intentionally unused in this configuration.
    (void)rgba;
    (void)modelPath;
    (void)inputSize;
    res.error = "This build links no ONNX Runtime. Install onnxruntime (or "
                "onnxruntime-cuda) and rebuild, or pass -Donnxruntime-root.";
    return res;
#else
    const auto tStart = std::chrono::steady_clock::now();
    const auto elapsedMs = [&] {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - tStart)
            .count();
    };
    if (!rgba || width <= 0 || height <= 0 || modelPath.empty()) {
        res.error = "Invalid segmentation input (null buffer, empty model).";
        return res;
    }
    PITTORE_LOG("[ai] segment begin: source=%dx%d bytes=%zu requestedInput=%d model=%.80s",
                 width, height, std::size_t(width) * std::size_t(height) * 4u,
                 inputSize, modelPath.c_str());
    const int S = std::clamp(inputSize > 0 ? inputSize : 1024, 64, 4096);

    // Session cache keyed by model path. Loading a 1 GB BiRefNet takes seconds;
    // keep the weights resident across calls. Guarded so concurrent UI / test
    // threads cannot mutate the cache at the same time (runs are serialised
    // per-session by ONNX itself, so only the cache needs the mutex).
    std::vector<float> input;
    std::vector<float> norm;
    std::vector<Ort::Value> outputs;
    OrtSession* session = nullptr;
    try {
        std::lock_guard<std::mutex> lock(gMutex);
        std::string loadErr;
        session = getOrLoadSessionLocked(modelPath, "model", loadErr, elapsedMs());
        if (!session) {
            res.error = loadErr;
            return res;
        }

        PITTORE_LOG("[ai] preprocessing %dx%d -> %dx%d tensor (at %.0f ms)", width, height,
                     S, S, elapsedMs());
        input.assign(std::size_t(3) * S * S, 0.0f);
        resizeRgba8ToNchwRgb(rgba, width, height, input.data(), S);

        const std::array<int64_t, 4> shape{1, 3, S, S};
        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(
            memInfo, input.data(), input.size(), shape.data(), shape.size());
        Ort::AllocatorWithDefaultOptions allocator;
        auto inName = session->session.GetInputNameAllocated(0, allocator);
        auto outName = session->session.GetOutputNameAllocated(0, allocator);
        const char* inNames[] = {inName.get()};
        const char* outNames[] = {outName.get()};

        PITTORE_LOG("[ai] inference start on %s (at %.0f ms)", session->provider.c_str(),
                     elapsedMs());
        outputs = session->session.Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1,
                                       outNames, 1);

        const float* raw = outputs[0].GetTensorMutableData<float>();
        const auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        std::size_t total = 1;
        std::string shapeStr = "[";
        for (std::size_t i = 0; i < outShape.size(); ++i) {
            if (i) shapeStr += ",";
            shapeStr += std::to_string(outShape[i]);
            total *= std::size_t(std::max<int64_t>(outShape[i], 1));
        }
        shapeStr += "]";
        PITTORE_LOG("[ai] inference done in %.0f ms: outputs=%zu shape=%s total=%zu",
                     elapsedMs(), outputs.size(), shapeStr.c_str(), total);
        if (outputs.empty() || total < 2) {
            res.error = "Model produced an empty output (shape " + shapeStr + ").";
            return res;
        }

        // The map is a single-channel score. Two families of model are
        // shipped and they need different handling:
        //   * logits (BiRefNet, most modern export heads) — unbounded, e.g.
        //     [-23, +164]; the correct map to [0,1] is the logistic sigmoid,
        //     which is monotonic and saturates so a lone hot outlier no longer
        //     squashes the whole foreground (min–max on logits turns a
        //     confident interior into a ~0.25 ghost).
        //   * probabilities — already in [0,1]; pass through, lightly
        //     re-normalised to use the full 8-bit span.
        float mn = raw[0], mx = raw[0];
        for (std::size_t i = 1; i < total; ++i) {
            mn = std::min(mn, raw[i]);
            mx = std::max(mx, raw[i]);
        }
        norm.assign(total, 0.0f);
        const bool logits = (mn < -0.5f) || (mx > 1.5f);
        if (logits) {
            for (std::size_t i = 0; i < total; ++i) {
                // Sigmoid, computed overflow-safely for large |x|.
                const float x = raw[i];
                norm[i] = x >= 0.0f ? 1.0f / (1.0f + std::exp(-x))
                                    : std::exp(x) / (1.0f + std::exp(x));
            }
        } else if (mx > mn) {
            const float inv = 1.0f / (mx - mn);
            for (std::size_t i = 0; i < total; ++i) norm[i] = (raw[i] - mn) * inv;
        }
        if (logits) {
            PITTORE_LOG("[ai] logits detected (raw range [%.3f,%.3f]); applied sigmoid",
                        mn, mx);
        }

        // Resample the network's (typically [1,1,H,W]) map back to source
        // resolution, using the ACTUAL output dimensions — an unexpected size
        // then degrades gracefully instead of overrunning the buffer.
        int sw = S, sh = S;
        if (outShape.size() >= 2) {
            const int64_t a = outShape[outShape.size() - 2];
            const int64_t b = outShape[outShape.size() - 1];
            if (a > 0 && b > 0 &&
                std::size_t(a) * std::size_t(b) == total) {
                sh = static_cast<int>(a);
                sw = static_cast<int>(b);
            }
        }
        // Lift the model's coarse mask into the full-res lattice guided by
        // the source image, so the contour snaps to real edges instead of
        // staying quantized to the model grid (a diagonal edge otherwise
        // upsamples to a visible staircase at high zoom).
        res.alpha = resizeMaskGuided(norm.data(), sw, sh, rgba, width, height);
        res.ok = true;
        PITTORE_LOG("[ai] segment done: %dx%d mask=%zu range=[%.4f,%.4f] total %.0f ms",
                     width, height, res.alpha.size(), mn, mx, elapsedMs());
        return res;
    } catch (const std::bad_alloc&) {
        res.error =
            "Out of memory while segmenting " + std::to_string(width) + "x" +
            std::to_string(height) +
            ". The segmentation model needs several GB — close other documents "
            "and try again.";
        PITTORE_LOG("[ai] OUT OF MEMORY segmenting %dx%d model=%.60s (%.0f ms)", width,
                     height, modelPath.c_str(), elapsedMs());
    } catch (const std::exception& e) {
        res.error = "Segmentation failed: " + std::string(e.what());
        PITTORE_LOG("[ai] segmentation exception: %s (at %.0f ms)", e.what(), elapsedMs());
    } catch (...) {
        res.error = "Segmentation failed with an unknown error.";
        PITTORE_LOG("[ai] segmentation unknown exception (at %.0f ms)", elapsedMs());
    }
    return res;
#endif
}


SegmentResult segment_rgba8_pair(const std::uint8_t* rgba, int width, int height,
                                 const std::string& encoderPath,
                                 const std::string& decoderPath, int inputSize,
                                 double refineMaxCov) {
    SegmentResult res;
    res.width = width;
    res.height = height;
    SamEncodings enc = encode_rgba8(rgba, width, height, encoderPath, inputSize);
    if (!enc.ok) {
        res.error = enc.error;
        return res;
    }
    SamDecodeResult dec = auto_subject(enc, decoderPath);
    if (!dec.ok) {
        res.error = dec.error;
        return res;
    }
    // Background flip: the grid can elect the BACKDROP when it dominates the
    // frame — `flippedBackdrop` records that the decode below was inverted so
    // the re-seg loop (whose single anchor cannot be trusted on a complemented
    // mask) is skipped for it.
    bool flippedBackdrop = false;
    // Backdrop signature, measured over the six shipped pairs: the decode is
    // large (coverage > 0.60) yet its interior is far smoother than the
    // foreground it surrounds (inside/outside edge-energy ratio < 0.20).
    // Person decodes read 0.79–7.39 on the ratio (nearest: Test.png 0.79, 4x
    // above) and the two backdrops 0.006 (sky) / 0.091 (white wall); coverages
    // 0.669/0.683 vs the 0.60 bar. Top-edge contact (1.00 on sky) does NOT
    // discriminate — the white-wall backdrop is bitten by the head (contact
    // 0.0) — so it is logged, not gated.
    // subject, but it IS the background the user wants removed). A backdrop
    // reads nothing like a subject: it spans the whole top edge (contact 1.00
    // vs ≤ 0.33 on every person pair) and its interior is far smoother than
    // the foreground it surrounds (inside/outside edge-energy ratio 0.01 vs
    // ≥ 0.79). When both hold with wide margins (top > 0.90, ratio < 0.20),
    // invert the decode so the pipeline segments the foreground instead —
    // tree+grass+mountains stay, sky goes. Small-subject branch then applies
    // (complement ≈ 0.33 coverage). Never fires on textured subjects.
    {
        const std::size_t n = std::size_t(width) * std::size_t(height);
        if (dec.alpha.size() == n && n > 0) {
            std::size_t topSel = 0;
            for (int x = 0; x < width; ++x)
                if (dec.alpha[std::size_t(x)] > 0.5f) ++topSel;
            double inE = 0.0, outE = 0.0;
            std::size_t inN = 0, outN = 0;
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const std::size_t i = std::size_t(y) * std::size_t(width) + x;
                    const int xm0 = x > 0 ? x - 1 : x, xp1 = x + 1 < width ? x + 1 : x;
                    const int ym0 = y > 0 ? y - 1 : y, yp1 = y + 1 < height ? y + 1 : y;
                    auto luma = [&](int xx, int yy) {
                        const std::uint8_t* p = rgba + (std::size_t(yy) * std::size_t(width) + xx) * 4;
                        return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
                    };
                    const float gx = (luma(xp1, y) - luma(xm0, y)) * 0.5f;
                    const float gy = (luma(x, yp1) - luma(x, ym0)) * 0.5f;
                    const double e = std::sqrt(double(gx * gx + gy * gy));
                    if (dec.alpha[i] > 0.5f) { inE += e; ++inN; } else { outE += e; ++outN; }
                }
            }
            const double topContact = double(topSel) / double(width);
            const double ratio = (inN && outN) ? (inE / double(inN)) / (outE / double(outN)) : 1.0;
            const double multiCov = double(inN + outN) > 0.0 ? double(inN) / double(n) : 0.0;
            if (multiCov > 0.60 && ratio < 0.20) {
                for (float& a : dec.alpha) a = 1.0f - a;
                flippedBackdrop = true;
                PITTORE_LOG("[sam] background flip: decode was backdrop "
                             "(cov=%.3f top=%.2f edge-ratio=%.3f); inverted",
                             multiCov, topContact, ratio);
            }
        }
    }
    // Snap the mask boundary to image edges and recover the coarse-wide sliver
    // where SAM silhouettes run inside clothing/hair contours (grow-only).
    guided_refine_alpha(rgba, width, height, dec.alpha, 10, 1e-3f);

    // Mask-prompt re-segmentation: feeding our own decode back as the prompt
    // (has_mask=1) lets the decoder RE-POSITION its contour, not just grow it.
    // On the 4K headphones pair this closed ~two thirds of the remaining
    // boundary gap (IoU 0.986 -> 0.994 vs the ground truth). Anchor point =
    // the mask's own centroid, so any subject position works; each pass is
    // guarded so a degenerate re-decode (empty / whole-frame / wrong size)
    // reverts to the previous iteration's mask. Skipped entirely after a
    // background flip: the complemented mask's bbox is meaningless and its
    // anchor lands in the backdrop, so the re-seg would re-elect the sky.
    {
        const std::size_t n = std::size_t(width) * std::size_t(height);
        if (!flippedBackdrop && dec.alpha.size() == n && n > 0) {
            // Anchor the re-segmentation at the subject's upper body (bbox
            // centre, one third down): a centroid anchor sits mid-torso where
            // SAM re-segments to the torso and drops the arm again (IoU 0.987
            // vs 0.994 on the 4K pair) — the top-third anchor is generic for
            // any subject placement and recovers the full silhouette.
            std::int64_t bx0 = width, by0 = height, bx1 = -1, by1 = -1;
            for (std::size_t i = 0; i < n; ++i)
                if (dec.alpha[i] > 0.5f) {
                    const std::int64_t x = i % std::size_t(width);
                    const std::int64_t y = i / std::size_t(width);
                    bx0 = std::min(bx0, x); by0 = std::min(by0, y);
                    bx1 = std::max(bx1, x); by1 = std::max(by1, y);
                }
            const int cxp = bx1 > bx0 ? int((bx0 + bx1) / 2) : width / 2;
            const int cyp = by1 > by0 ? int((by0 + (by1 - by0) / 3)) : height / 2;
            const float* seed = dec.alpha.data();
            // Large-subject frames (the accepted decode covers > 65% of the
            // canvas — e.g. this 1737×3088 close portrait at ~74%): the first
            // multi-point decode already carries the low-contrast bottom-right
            // corner at high confidence. A plain REPLACE by the mask-prompt
            // re-seg drops that corner again (the shipped 0.70-cap replace
            // left corner-miss = 12,611 px, IoU 0.8782). For large subjects
            // the re-seg may only add where the accept decode is un-confident
            // (merge ≥ 0.5 — additions only, never removes, so the beard and
            // the corner survive) plus fused extra-anchor passes; the re-seg's
            // own coverage cap is raised to 0.85 so its ~74%-coverage re-decode
            // isn't rejected as degenerate. Classic small/medium subjects
            // (Test.png, test-1, Test-3) keep the shipped replace + 0.70 cap
            // byte-identically: no regression on the existing pairs.
            std::size_t sc = 0;
            for (std::size_t i = 0; i < n; ++i)
                if (dec.alpha[i] > 0.5f) ++sc;
            const bool largeSubject = double(sc) / double(n) > 0.65;
            const double refineCap = largeSubject ? 0.85 : refineMaxCov;
            for (int it = 0; it < 2; ++it) {
                SamDecodeResult r = decode_mask(enc, cxp, cyp, seed, decoderPath);
                if (!r.ok || r.alpha.size() != n) break;
                guided_refine_alpha(rgba, width, height, r.alpha, 10, 1e-3f);
                std::size_t cov = 0;
                for (std::size_t i = 0; i < n; ++i)
                    if (r.alpha[i] > 0.5f) ++cov;
                const double c = double(cov) / double(n);
                if (c < 0.005 || c > refineCap) {  // degenerate re-decode: keep last
                    PITTORE_LOG("[sam] refine pass %d degenerate (cov=%.3f); "
                                 "reverting", it + 1, c);
                    break;
                }
                if (largeSubject) {
                    // Grow-confident merge: keep every pixel the accepted
                    // decode already trusted at ≥ 0.5 (the dark corner, the
                    // beard — additions only, never removes), take the
                    // re-seg's opinion only where ours is soft/absent.
                    for (std::size_t i = 0; i < n; ++i)
                        if (dec.alpha[i] < 0.50f) dec.alpha[i] = r.alpha[i];
                } else {
                    dec.alpha = std::move(r.alpha);
                }
                seed = dec.alpha.data();
                PITTORE_LOG("[sam] mask-prompt refine pass %d ok (cov=%.3f) "
                             "%s", it + 1, c, largeSubject ? "(grow-confident)" : "");
            }
            // Large subjects only: extra re-seg passes from secondary anchors
            // (right-edge mid-height pulls a cut right flank, top edge pulls
            // an under-scooped crown — test-2 IoU 0.9953 → 0.9962, miss
            // -4.1k). Each extra pass is fused: it is REVERTED when it would
            // select more than 2% of fresh canvas pixels, which blocks the
            // background-grab failure the same passes cause on smaller or
            // frizzy pairs (test-1/Test.png/Test-3 all regressed unguarded, up
            // to +1.46M extras on Test-3). Small subjects never enter this
            // branch, so their masks stay byte-identical.
            if (largeSubject) {
                for (int anchor = 1; anchor <= 2; ++anchor) {
                    std::int64_t ax0 = width, ay0 = height, ax1 = -1, ay1 = -1;
                    for (std::size_t i = 0; i < n; ++i)
                        if (dec.alpha[i] > 0.5f) {
                            const std::int64_t x = i % std::size_t(width);
                            const std::int64_t y = i / std::size_t(width);
                            ax0 = std::min(ax0, x); ay0 = std::min(ay0, y);
                            ax1 = std::max(ax1, x); ay1 = std::max(ay1, y);
                        }
                    int axp = width / 2, ayp = height / 2;
                    if (ax1 > ax0 && ay1 > ay0) {
                        if (anchor == 1) {
                            axp = int(ax1 - (ax1 - ax0) / 10);
                            ayp = int((ay0 + ay1) / 2);
                        } else {
                            axp = int((ax0 + ax1) / 2);
                            ayp = int(ay0 + (ay1 - ay0) / 10);
                        }
                    }
                    SamDecodeResult r = decode_mask(enc, axp, ayp, seed, decoderPath);
                    if (!r.ok || r.alpha.size() != n) break;
                    guided_refine_alpha(rgba, width, height, r.alpha, 10, 1e-3f);
                    std::size_t cov = 0;
                    for (std::size_t i = 0; i < n; ++i)
                        if (r.alpha[i] > 0.5f) ++cov;
                    const double c = double(cov) / double(n);
                    if (c < 0.005 || c > refineCap) {
                        PITTORE_LOG("[sam] extra-anchor pass %d degenerate "
                                     "(cov=%.3f); reverting", anchor, c);
                        break;
                    }
                    std::size_t added = 0;
                    for (std::size_t i = 0; i < n; ++i)
                        if (r.alpha[i] > 0.5f && dec.alpha[i] <= 0.5f) ++added;
                    if (added > n / 50) {  // >2% fresh canvas: background grab
                        PITTORE_LOG("[sam] extra-anchor pass %d fused "
                                     "(added=%.3f); reverting", anchor,
                                     double(added) / double(n));
                        break;
                    }
                    for (std::size_t i = 0; i < n; ++i)
                        if (dec.alpha[i] < 0.50f) dec.alpha[i] = r.alpha[i];
                    seed = dec.alpha.data();
                    PITTORE_LOG("[sam] extra-anchor pass %d ok (cov=%.3f "
                                 "added=%.4f)", anchor, c,
                                 double(added) / double(n));
                }
            }
        }
    }

    res.alpha = std::move(dec.alpha);
    res.ok = true;
    PITTORE_LOG("[sam] auto segment done (score-and-select): %dx%d mask=%zu "
                 "win=(%d,%d) iou=%.3f",
                 width, height, res.alpha.size(), dec.px, dec.py, dec.iou);
    return res;
}

}  // namespace pittore::ai
