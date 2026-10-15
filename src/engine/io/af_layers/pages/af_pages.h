#pragma once
#include <cstdint>
#include <vector>

namespace pittore::io::af_detail {

// One spread's page union in source coords (PagR rctp union, else SprB,
// else DfSz). Pure geometry, no graph dependency so it unit tests alone.
struct PageRect {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

// Vertical stack of page rects: widest page wide, every page tall plus a gap
// between neighbours. Single-page stacks keep their exact size (no gap).
// Pages are centred horizontally; each page's origin maps to (xOff, yOff).
struct StackedLayout {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int gap = 0;
    std::vector<std::uint32_t> pageW;
    std::vector<std::uint32_t> pageH;
    std::vector<std::uint32_t> xOff;
    std::vector<std::uint32_t> yOff;
    bool ok = false;
};

StackedLayout stackPages(const std::vector<PageRect>& pages, int gap);
}  // namespace pittore::io::af_detail
