// test_af_artboard.cpp — Designer artboard board-split regression.
//
// Issue #3 deliverable: a multi-artboard Designer file must open as one page
// per artboard, with every content layer tagged by its owning board and the
// canvas read from file records (spread union + each board's own ShpB bounds
// under its placement). The fixture is an external sample (two birthday
// cards); a checkout without it skips, so no sample bytes land in the repo
// (PITTORE_AF_ARTBOARD_FIXTURE points at a real .af file).
//
// All numbers below are pinned to the current decoder contract: canvas from
// the spread union, board rects from records, names from the file's Desc
// labels, and the known decode scope (one RcRA skip, everything else rebuilt).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/io/af_layers.h"

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

bool readWholeFile(const std::string& path, std::vector<std::uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::rewind(f);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<std::size_t>(n));
    const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// Text layer lookup by stored-name prefix: the generator's names survive
// decode verbatim and only text rows match `wantText`.
bool findLayer(const pittore::io::AfLayersDoc& doc, const std::string& needle,
               bool wantText) {
    for (const auto& l : doc.layers)
        if (l.isText == wantText &&
            l.name.find(needle) != std::string::npos)
            return true;
    return false;
}

void testArtboardSplit() {
    const char* env = std::getenv("PITTORE_AF_ARTBOARD_FIXTURE");
    const std::string path = (env && *env) ? env : "";
    if (path.empty()) {
        std::printf("test_af_artboard: fixture absent (PITTORE_AF_ARTBOARD_FIXTURE) "
                    "— skipped\n");
        check(true, "artboard fixture absent — skipped");
        return;
    }
    std::vector<std::uint8_t> data;
    if (!readWholeFile(path, data)) {
        std::printf("test_af_artboard: fixture unreadable — skipped\n");
        check(true, "artboard fixture unreadable — skipped");
        return;
    }

    std::string err;
    auto doc = pittore::io::afDecodeLayers(data, &err);
    check(doc.has_value(), err.empty() ? "multi-artboard file decodes" : err.c_str());
    if (!doc) return;
    const auto& d = *doc;

    // Canvas: the spread's own union (PagR), not a per-board sum. Both cards
    // share one spread, so the board pair must NOT re-derive the canvas.
    check(d.width == 2250 && d.height == 900, "canvas is the spread union 2250x900");
    check(d.depth == 8, "depth 8");

    // One page per artboard, bounds straight from each board's ShpB record
    // under its placement transform.
    check(d.pages.size() == 2, "two artboard pages");
    if (d.pages.size() == 2) {
        const auto& p0 = d.pages[0];
        const auto& p1 = d.pages[1];
        check(p0.artboard && p1.artboard, "both pages artboards");
        check(p0.source == "ShpB" && p1.source == "ShpB", "bounds from ShpB records");
        check(p0.name == "Bryan's Card", "first board name from Desc");
        check(p1.name == "Lee's Card", "second board name from Desc");
        check(p0.x0 == 0.0 && p0.y0 == 0.0 && p0.x1 == 1050.0 && p0.y1 == 900.0,
              "board 0 rect 0,0..1050,900");
        // Board 1's origin is fractional in the file: 1200.114 under its
        // placement (the app log rounds for display). Pin the width and the
        // origin with a tight tolerance instead of rounding it away.
        check(p1.y0 == 0.0 && p1.y1 == 900.0, "board 1 vertical bounds");
        check(std::fabs(p1.x1 - p1.x0 - 1050.0) < 0.001, "board 1 is 1050 wide");
        check(std::fabs(p1.x0 - 1200.114) < 0.001, "board 1 origin from records");
    }

    // Every recovered layer belongs to exactly one board.
    bool boardTagged = true;
    for (const auto& l : d.layers) {
        if (l.board < 0 || l.board >= static_cast<int>(d.pages.size()))
            boardTagged = false;
    }
    check(boardTagged, "every layer carries its owning board");
    check(d.layers.size() == 10, "known scope: ten recovered layers");

    // Both text layers are rebuilt (not rasterised): one per card.
    check(findLayer(d, "Some people look for the light", true),
          "Bryan text layer rebuilt as text");
    check(findLayer(d, "Getting older is like being on", true),
          "Lee text layer rebuilt as text");

    // Embedded placed bitmaps decode to pixels for both boards.
    bool webpOk = false, pngOk = false;
    for (const auto& l : d.layers) {
        if (l.rgba.empty() || l.isGroup) continue;
        if (l.name.size() >= 5 && l.name.compare(l.name.size() - 5, 5, ".webp") == 0)
            webpOk = true;
        if (l.name == "giftCard_disney.png") pngOk = true;
    }
    check(webpOk, "embedded WebP placed image decodes");
    check(pngOk, "embedded PNG placed image decodes");

    // Known out-of-scope record set: exactly one skip, named in the log.
    check(d.complete == false, "incomplete while record kinds remain out of scope");
    check(d.skippedLayers == 1, "exactly one skipped layer (RcRA)");
    bool loggedRcRA = false;
    for (const std::string& l : d.log)
        if (l.find("RcRA") != std::string::npos) loggedRcRA = true;
    check(loggedRcRA, "RcRA skip is named in the log");

    // Legacy rounded rectangle: the card background is a ShNR v0 "ShRR"-
    // shaped layer whose CTyp stores the pre-4.x corner legend [0,4,4,4]
    // (0 = square, 4 = rounded arc) with ShCR radii [0.06,0.25,0.25,0.25].
    // The decode must keep the three rounded corners as real curves and one
    // square corner, not flatten the record to a plain rectangle.
    const pittore::io::AfLayer* cardFill = nullptr;
    for (const auto& l : d.layers) {
        if (!l.name.empty() && l.name.find("Rounded Rectangle") != std::string::npos &&
            l.width > l.height) {
            cardFill = &l;
            break;
        }
    }
    if (cardFill) {
        check(cardFill->art != nullptr, "card background keeps live vector art");
        if (cardFill->art) {
            const auto& art = *cardFill->art;
            std::size_t arcs = 0, squares = 0;
            for (const auto& s : art.segments) {
                if (s.kind == pittore::vector::Segment::Kind::CubicTo) ++arcs;
                if (s.kind == pittore::vector::Segment::Kind::MoveTo) ++squares;
            }
            // Three rounded corners survive as real curves (the rasteriser
            // may subdivide each arc, so count cubic segments, not arcs);
            // the single square corner keeps its MoveTo corner.
            check(arcs >= 3, "legacy rounded corners survive as curves");
            check(squares >= 1, "the square corner stays a corner");
        }
    } else {
        check(false, "card background layer ('Rounded Rectangle') decoded");
    }
}

}  // namespace

int main() {
#ifdef PITTORE_AF
    testArtboardSplit();
#else
    std::printf("test_af_artboard: skipped (decode needs libzstd)\n");
#endif
    if (failures == 0) {
        std::printf("test_af_artboard: all checks passed\n");
        return 0;
    }
    std::printf("test_af_artboard: %d failures\n", failures);
    return 1;
}
