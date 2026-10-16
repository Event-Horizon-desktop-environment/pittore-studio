#pragma once

#include <QLayout>
#include <QList>
#include <QSize>

class QRect;

namespace pittore::ui {

// A single-line layout that wraps onto further lines when it is narrower
// than its items: each item is placed at its size hint left to right and
// starts a new row when it would overflow; rows are top-packed. Height is
// a function of width, so a parent layout may shrink the widget below the
// one-row width — the options bar uses that to wrap a crowded tool's
// controls instead of pinning the whole window wide (issue #2).
class FlowLayout final : public QLayout {
  public:
    explicit FlowLayout(QWidget* parent, int spacing = 10);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    void setGeometry(const QRect& rect) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

  private:
    // Shared row-packing pass: with `place` the items are also positioned
    // inside `rect`; otherwise only the total height is computed. Returns
    // the height needed, contents margins included.
    int pack(const QRect& rect, bool place) const;

    QList<QLayoutItem*> items_;
};

}  // namespace pittore::ui
