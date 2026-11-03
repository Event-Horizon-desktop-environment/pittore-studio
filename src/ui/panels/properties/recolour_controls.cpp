#include "ui/panels/properties/recolour_controls.h"

#include <QCheckBox>
#include <QColor>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>

#include <cmath>

#include "ui/app_state.h"

namespace pittore::ui {
namespace {

constexpr int kHueParam = 0;
constexpr int kSaturationParam = 1;
constexpr int kLightnessParam = 2;

QString trackStyle(const QString& stops) {
    return QStringLiteral(
               "QSlider::groove:horizontal {height:8px; border-radius:4px; "
               "background: qlineargradient(x1:0,y1:0,x2:1,y2:0, %1);} "
               "QSlider::handle:horizontal {width:14px; margin:-4px 0; "
               "border-radius:7px; background:#f2f2f2; border:1px solid "
               "#555;}")
        .arg(stops);
}

QString hueStops() {
    return QStringLiteral(
        "stop:0 #ff0000, stop:0.1667 #ffff00, stop:0.3333 #00ff00, "
        "stop:0.5 #00ffff, stop:0.6667 #0000ff, stop:0.8333 #ff00ff, "
        "stop:1 #ff0000");
}

QString saturationStops(float hue) {
    const QColor grey = QColor::fromHsvF(hue, 0.0f, 1.0f);
    const QColor full = QColor::fromHsvF(hue, 1.0f, 1.0f);
    return QStringLiteral("stop:0 %1, stop:1 %2")
        .arg(grey.name(), full.name());
}

QString lightnessStops(float hue, float saturation) {
    const QColor black(0, 0, 0);
    const QColor mid = QColor::fromHslF(hue, saturation, 0.5);
    const QColor white(255, 255, 255);
    return QStringLiteral("stop:0 %1, stop:0.5 %2, stop:1 %3")
        .arg(black.name(), mid.name(), white.name());
}

class RecolourControls final : public QWidget {
  public:
    RecolourControls(AppState* state, QWidget* parent)
        : QWidget(parent), state_(state) {
        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(6);
        grid->setColumnStretch(1, 1);

        DocumentItem* doc = state_->activeDocument();
        LayerItem* layer = state_->activeLayer();
        const float hue = layer && doc ? layer->adjustmentParams[kHueParam] : 0.0f;
        const float saturation =
            layer && doc ? layer->adjustmentParams[kSaturationParam] : 1.0f;
        const float lightness =
            layer && doc ? layer->adjustmentParams[kLightnessParam] : 0.0f;

        hue_ = addRow(grid, 0, tr("Hue"), 0, 3600,
                      static_cast<int>(std::lround(
                          qBound(0.0f, hue, 360.0f) * 10.0f)),
                      QStringLiteral("RecolourHueSlider"));
        hueSpin_ = addSpin(grid, 0, 0.0, 360.0, 0.1,
                           QStringLiteral(" °"),
                           QStringLiteral("RecolourHueSpin"), hue);

        saturation_ =
            addRow(grid, 1, tr("Saturation"), 0, 1000,
                   static_cast<int>(std::lround(
                       qBound(0.0f, saturation, 1.0f) * 1000.0f)),
                   QStringLiteral("RecolourSaturationSlider"));
        saturationSpin_ = addSpin(grid, 1, 0.0, 100.0, 0.1,
                                  QStringLiteral(" %"),
                                  QStringLiteral("RecolourSaturationSpin"),
                                  saturation * 100.0f);

        lightness_ =
            addRow(grid, 2, tr("Lightness"), -1000, 1000,
                   static_cast<int>(std::lround(
                       qBound(-1.0f, lightness, 1.0f) * 1000.0f)),
                   QStringLiteral("RecolourLightnessSlider"));
        lightnessSpin_ = addSpin(grid, 2, -100.0, 100.0, 0.1,
                                 QStringLiteral(" %"),
                                 QStringLiteral("RecolourLightnessSpin"),
                                 lightness * 100.0f);

        auto* attach = new QCheckBox(tr("Attach to layer below"), this);
        attach->setObjectName(QStringLiteral("RecolourAttach"));
        attach->setToolTip(
            tr("Clip this adjustment to the layer below so it does not grade "
               "the rest of the stack."));
        attach->setChecked(layer && layer->clipped);
        grid->addWidget(attach, 3, 0, 1, 3);
        connect(attach, &QCheckBox::toggled, this, [this](bool on) {
            LayerItem* current = state_->activeLayer();
            if (!current || current->clipped == on) return;
            // layersChanged rebuilds this panel, so run the mutation after the
            // checkbox signal has finished.
            QTimer::singleShot(0, this, [this] {
                state_->toggleLayerClipped();
            });
        });

        connect(hue_, &QSlider::sliderPressed, this,
                [this] { beginDrag(hue_); });
        connect(saturation_, &QSlider::sliderPressed, this,
                [this] { beginDrag(saturation_); });
        connect(lightness_, &QSlider::sliderPressed, this,
                [this] { beginDrag(lightness_); });
        connect(hue_, &QSlider::sliderReleased, this, [this] { endDrag(); });
        connect(saturation_, &QSlider::sliderReleased, this,
                [this] { endDrag(); });
        connect(lightness_, &QSlider::sliderReleased, this,
                [this] { endDrag(); });

        connect(hue_, &QSlider::valueChanged, this, [this](int v) {
            writeValue(kHueParam, v / 10.0);
            refreshRamps();
        });
        connect(saturation_, &QSlider::valueChanged, this, [this](int v) {
            writeValue(kSaturationParam, v / 1000.0);
            refreshRamps();
        });
        connect(lightness_, &QSlider::valueChanged, this, [this](int v) {
            writeValue(kLightnessParam, v / 1000.0);
        });

        connect(hueSpin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (syncing_) return;
            {
                const QSignalBlocker block(hue_);
                hue_->setValue(static_cast<int>(std::lround(v * 10.0)));
            }
            writeCommitted(kHueParam, v);
            refreshRamps();
        });
        connect(saturationSpin_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) {
                    if (syncing_) return;
                    {
                        const QSignalBlocker block(saturation_);
                        saturation_->setValue(
                            static_cast<int>(std::lround(v * 10.0)));
                    }
                    writeCommitted(kSaturationParam, v / 100.0);
                    refreshRamps();
                });
        connect(lightnessSpin_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) {
                    if (syncing_) return;
                    {
                        const QSignalBlocker block(lightness_);
                        lightness_->setValue(
                            static_cast<int>(std::lround(v * 10.0)));
                    }
                    writeCommitted(kLightnessParam, v / 100.0);
                });

