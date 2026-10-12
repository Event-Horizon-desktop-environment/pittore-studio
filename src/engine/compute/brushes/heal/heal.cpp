#include "engine/compute/brushes/heal/heal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "engine/core/log.h"
#include "engine/core/parallel.h"
#include "engine/core/pixel.h"

namespace pittore::compute {

bool spot_heal_host(RGBAf* dst, std::uint32_t w, std::uint32_t h, float cx,
                     float cy, float radius, float hardness,
                     const RGBAf* src, HealType type, int diffusion,
                     int* bbox, const SelectionMask* selection) {
    if (!dst || !src || w == 0 || h == 0 || radius <= 0.0f) return false;
    const int wi = static_cast<int>(w), hi = static_cast<int>(h);
    const float hard = std::clamp(hardness, 0.0f, 1.0f);
    const int diff = std::clamp(diffusion, 1, 7);
    const float blurR = static_cast<float>(diff * 2);  // 2..14px split radius

    // Brush footprint (same bbox/hardness kernel as paint_dab_host).
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int y1 = std::min(hi - 1, static_cast<int>(std::ceil(cy + radius)));
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int x1 = std::min(wi - 1, static_cast<int>(std::ceil(cx + radius)));
    if (x1 < x0 || y1 < y0) return false;
    // Stage clock: the gated log line at the bottom reports where the ms
    // went (ring / donor / refine / apply) — the big-brush cost autopsy.
    const auto t0 = std::chrono::steady_clock::now();
    const float inv_r = 1.0f / radius;
    const float soft_span = std::max(1.0f - hard, 1e-4f);
    const auto brush_cov = [&](int x, int y) {
        const float dx = static_cast<float>(x) + 0.5f - cx;
        const float dy = static_cast<float>(y) + 0.5f - cy;
        const float t = std::sqrt(dx * dx + dy * dy) * inv_r;
        if (t >= 1.0f) return 0.0f;
        if (t > 1.0f - soft_span) return (1.0f - t) / soft_span;
        return 1.0f;
    };
    const auto srcAt = [&](int x, int y) -> const RGBAf& {
        return src[std::size_t(y) * w + x];
    };

    // Surrounding ring (the hole's own rim out to rim + max(brush, 6px)).
    // Its mean seeds the low-frequency inpaint; its texture feeds scoring
    // (Proximity/Content-Aware) and the detail pool (Create Texture).
    const float ringOut = radius + std::max(6.0f, radius);
    double rr = 0, rg = 0, rb = 0;
    int rn = 0;
    const int rby0 = std::max(0, static_cast<int>(std::floor(cy - ringOut)));
    const int rby1 = std::min(hi - 1, static_cast<int>(std::ceil(cy + ringOut)));
    const int rbx0 = std::max(0, static_cast<int>(std::floor(cx - ringOut)));
    const int rbx1 = std::min(wi - 1, static_cast<int>(std::ceil(cx + ringOut)));
    for (int y = rby0; y <= rby1; ++y) {
        for (int x = rbx0; x <= rbx1; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float dy = static_cast<float>(y) + 0.5f - cy;
            const float t = std::sqrt(dx * dx + dy * dy) * inv_r;
            if (t <= 1.0f || t * radius > ringOut) continue;
            const RGBAf& c = srcAt(x, y);
            if (c.a <= 0.01f) continue;
            rr += c.r;
            rg += c.g;
            rb += c.b;
            ++rn;
        }
    }
    if (rn == 0) return false;  // nothing to heal from (off-layer dab)
    const RGBAf ringMean{static_cast<float>(rr / rn),
                         static_cast<float>(rg / rn),
                         static_cast<float>(rb / rn), 1.0f};
    const auto tRing = std::chrono::steady_clock::now();

    // Donor scoring over a dense annulus (hole rim → scoreOuter), strided to
    // a bounded texel budget with early termination: unlike a lone perimeter
    // ring — which can sit entirely between the repeats of a periodic
    // pattern and vote blind — the filled annulus sees repeats, so the
    // phase-correct donor wins instead of scan order. Pairs whose donor side
    // falls back inside the hole (the blemish itself) are skipped. `best`
    // aborts hopeless candidates early (running mean already worse).
    const float scoreOuter =
        std::min(std::max(4.0f * radius, 32.0f), 256.0f);
    const float scoreArea =
        3.14159265f * (scoreOuter * scoreOuter - radius * radius);
    const int sstride =
        std::max(2, static_cast<int>(std::sqrt(scoreArea / 1200.0f)));
    const auto annulusScore = [&](float ox, float oy, float best) {
        double ssd = 0;
        int n = 0;
        const int ex0 = static_cast<int>(std::floor(cx - scoreOuter));
        const int ex1 = static_cast<int>(std::ceil(cx + scoreOuter));
        const int ey0 = static_cast<int>(std::floor(cy - scoreOuter));
        const int ey1 = static_cast<int>(std::ceil(cy + scoreOuter));
        for (int hy = ey0; hy <= ey1; hy += sstride)
            for (int hx = ex0; hx <= ex1; hx += sstride) {
                const float rdx = static_cast<float>(hx) + 0.5f - cx;
                const float rdy = static_cast<float>(hy) + 0.5f - cy;
                const float dd = std::sqrt(rdx * rdx + rdy * rdy);
                if (dd <= radius || dd > scoreOuter) continue;
                const int dx = static_cast<int>(std::floor(hx + ox));
                const int dy = static_cast<int>(std::floor(hy + oy));
                if (hx < 0 || hy < 0 || hx >= wi || hy >= hi) continue;
                if (dx < 0 || dy < 0 || dx >= wi || dy >= hi) continue;
                // Donor side inside the hole would score the blemish: skip.
                const float qdx = static_cast<float>(dx) + 0.5f - cx;
                const float qdy = static_cast<float>(dy) + 0.5f - cy;
                if (qdx * qdx + qdy * qdy <= radius * radius) continue;
                const RGBAf& a = srcAt(hx, hy);
                const RGBAf& b = srcAt(dx, dy);
                if (a.a <= 0.01f || b.a <= 0.01f) continue;
                const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
                ssd += dr * dr + dg * dg + db * db;
                ++n;
                if (n >= 12 && ssd / n > best)
                    return std::numeric_limits<float>::infinity();
            }
        if (n < 12) return std::numeric_limits<float>::infinity();
        return static_cast<float>(ssd / n);
    };
    const auto donorDiscOk = [&](float ox, float oy) {
        // Donor disc (radius r) must sit inside the image and clear the hole
        // (centres ≥ 2r apart), and its centre must be visible.
        if (std::hypot(ox, oy) < 2.0f * radius) return false;
        const int dx = static_cast<int>(std::floor(cx + ox));
        const int dy = static_cast<int>(std::floor(cy + oy));
        if (dx < 0 || dy < 0 || dx >= wi || dy >= hi) return false;
        if (dx - radius < 0 || dy - radius < 0 || dx + radius >= wi ||
            dy + radius >= hi)
            return false;
        return srcAt(dx, dy).a > 0.01f;
    };

    // Donor offset (hole → donor), always integer: the transfer
    // nearest-samples, so fractional offsets only misalign periodic content
    // without adding any resampling benefit. Proximity spirals nearby;
    // Content-Aware scans outward ring by ring (nearest first: ties prefer
    // nearby content, and good donors abort the search early via the
    // running best). Candidate counts stay bounded (~1k) at any brush size.
    float bestOx = 0, bestOy = 0;
    float globalBest = std::numeric_limits<float>::infinity();
    bool haveDonor = (type == HealType::CreateTexture);
    if (type == HealType::Proximity) {
        float best = std::numeric_limits<float>::infinity();
        for (float rr2 : {2.0f, 2.75f, 3.5f}) {
            for (int k = 0; k < 8; ++k) {
                const float a = k * 3.14159265f / 4.0f;
                const float ox =
                    std::round(std::cos(a) * radius * rr2);
                const float oy =
                    std::round(std::sin(a) * radius * rr2);
                if (!donorDiscOk(ox, oy)) continue;
                const float s = annulusScore(ox, oy, best);
                if (s < best) {
                    best = s;
                    bestOx = ox;
                    bestOy = oy;
                }
            }
            if (best < 1e-6f) break;  // exact continuation found
        }
        haveDonor = best < std::numeric_limits<float>::infinity();
    } else if (type == HealType::ContentAware) {
        const int istep = std::clamp(static_cast<int>(radius / 6.0f), 1, 4);
        const int ihalf =
            std::clamp(static_cast<int>(4.0f * radius), 64, 256);
        static constexpr int kAngles = 24;
        float best = std::numeric_limits<float>::infinity();
        for (int rad = 0; rad <= ihalf; rad += istep) {
            for (int k = 0; k < kAngles; ++k) {
                const float a = k * 2.0f * 3.14159265f / kAngles;
                const float ox = std::round(std::cos(a) * rad);
                const float oy = std::round(std::sin(a) * rad);
                if (!donorDiscOk(ox, oy)) continue;
                const float s = annulusScore(ox, oy, best);
                if (s < best) {
                    best = s;
                    bestOx = ox;
                    bestOy = oy;
                }
            }
            if (best < 1e-6f) break;  // exact continuation found
        }
        globalBest = best;
        haveDonor = best < std::numeric_limits<float>::infinity();
    }
    if (!haveDonor) return false;
    std::fprintf(stderr, "[heal] type=%d donor=(%.1f,%.1f) gbest=%g\n",
                 static_cast<int>(type), bestOx, bestOy, globalBest);
    const auto tDonor = std::chrono::steady_clock::now();

    // Accuracy counters for the per-dab log line: lowFb counts blur windows
    // that could not see past the hole (fell back to the ring mean — the
    // flat-centre symptom on big brushes). Atomic: the apply loop below
    // runs in parallel and blurAt() may be called from any range.
    std::atomic<int> lowFb{0};

    // On-demand box blur that never sees the blemish: hole-disc texels are
    // skipped (count-normalised), so no snapshot/fill dance is needed — donor
    // reads provably land outside the disc (offsets clear it by ≥2r, shown
    // below), and only unmodified texels are ever sampled. Falls back to the
    // ring mean when the window holds nothing usable.
    const int br = std::max(1, static_cast<int>(blurR));
    const auto blurAt = [&](int qx, int qy) {
        double sr = 0, sg = 0, sb = 0;
        int n = 0;
        for (int y = qy - br; y <= qy + br; ++y) {
            if (y < 0 || y >= hi) continue;
            for (int x = qx - br; x <= qx + br; ++x) {
                if (x < 0 || x >= wi) continue;
                const float hx = static_cast<float>(x) + 0.5f - cx;
                const float hy = static_cast<float>(y) + 0.5f - cy;
                if (hx * hx + hy * hy < radius * radius) continue;
                const RGBAf& c = srcAt(x, y);
                if (c.a <= 0.01f) continue;
                sr += c.r;
                sg += c.g;
                sb += c.b;
                ++n;
            }
        }
        if (n == 0) {
            ++lowFb;
            return ringMean;
        }
        return RGBAf{static_cast<float>(sr / n), static_cast<float>(sg / n),
                     static_cast<float>(sb / n), 1.0f};
    };

    // Content-Aware per-pixel refinement (PatchMatch-lite): the global spiral
    // above nails the phase, but one offset cannot continue two features at
    // once (a junction needs different donors per side). Starting from the
    // global best, each hole texel propagates neighbours' offsets and probes
    // random ones, scored by patch SSD over known texels only. Two sweeps;
    // bounded work per dab.
    struct Off { float x, y; };
    std::vector<Off> offMap;
    // Offset-lattice stride: refinement solves every g-th texel of a big
    // hole and bilinear-fills the rest (PatchMatch work is per solved texel,
    // so g=2 quarters it; offsets vary smoothly over 2 px so the fill is
    // lossless in practice). Holes up to r=16 solve per texel, as before.
    const int g = radius > 16.0f ? 2 : 1;
    const int cw = (x1 - x0) / g + 1;
    const int ch = (y1 - y0) / g + 1;
    // Refinement runs only when the global match is inexact: an exact
    // annulus continuation needs no per-pixel help, and refining a flat
    // landscape would random-walk the phase away (regularisation by
    // construction).
    const bool refine =
        type == HealType::ContentAware && globalBest >= 1e-6f;
    // Stage marks for the log: the pyramid (coarse) level is the newest
    // big-brush cost, so it gets its own bucket beside `fine`.
    auto tPyrBeg = std::chrono::steady_clock::now();
    auto tPyrEnd = tPyrBeg;
    if (refine) {
        // Patch radius scales with the hole: refinement needs patches that
        // reach known texels past the rim (tiny patches deep inside a big
        // hole see nothing but blemish and can never improve). Capped at 10:
        // past that each extra ring costs every full patch eval ~17% more
        // while the rim it exists to match is already covered.
        const int prad = std::clamp(static_cast<int>(radius / 2.0f), 4, 10);
        // Donor centres need only differ by a patch radius (self-matching
        // would score a vacuous zero and freeze the map); the transfer
        // skips donor texels that fall back inside the hole per-texel, so
        // no disc clearance is needed. Patch must fit the image; centre
        // must be visible. Offsets are capped too, and the cap is what
        // makes the structure term below honest: every donor a valid
        // candidate can reach then sits inside the planes, so the gradient
        // term is applied to *all* candidates alike (a term silently
        // skipped for far donors would otherwise bias scoring toward them).
        const float kMaxOff = std::min(768.0f, std::max(96.0f, 8.0f * radius));
        const auto validNNF = [&](float ox, float oy, int rc) {
            if (std::fabs(ox) > kMaxOff || std::fabs(oy) > kMaxOff)
                return false;
            if (std::hypot(ox, oy) < rc) return false;
            const int dx = static_cast<int>(std::floor(cx + ox));
            const int dy = static_cast<int>(std::floor(cy + oy));
            if (dx - rc < 0 || dy - rc < 0 || dx + rc >= wi || dy + rc >= hi)
                return false;
            return srcAt(dx, dy).a > 0.01f;
        };
        // Structure planes: luma gradients over the dab box padded to the
        // farthest reachable donor (stride-capped so a huge brush cannot
        // blow the build cost). Patch scoring then adds gradient alignment
        // — guided matching — so edges and junctions continue across the
        // hole instead of being averaged into mush.
        const int stpad = static_cast<int>(kMaxOff + 2.0f * prad);
        const int stx0 = std::max(0, x0 - stpad);
        const int stx1 = std::min(wi - 1, x1 + stpad);
        const int sty0 = std::max(0, y0 - stpad);
        const int sty1 = std::min(hi - 1, y1 + stpad);
        const int ststr = std::max(1, (stx1 - stx0 + 512) / 512);
        const int stw = (stx1 - stx0) / ststr + 1;
        const int sth = (sty1 - sty0) / ststr + 1;
        std::vector<float> stGX(std::size_t(stw) * sth, 0.0f);
        std::vector<float> stGY(std::size_t(stw) * sth, 0.0f);
        const auto lumaAt = [&](int x, int y) {
            const RGBAf& c = srcAt(x, y);
            return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
        };
        core::parallel_for(static_cast<std::uint32_t>(sth), 64,
                           [&](std::uint32_t r0, std::uint32_t r1) {
            for (std::uint32_t iy = r0; iy < r1; ++iy) {
                const int y = sty0 + static_cast<int>(iy) * ststr;
                const int ya = std::max(0, y - ststr);
                const int yb = std::min(hi - 1, y + ststr);
                for (int ix = 0; ix < stw; ++ix) {
                    const int x = stx0 + ix * ststr;
                    const int xa = std::max(0, x - ststr);
                    const int xb = std::min(wi - 1, x + ststr);
                    stGX[std::size_t(iy) * stw + ix] = lumaAt(xb, y) - lumaAt(xa, y);
                    stGY[std::size_t(iy) * stw + ix] = lumaAt(x, yb) - lumaAt(x, ya);
                }
            }
        });
        const auto gradAt = [&](int x, int y, float& gx, float& gy) {
            if (x < stx0 || y < sty0) return false;
            const int ix = (x - stx0) / ststr;
            const int iy = (y - sty0) / ststr;
            if (ix >= stw || iy >= sth) return false;
            const std::size_t i = std::size_t(iy) * stw + ix;
            gx = stGX[i];
            gy = stGY[i];
            return true;
        };
        // Structure weight: how much luma-gradient alignment counts beside
        // colour SSD (guided matching). 0.5 keeps edges/junctions continuing
        // across the hole without letting a faint edge outweigh colour.
        const float stW = 0.5f;
        // Patch SSD between the hole patch (blemish texels skipped) and the
        // donor patch (ditto), normalised, plus gradient alignment from the
        // planes; +inf under 6 known pairs. (Center sampling means rim
        // patches legitimately hold few known texels; propagation carries
        // vetted offsets inward from there.) `best` aborts hopeless
        // candidates early. A patch that cannot reach outside the hole at
        // all returns +inf before touching memory: that is every texel
        // deeper than the patch radius inside a big hole, and scoring those
        // is pure waste — they inherit the rim's vetted offsets by
        // propagation instead (see the rank carried in the sweeps below).
        // `sm` is the sample stride, `cells` the half-size in samples.
        const auto patchSSD = [&](int hx, int hy, float ox, float oy,
                                  float best, int sm, int cells) {
            const float dhx = static_cast<float>(hx) + 0.5f - cx;
            const float dhy = static_cast<float>(hy) + 0.5f - cy;
            const float marg =
                radius - static_cast<float>(cells * sm) * 1.41421356f;
            if (marg > 0.0f && dhx * dhx + dhy * dhy <= marg * marg)
                return std::numeric_limits<float>::infinity();
            double ssd = 0;
            double gssd = 0;  // gradient term, own normalisation (see below)
            int gn = 0;
            int n = 0;
            for (int dy = -cells; dy <= cells; ++dy)
                for (int dx = -cells; dx <= cells; ++dx) {
                    const int px = hx + dx * sm, py = hy + dy * sm;
                    const float phx = static_cast<float>(px) + 0.5f - cx;
                    const float phy = static_cast<float>(py) + 0.5f - cy;
                    if (phx * phx + phy * phy < radius * radius) continue;
                    if (px < 0 || py < 0 || px >= wi || py >= hi) continue;
                    const int qx = static_cast<int>(std::floor(px + ox));
                    const int qy = static_cast<int>(std::floor(py + oy));
                    if (qx < 0 || qy < 0 || qx >= wi || qy >= hi) continue;
                    const float qhx = static_cast<float>(qx) + 0.5f - cx;
                    const float qhy = static_cast<float>(qy) + 0.5f - cy;
                    if (qhx * qhx + qhy * qhy < radius * radius) continue;
                    const RGBAf& a = srcAt(px, py);
                    const RGBAf& b = srcAt(qx, qy);
                    if (a.a <= 0.01f || b.a <= 0.01f) continue;
                    const float dr = a.r - b.r, dg = a.g - b.g,
                                db = a.b - b.b;
                    ssd += dr * dr + dg * dg + db * db;
                    // Structure term on every 2nd sample per axis: gradient
                    // alignment is a smooth signal, so a quarter of the patch
                    // carries it just as well — while each gradAt walks a
                    // padded plane that dwarfs the colour reads, so scoring
                    // all of them cost more than the whole colour score.
                    if (stW > 0.0f && (dx & 1) == 0 && (dy & 1) == 0) {
                        float lgv = 0, lgw = 0, qgv = 0, qgw = 0;
                        if (gradAt(px, py, lgv, lgw) &&
                            gradAt(qx, qy, qgv, qgw)) {
                            const float egx = lgv - qgv, egy = lgw - qgw;
                            gssd += egx * egx + egy * egy;
                            ++gn;
                        }
                    }
                    ++n;
                    if (n >= 6 &&
                        ssd / n + stW * (gn > 0 ? gssd / gn : 0.0) > best)
                        return std::numeric_limits<float>::infinity();
                }
            if (n < 6) return std::numeric_limits<float>::infinity();
            return static_cast<float>(ssd / n +
                                      stW * (gn > 0 ? gssd / gn : 0.0));
        };
        // One PatchMatch level over a (step) lattice: parallel init (every
        // lattice point draws from its own hashed stream, so the split
        // cannot change results), then serial scan sweeps. The sweeps stay
        // single-threaded on purpose: the raster chain that floods vetted
        // offsets across the whole hole in one pass *is* the algorithm —
        // partitioning it would cut propagation to one cell per pass.
        struct Lattice {
            std::vector<Off> cells;
            // Score behind each offset: the cell's own patch score, or — for
            // cells too deep inside the hole to have one — the rim score it
            // inherited from a verified neighbour. Lets provenance flood to
            // the centre where scoring cannot.
            std::vector<float> rank;
            // The cell's *own* score for its current offset (inf when its
            // patch can't reach known texel). Cached because it depends only
            // on (cell, offset) — both known — so recomputing the full patch
            // on every pass returned the identical number.
            std::vector<float> own;
            int lw = 0, lh = 0, stride = 1;
        };
        const std::uint32_t rngBase =
            ((static_cast<std::uint32_t>(cx * 1274126177.0f) ^
              static_cast<std::uint32_t>(cy * 281828183.0f)) ^
             static_cast<std::uint32_t>(w * 613566757u)) |
            1u;
        const auto cellSeed = [&](int x, int y, std::uint32_t salt) {
            std::uint32_t v = rngBase ^ salt ^
                              (static_cast<std::uint32_t>(x) * 0x9E3779B1u) ^
                              (static_cast<std::uint32_t>(y) * 0x85EBCA77u);
            v ^= v >> 16;
            v *= 0x7FEB352Du;
            v ^= v >> 15;
            v *= 0x846CA68Bu;
            v ^= v >> 16;
            return v | 1u;
        };
        // Random initial field (PatchMatch §3): uniform over a wide window so
        // some texel starts near every plausible answer; propagation then
        // floods coherent answers across neighbourhoods.
        const int initHalf = std::clamp(static_cast<int>(4.0f * radius), 32, 256);
        const auto runLevel = [&](int step, int sm, int cells, int iters,
                                  int tries, const Lattice* from,
                                  std::uint32_t salt, Lattice& out) {
            out.lw = (x1 - x0) / step + 1;
            out.lh = (y1 - y0) / step + 1;
            out.stride = step;
            out.cells.assign(std::size_t(out.lw) * out.lh,
                             Off{bestOx, bestOy});
            out.rank.assign(std::size_t(out.lw) * out.lh,
                            std::numeric_limits<float>::infinity());
            out.own.assign(std::size_t(out.lw) * out.lh,
                           std::numeric_limits<float>::infinity());
            const int rc = cells * sm;
            const int xEnd = x0 + (out.lw - 1) * step;
            const int yEnd = y0 + (out.lh - 1) * step;
            const auto at = [&](int x, int y) -> Off& {
                return out.cells[std::size_t((y - y0) / step) * out.lw +
                                 (x - x0) / step];
            };
            const auto rankAt = [&](int x, int y) -> float& {
                return out.rank[std::size_t((y - y0) / step) * out.lw +
                                (x - x0) / step];
            };
            const auto ownAt = [&](int x, int y) -> float& {
                return out.own[std::size_t((y - y0) / step) * out.lw +
                               (x - x0) / step];
            };
            const auto holeIn = [&](int x, int y) {
                const float mx = static_cast<float>(x) + 0.5f - cx;
                const float my = static_cast<float>(y) + 0.5f - cy;
                return mx * mx + my * my < radius * radius;
            };
            // Init: upsample the coarser level when there is one (aligned
            // points copy exactly, the rest interpolate), then best-of-`tries`
            // random offsets over a wide window. Parallel over lattice rows:
            // each cell is a pure function of (src, coarser field, its own
            // hashed stream), so the range split cannot change any result.
            core::parallel_for(static_cast<std::uint32_t>(out.lh),
                               from ? 4u : 8u,
                               [&](std::uint32_t r0, std::uint32_t r1) {
                for (std::uint32_t liy = r0; liy < r1; ++liy) {
                    const int hy = y0 + static_cast<int>(liy) * step;
                    for (int lix = 0; lix < out.lw; ++lix) {
                        const int hx = x0 + lix * step;
                        if (!holeIn(hx, hy)) continue;
                        float bo = bestOx, bp = bestOy;
                        if (from) {
                            const float fx =
                                static_cast<float>(hx - x0) / from->stride;
                            const float fy =
                                static_cast<float>(hy - y0) / from->stride;
                            const int ix =
                                std::min(static_cast<int>(fx), from->lw - 1);
                            const int iy =
                                std::min(static_cast<int>(fy), from->lh - 1);
                            const int ix1 = std::min(ix + 1, from->lw - 1);
                            const int iy1 = std::min(iy + 1, from->lh - 1);
                            const float tx = std::clamp(fx - ix, 0.0f, 1.0f);
                            const float ty = std::clamp(fy - iy, 0.0f, 1.0f);
                            const Off& a =
                                from->cells[std::size_t(iy) * from->lw + ix];
                            const Off& b =
                                from->cells[std::size_t(iy) * from->lw + ix1];
                            const Off& c =
                                from->cells[std::size_t(iy1) * from->lw + ix];
                            const Off& dOff =
                                from->cells[std::size_t(iy1) * from->lw + ix1];
                            const float w00 = (1.0f - tx) * (1.0f - ty);
                            const float w10 = tx * (1.0f - ty);
                            const float w01 = (1.0f - tx) * ty;
                            const float w11 = tx * ty;
                            bo = a.x * w00 + b.x * w10 + c.x * w01 +
                                 dOff.x * w11;
                            bp = a.y * w00 + b.y * w10 + c.y * w01 +
                                 dOff.y * w11;
                        }
                        float bs = patchSSD(hx, hy, bo, bp,
                                            std::numeric_limits<float>::infinity(),
                                            sm, cells);
                        std::uint32_t st = cellSeed(hx, hy, salt);
                        for (int t = 0; t < tries; ++t) {
                            st = st * 1664525u + 1013904223u;
                            const float ox =
                                ((static_cast<int>(st >> 16) % 2001) /
                                     1000.0f -
                                 1.0f) *
                                initHalf;
                            st = st * 1664525u + 1013904223u;
                            const float oy =
                                ((static_cast<int>(st >> 16) % 2001) /
                                     1000.0f -
                                 1.0f) *
                                initHalf;
                            if (!validNNF(ox, oy, rc)) continue;
                            const float s = patchSSD(hx, hy, ox, oy, bs, sm,
                                                     cells);
                            if (s < bs) {
                                bs = s;
                                bo = ox;
                                bp = oy;
                            }
                        }
                        at(hx, hy).x = bo;
                        at(hx, hy).y = bp;
                        rankAt(hx, hy) = bs;
                        ownAt(hx, hy) = bs;
                    }
                }
            });
            // Serial scan sweeps: forward propagates left/up, backward
            // right/down, and because a pass updates in place a single pass
            // chains a vetted offset clear across the hole. Cells with no
            // local evidence (their patch never reaches a known texel —
            // everything deeper than the patch radius inside a big hole)
            // cannot be scored at all, so they take the best-ranked
            // neighbour's offset instead: the rim's verified continuations
            // keep flowing to the centre rather than dying at the first
            // blind ring.
            const float kInf = std::numeric_limits<float>::infinity();
            for (int sweep = 0; sweep < iters; ++sweep) {
                // Forward: propagate left/up; backward: right/down.
                for (int pass = 0; pass < 2; ++pass) {
                    const int yS = pass == 0 ? y0 : yEnd;
                    const int yE = pass == 0 ? yEnd : y0;
                    const int yD = pass == 0 ? 1 : -1;
                    const int xS = pass == 0 ? x0 : xEnd;
                    const int xE = pass == 0 ? xEnd : x0;
                    const int xD = pass == 0 ? 1 : -1;
                    for (int hy = yS; pass == 0 ? hy <= yE : hy >= yE;
                         hy += yD * step)
                        for (int hx = xS; pass == 0 ? hx <= xE : hx >= xE;
                             hx += xD * step) {
                            if (!holeIn(hx, hy))
                                continue;  // outside: no offset needed
                            // Own offset + score tracked locally: an accepted
                            // probe updates both, so the next probe's
                            // early-abort bound is that same value — one
                            // scoring call per probe instead of re-scoring
                            // the map entry each time (identical accept
                            // decisions, half the patch work).
                            const Off self = at(hx, hy);
                            float cOx = self.x, cOy = self.y;
                            // Own score comes from the per-cell cache: it
                            // depends only on (cell, offset), both already
                            // known here, so re-scoring the full patch each
                            // pass just returned the same number — a third
                            // of all scoring work for nothing.
                            float cS = ownAt(hx, hy);
                            float rk = cS < kInf ? cS : rankAt(hx, hy);
                            const int nx[2] = {hx - xD * step, hx};
                            const int ny[2] = {hy, hy - yD * step};
                            for (int k = 0; k < 2; ++k) {
                                if (nx[k] < x0 || nx[k] > x1 || ny[k] < y0 ||
                                    ny[k] > y1)
                                    continue;
                                const Off nb = at(nx[k], ny[k]);
                                if (!validNNF(nb.x, nb.y, rc)) continue;
                                const float s = patchSSD(hx, hy, nb.x, nb.y,
                                                         cS, sm, cells);
                                if (s < cS) {
                                    cS = s;
                                    cOx = nb.x;
                                    cOy = nb.y;
                                    rk = s;
                                } else if (!(cS < kInf) &&
                                           rankAt(nx[k], ny[k]) < rk) {
                                    // Blind here: inherit the neighbour's
                                    // vetted offset *and* its provenance so
                                    // the flood can keep walking inward.
                                    cOx = nb.x;
                                    cOy = nb.y;
                                    rk = rankAt(nx[k], ny[k]);
                                }
                            }
                            // Random search, shrinking radii (PatchMatch §3):
                            // from image scale down to a pixel, so a bad init
                            // can still escape to the right neighbourhood —
                            // but only where a probe can be judged: with no
                            // local evidence every probe scores +inf, and the
                            // loop would just burn full patches for nothing.
                            // One random round per sweep (forward pass), the
                            // cadence the algorithm specifies — the backward
                            // pass exists to propagate, and pays a full
                            // random round for sweeps it would only polish.
                            if (cS < kInf && pass == 0) {
                                std::uint32_t st = cellSeed(
                                    hx, hy,
                                    salt ^ (static_cast<std::uint32_t>(
                                                sweep * 4 + pass) *
                                            0x9E3779B1u));
                                for (int R = 128; R >= 1; R /= 2) {
                                    st = st * 1664525u + 1013904223u;
                                    const float jx =
                                        ((static_cast<int>(st >> 16) % 2001) /
                                             1000.0f -
                                         1.0f) *
                                        R;
                                    st = st * 1664525u + 1013904223u;
                                    const float jy =
                                        ((static_cast<int>(st >> 16) % 2001) /
                                             1000.0f -
                                         1.0f) *
                                        R;
                                    const float ox = cOx + jx, oy = cOy + jy;
                                    if (!validNNF(ox, oy, rc)) continue;
                                    const float s =
                                        patchSSD(hx, hy, ox, oy, cS, sm, cells);
                                    if (s < cS) {
                                        cS = s;
                                        cOx = ox;
                                        cOy = oy;
                                        rk = s;
                                    }
                                }
                            }
                            at(hx, hy).x = cOx;
                            at(hx, hy).y = cOy;
                            rankAt(hx, hy) = rk;
                            // cS is the own score of the offset now stored:
                            // a probe/neighbour accepted via `s < cS` set it
                            // to that candidate's own score; the blind
                            // inherit left it at +inf (the candidate's own
                            // score was +inf too, or it would have won).
                            ownAt(hx, hy) = cS;
                        }
                }
            }
        };
        // Coarse-to-fine: the coarse level solves every (2g)-th texel
        // sampling every 2nd pixel, so for the same patch budget it reaches
        // twice as deep — it verifies offsets across a much wider band of a
        // big hole (bridging structure the fine level cannot score), and the
        // fine level then starts from that field instead of one global
        // donor. Small holes (g=1) skip it: their whole area is scorable.
        Lattice coarseLv, fineLv;
        tPyrBeg = std::chrono::steady_clock::now();
        if (g > 1)
            runLevel(2 * g, 2,
                     std::clamp(static_cast<int>(radius / 5.66f), 4, 12), 1, 8,
                     nullptr, 0x51ED270Bu, coarseLv);
        tPyrEnd = std::chrono::steady_clock::now();
        // Sweep count: the flood lands in the first sweep (the in-place
        // raster chains it across the whole hole), so extra sweeps only add
        // random-search polish — buy fewer of them where each patch is
        // expensive (big holes), keep 5 where patches are small.
        runLevel(g, 1, prad, radius > 40.0f ? 1 : (radius > 16.0f ? 3 : 5),
                 g > 1 ? 4 : 8, g > 1 ? &coarseLv : nullptr, 0x1B873593u,
                 fineLv);
        offMap = fineLv.cells;
    }
    const auto tRefine = std::chrono::steady_clock::now();

    // Create Texture detail pool: ring texels minus their own low frequency —
    // pure guessed grain, no donor structure.
    std::vector<RGBAf> pool;
    if (type == HealType::CreateTexture) {
        for (int y = rby0; y <= rby1; ++y)
            for (int x = rbx0; x <= rbx1; ++x) {
                const float dx = static_cast<float>(x) + 0.5f - cx;
                const float dy = static_cast<float>(y) + 0.5f - cy;
                const float dd = std::sqrt(dx * dx + dy * dy);
                if (dd <= radius || dd > ringOut) continue;
                const RGBAf& c = srcAt(x, y);
                if (c.a <= 0.01f) continue;
                const RGBAf l = blurAt(x, y);
                pool.push_back(
                    RGBAf{c.r - l.r, c.g - l.g, c.b - l.b, 1.0f});
            }
        if (pool.empty()) return false;
    }
    // Deterministic per-texel stream (stable tests, stable strokes): the
    // apply loop below runs in row ranges that can start anywhere, so pool
    // picks must not depend on visit order.
    std::uint32_t rng =
        ((static_cast<std::uint32_t>(cx * 73856093.0f) ^
          static_cast<std::uint32_t>(cy * 19349663.0f)) ^
         static_cast<std::uint32_t>(w * 83492791u)) |
        1u;
    const auto texelRnd = [&](int x, int y) {
        std::uint32_t v = rng ^ (static_cast<std::uint32_t>(x) * 0x9E3779B1u) ^
                          (static_cast<std::uint32_t>(y) * 0x85EBCA77u);
        v ^= v >> 16;
        v *= 0x7FEB352Du;
        v ^= v >> 15;
        v *= 0x846CA68Bu;
        v ^= v >> 16;
        return v;
    };

    // Low-frequency field for the hole: seed a coarse grid over the dab box
    // with blurred known texels (rim boundary), diffuse it with fixed-count
    // Jacobi sweeps (harmonic interpolation = smooth colour continuation),
    // bilinear-sample it per hole texel. A per-texel blur window cannot see
    // past the rim once the hole is wider than the window — every window
    // then sits fully inside the hole and falls back to the flat ring mean
    // (the muddy-centre symptom on big brushes). Fixed iteration count keeps
    // the field seed-deterministic.
    const int cf = std::max(4, static_cast<int>(radius / 6.0f));
    const int ax0 = std::max(0, x0 - 1), ax1 = std::min(wi - 1, x1 + 1);
    const int ay0 = std::max(0, y0 - 1), ay1 = std::min(hi - 1, y1 + 1);
    const int gw = (ax1 - ax0) / cf + 1;
    const int gh = (ay1 - ay0) / cf + 1;
    std::vector<RGBAf> field(std::size_t(gw) * gh, ringMean);
    std::vector<RGBAf> scratch(std::size_t(gw) * gh, ringMean);
    std::vector<unsigned char> seeded(std::size_t(gw) * gh, 0);
    int ffb = 0;  // grid cells with no known texel at all (telemetry)
    for (int gy = 0; gy < gh; ++gy) {
        const int cy0 = ay0 + gy * cf;
        const int cy1 = std::min(ay1, cy0 + cf - 1);
        for (int gx = 0; gx < gw; ++gx) {
            const int cx0 = ax0 + gx * cf;
            const int cx1 = std::min(ax1, cx0 + cf - 1);
            double sr = 0, sg = 0, sb = 0;
            int n = 0;
            for (int y = cy0; y <= cy1; ++y)
                for (int x = cx0; x <= cx1; ++x) {
                    const float hxx = static_cast<float>(x) + 0.5f - cx;
                    const float hyy = static_cast<float>(y) + 0.5f - cy;
                    if (hxx * hxx + hyy * hyy < radius * radius) continue;
                    const RGBAf& c = srcAt(x, y);
                    if (c.a <= 0.01f) continue;
                    const RGBAf b = blurAt(x, y);
                    sr += b.r;
                    sg += b.g;
                    sb += b.b;
                    ++n;
                }
            const std::size_t i = std::size_t(gy) * gw + gx;
            if (n == 0) {
                ++ffb;
                continue;
            }
            field[i] = RGBAf{static_cast<float>(sr / n),
                             static_cast<float>(sg / n),
                             static_cast<float>(sb / n), 1.0f};
            seeded[i] = 1;
        }
    }
    // Jacobi: unseeded cells take the mean of their 4-neighbour values;
    // seeded cells hold the rim boundary. ~2 sweeps per cell axis + margin
    // smooths the interior well past its own cell scale.
    const int jiters = std::clamp(2 * std::max(gw, gh) + 8, 16, 96);
    for (int it = 0; it < jiters; ++it) {
        for (int gy = 0; gy < gh; ++gy)
            for (int gx = 0; gx < gw; ++gx) {
                const std::size_t i = std::size_t(gy) * gw + gx;
                if (seeded[i]) {
                    scratch[i] = field[i];
                    continue;
                }
                double ar = 0, ag = 0, ab = 0;
                int an = 0;
                if (gx > 0) {
                    const RGBAf& v = field[i - 1];
                    ar += v.r;
                    ag += v.g;
                    ab += v.b;
                    ++an;
                }
                if (gx + 1 < gw) {
                    const RGBAf& v = field[i + 1];
                    ar += v.r;
                    ag += v.g;
                    ab += v.b;
                    ++an;
                }
                if (gy > 0) {
                    const RGBAf& v = field[i - std::size_t(gw)];
                    ar += v.r;
                    ag += v.g;
                    ab += v.b;
                    ++an;
                }
                if (gy + 1 < gh) {
                    const RGBAf& v = field[i + std::size_t(gw)];
                    ar += v.r;
                    ag += v.g;
                    ab += v.b;
                    ++an;
                }
                scratch[i] = an ? RGBAf{static_cast<float>(ar / an),
                                        static_cast<float>(ag / an),
                                        static_cast<float>(ab / an), 1.0f}
                                : field[i];
            }
        field.swap(scratch);
    }
    // Bilinear sample at cell centres, clamped at the box edges.
    const auto fieldAt = [&](int x, int y) {
        const float ffx = (x - ax0 + 0.5f) / cf - 0.5f;
        const float ffy = (y - ay0 + 0.5f) / cf - 0.5f;
        const int ix =
            std::clamp(static_cast<int>(std::floor(ffx)), 0, gw - 1);
        const int iy =
            std::clamp(static_cast<int>(std::floor(ffy)), 0, gh - 1);
        const int ix1 = std::min(ix + 1, gw - 1);
        const int iy1 = std::min(iy + 1, gh - 1);
        const float tx = std::clamp(ffx - ix, 0.0f, 1.0f);
        const float ty = std::clamp(ffy - iy, 0.0f, 1.0f);
        const RGBAf& a = field[std::size_t(iy) * gw + ix];
        const RGBAf& b = field[std::size_t(iy) * gw + ix1];
        const RGBAf& c = field[std::size_t(iy1) * gw + ix];
        const RGBAf& d = field[std::size_t(iy1) * gw + ix1];
        const float w00 = (1.0f - tx) * (1.0f - ty);
        const float w10 = tx * (1.0f - ty);
        const float w01 = (1.0f - tx) * ty;
        const float w11 = tx * ty;
        return RGBAf{a.r * w00 + b.r * w10 + c.r * w01 + d.r * w11,
                     a.g * w00 + b.g * w10 + c.g * w01 + d.g * w11,
                     a.b * w00 + b.b * w10 + c.b * w01 + d.b * w11, 1.0f};
    };

    // Apply into the hole. Rows are independent (dst writes are disjoint,
    // every read is const), so the loop splits over row ranges; the
    // changed-region box is merged from per-range bounds — min/max over
    // disjoint ranges is order-independent, so the box comes out identical
    // to a serial scan.
    int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
    bool any = false;
    // Per-texel donor -> usable source texel, falling back to the global
    // donor (provably clear of the hole) when the refined offset points
    // back inside it: a propagated rim offset only clears the hole from the
    // rim's own position, and skipping the texel instead would leave the
    // blemish behind.
    const auto donorTexel = [&](int x, int y, float ox, float oy, int& sx,
                                int& sy) {
        const auto usable = [&](int ax, int ay) {
            if (ax < 0 || ay < 0 || ax >= wi || ay >= hi) return false;
            const float qx = static_cast<float>(ax) + 0.5f - cx;
            const float qy = static_cast<float>(ay) + 0.5f - cy;
            if (qx * qx + qy * qy < radius * radius) return false;
            return srcAt(ax, ay).a > 0.01f;
        };
        sx = static_cast<int>(std::floor(x + ox));
        sy = static_cast<int>(std::floor(y + oy));
        if (usable(sx, sy)) return true;
        sx = static_cast<int>(std::floor(x + bestOx));
        sy = static_cast<int>(std::floor(y + bestOy));
        return usable(sx, sy);
    };
    std::atomic<int> ubx0{0}, uby0{0}, ubx1{-1}, uby1{-1};
    core::parallel_for(static_cast<std::uint32_t>(y1 - y0 + 1), 16,
                       [&](std::uint32_t r0, std::uint32_t r1) {
        int lx0 = 0, ly0 = 0, lx1 = -1, ly1 = -1;
        bool lany = false;
        for (std::uint32_t ri = r0; ri < r1; ++ri) {
            const int y = y0 + static_cast<int>(ri);
            for (int x = x0; x <= x1; ++x) {
                float cov = brush_cov(x, y);
                if (cov <= 0.0f) continue;
                if (selection) {
                    cov *= selection->coverage(x, y);
                    if (cov <= 0.0f) continue;
                }
                RGBAf& d = dst[std::size_t(y) * w + x];
                const RGBAf before = d;
                // Per-texel donor offset: the refined lattice
                // (bilinear-filled) for Content-Aware, the single global
                // donor otherwise.
                float ox = bestOx, oy = bestOy;
                if (type == HealType::ContentAware && refine) {
                    const float fx = static_cast<float>(x - x0) / g;
                    const float fy = static_cast<float>(y - y0) / g;
                    const int ix = std::min(static_cast<int>(fx), cw - 1);
                    const int iy = std::min(static_cast<int>(fy), ch - 1);
                    const int ix1 = std::min(ix + 1, cw - 1);
                    const int iy1 = std::min(iy + 1, ch - 1);
                    const float tx = std::clamp(fx - ix, 0.0f, 1.0f);
                    const float ty = std::clamp(fy - iy, 0.0f, 1.0f);
                    const Off& a = offMap[std::size_t(iy) * cw + ix];
                    const Off& b = offMap[std::size_t(iy) * cw + ix1];
                    const Off& c = offMap[std::size_t(iy1) * cw + ix];
                    const Off& dOff = offMap[std::size_t(iy1) * cw + ix1];
                    const float w00 = (1.0f - tx) * (1.0f - ty);
                    const float w10 = tx * (1.0f - ty);
                    const float w01 = (1.0f - tx) * ty;
                    const float w11 = tx * ty;
                    ox = a.x * w00 + b.x * w10 + c.x * w01 + dOff.x * w11;
                    oy = a.y * w00 + b.y * w10 + c.y * w01 + dOff.y * w11;
                }
                const RGBAf low = fieldAt(x, y);
                if (d.a <= 0.01f) {
                    // Nothing to heal from here: deposit donor content so
                    // healing onto transparency still paints pixels (alpha
                    // ramps with the brush, like a stamp).
                    RGBAf stamp{low.r, low.g, low.b, 1.0f};
                    if (type == HealType::CreateTexture) {
                        const RGBAf det = pool[texelRnd(x, y) % pool.size()];
                        stamp.r = std::clamp(low.r + det.r, 0.0f, 1.0f);
                        stamp.g = std::clamp(low.g + det.g, 0.0f, 1.0f);
                        stamp.b = std::clamp(low.b + det.b, 0.0f, 1.0f);
                    } else {
                        int sx = 0, sy = 0;
                        if (!donorTexel(x, y, ox, oy, sx, sy)) continue;
                        stamp = srcAt(sx, sy);
                    }
                    d.r = before.r + (stamp.r - before.r) * cov;
                    d.g = before.g + (stamp.g - before.g) * cov;
                    d.b = before.b + (stamp.b - before.b) * cov;
                    d.a = before.a + (stamp.a - before.a) * cov;
                } else {
                    RGBAf detail{0, 0, 0, 1};
                    if (type == HealType::CreateTexture) {
                        detail = pool[texelRnd(x, y) % pool.size()];
                    } else {
                        int sx = 0, sy = 0;
                        if (!donorTexel(x, y, ox, oy, sx, sy)) continue;
                        const RGBAf& s = srcAt(sx, sy);
                        const RGBAf sl = blurAt(sx, sy);
                        detail =
                            RGBAf{s.r - sl.r, s.g - sl.g, s.b - sl.b, 1.0f};
                    }
                    d.r = before.r +
                          (std::clamp(low.r + detail.r, 0.0f, 1.0f) - before.r) *
                              cov;
                    d.g = before.g +
                          (std::clamp(low.g + detail.g, 0.0f, 1.0f) - before.g) *
                              cov;
                    d.b = before.b +
                          (std::clamp(low.b + detail.b, 0.0f, 1.0f) - before.b) *
                              cov;
                }
                const float moved =
                    std::max(std::fabs(d.r - before.r),
                             std::max(std::fabs(d.g - before.g),
                                      std::max(std::fabs(d.b - before.b),
                                               std::fabs(d.a - before.a))));
                if (moved < 1e-6f) continue;
                if (!lany) {
                    lx0 = lx1 = x;
                    ly0 = ly1 = y;
                    lany = true;
                } else {
                    lx0 = std::min(lx0, x);
                    ly0 = std::min(ly0, y);
                    lx1 = std::max(lx1, x);
                    ly1 = std::max(ly1, y);
                }
            }
        }
        if (!lany) return;
        // Merge this range's box (min/max: order-independent).
        int c = ubx0.load(std::memory_order_relaxed);
        while (lx0 < c &&
               !ubx0.compare_exchange_weak(c, lx0, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
        }
        c = uby0.load(std::memory_order_relaxed);
        while (ly0 < c &&
               !uby0.compare_exchange_weak(c, ly0, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
        }
        c = ubx1.load(std::memory_order_relaxed);
        while (lx1 > c &&
               !ubx1.compare_exchange_weak(c, lx1, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
        }
        c = uby1.load(std::memory_order_relaxed);
        while (ly1 > c &&
               !uby1.compare_exchange_weak(c, ly1, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
        }
    });
    any = ubx1.load(std::memory_order_relaxed) >= 0;
    if (any) {
        rx0 = ubx0.load(std::memory_order_relaxed);
        ry0 = uby0.load(std::memory_order_relaxed);
        rx1 = ubx1.load(std::memory_order_relaxed);
        ry1 = uby1.load(std::memory_order_relaxed);
    }

    const auto tApply = std::chrono::steady_clock::now();
    const double totalMs =
        std::chrono::duration<double, std::milli>(tApply - t0).count();
    // Per-dab stage breakdown: always when the dab is at/over the slow-event
    // budget, or every dab under PITTORE_STROKE_TRACE=1 (or set
    // PITTORE_SLOW_EVENT_MS=0 to trace all). lowfb reports the flat-centre
    // symptom on big brushes (blur windows blind to outside-the-hole texels).
    if (core::log::strokeTrace() || totalMs >= core::log::slowEventMs()) {
        const auto stageMs = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        PITTORE_LOG(
            "[dab:heal] r=%.1f type=%d refine=%d g=%d lowfb=%d ffb=%d "
            "gbest=%g ms=%.2f ring=%.2f donor=%.2f coarse=%.2f fine=%.2f "
            "refine=%.2f apply=%.2f",
            radius, static_cast<int>(type), refine ? 1 : 0, g,
            lowFb.load(std::memory_order_relaxed), ffb, globalBest, totalMs,
            stageMs(t0, tRing), stageMs(tRing, tDonor),
            stageMs(tPyrBeg, tPyrEnd), stageMs(tPyrEnd, tRefine),
            stageMs(tDonor, tRefine), stageMs(tRefine, tApply));
    }
    if (!any) return false;
    if (bbox) {
        bbox[0] = rx0;
        bbox[1] = ry0;
        bbox[2] = rx1 + 1;  // half-open
        bbox[3] = ry1 + 1;
    }
    return true;
}


void translate_heal_donor_host(const RGBAf* base, RGBAf* donor,
                               std::uint32_t w, std::uint32_t h, float ox,
                               float oy) {
    if (!base || !donor || w == 0 || h == 0) return;
    const int ix = static_cast<int>(std::floor(ox));
    const int iy = static_cast<int>(std::floor(oy));
    core::parallel_rows(h, [&](std::uint32_t y0, std::uint32_t y1) {
        for (std::uint32_t y = y0; y < y1; ++y) {
            const int sy =
                std::clamp(static_cast<int>(y) - iy, 0, static_cast<int>(h) - 1);
            for (std::uint32_t x = 0; x < w; ++x) {
                const int sx = std::clamp(static_cast<int>(x) - ix, 0,
                                          static_cast<int>(w) - 1);
                donor[std::size_t(y) * w + x] =
                    base[std::size_t(sy) * w + sx];
            }
        }
    });
}


bool redeye_fix_host(RGBAf* dst, std::uint32_t w, std::uint32_t h, int x0,
                     int y0, int x1, int y1, float darken, int* bbox) {
    if (!dst || w == 0 || h == 0) return false;
    x0 = std::clamp(x0, 0, static_cast<int>(w));
    y0 = std::clamp(y0, 0, static_cast<int>(h));
    x1 = std::clamp(x1, 0, static_cast<int>(w));
    y1 = std::clamp(y1, 0, static_cast<int>(h));
    if (x1 <= x0 || y1 <= y0) return false;
    const float dk = std::clamp(darken, 0.0f, 1.0f);
    const float keep = 1.0f - 0.75f * dk;
    std::atomic<bool> touched{false};
    core::parallel_rows(std::uint32_t(y1 - y0),
                        [&](std::uint32_t lo, std::uint32_t hi) {
                            bool local = false;
                            for (std::uint32_t r = lo; r < hi; ++r) {
                                const int y = y0 + int(r);
                                for (int x = x0; x < x1; ++x) {
                                    RGBAf& d =
                                        dst[std::size_t(y) * w + x];
                                    if (d.r < 0.25f || d.r < 1.4f * d.g ||
                                        d.r < 1.4f * d.b)
                                        continue;
                                    // 1px feather at the box rim.
                                    const int ex = std::min(x - x0, x1 - 1 - x);
                                    const int ey = std::min(y - y0, y1 - 1 - y);
                                    const float edge =
                                        std::clamp(float(std::min(ex, ey)),
                                                   0.0f, 1.0f);
                                    if (!(edge > 0.0f)) continue;
                                    const float luma = 0.2126f * d.r +
                                                       0.7152f * d.g +
                                                       0.0722f * d.b;
                                    const float v = luma * keep * edge +
                                                    d.r * (1.0f - edge);
                                    const float vg = luma * keep * edge +
                                                     d.g * (1.0f - edge);
                                    const float vb = luma * keep * edge +
                                                     d.b * (1.0f - edge);
                                    if (v == d.r && vg == d.g && vb == d.b)
                                        continue;
                                    d.r = v;
                                    d.g = vg;
                                    d.b = vb;
                                    local = true;
                                }
                            }
                            if (local) touched.store(true);
                        });
    if (bbox != nullptr) {
        bbox[0] = x0;
        bbox[1] = y0;
        bbox[2] = x1;
        bbox[3] = y1;
    }
    return touched.load();
}

}  // namespace pittore::compute
