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


class LibrariesPanel final : public QWidget {
  public:
    LibrariesPanel(AppState* state, QWidget* parent) : QWidget(parent), state_(state) {
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(12, 12, 12, 12);
        column->setSpacing(8);
        auto* search = new QLineEdit(this);
        search->setPlaceholderText(tr("Search assets"));
        column->addWidget(search);
        auto* list = new QListWidget(this);
        list->setViewMode(QListView::IconMode);
        list->setIconSize(QSize(56, 56));
        list->setResizeMode(QListView::Adjust);
        for (int i = 0; i < 9; ++i) {
            // 1x + 2x bakes so the icons stay crisp on fractional scales.
            QIcon icon;
            for (const qreal dpr : {1.0, 2.0}) {
                QPixmap swatch(qMax(1, qRound(56.0 * dpr)),
                               qMax(1, qRound(56.0 * dpr)));
                swatch.setDevicePixelRatio(dpr);
                swatch.fill(QColor::fromHsv((i * 37) % 360, 120, 200));
                icon.addPixmap(swatch);
            }
            list->addItem(new QListWidgetItem(icon, tr("Asset %1").arg(i + 1)));
        }
        column->addWidget(list, 1);
    }

  private:
    AppState* state_;
};

}  // namespace

QWidget* createLibrariesPanel(AppState* state, QWidget* parent) {
    return new LibrariesPanel(state, parent);
}

}  // namespace pittore::ui
