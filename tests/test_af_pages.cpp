// test_af_pages.cpp — page-stack layout for multi-spread Affinity import.
// Pure geometry: no fixtures, no libzstd, runs everywhere.
#include <cstdio>

#include "engine/io/af_layers/pages/af_pages.h"

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    if (cond) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}
}  // namespace

int main() {
    using pittore::io::af_detail::PageRect;
    using pittore::io::af_detail::stackPages;
    // Two 1200x1800 pages (the real two-page file) stack with a 128 gap.
    {
        const auto l = stackPages({{0, 0, 1200, 1800}, {0, 0, 1200, 1800}}, 128);
        check(l.ok, "two pages stack");
        check(l.width == 1200 && l.height == 3728, "stacked canvas 1200x3728");
        check(l.gap == 128, "gap kept");
        check(l.xOff.size() == 2 && l.xOff[0] == 0 && l.xOff[1] == 0, "x offsets");
        check(l.yOff.size() == 2 && l.yOff[0] == 0 && l.yOff[1] == 1928, "y offsets");
        check(l.pageW[0] == 1200 && l.pageH[1] == 1800, "page extents");
    }
    // Single page keeps its exact size and takes no gap.
    {
        const auto l = stackPages({{0, 0, 900, 1500}}, 128);
        check(l.ok, "single page stacks");
        check(l.width == 900 && l.height == 1500, "single canvas unchanged");
        check(l.gap == 0 && l.xOff[0] == 0 && l.yOff[0] == 0, "single offsets");
    }
    // Narrower pages centre on the widest.
    {
        const auto l = stackPages({{0, 0, 800, 600}, {0, 0, 1200, 600}}, 100);
        check(l.ok, "mixed widths stack");
        check(l.width == 1200 && l.height == 1300, "mixed canvas");
        check(l.xOff[0] == 200 && l.xOff[1] == 0, "narrow page centred");
        check(l.yOff[0] == 0 && l.yOff[1] == 700, "mixed y offsets");
    }
    // Degenerate input never produces a canvas.
    check(!stackPages({}, 128).ok, "empty rejects");
    check(!stackPages({{0, 0, 0, 10}}, 128).ok, "zero width rejects");
    check(!stackPages({{0, 0, 10, 10}}, -1).ok, "negative gap rejects");

    if (failures == 0) {
        std::printf("test_af_pages: all checks passed\n");
        return 0;
    }
    std::printf("test_af_pages: %d failures\n", failures);
    return 1;
}
