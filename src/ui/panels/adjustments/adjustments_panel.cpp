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
// Adjustments
// ---------------------------------------------------------------------------
class AdjustmentsPanel final : public QWidget {
  public:
    AdjustmentsPanel(AppState* state, QWidget* parent) : QWidget(parent), state_(state) {
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(12, 12, 12, 12);
        column->setSpacing(8);
        column->addWidget(sectionLabel(tr("Add an adjustment"), state_, this));

        auto* grid = new QGridLayout;
        grid->setSpacing(6);
        // Live adjustment layers only: every button here creates a row the
        // engine actually composites (see AppState::addAdjustmentLayer).
        const struct {
            const char* name;
            const char* icon;
            int kind;
        } items[] = {
            {"Brightness/Contrast", "adjustments",
             static_cast<int>(
                 pittore::compute::AdjustmentKind::BrightnessContrast)},
            {"Levels", "properties",
             static_cast<int>(pittore::compute::AdjustmentKind::Levels)},
            {"Curves", "paths",
             static_cast<int>(pittore::compute::AdjustmentKind::Curves)},
            {"Exposure", "dodge",
             static_cast<int>(pittore::compute::AdjustmentKind::Exposure)},
            {"Vibrance", "color",
             static_cast<int>(pittore::compute::AdjustmentKind::Vibrance)},
            {"Hue/Saturation", "swatches",
             static_cast<int>(
                 pittore::compute::AdjustmentKind::HueSaturation)},
            {"Invert", "eraser-magic",
             static_cast<int>(pittore::compute::AdjustmentKind::Invert)},
            {"Threshold", "sharpen",
             static_cast<int>(pittore::compute::AdjustmentKind::Threshold)},
            {"Posterize", "count",
             static_cast<int>(pittore::compute::AdjustmentKind::Posterize)},
            {"Photo Filter", "gradient",
             static_cast<int>(pittore::compute::AdjustmentKind::PhotoFilter)},
            {"White Balance", "sampler",
             static_cast<int>(pittore::compute::AdjustmentKind::WhiteBalance)},
            {"Black & White", "channels",
             static_cast<int>(pittore::compute::AdjustmentKind::BlackWhite)},
            {"Channel Mixer", "mixer",
             static_cast<int>(pittore::compute::AdjustmentKind::ChannelMixer)},
            {"Color Balance", "histogram",
             static_cast<int>(pittore::compute::AdjustmentKind::ColorBalance)},
            {"Recolour", "recolor",
             static_cast<int>(pittore::compute::AdjustmentKind::Recolour)},
        };
        int row = 0;
        int col = 0;
        const ThemeColors c = colorsFor(state_->theme());
        for (const auto& item : items) {
            auto* button = new QToolButton(this);
            button->setAutoRaise(true);
            button->setIcon(chromeIcon(QString::fromUtf8(item.icon), c.text, c.accentText));
            button->setIconSize(QSize(18, 18));
            button->setToolTip(tr(item.name));
            button->setFixedSize(26, 24);
            const int kind = item.kind;
            connect(button, &QToolButton::clicked, this, [this, kind] {
                state_->addAdjustmentLayer(kind);
            });
            grid->addWidget(button, row, col);
            // A 3-across arrangement; rows grow as E4 lands kinds.
            if (++col >= 3) {
                col = 0;
                ++row;
            }
        }
        column->addLayout(grid);
        column->addStretch(1);
    }

  private:
    AppState* state_;
};

}  // namespace

QWidget* createAdjustmentsPanel(AppState* state, QWidget* parent) {
    return new AdjustmentsPanel(state, parent);
}

}  // namespace pittore::ui
