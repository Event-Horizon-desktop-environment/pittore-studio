#include "ui/tools_panel.h"

#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

#include <algorithm>
#include <vector>

#include "ui/icons.h"
#include "ui/tools/log/tool_log.h"

namespace pittore::ui {
namespace {

constexpr int kButton = 26;
constexpr int kIcon = 20;
// Auto mode may stack up to this many columns on short windows: the
// smallest whole-strip height a 720p-class screen can still fit with the
// chrome around it (issue #2). The grip / Preferences still pin one or two.
constexpr int kMaxColumns = 4;
// Tablet mode (pen taps): roomier strip targets, same icons.
constexpr int kTabletButton = 40;
constexpr int kTabletIcon = 32;
// Grid gap every decision is measured at; the strip never spreads rows to
// chase leftover room (that looks gappy) and never scrolls.
constexpr int kGridSpacing = 4;
// Hold duration before a press becomes a flyout (replaces DelayedPopup so the
// menu position is ours, conventional, instead of Qt's below-button default
// that spills a 38-row flyout over the strip).
constexpr int kFlyoutHoldMs = 450;

// Tool button with a flyout corner tick (theme CSS hides the
// stock indicator, so we draw our own) and hold-to-open flyout.
class FlyoutButton final : public QToolButton {
public:
    FlyoutButton(QWidget* parent) : QToolButton(parent) {}

    void setFlyoutMarker(bool on, QColor color) {
        marker_ = on;
        markerColor_ = color;
        update();
    }

    // Fired on long-press instead of the click action; ToolsPanel opens the
    // group's flyout anchored right of the strip.
    std::function<void()> flyoutRequested;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        QToolButton::mousePressEvent(event);
        if (event->button() == Qt::LeftButton && flyoutRequested) {
            pressed_ = true;
            QTimer::singleShot(kFlyoutHoldMs, this, [this] {
                if (pressed_) {
                    pressed_ = false;
                    menuShown_ = true;
                    flyoutRequested();
                }
            });
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        pressed_ = false;
        if (menuShown_) {
            // The press became a flyout: swallow the click so the tool does
            // not activate, and reset the visual press state ourselves.
            menuShown_ = false;
            setDown(false);
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        QToolButton::paintEvent(event);
        if (!marker_) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const int s = 4;  // triangle leg, with a 1px inset from the corner
        p.setPen(Qt::NoPen);
        p.setBrush(markerColor_);
        p.drawPolygon(QPolygonF({QPointF(width() - 1 - s, height() - 1),
                                 QPointF(width() - 1, height() - 1),
                                 QPointF(width() - 1, height() - 1 - s)}));
    }

private:
    bool marker_ = false;
    QColor markerColor_;
    bool pressed_ = false;
    bool menuShown_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// ColorWells
// ---------------------------------------------------------------------------
ColorWells::ColorWells(AppState* state, QWidget* parent) : QWidget(parent), state_(state) {
    setToolTip(tr("Foreground and background colour\nX swaps · D resets"));
    connect(state_, &AppState::colorsChanged, this, qOverload<>(&QWidget::update));
}

QSize ColorWells::sizeHint() const { return {48, 48}; }

QRectF ColorWells::foregroundRect() const { return QRectF(4, 10, 24, 24); }
QRectF ColorWells::backgroundRect() const { return QRectF(20, 26, 24, 24); }
QRectF ColorWells::swapRect() const { return QRectF(31, 1, 14, 14); }
QRectF ColorWells::resetRect() const { return QRectF(1, 38, 11, 11); }

void ColorWells::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const ThemeColors c = colorsFor(state_->theme());

    // Back well first so the front one overlaps.
    const QRectF bg = backgroundRect();
    p.fillRect(bg, state_->background());
    p.setPen(QPen(c.border, 1));
    p.drawRect(bg.adjusted(0.5, 0.5, -0.5, -0.5));

    const QRectF fg = foregroundRect();
    p.fillRect(fg, state_->foreground());
    p.setPen(QPen(c.text, 1));
    p.drawRect(fg.adjusted(0.5, 0.5, -0.5, -0.5));

    // Swap arrow (X).
    p.setPen(QPen(c.text, 1.2));
    const QRectF sw = swapRect();
    p.drawLine(QPointF(sw.left() + 2, sw.bottom() - 3), QPointF(sw.left() + 2, sw.top() + 3));
    p.drawLine(QPointF(sw.left() + 2, sw.top() + 3), QPointF(sw.right() - 2, sw.top() + 3));
    p.drawLine(QPointF(sw.right() - 2, sw.top() + 3), QPointF(sw.right() - 2, sw.bottom() - 4));
    p.setBrush(c.text);
    p.drawPolygon(QPolygonF({QPointF(sw.right() - 2, sw.bottom()),
                             QPointF(sw.right() - 5, sw.bottom() - 4),
                             QPointF(sw.right() + 1, sw.bottom() - 4)}));
    p.drawPolygon(QPolygonF({QPointF(sw.left() - 1, sw.top() + 3),
                             QPointF(sw.left() + 2, sw.top() - 1),
                             QPointF(sw.left() + 5, sw.top() + 3)}));

    // Reset mini-pair (D).
    p.setBrush(Qt::white);
    p.setPen(QPen(c.border, 1));
    const QRectF rs = resetRect();
    p.drawRect(QRectF(rs.left() + 4, rs.top() + 4, 7, 7));
    p.setBrush(Qt::black);
    p.drawRect(QRectF(rs.left(), rs.top(), 7, 7));
}

void ColorWells::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    if (swapRect().adjusted(-3, -3, 3, 3).contains(pos)) {
        state_->swapColors();
    } else if (resetRect().adjusted(-2, -2, 4, 4).contains(pos)) {
        state_->resetColors();
    } else if (foregroundRect().contains(pos)) {
        mouseDoubleClickEvent(event);
    } else if (backgroundRect().contains(pos)) {
        mouseDoubleClickEvent(event);
    }
    update();
}

void ColorWells::mouseDoubleClickEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    const bool isForeground = foregroundRect().contains(pos);
    if (!isForeground && !backgroundRect().contains(pos)) return;

