#include "ui/flow_layout.h"

#include <QRect>
#include <QWidget>

#include <algorithm>

namespace pittore::ui {

FlowLayout::FlowLayout(QWidget* parent, int spacing) : QLayout(parent) {
    setSpacing(spacing);
}

FlowLayout::~FlowLayout() {
    qDeleteAll(items_);
    items_.clear();
}

void FlowLayout::addItem(QLayoutItem* item) { items_.append(item); }

int FlowLayout::count() const { return items_.size(); }

QLayoutItem* FlowLayout::itemAt(int index) const {
    return items_.value(index);
}

QLayoutItem* FlowLayout::takeAt(int index) {
    if (index < 0 || index >= items_.size()) return nullptr;
    return items_.takeAt(index);
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    pack(rect, true);
}

QSize FlowLayout::sizeHint() const {
    // Preferred shape: one row of everything.
    int width = 0;
    int height = 0;
    for (const QLayoutItem* item : items_) {
        const QSize hint = item->sizeHint();
        width += hint.width();
        height = std::max(height, hint.height());
    }
    if (!items_.isEmpty()) width += spacing() * (items_.size() - 1);
    int left = 0, top = 0, right = 0, bottom = 0;
    getContentsMargins(&left, &top, &right, &bottom);
    return {width + left + right, height + top + bottom};
}

QSize FlowLayout::minimumSize() const {
    int left = 0, top = 0, right = 0, bottom = 0;
    getContentsMargins(&left, &top, &right, &bottom);
    // Two content-derived floors: never narrower than the two widest items
    // side by side (a lone widest item strands a whole row anyway), and
    // never narrower than half the one-row width (so the wrapped form
    // stays a handful of rows instead of a tall stack at the window's
    // floor width, which would push the minimum HEIGHT back up).
    int widest = 0;
    int second = 0;
    int oneRow = 0;
    for (const QLayoutItem* item : items_) {
        const int w = item->minimumSize().width();
        if (w > widest) {
            second = widest;
            widest = w;
        } else if (w > second) {
            second = w;
        }
        oneRow += item->sizeHint().width();
    }
    if (!items_.isEmpty()) oneRow += spacing() * (items_.size() - 1);
    int content = widest;
    if (second > 0) content = std::max(content, widest + spacing() + second);
    content = std::max(content, oneRow / 2);
    const int width = content + left + right;
    return {width, heightForWidth(width)};
}

int FlowLayout::heightForWidth(int width) const {
    // Geometry and placement are irrelevant here: only x positions drive
    // the wrap, so a zero-height rect bounds the pass.
    return pack(QRect(0, 0, width, 0), false);
}

int FlowLayout::pack(const QRect& rect, bool place) const {
    int left = 0, top = 0, right = 0, bottom = 0;
    getContentsMargins(&left, &top, &right, &bottom);
    const int gap = spacing();
    const int rowLeft = rect.left() + left;
    const int rowRight = rect.right() - right;
    int x = rowLeft;
    int y = rect.top() + top;
    int rowHeight = 0;
    for (QLayoutItem* item : items_) {
        const QSize hint = item->sizeHint();
        if (x > rowLeft && x + hint.width() > rowRight + 1) {
            x = rowLeft;
            y += rowHeight + gap;
            rowHeight = 0;
        }
        if (place) item->setGeometry(QRect(QPoint(x, y), hint));
        x += hint.width() + gap;
        rowHeight = std::max(rowHeight, hint.height());
    }
    if (items_.isEmpty()) return top + bottom;
    return y - rect.top() + rowHeight + bottom;
}

}  // namespace pittore::ui
