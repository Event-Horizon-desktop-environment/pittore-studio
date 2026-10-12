#include "ui/contextual_task_bar.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QToolButton>

#include "ui/icons.h"
#include "ui/tools/log/tool_log.h"

namespace pittore::ui {
namespace {
constexpr int kRadius = 8;
constexpr int kGripWidth = 14;
constexpr int kBottomMargin = 28;
}  // namespace

ContextualTaskBar::ContextualTaskBar(AppState* state, QWidget* parent)
    : QWidget(parent), state_(state) {
    setObjectName(QStringLiteral("contextualTaskBar"));
    setAttribute(Qt::WA_StyledBackground, false);
    setCursor(Qt::ArrowCursor);

    row_ = new QHBoxLayout(this);
    row_->setContentsMargins(kGripWidth + 8, 6, 10, 6);
    row_->setSpacing(6);

    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 3);
    shadow->setColor(QColor(0, 0, 0, 160));
    setGraphicsEffect(shadow);

    connect(state_, &AppState::taskContextChanged, this, [this](TaskContext c) {
        tool_log(state_->activeTool(), "contextual-task-bar/rebuild", taskContextName(c));
        rebuild();
    });
    connect(state_, &AppState::toolChanged, this, [this](ToolId id) {
        tool_log(id, "contextual-task-bar/rebuild", "toolChanged");
        rebuild();
    });
    connect(state_, &AppState::selectionChanged, this, [this] { rebuild(); });
    connect(state_, &AppState::layersChanged, this, [this] { rebuild(); });
    connect(state_, &AppState::themeChanged, this, [this] { rebuild(); });
    // No document = dimmed bar: these are all layer/pixel commands.
    connect(state_, &AppState::activeDocumentChanged, this, [this] { rebuild(); });
    connect(state_, &AppState::documentsChanged, this, [this] { rebuild(); });

    rebuild();
}

void ContextualTaskBar::setPinned(bool on) {
    pinned_ = on;
    if (on) pinnedPos_ = pos();
    else reflow();
}

void ContextualTaskBar::resetPosition() {
    pinned_ = false;
    reflow();
}

void ContextualTaskBar::reflow() {
    if (!parentWidget()) return;
    if (pinned_ && !pinnedPos_.isNull()) {
        move(pinnedPos_);
        return;
    }
    if (row_) row_->activate();
    adjustSize();
    const int x = (parentWidget()->width() - width()) / 2;
    const int y = parentWidget()->height() - height() - kBottomMargin;
    move(qMax(8, x), qMax(8, y));
}

void ContextualTaskBar::addDivider() {
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedSize(1, 20);
    line->setStyleSheet(
        QStringLiteral("background: %1;").arg(cssColor(colorsFor(state_->theme()).divider)));
    row_->addWidget(line);
}

void ContextualTaskBar::addCommand(const QString& id, const QString& label,
                                   const QString& iconKey, bool primary) {
    const ThemeColors c = colorsFor(state_->theme());
    auto* button = new QPushButton(label, this);
    if (!iconKey.isEmpty()) {
        button->setIcon(chromeIcon(iconKey, c.text, c.accentText));
        button->setIconSize(QSize(15, 15));
    }
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(
        primary
            ? QStringLiteral("QPushButton { background: %1; color: %2; border: none;"
                             " border-radius: 4px; padding: 4px 12px; }"
                             "QPushButton:hover { background: %3; }")
                  .arg(c.accent.name(), c.accentText.name(), c.accent.lighter(115).name())
            : QStringLiteral("QPushButton { background: transparent; border: none;"
                             " border-radius: 4px; padding: 4px 10px; color: %1; }"
                             "QPushButton:hover { background: %2; }")
                  .arg(c.text.name(), c.hover.name()));
    connect(button, &QPushButton::clicked, this, [this, id] { emit commandTriggered(id); });
    row_->addWidget(button);
}