    const QColor start = isForeground ? state_->foreground() : state_->background();
    const QColor picked = QColorDialog::getColor(
        start, this, isForeground ? tr("Foreground Color") : tr("Background Color"),
        QColorDialog::ShowAlphaChannel);
    if (!picked.isValid()) return;

    if (isForeground) state_->setForeground(picked);
    else state_->setBackground(picked);
    state_->addSwatch(picked);
}

// ---------------------------------------------------------------------------
// ToolsPanel
// ---------------------------------------------------------------------------
ToolsPanel::ToolsPanel(AppState* state, QWidget* parent) : QFrame(parent), state_(state) {
    setObjectName(QStringLiteral("toolsPanel"));
    setFrameShape(QFrame::NoFrame);

    auto* outer = new QVBoxLayout(this);
    outer_ = outer;
    outer->setContentsMargins(4, 6, 4, 8);
    outer->setSpacing(4);

    // Drag grip doubles as the one/two-column toggle.
    auto* grip = new QToolButton(this);
    grip->setAutoRaise(true);
    grip->setIconSize(QSize(14, 14));
    grip->setToolTip(tr("Toggle one/two column layout"));
    grip->setCursor(Qt::PointingHandCursor);
    connect(grip, &QToolButton::clicked, this, [this] { setTwoColumn(!twoColumn()); });
    outer->addWidget(grip, 0, Qt::AlignHCenter);
    grip->setProperty("roleKey", QStringLiteral("columns"));

    toolArea_ = new QWidget(this);
    grid_ = new QGridLayout(toolArea_);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(kGridSpacing);
    outer->addWidget(toolArea_, 0, Qt::AlignHCenter);

    outer->addStretch(1);

    // Footer: edit toolbar, colour wells, quick mask, screen mode.
    auto* edit = new QToolButton(this);
    edit->setAutoRaise(true);
    edit->setIconSize(QSize(kIcon, kIcon));
    edit->setToolTip(tr("Edit Toolbar…"));
    edit->setProperty("roleKey", QStringLiteral("edittoolbar"));
    connect(edit, &QToolButton::clicked, this, &ToolsPanel::editToolbarRequested);
    outer->addWidget(edit, 0, Qt::AlignHCenter);

    wells_ = new ColorWells(state_, this);
    outer->addWidget(wells_, 0, Qt::AlignHCenter);

    quickMaskButton_ = new QToolButton(this);
    quickMaskButton_->setAutoRaise(true);
    quickMaskButton_->setCheckable(true);
    quickMaskButton_->setIconSize(QSize(kIcon, kIcon));
    quickMaskButton_->setToolTip(tr("Edit in Quick Mask Mode (Q)"));
    quickMaskButton_->setProperty("roleKey", QStringLiteral("quickmask"));
    connect(quickMaskButton_, &QToolButton::toggled, state_, &AppState::setQuickMask);
    connect(state_, &AppState::quickMaskChanged, quickMaskButton_, &QToolButton::setChecked);
    outer->addWidget(quickMaskButton_, 0, Qt::AlignHCenter);

    screenModeButton_ = new QToolButton(this);
    screenModeButton_->setAutoRaise(true);
    screenModeButton_->setIconSize(QSize(kIcon, kIcon));
    screenModeButton_->setToolTip(tr("Change Screen Mode (F)"));
    screenModeButton_->setProperty("roleKey", QStringLiteral("screenmode"));
    connect(screenModeButton_, &QToolButton::clicked, this, &ToolsPanel::screenModeCycleRequested);
    outer->addWidget(screenModeButton_, 0, Qt::AlignHCenter);

    connect(state_, &AppState::toolChanged, this, [this](ToolId id) {
        tool_log(id, "tools-panel/group-selection", "flyout selection + strip rebuild");
        if (const ToolGroup* g = groupForTool(id)) {
            groupSelection_.insert(static_cast<int>(g->leader), id);
            rebuild();
            tool_log(id, "tools-panel/group-selection", "rebuild done");
        } else {
            tool_log(id, "tools-panel/group-selection", "tool is in no group");
        }
    });
    connect(state_, &AppState::themeChanged, this, [this] { applyTheme(); });
    connect(state_, &AppState::settingsChanged, this, [this] {
        rebuild();
        applyTheme();
    });

    rebuild();
    applyTheme();
}

void ToolsPanel::setTwoColumn(bool on, bool manual) {
    twoColumnManual_ = manual;
    setColumns(on ? 2 : 1);
}

void ToolsPanel::setColumns(int n) {
    n = qBound(1, n, kMaxColumns);
    if (columns_ == n) return;
    columns_ = n;
    rebuild();
}

void ToolsPanel::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    updateAutoColumns();
}

