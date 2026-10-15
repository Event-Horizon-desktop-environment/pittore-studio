#include "engine/io/af_layers/pages/af_pages.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace pittore::io::af_detail {

StackedLayout stackPages(const std::vector<PageRect>& pages, int gap) {
    StackedLayout out;
    if (pages.empty() || gap < 0) return out;
    out.gap = static_cast<int>(pages.size()) > 1 ? gap : 0;
    out.pageW.reserve(pages.size());
    out.pageH.reserve(pages.size());
    std::uint32_t width = 0;
    std::uint64_t height = 0;
    for (const PageRect& p : pages) {
        const double w = p.x1 - p.x0;
        const double h = p.y1 - p.y0;
        if (!(w > 0.0 && h > 0.0)) return StackedLayout{};
        const std::uint32_t pw = static_cast<std::uint32_t>(std::lround(w));
        const std::uint32_t ph = static_cast<std::uint32_t>(std::lround(h));
        if (pw == 0 || ph == 0 || pw > (1u << 20) || ph > (1u << 20))
            return StackedLayout{};
        out.pageW.push_back(pw);
        out.pageH.push_back(ph);
        width = std::max(width, pw);
        height += ph;
    }
    height += std::uint64_t(out.gap) * (pages.size() - 1);
    if (width == 0 || height == 0 || width > (1u << 20) || height > (1u << 20))
        return StackedLayout{};
    out.width = width;
    out.height = static_cast<std::uint32_t>(height);
    out.xOff.reserve(pages.size());
    out.yOff.reserve(pages.size());
    std::uint32_t y = 0;
    for (size_t i = 0; i < pages.size(); ++i) {
        out.xOff.push_back((width - out.pageW[i]) / 2);
        out.yOff.push_back(y);
        y += out.pageH[i] + std::uint32_t(out.gap);
    }
    out.ok = true;
    return out;
}

}  // namespace pittore::io::af_detail