void ContextualTaskBar::rebuild() {
    while (QLayoutItem* item = row_->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();  // hide now; the deferred delete would keep painting
            w->deleteLater();
        }
        delete item;
    }

    const ThemeColors c = colorsFor(state_->theme());
    const TaskContext context = state_->taskContext();

    switch (context) {
        case TaskContext::Selection: {
            // Blank prompt = remove the object.
            auto* prompt = new QLineEdit(this);
            prompt->setPlaceholderText(tr("Describe what to generate…"));
            prompt->setFixedWidth(210);
            prompt->setStyleSheet(
                QStringLiteral("QLineEdit { background: %1; border: 1px solid %2;"
                               " border-radius: 4px; padding: 4px 8px; }")
                    .arg(c.chromeSunken.name(), c.border.name()));
            row_->addWidget(prompt);
            addCommand(QStringLiteral("generative_fill"), tr("Generative Fill"),
                       QStringLiteral("sparkle"), true);
            addDivider();
            addCommand(QStringLiteral("select_subject"), tr("Select Subject"),
                       QStringLiteral("object-sel"));
            addCommand(QStringLiteral("remove_background"), tr("Remove Background"),
                       QStringLiteral("eraser"));
            addCommand(QStringLiteral("select_and_mask"), tr("Select and Mask…"),
                       QStringLiteral("mask"));
            addCommand(QStringLiteral("refine"), tr("Refine…"),
                       QStringLiteral("mask"));
            addCommand(QStringLiteral("enhance_edges"), tr("Enhance Edges"),
                       QStringLiteral("sparkle"));
            addCommand(QStringLiteral("auto_refine"), tr("Auto Refine"),
                       QStringLiteral("sparkle"));
            addDivider();
            addCommand(QStringLiteral("invert_selection"), tr("Invert"));
            addCommand(QStringLiteral("deselect"), tr("Deselect"));
            break;
        }

        case TaskContext::Text: {
            auto* family = new QComboBox(this);
            family->addItems({QStringLiteral("Inter"), QStringLiteral("Source Sans 3"),
                              QStringLiteral("IBM Plex Sans"), QStringLiteral("Noto Serif")});
            family->setFixedWidth(130);
            row_->addWidget(family);

            auto* size = new QComboBox(this);
            size->setEditable(true);
            size->addItems({QStringLiteral("12 pt"), QStringLiteral("18 pt"),
                            QStringLiteral("24 pt"), QStringLiteral("36 pt"),
                            QStringLiteral("72 pt")});
            size->setCurrentIndex(3);
            size->setFixedWidth(76);
            row_->addWidget(size);
            addDivider();
            addCommand(QStringLiteral("align_left"), tr("Left"));
            addCommand(QStringLiteral("align_center"), tr("Center"));
            addCommand(QStringLiteral("align_right"), tr("Right"));
            addDivider();
            addCommand(QStringLiteral("text_color"), tr("Color"), QStringLiteral("color"));
            addCommand(QStringLiteral("warp_text"), tr("Warp"));
            addCommand(QStringLiteral("commit_text"), tr("Done"), QStringLiteral("check"), true);
            break;
        }

        case TaskContext::Crop: {
            auto* label = new QLabel(tr("Crop"), this);
            label->setStyleSheet(QStringLiteral("color: %1;").arg(c.textDim.name()));
            row_->addWidget(label);
            auto* ratio = new QComboBox(this);
            ratio->addItems({tr("Original Ratio"), tr("1:1"), tr("4:5"), tr("16:9"), tr("Custom")});
            ratio->setFixedWidth(130);
            row_->addWidget(ratio);
            addDivider();
            addCommand(QStringLiteral("straighten"), tr("Straighten"), QStringLiteral("ruler"));
            addCommand(QStringLiteral("cancel_crop"), tr("Cancel"), QStringLiteral("close"));
            addCommand(QStringLiteral("apply_crop"), tr("Apply"), QStringLiteral("check"), true);
            break;
        }

        case TaskContext::Transform: {
            addCommand(QStringLiteral("flip_h"), tr("Flip Horizontal"));
            addCommand(QStringLiteral("flip_v"), tr("Flip Vertical"));
            addDivider();
            addCommand(QStringLiteral("warp"), tr("Warp"));
            addCommand(QStringLiteral("cancel_transform"), tr("Cancel"), QStringLiteral("close"));
            addCommand(QStringLiteral("apply_transform"), tr("Apply"), QStringLiteral("check"),
                       true);
            break;
        }

        case TaskContext::Path: {
            addCommand(QStringLiteral("path_to_selection"), tr("Make Selection"));
            addCommand(QStringLiteral("fill_path"), tr("Fill Path"));
            addCommand(QStringLiteral("stroke_path"), tr("Stroke Path"));
            addCommand(QStringLiteral("text_on_path"), tr("Text on Path"));
            addDivider();
            addCommand(QStringLiteral("path_to_shape"), tr("Convert to Shape"));
            break;
        }

        case TaskContext::ShapeLayer: {
            addCommand(QStringLiteral("shape_fill"), tr("Fill"), QStringLiteral("color"));
            addCommand(QStringLiteral("shape_stroke"), tr("Stroke"));
            addDivider();
            addCommand(QStringLiteral("combine_shapes"), tr("Combine Shapes"));
            break;
        }

        case TaskContext::GenerativeResult: {
            auto* label = new QLabel(tr("Variation 1 of 3"), this);
            label->setStyleSheet(QStringLiteral("color: %1;").arg(c.textDim.name()));
            addCommand(QStringLiteral("prev_variation"), QStringLiteral("‹"));
            row_->addWidget(label);
            addCommand(QStringLiteral("next_variation"), QStringLiteral("›"));
            addDivider();
            addCommand(QStringLiteral("generate_again"), tr("Generate"),
                       QStringLiteral("sparkle"), true);
            break;
        }

        case TaskContext::None:
        default: {
            // Idle: handy layer shortcuts.
            addCommand(QStringLiteral("select_subject"), tr("Select Subject"),
                       QStringLiteral("object-sel"));
            addCommand(QStringLiteral("remove_background"), tr("Remove Background"),
                       QStringLiteral("eraser"));
            addCommand(QStringLiteral("auto_refine"), tr("Auto Refine"),
                       QStringLiteral("sparkle"));
            addDivider();
            addCommand(QStringLiteral("adjustment_layer"), tr("Adjustment Layer"),
                       QStringLiteral("adjustments"));
            addCommand(QStringLiteral("add_mask"), tr("Add Mask"), QStringLiteral("mask"));
            break;
        }
    }

    // Overflow menu, always last.
    auto* more = new QToolButton(this);
    more->setToolTip(tr("More actions"));
    more->setIcon(chromeIcon(QStringLiteral("menu"), c.textDim, c.text));
    more->setIconSize(QSize(15, 15));
    more->setAutoRaise(true);
    more->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more);
    QAction* pin = menu->addAction(tr("Pin Bar Position"));
    pin->setCheckable(true);
    pin->setChecked(pinned_);
    connect(pin, &QAction::toggled, this, &ContextualTaskBar::setPinned);
    connect(menu->addAction(tr("Reset Bar Position")), &QAction::triggered, this,
            &ContextualTaskBar::resetPosition);
    menu->addSeparator();
    connect(menu->addAction(tr("Hide Bar")), &QAction::triggered, this, [this] { hide(); });
    more->setMenu(menu);
    row_->addWidget(more);

    // New children start hidden; show them so layout measures real content.
    for (int i = 0; i < row_->count(); ++i)
        if (QWidget* w = row_->itemAt(i)->widget()) w->show();
    row_->invalidate();
    row_->activate();
    adjustSize();
    reflow();
    // Disabled visual language, same contract as the options bar: with no
    // document every command dims instead of looking live.
    setEnabled(state_->activeDocument() != nullptr);
    update();
}

void ContextualTaskBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const ThemeColors c = colorsFor(state_->theme());

    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
    p.fillPath(path, c.chromeAlt);
    p.setPen(QPen(c.border, 1));
    p.drawPath(path);

    // Grip dots on the left edge.
    p.setPen(Qt::NoPen);
    p.setBrush(c.textDim);
    const int cx = kGripWidth / 2 + 2;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 2; ++j)
            p.drawEllipse(QPointF(cx - 2 + j * 4, height() / 2.0 - 5 + i * 5), 1.1, 1.1);
}

void ContextualTaskBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && event->position().x() < kGripWidth + 6) {
        dragging_ = true;
        dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ContextualTaskBar::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_ || !parentWidget()) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    QPoint target = parentWidget()->mapFromGlobal(event->globalPosition().toPoint() - dragOffset_);
    target.setX(qBound(4, target.x(), parentWidget()->width() - width() - 4));
    target.setY(qBound(4, target.y(), parentWidget()->height() - height() - 4));
    move(target);
    event->accept();
}

void ContextualTaskBar::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_) {
        dragging_ = false;
        pinned_ = true;
        pinnedPos_ = pos();
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ContextualTaskBar::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    QAction* pin = menu.addAction(tr("Pin Bar Position"));
    pin->setCheckable(true);
    pin->setChecked(pinned_);
    connect(pin, &QAction::toggled, this, &ContextualTaskBar::setPinned);
    connect(menu.addAction(tr("Reset Bar Position")), &QAction::triggered, this,
            &ContextualTaskBar::resetPosition);
    menu.addSeparator();
    connect(menu.addAction(tr("Hide Bar")), &QAction::triggered, this, [this] { hide(); });
    menu.exec(event->globalPos());
}

}  // namespace pittore::ui