QSize ToolsPanel::minimumSizeHint() const {
    // Floor at the COMPACT form, not the current one: if the minimum tracked
    // the live layout, the window could never shrink far enough for
    // auto-columns to engage — it would stay taller than the screen instead
    // (issues #2/#4). Automatic mode floors at the quadruple-up form, the
    // shortest the whole set fits without scrolling. A pinned strip keeps
    // the doubled floor: that height is the user's own explicit choice.
    if (sectionSizes_.isEmpty()) return QWidget::minimumSizeHint();
    if (twoColumnManual_) return {columns_ * kButton + 12, needHeight(2)};
    return {kMaxColumns * kButton + 12, needHeight(kMaxColumns)};
}

int ToolsPanel::needHeight(int columns) const {
    if (sectionSizes_.isEmpty() || !outer_ || !grid_) return 0;
    // Exact row packing, separators included. Buttons pack densely with no
    // stranded holes (a new section completes the partial row first), so
    // the button rows are exactly ceil(total/columns); every separator is
    // one full row on top of that.
    int total = 0;
    for (int s : sectionSizes_) total += s;
    const int buttonRowCount = (total + columns - 1) / columns;
    const int gridRows = buttonRowCount + sepRows_;
    const int gridH = buttonRowCount * buttonSize() + sepRows_ * 1 +
                      std::max(0, gridRows - 1) * kGridSpacing;
    int rest = 0;
    const int n = outer_->count();
    for (int i = 0; i < n; ++i) {
        QLayoutItem* item = outer_->itemAt(i);
        if (!item || item->widget() == toolArea_) continue;
        rest += item->sizeHint().height();
    }
    int top = 0, bottom = 0;
    outer_->getContentsMargins(nullptr, &top, nullptr, &bottom);
    return gridH + rest + std::max(0, n - 1) * outer_->spacing() + top + bottom;
}

