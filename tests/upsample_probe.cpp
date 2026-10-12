// Guided vs plain mask upsample probe.
//
// Reproduces the hair-staircase artifact: a synthetic full-res scene with a
// curved bright/dark boundary, a ground-truth matte, and a coarse (model
// resolution) version of that matte. Upsampling the coarse matte back to full
// resolution with plain bilinear keeps the 0.5-crossing on the coarse lattice
// (a staircase at high zoom); guided / joint-bilateral upsampling re-places it
// on the real image edge. Reports RMSE vs ground truth and 50%-contour
// localization error for both, for a soft (BiRefNet-like) and a near-binary
// (SAM-like) coarse mask.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "engine/ai/shared/ai_preprocess.h"

using pittore::ai::ai_detail::resizeMaskGuided;
using pittore::ai::ai_detail::resizeMaskToSize;

namespace {

constexpr int W = 640;
constexpr int H = 640;
constexpr int R = 8;  // full-res / model-res ratio -> 80x80 coarse
constexpr double kPi = 3.14159265358979323846;

// Curved boundary x = edge(y): the "hair silhouette".
double edgeX(int y) {
    return W * 0.5 + 90.0 * std::sin(y * (2.5 * kPi / H)) +
           25.0 * std::sin(y * (7.0 * kPi / H));
}

// Ground-truth matte: 1 subject (left of boundary), 0 background, 1px AA.
float gtAlpha(int x, int y) {
    const double d = x - edgeX(y);  // >0 = background side
    const double aa = 1.0;
    return static_cast<float>(std::clamp((aa - d) / (2.0 * aa), 0.0, 1.0));
}

// Scene: warm bright subject vs cool dark background + mild texture.
void fillGuide(std::vector<std::uint8_t>& rgba) {
    rgba.resize(std::size_t(W) * H * 4u);
    // Deterministic pseudo-noise so the boundary is crisp but interiors
    // are not perfectly flat (realistic for the range kernel).
    auto noise = [](int x, int y) {
        unsigned h = unsigned(x) * 374761393u + unsigned(y) * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return int((h >> 16) & 15) - 7;  // [-7, 7]
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const bool subj = x < edgeX(y);
            const int n = noise(x, y);
            std::uint8_t* p = &rgba[(std::size_t(y) * W + x) * 4u];
            if (subj) {
                p[0] = std::clamp(210 + n, 0, 255);
                p[1] = std::clamp(175 + n, 0, 255);
                p[2] = std::clamp(140 + n, 0, 255);
            } else {
                p[0] = std::clamp(70 + n, 0, 255);
                p[1] = std::clamp(95 + n, 0, 255);
                p[2] = std::clamp(130 + n, 0, 255);
            }
            p[3] = 255;
        }
    }
}

// Box-downsample a full-res float image to (W/R x H/R).
std::vector<float> downBox(const std::vector<float>& src) {
    const int cw = W / R, ch = H / R;
    std::vector<float> out(std::size_t(cw) * ch);
    for (int cy = 0; cy < ch; ++cy)
        for (int cx = 0; cx < cw; ++cx) {
            double s = 0.0;
            for (int dy = 0; dy < R; ++dy)
                for (int dx = 0; dx < R; ++dx)
                    s += src[std::size_t(cy * R + dy) * W + (cx * R + dx)];
            out[std::size_t(cy) * cw + cx] = static_cast<float>(s / (R * R));
        }
    return out;
}

// 50%-contour localization: per row, the interpolated x where `m` crosses
// 0.5, compared to the analytic boundary. Returns mean |error|.
double edgeError(const std::vector<float>& m) {
    double sum = 0.0;
    int rows = 0;
    for (int y = 0; y < H; ++y) {
        const double gt = edgeX(y);
        if (gt < 2 || gt > W - 3) continue;
        // Search a window around the true edge for the 0.5 crossing.
        int lo = std::max(0, int(gt) - 4 * R);
        int hi = std::min(W - 1, int(gt) + 4 * R);
        double cross = -1.0;
        for (int x = lo; x < hi; ++x) {
            const float a = m[std::size_t(y) * W + x];
            const float b = m[std::size_t(y) * W + x + 1];
            if ((a < 0.5f && b >= 0.5f) || (a >= 0.5f && b < 0.5f)) {
                cross = x + (0.5f - a) / (b - a);
                break;
            }
        }
        if (cross >= 0.0) {
            sum += std::fabs(cross - gt);
            ++rows;
        }
    }
    return rows ? sum / rows : -1.0;
}

