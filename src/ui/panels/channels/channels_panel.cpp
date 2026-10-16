#include "ui/panels.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDataStream>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtMath>

#include <cmath>

#include "ui/ai_models.h"
#include "ui/icons.h"
#include "ui/tool_registry.h"

#include <chrono>

#include "engine/core/log.h"
#include "engine/compute/adjust.h"
#include "ui/curve_editor.h"
#include "ui/panels/shared/panel_helpers.h"

namespace pittore::ui {
namespace {


// ---------------------------------------------------------------------------
// Channels / Paths
// ---------------------------------------------------------------------------
class ChannelsPanel final : public QWidget {
  public:
    ChannelsPanel(AppState* state, QWidget* parent) : QWidget(parent), state_(state) {
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(0, 4, 0, 0);
        column->setSpacing(8);

        list_ = new QListWidget(this);
        list_->setIconSize(QSize(34, 24));
        column->addWidget(list_, 1);

        column->addWidget(makePanelFooter(
            state_,
            {{QStringLiteral("mask"), tr("Load channel as selection")},
             {QStringLiteral("quickmask"), tr("Save selection as channel")},
             {QStringLiteral("newlayer"), tr("Create new channel")},
             {QStringLiteral("trash"), tr("Delete channel")}},
            this, [](const QString&) {}));

        connect(state_, &AppState::activeDocumentChanged, this, [this] { rebuild(); });
        rebuild();
    }

  private:
    void rebuild() {
        list_->clear();
        DocumentItem* d = state_->activeDocument();
        if (!d) return;
        const ThemeColors c = colorsFor(state_->theme());
        const struct { const char* name; const char* shortcut; QColor tint; } channels[] = {
            {"RGB", "Ctrl+2", QColor(0xd0, 0xd0, 0xd0)},
            {"Red", "Ctrl+3", QColor(0xd0, 0x40, 0x40)},
            {"Green", "Ctrl+4", QColor(0x40, 0xd0, 0x40)},
            {"Blue", "Ctrl+5", QColor(0x40, 0x60, 0xd0)},
        };
        for (const auto& ch : channels) {
            // 1x + 2x bakes so the icons stay crisp on fractional scales.
            QIcon icon;
            for (const qreal dpr : {1.0, 2.0}) {
                QPixmap swatch(qMax(1, qRound(34.0 * dpr)),
                               qMax(1, qRound(24.0 * dpr)));
                swatch.setDevicePixelRatio(dpr);
                swatch.fill(ch.tint);
                icon.addPixmap(swatch);
            }
            auto* item = new QListWidgetItem(
                icon, QStringLiteral("%1\t%2").arg(ch.name, ch.shortcut));
            item->setForeground(c.text);
            list_->addItem(item);
        }
        list_->setCurrentRow(0);
    }

    AppState* state_;
    QListWidget* list_ = nullptr;
};

}  // namespace

QWidget* createChannelsPanel(AppState* state, QWidget* parent) {
    return new ChannelsPanel(state, parent);
}

}  // namespace pittore::ui