void ToolsPanel::updateAutoColumns() {
    if (twoColumnManual_ || fitting_ || !isVisible() || height() <= 0) return;
    // Smallest column count that fits: one while it fits, doubling up only
    // as far as needed (never scrolling) and no further, capped at the
    // compact floor's count so the strip always fits the minimum window.
    // The strip stays top-aligned and compact; any leftover room sits above
    // the pinned footer.
    int want = 1;
    while (want < kMaxColumns && needHeight(want) > height()) ++want;
    if (want == columns_) return;
    fitting_ = true;
    columns_ = want;
    rebuild();
    fitting_ = false;
}

QStringList ToolsPanel::hiddenTools() const {
    QStringList out;
    for (int id : hidden_) out << QString::number(id);
    return out;
}

void ToolsPanel::setHiddenTools(const QStringList& ids) {
    hidden_.clear();
    for (const QString& s : ids) hidden_.insert(s.toInt());
    rebuild();
}

QToolButton* ToolsPanel::makeToolButton(const ToolGroup& group) {
    const int leaderKey = static_cast<int>(group.leader);
    const ToolId shown = groupSelection_.value(leaderKey, group.leader);
    // Per-tool line: the strip is what "sets up" each tool at startup, so a
    // broken icon/tooltip shows up in that tool's own log, not in a crowd.
    tool_log(shown, "tools-panel/build-button", "begin");

    auto* button = new FlyoutButton(toolArea_);
    button->setAutoRaise(true);
    button->setCheckable(true);
    button->setFixedSize(buttonSize(), buttonSize());
    button->setIconSize(QSize(iconSize(), iconSize()));
    button->setProperty("toolId", static_cast<int>(shown));
    button->setProperty("groupLeader", leaderKey);
    button->setProperty("hasFlyout", group.members.size() > 1);
    button->setFlyoutMarker(group.members.size() > 1, colorsFor(state_->theme()).text);

    const QString shortcut = toolShortcutText(shown);
    button->setToolTip(shortcut.isEmpty()
                           ? toolName(shown)
                           : QStringLiteral("%1  (%2)").arg(toolName(shown), shortcut));
    button->setChecked(state_->activeTool() == shown ||
                       (groupForTool(state_->activeTool()) &&
                        groupForTool(state_->activeTool())->leader == group.leader));

    connect(button, &QToolButton::clicked, this, [this, shown] { state_->setActiveTool(shown); });

    if (group.members.size() > 1) {
        // Hold or right-click opens the flyout, anchored right of the strip
        // (never over the tools, however tall the menu gets). Handlers carry
        // only the button: the group is resolved live from its leader id, so
        // no heap-copied ToolGroup (and its member vector) rides in a functor
        // that outlives a strip rebuild.
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QToolButton::customContextMenuRequested, this,
                [this, button] { showFlyoutFor(button); });
        button->flyoutRequested = [this, button] { showFlyoutFor(button); };
    }
    tool_log(shown, "tools-panel/build-button", "done");
    return button;
}