// Alpha transition width: mean number of pixels per row whose alpha lies in
// (0.1, 0.9) — i.e. how many pixels the edge ramp spans. Plain bilinear of a
// ratio-8 mask spreads the transition over ~8 px (a soft, mushy edge that
// reads as blocky once thresholded to a selection); joint-bilateral upsampling
// guided by the image collapses it onto the true edge in ~1–2 px.
double transWidth(const std::vector<float>& m) {
    double sum = 0.0;
    int rows = 0;
    for (int y = 0; y < H; ++y) {
        const double gt = edgeX(y);
        if (gt < 2 || gt > W - 3) continue;
        int count = 0;
        for (int x = std::max(0, int(gt) - 4 * R); x < std::min(W, int(gt) + 4 * R);
             ++x)
            if (m[std::size_t(y) * W + x] > 0.1f &&
                m[std::size_t(y) * W + x] < 0.9f)
                ++count;
        sum += count;
        ++rows;
    }
    return rows ? sum / rows : -1.0;
}

void report(const char* name, const std::vector<float>& m,
            const std::vector<float>& gt) {
    double se = 0.0, bse = 0.0;
    std::size_t band = 0;
    for (std::size_t i = 0; i < gt.size(); ++i) {
        const double e = m[i] - gt[i];
        se += e * e;
        if (gt[i] > 0.05f && gt[i] < 0.95f) {
            bse += e * e;
            ++band;
        }
    }
    const double rmse = std::sqrt(se / gt.size());
    const double brmse = std::sqrt(bse / std::max<std::size_t>(band, 1));
    const double ee = edgeError(m);
    const double tw = transWidth(m);
    std::printf("  %-10s rmse=%.4f  edge-err=%.3fpx  width=%.2fpx  band-rmse=%.4f\n",
                name, rmse, ee, tw, brmse);
}

}  // namespace

int main() {
    std::vector<std::uint8_t> guide;
    fillGuide(guide);

    std::vector<float> gt(std::size_t(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            gt[std::size_t(y) * W + x] = gtAlpha(x, y);

    const int cw = W / R, ch = H / R;
    const std::vector<float> softCoarse = downBox(gt);

    // SAM-like near-binary coarse mask (steep, still slightly soft).
    std::vector<float> hardCoarse(softCoarse.size());
    for (std::size_t i = 0; i < softCoarse.size(); ++i)
        hardCoarse[i] =
            std::clamp((softCoarse[i] - 0.5f) * 6.0f + 0.5f, 0.0f, 1.0f);

    for (int variant = 0; variant < 2; ++variant) {
        const std::vector<float>& coarse =
            variant == 0 ? softCoarse : hardCoarse;
        const char* vname = variant == 0 ? "soft (BiRefNet-like)"
                                         : "hard (SAM-like)";
        std::printf("%s coarse %dx%d -> full %dx%d (ratio %d)\n", vname, cw,
                    ch, W, H, R);

        std::vector<float> bil = resizeMaskToSize(coarse.data(), cw, ch, W, H);
        std::vector<float> grd =
            resizeMaskGuided(coarse.data(), cw, ch, guide.data(), W, H);
        if (bil.size() != gt.size() || grd.size() != gt.size()) {
            std::printf("  FAILED: wrong output size (bil=%zu grd=%zu)\n",
                        bil.size(), grd.size());
            return 1;
        }
        report("bilinear", bil, gt);
        report("guided", grd, gt);

        const double b_ee = edgeError(bil), g_ee = edgeError(grd);
        const double b_tw = transWidth(bil), g_tw = transWidth(grd);
        double b_se = 0.0, g_se = 0.0;
        for (std::size_t i = 0; i < gt.size(); ++i) {
            const double be = bil[i] - gt[i], ge = grd[i] - gt[i];
            b_se += be * be;
            g_se += ge * ge;
        }
        // Guided must localize the contour better, collapse the transition to
        // a sharper edge, and reduce overall error. band-rmse is reported but
        // not gated: it rewards reproducing the GT's deliberately wide AA ramp,
        // which a legitimately sharper guided edge is allowed to beat.
        const bool ok = g_ee < b_ee && g_tw < b_tw && g_se < b_se;
        std::printf("  -> %s: edge %.3f<%.3f  width %.2f<%.2f  rmse %.4f<%.4f\n\n",
                    ok ? "guided SHARPER + more accurate" : "FAIL", g_ee, b_ee,
                    g_tw, b_tw, std::sqrt(g_se / gt.size()),
                    std::sqrt(b_se / gt.size()));
        if (!ok) return 2;
    }
    std::printf("PASS: guided upsampling gives a sharper, more accurately-placed "
                "edge than bilinear on both soft and hard coarse masks.\n");
    return 0;
}
