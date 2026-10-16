#pragma once
#include <QFrame>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QWidget>

#include "ui/app_state.h"
#include "ui/tool_registry.h"

class QGridLayout;
class QResizeEvent;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace pittore::ui {

class AppState;

// Foreground/background wells from the toolbar bottom: two swatches,
// swap (X) and reset (D). The active well is where Color-panel edits go.
class ColorWells final : public QWidget {
    Q_OBJECT

  public:
    explicit ColorWells(AppState* state, QWidget* parent = nullptr);

    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

  private:
    QRectF foregroundRect() const;
    QRectF backgroundRect() const;
    QRectF swapRect() const;
    QRectF resetRect() const;

    AppState* state_;
};

// The vertical tools strip: selection, flyouts, Shift+letter cycling,
// responsive 1-to-4-column layout, plus quick mask / screen mode at the
// bottom.
class ToolsPanel final : public QFrame {
    Q_OBJECT

  public:
    explicit ToolsPanel(AppState* state, QWidget* parent = nullptr);

    // The strip is at least doubled up; auto mode may stack more columns
    // on short windows (see updateAutoColumns).
    bool twoColumn() const { return columns_ >= 2; }
    // `manual` records an explicit choice (grip toggle, Preferences,
    // workspace restore): automatic doubling leaves it alone afterwards.
    void setTwoColumn(bool on, bool manual = true);
    bool twoColumnManual() const { return twoColumnManual_; }

    // Hidden via Edit Toolbar… — lives under "…" instead of the strip.
    QStringList hiddenTools() const;
    void setHiddenTools(const QStringList& ids);

  signals:
    void editToolbarRequested();
    void screenModeCycleRequested();

  private:
    void rebuild();
    // Krita-style responsive columns: with no explicit choice the strip
    // uses the smallest column count whose height fits, up to the compact
    // floor's count, instead of forcing the window taller than the screen.
    // Never scrolls.
    void updateAutoColumns();
    void setColumns(int n);
    int needHeight(int columns) const;
    QSize minimumSizeHint() const override;
    void resizeEvent(QResizeEvent* event) override;
    void showFlyout(const ToolGroup& group, QToolButton* anchor);
    // Fire-time group resolution for button handlers (see makeToolButton).
    void showFlyoutFor(QToolButton* anchor);
    QToolButton* makeToolButton(const ToolGroup& group);
    void applyTheme();
    // Strip metrics follow tablet mode (pen-sized tap targets).
    int buttonSize() const;
    int iconSize() const;

    AppState* state_;
    QGridLayout* grid_ = nullptr;
    QVBoxLayout* outer_ = nullptr;
    QWidget* toolArea_ = nullptr;
    ColorWells* wells_ = nullptr;
    QToolButton* quickMaskButton_ = nullptr;
    QToolButton* screenModeButton_ = nullptr;
    QHash<int, QToolButton*> buttons_;   // group leader id -> button
    QHash<int, ToolId> groupSelection_;  // group leader id -> last used member
    QSet<int> hidden_;
    int columns_ = 1;
    bool twoColumnManual_ = false;
    // Last rebuild's grid shape: per-section button counts for the exact
    // auto-column fit check, plus the separator row count.
    QVector<int> sectionSizes_;
    int sepRows_ = 0;
    bool fitting_ = false;
};

}  // namespace pittore::ui