void ToolsPanel::showFlyoutFor(QToolButton* anchor) {
    const int leader =
        anchor ? anchor->property("groupLeader").toInt() : -1;
    for (const ToolGroup& group : allGroups()) {
        if (static_cast<int>(group.leader) == leader) {
            showFlyout(group, anchor);
            return;
        }
    }
}
// Compact multi-column flyout (conventional): a Qt::Popup grid of icon+name
// buttons that closes on selection or outside click. A 38-row single-column
// menu would span the screen and cover the rulers; the grid stays beside the
// button.
class FlyoutPopup final : public QWidget {
public:
    FlyoutPopup(const ToolGroup& group, AppState* state, QWidget* parent)
        : QWidget(parent, Qt::Popup), state_(state) {
        setObjectName(QStringLiteral("toolFlyout"));
        setAttribute(Qt::WA_DeleteOnClose);
        const ThemeColors c = colorsFor(state->theme());
        setStyleSheet(
            QStringLiteral("#toolFlyout { background: %1; border: 1px solid %2; "
                           "border-radius: 6px; }")
                .arg(c.chromeAlt.name(), c.border.name()));
        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(4, 4, 4, 4);
        grid->setSpacing(2);
        constexpr int kColumns = 2;
        int i = 0;
        for (ToolId member : group.members) {
            tool_log(member, "tools-panel/flyout-button", "begin");
            auto* b = new QToolButton(this);
            b->setText(toolName(member));
            const QString shortcut = toolShortcutText(member);
            b->setToolTip(shortcut.isEmpty()
                              ? toolName(member)
                              : QStringLiteral("%1  (%2)").arg(toolName(member), shortcut));
            b->setIcon(toolIcon(member, c.text, c.accentText));
            const int flyIcon =
                (state_ && state_->settings().tabletMode) ? 32 : 20;
            b->setIconSize(QSize(flyIcon, flyIcon));
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            b->setCheckable(true);
            b->setChecked(member == state_->activeTool());
            b->setAutoRaise(true);
            b->setCursor(Qt::PointingHandCursor);
            connect(b, &QToolButton::clicked, this, [this, member] {
                state_->setActiveTool(member);
                // hide()+deleteLater, never close(): closing a popup from
                // inside its button's own mouse-release tears the native
                // window down under Qt's dispatch stack (SEGV in
                // QWidgetPrivate::setWinId / processCloseEvent on the way
                // back up). Hide is synchronous-safe; deletion is deferred.
                hide();
                deleteLater();
            });
            grid->addWidget(b, i / kColumns, i % kColumns);
            tool_log(member, "tools-panel/flyout-button", "done");
            ++i;
        }
    }

private:
    AppState* state_;
};

void ToolsPanel::showFlyout(const ToolGroup& group, QToolButton* anchor) {
    if (group.members.size() < 2) return;
    // One popup at a time: retire a stale one instead of stacking.
    if (QWidget* stale = findChild<QWidget*>(QStringLiteral("toolFlyout")))
        stale->deleteLater();
    auto* popup = new FlyoutPopup(group, state_, this);
    // conventional anchor: right of the strip, top-aligned with the button,
    // clamped into the screen. The grid is short enough to sit clear of the
    // rulers instead of spanning edge to edge.
    QPoint pos = anchor->mapToGlobal(QPoint(anchor->width() + 2, 0));
    if (QScreen* screen = QGuiApplication::screenAt(pos)) {
        const QRect avail = screen->availableGeometry();
        const QSize hint = popup->sizeHint();
        if (pos.y() + hint.height() > avail.bottom())
            pos.setY(qMax(avail.top(), avail.bottom() - hint.height()));
        if (pos.x() + hint.width() > avail.right())
            pos.setX(qMax(avail.left(), avail.right() - hint.width()));
    }
    popup->move(pos);
    popup->show();
}

int ToolsPanel::buttonSize() const {
    return (state_ && state_->settings().tabletMode) ? kTabletButton
                                                     : kButton;
}

int ToolsPanel::iconSize() const {
    return (state_ && state_->settings().tabletMode) ? kTabletIcon : kIcon;
}