        refreshRamps();
    }

  private:
    QSlider* addRow(QGridLayout* grid, int row, const QString& label, int lo,
                    int hi, int value, const QString& objectName) {
        auto* name = new QLabel(label, this);
        auto* slider = new QSlider(Qt::Horizontal, this);
        slider->setObjectName(objectName);
        slider->setRange(lo, hi);
        slider->setValue(qBound(lo, value, hi));
        grid->addWidget(name, row, 0);
        grid->addWidget(slider, row, 1);
        return slider;
    }

    QDoubleSpinBox* addSpin(QGridLayout* grid, int row, double lo, double hi,
                            double step, const QString& suffix,
                            const QString& objectName, double value) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setObjectName(objectName);
        spin->setRange(lo, hi);
        spin->setDecimals(1);
        spin->setSingleStep(step);
        spin->setSuffix(suffix);
        spin->setKeyboardTracking(false);
        spin->setFixedWidth(96);
        spin->setValue(qBound(lo, value, hi));
        grid->addWidget(spin, row, 2);
        return spin;
    }

    void beginDrag(QSlider* slider) {
        if (state_->undoStepActive()) return;
        state_->beginUndoStep();
        sliderGesture_ = true;
        dragSlider_ = slider;
        sliderStart_ = slider->value();
    }

    void endDrag() {
        if (!sliderGesture_) return;
        sliderGesture_ = false;
        const int now = dragSlider_ ? dragSlider_->value() : sliderStart_;
        dragSlider_ = nullptr;
        if (now == sliderStart_)
            state_->discardUndoStep();
        else
            state_->commitUndoStep(tr("Recolour"),
                                   QStringLiteral("adjustments"));
    }

    void writeValue(int param, double value) {
        const bool wasSyncing = syncing_;
        syncing_ = true;
        {
            const QSignalBlocker block(hueSpin_);
            const QSignalBlocker blockSat(saturationSpin_);
            const QSignalBlocker blockLight(lightnessSpin_);
            if (param == kHueParam)
                hueSpin_->setValue(value);
            else if (param == kSaturationParam)
                saturationSpin_->setValue(value * 100.0);
            else
                lightnessSpin_->setValue(value * 100.0);
        }
        state_->setAdjustmentParam(param, static_cast<float>(value));
        syncing_ = wasSyncing;
    }

    void writeCommitted(int param, double value) {
        if (state_->undoStepActive()) {
            state_->setAdjustmentParam(param, static_cast<float>(value));
            return;
        }
        state_->beginUndoStep();
        if (state_->setAdjustmentParam(param, static_cast<float>(value)))
            state_->commitUndoStep(tr("Recolour"),
                                   QStringLiteral("adjustments"));
        else
            state_->discardUndoStep();
    }

    void refreshRamps() {
        const float hue = qBound(0.0f, hue_->value() / 3600.0f, 1.0f);
        const float saturation =
            qBound(0.0f, saturation_->value() / 1000.0f, 1.0f);
        hue_->setStyleSheet(trackStyle(hueStops()));
        saturation_->setStyleSheet(trackStyle(saturationStops(hue)));
        lightness_->setStyleSheet(
            trackStyle(lightnessStops(hue, saturation)));
    }

    AppState* state_ = nullptr;
    QSlider* hue_ = nullptr;
    QSlider* saturation_ = nullptr;
    QSlider* lightness_ = nullptr;
    QDoubleSpinBox* hueSpin_ = nullptr;
    QDoubleSpinBox* saturationSpin_ = nullptr;
    QDoubleSpinBox* lightnessSpin_ = nullptr;
    bool syncing_ = false;
    bool sliderGesture_ = false;
    QSlider* dragSlider_ = nullptr;
    int sliderStart_ = 0;
};

}  // namespace

QWidget* createRecolourControls(AppState* state, QWidget* parent) {
    return new RecolourControls(state, parent);
}

}  // namespace pittore::ui