void ToolsPanel::rebuild() {
    tool_log(state_->activeTool(), "tools-panel/rebuild", "begin");
    // Clear the tools, keep the footer.
    while (QLayoutItem* item = grid_->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();  // hide now; the deferred delete would keep painting
            w->deleteLater();
        }
        delete item;
    }
    buttons_.clear();

    const int columns = columns_;
    int row = 0;
    int column = 0;
    bool dividerPending = false;
    sectionSizes_.clear();
    sepRows_ = 0;

    // Visible groups up front: the fill-up flow below needs an indexable
    // list (lookahead for the trailing-divider guard).
    std::vector<const ToolGroup*> vis;
    for (const ToolGroup& group : allGroups()) {
        if (hidden_.contains(static_cast<int>(group.leader))) continue;
        vis.push_back(&group);
    }
    {
        std::string dump;
        ToolSection prev = vis.empty() ? ToolSection::Selection : vis[0]->section;
        int run = 0;
        for (const ToolGroup* g : vis) {
            if (g->section != prev) {
                dump += "[" + std::to_string(run) + "]";
                prev = g->section;
                run = 0;
            }
            ++run;
        }
        dump += "[" + std::to_string(run) + "]";
        std::fprintf(stderr, "[tools-panel] sections%s columns=%d\n", dump.c_str(),
                     columns_);
    }

    auto addSeparator = [&] {
        // A section can end mid-row in two-column mode: finish the partial
        // row first, otherwise the full-width line lands on top of the
        // button already sitting at (row, 0).
        if (column != 0) {
            ++row;
            column = 0;
        }
        auto* line = new QFrame(toolArea_);
        line->setFrameShape(QFrame::NoFrame);
        line->setFixedHeight(1);
        line->setStyleSheet(QStringLiteral("background: %1;")
                                .arg(cssColor(colorsFor(state_->theme()).divider)));
        grid_->addWidget(line, row, 0, 1, columns);
        ++row;
        ++sepRows_;
        column = 0;
    };

    for (size_t gi = 0; gi < vis.size(); ++gi) {
        const ToolGroup& group = *vis[gi];
        const bool lastGroup = (gi + 1 == vis.size());
        if (gi == 0 || group.section != vis[gi - 1]->section) {
            // Section boundary. On a fresh row the divider takes its own
            // row; on a partial row (two-column odd section above) the new
            // section's buttons first complete the shared row so no hole is
            // stranded mid-strip, and the divider goes below it.
            if (gi != 0) {
                if (columns == 1 || column == 0) {
                    addSeparator();
                } else {
                    dividerPending = true;
                }
            }
            sectionSizes_.append(0);
        }

        QToolButton* button = makeToolButton(group);
        buttons_.insert(static_cast<int>(group.leader), button);
        grid_->addWidget(button, row, column);
        ++sectionSizes_.last();
        if (++column >= columns) {
            column = 0;
            ++row;
            // A completed shared row earns its divider — unless this was
            // the final button, which must never leave a trailing line.
            if (dividerPending && !lastGroup) addSeparator();
            dividerPending = false;
        }
    }
    // New buttons start hidden; show them before measuring (layout skips hidden).
    for (int i = 0; i < grid_->count(); ++i)
        if (QWidget* w = grid_->itemAt(i)->widget()) w->show();
    applyTheme();
    grid_->invalidate();
    grid_->activate();
    toolArea_->adjustSize();
    setFixedWidth(columns_ * kButton + 12);
    tool_log(state_->activeTool(), "tools-panel/rebuild", "done");
    // Persona switches and tablet-mode changes alter the button count while
    // the height stands still: re-fit the columns to the current height.
    if (!fitting_) updateAutoColumns();
}

void ToolsPanel::applyTheme() {
    const ThemeColors c = colorsFor(state_->theme());
    setStyleSheet(QStringLiteral("#toolsPanel { background: %1; border-right: 1px solid %2; }")
                      .arg(c.chrome.name(), cssColor(c.divider)));

    // Icons are rebuilt for the theme, not recoloured.
    const auto buttons = findChildren<QToolButton*>();
    for (QToolButton* b : buttons) {
        const QVariant role = b->property("roleKey");
        if (role.isValid()) {
            b->setIcon(chromeIcon(role.toString(), c.text, c.accentText));
            b->setIconSize(QSize(role.toString() == QLatin1String("columns") ? 14 : iconSize(),
                                 role.toString() == QLatin1String("columns") ? 14 : iconSize()));
            continue;
        }
        const QVariant tool = b->property("toolId");
        if (tool.isValid())
            b->setIcon(toolIcon(static_cast<ToolId>(tool.toInt()), c.text, c.accentText));
        // Recolour the flyout ticks. Only buttons carrying hasFlyout are
        // FlyoutButtons: static_casting the popup's plain grid buttons would
        // write marker state past the end of the object (heap corruption).
        if (b->property("hasFlyout").isValid())
            static_cast<FlyoutButton*>(b)->setFlyoutMarker(
                b->property("hasFlyout").toBool(), c.text);
    }
    if (wells_) wells_->update();
}

}  // namespace pittore::ui
