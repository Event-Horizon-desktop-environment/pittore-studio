#include "ui/panels/properties/recolour_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QConicalGradient>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/compute/adjust.h"
#include "ui/adjustment_presets.h"
#include "ui/app_state.h"
#include "ui/curve_editor.h"
#include "ui/panels/properties/recolour_controls.h"

namespace pittore::ui {
namespace {

constexpr int kHueParam = 0;
constexpr int kSaturationParam = 1;
constexpr int kLightnessParam = 2;
constexpr int kHueSatKind =
    static_cast<int>(pittore::compute::AdjustmentKind::HueSaturation);
constexpr int kLevelsKind =
    static_cast<int>(pittore::compute::AdjustmentKind::Levels);
constexpr int kWhiteBalanceKind =
    static_cast<int>(pittore::compute::AdjustmentKind::WhiteBalance);
constexpr int kRecolourKind =
    static_cast<int>(pittore::compute::AdjustmentKind::Recolour);
constexpr int kCurvesKind =
    static_cast<int>(pittore::compute::AdjustmentKind::Curves);

QString dialogStyle() {
    return QStringLiteral(
        "QDialog { background:#2b2b2b; }"
        "QLabel { color:#d6d6d6; font-size:12px; }"
        "QPushButton { background:transparent; border:0; color:#cfcfcf;"
        "  padding:4px 6px; font-size:12px; }"
        "QPushButton:hover { color:#ffffff; }"
        "QPushButton:disabled { color:#666666; }"
        "QDoubleSpinBox { background:#3a3a3a; border:1px solid #4a4a4a;"
        "  border-radius:3px; color:#ffffff; padding:2px 4px; }"
        "QComboBox { background:#3a3a3a; border:1px solid #4a4a4a;"
        "  border-radius:3px; color:#ffffff; padding:2px 6px; }"
        "QComboBox QAbstractItemView { background:#2b2b2b; color:#e6e6e6;"
        "  selection-background-color:#3d6fb6; }"
        "QCheckBox { color:#d6d6d6; font-size:12px; }");
}

QString trackStyle(const QString& stops) {
    return QStringLiteral(
               "QSlider::groove:horizontal {height:8px; border-radius:4px; "
               "background: qlineargradient(x1:0,y1:0,x2:1,y2:0, %1);} "
               "QSlider::handle:horizontal {width:14px; margin:-4px 0; "
               "border-radius:7px; background:#f2f2f2; border:1px solid "
               "#555;}")
        .arg(stops);
}

QString rampStops() { return QStringLiteral("stop:0 #000000, stop:1 #ffffff"); }
QString tempStops() {
    return QStringLiteral("stop:0 #27408b, stop:0.5 #e8e0d0, stop:1 #ffb43a");
}
QString tintStops() {
    return QStringLiteral("stop:0 #2fbf4f, stop:0.5 #a8a8a8, stop:1 #d65ac0");
}
QString rainbowStops() {
    return QStringLiteral(
        "stop:0 #ff0000, stop:0.1667 #ffff00, stop:0.3333 #00ff00, "
        "stop:0.5 #00ffff, stop:0.6667 #0000ff, stop:0.8333 #ff00ff, "
        "stop:1 #ff0000");
}

QString trackFor(int kind, int pi) {
    if (kind == kLevelsKind) return rampStops();
    if (kind == kWhiteBalanceKind)
        return pi == 0 ? tempStops() : tintStops();
    if (kind == kHueSatKind) {
        if (pi == 0) return rainbowStops();
        if (pi == 1)
            return QStringLiteral("stop:0 #808080, stop:1 #ff2a1a");
        return rampStops();
    }
    return rampStops();
}

double factorFor(int kind, int pi) {
    if (kind == kLevelsKind) return 100.0;
    if (kind == kWhiteBalanceKind) return pi == 0 ? 1.0 : 100.0;
    if (kind == kHueSatKind) return pi == 0 ? 1.0 : 100.0;
    return 100.0;
}

QString suffixFor(int kind, int pi) {
    if (kind == kLevelsKind) return QStringLiteral(" %");
    if (kind == kWhiteBalanceKind)
        return pi == 0 ? QStringLiteral(" K") : QStringLiteral(" %");
    if (kind == kHueSatKind)
        return pi == 0 ? QStringLiteral(" \u00b0") : QStringLiteral(" %");
    return QStringLiteral(" %");
}

QImage histogramPixmap(const QImage& source, int w, int h) {
    QImage out(w, h, QImage::Format_RGB32);
    out.fill(QColor(0x18, 0x18, 0x18));
    if (source.isNull()) return out;
    std::array<int, 256> bins{};
    const QImage s = source.scaled(320, 200, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
    for (int y = 0; y < s.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(s.constScanLine(y));
        for (int x = 0; x < s.width(); ++x) {
            const int lum = qGray(row[x]);
            ++bins[static_cast<std::size_t>(lum)];
        }
    }
    const int peak = *std::max_element(bins.begin(), bins.end());
    if (peak <= 0) return out;
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QColor(0x9a, 0x9a, 0x9a));
    for (int i = 0; i < 256; ++i) {
        const int bh = static_cast<int>(
            std::lround(std::log1p(bins[static_cast<std::size_t>(i)]) /
                        std::log1p(peak) * (h - 4)));
        const int x = i * w / 256;
        p.drawLine(x, h - 1, x, h - 1 - bh);
    }
    return out;
}

QImage colourWheel(int size) {
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPointF c(size / 2.0, size / 2.0);
    const double outer = size / 2.0 - 2.0;
    const double inner = outer * 0.62;
    QConicalGradient hue(c, 90.0);
    for (int i = 0; i <= 12; ++i)
        hue.setColorAt(i / 12.0, QColor::fromHsvF((i % 12) / 12.0, 1.0, 1.0));
    QPainterPath ring;
    ring.setFillRule(Qt::OddEvenFill);
    ring.addEllipse(c, outer, outer);
    ring.addEllipse(c, inner, inner);
    p.fillPath(ring, hue);
    p.setPen(QPen(QColor(0x1e, 0x1e, 0x1e), 1));
    p.drawEllipse(c, outer, outer);
    p.drawEllipse(c, inner, inner);
    return img;
}

class AdjustmentDialog final : public QDialog {
  public:
    AdjustmentDialog(AppState* state, int layerIndex, QWidget* parent)
        : QDialog(parent), state_(state), layerIndex_(layerIndex) {
        DocumentItem* doc = state_->activeDocument();
        LayerItem* layer =
            doc && layerIndex_ >= 0 && layerIndex_ < doc->layers.size()
                ? &doc->layers[layerIndex_]
                : nullptr;
        kind_ = layer ? layer->adjustmentKind : -1;

        setObjectName(QStringLiteral("AdjustmentDialog"));
        setProperty("layerIndex", layerIndex);
        setProperty("adjustmentKind", kind_);
        setWindowTitle(layer ? layer->name : tr("Adjustment"));
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_StyledBackground, true);
        setModal(false);
        setStyleSheet(dialogStyle());
        setFixedWidth(kind_ == kHueSatKind ? 470 : 500);

        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(10, 6, 10, 10);
        column->setSpacing(6);

        auto* header = new QWidget(this);
        header->setObjectName(QStringLiteral("AdjustmentHeader"));
        header->setFixedHeight(26);
        header->installEventFilter(this);
        auto* head = new QHBoxLayout(header);
        head->setContentsMargins(0, 0, 0, 0);
        auto* title = new QLabel(layer ? layer->name : tr("Adjustment"), header);
        title->setAlignment(Qt::AlignCenter);
        QFont tf = title->font();
        tf.setPixelSize(13);
        title->setFont(tf);
        auto* closeButton = new QPushButton(QStringLiteral("\u2715"), header);
        closeButton->setFixedSize(24, 22);
        closeButton->setToolTip(tr("Close"));
        head->addStretch(1);
        head->addWidget(title);
        head->addStretch(1);
        head->addWidget(closeButton);
        column->addWidget(header);
        connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

        auto* presetRow = new QHBoxLayout;
        presetRow->setContentsMargins(0, 0, 0, 0);
        presetRow->setSpacing(4);
        auto* addPreset = new QPushButton(tr("Add Preset"), this);
        presetRow->addWidget(addPreset);
        presetRow->addStretch(1);
        auto* merge = new QPushButton(tr("Merge"), this);
        auto* del = new QPushButton(tr("Delete"), this);
        auto* reset = new QPushButton(tr("Reset"), this);
        merge->setEnabled(false);
        del->setEnabled(false);
        presetRow->addWidget(merge);
        presetRow->addWidget(del);
        presetRow->addWidget(reset);
        column->addLayout(presetRow);
        connect(addPreset, &QPushButton::clicked, this, [this] {
            state_->setStatusHint(
                tr("Adjustment presets are not available yet."));
        });

        column->addSpacing(2);
        if (kind_ == kRecolourKind) {
            column->addWidget(createRecolourControls(state_, this, false));
        } else {
            buildParamRows(column, layer);
        }

        auto* footer = new QHBoxLayout;
        footer->setContentsMargins(0, 6, 0, 0);
        footer->setSpacing(8);
        auto* opacityLabel = new QLabel(tr("Opacity:"), this);
        opacity_ = new QComboBox(this);
        opacity_->setFixedWidth(88);
        for (int p = 0; p <= 100; ++p)
            opacity_->addItem(QStringLiteral("%1 %").arg(p), p);
        footer->addWidget(opacityLabel);
        footer->addWidget(opacity_);
        footer->addStretch(1);
        auto* blendLabel = new QLabel(tr("Blend Mode:"), this);
        blend_ = new QComboBox(this);
        blend_->setMinimumWidth(170);
        const QStringList& names = blendModeNames();
        for (int i = 0; i < names.size(); ++i) {
            if (blendModeIsSeparatorBefore(i))
                blend_->insertSeparator(blend_->count());
            blend_->addItem(names.at(i));
        }
        footer->addWidget(blendLabel);
        footer->addWidget(blend_);
        column->addLayout(footer);

        if (layer) {
            {
                const QSignalBlocker block(opacity_);
                opacity_->setCurrentIndex(layer->opacity);
            }
            {
                const QSignalBlocker block(blend_);
                blend_->setCurrentText(layer->blendMode);
            }
        }
        connect(opacity_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int index) {
                    if (index >= 0) state_->setActiveLayerOpacity(index);
                });
        connect(blend_, &QComboBox::currentTextChanged, this,
                [this](const QString& name) {
                    if (!name.isEmpty())
                        state_->setActiveLayerBlendMode(name);
                });
        connect(reset, &QPushButton::clicked, this, [this] {
            int count = 0;
            const AdjustmentPreset* presets = adjustmentPresets(kind_, &count);
            if (presets && count > 0) {
                state_->beginUndoStep();
                if (state_->setAdjustmentParams(layerIndex_, presets[0].params))
                    state_->commitUndoStep(tr("Reset"),
                                           QStringLiteral("adjustments"));
                else
                    state_->discardUndoStep();
            }
            state_->setActiveLayerOpacity(100);
            state_->setActiveLayerBlendMode(QStringLiteral("Normal"));
            const int index = layerIndex_;
            QWidget* owner = parentWidget();
            QTimer::singleShot(0, this, [this, index, owner] {
                close();
                if (owner) showAdjustmentDialog(state_, index, owner);
            });
        });
        connect(state_, &AppState::activeLayerChanged, this, [this] {
            DocumentItem* d = state_->activeDocument();
            if (!d || d->activeLayer != layerIndex_) close();
        });
        connect(state_, &AppState::layersChanged, this, [this] {
            DocumentItem* d = state_->activeDocument();
            if (!d || layerIndex_ >= d->layers.size() ||
                d->layers[layerIndex_].kind != LayerItem::Kind::Adjustment)
                close();
        });

        adjustSize();
        if (parent) {
            const QRect pr = parent->window()->geometry();
            move(pr.center() - rect().center());
        }
    }

  protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        if (obj->objectName() == QStringLiteral("AdjustmentHeader")) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto* me = static_cast<QMouseEvent*>(event);
                if (me->button() == Qt::LeftButton) {
                    dragging_ = true;
                    dragOffset_ = me->globalPosition().toPoint() - pos();
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove && dragging_) {
                auto* me = static_cast<QMouseEvent*>(event);
                move(me->globalPosition().toPoint() - dragOffset_);
                return true;
            } else if (event->type() == QEvent::MouseButtonRelease) {
                dragging_ = false;
                return true;
            }
        }
        return QDialog::eventFilter(obj, event);
    }

  private:
    void buildParamRows(QVBoxLayout* column, LayerItem* layer) {
        DocumentItem* doc = state_->activeDocument();
        if (kind_ == kLevelsKind) {
            auto* hist = new QLabel(this);
            const QImage source = doc ? doc->composite : QImage();
            hist->setPixmap(QPixmap::fromImage(
                histogramPixmap(source, 470, 78)));
            hist->setFixedHeight(78);
            column->addWidget(hist);
            auto* channelRow = new QHBoxLayout;
            auto* rgb = new QComboBox(this);
            rgb->addItems({tr("RGB"), tr("Red"), tr("Green"), tr("Blue")});
            rgb->setFixedWidth(80);
            auto* channel = new QComboBox(this);
            channel->addItems({tr("Master"), tr("Shadows"), tr("Midtones"),
                               tr("Highlights")});
            channelRow->addWidget(rgb);
            channelRow->addWidget(channel, 1);
            column->addLayout(channelRow);
        } else if (kind_ == kHueSatKind) {
            auto* hsv = new QCheckBox(tr("HSV"), this);
            column->addWidget(hsv);
            auto* wheel = new QLabel(this);
            wheel->setPixmap(QPixmap::fromImage(colourWheel(156)));
            wheel->setAlignment(Qt::AlignCenter);
            column->addWidget(wheel);
        } else if (kind_ == kWhiteBalanceKind) {
            auto* picker = new QPushButton(tr("Picker"), this);
            picker->setToolTip(
                tr("Sample a neutral point on the canvas with the eyedropper."));
            connect(picker, &QPushButton::clicked, this, [this] {
                state_->setStatusHint(
                    tr("Use the Eyedropper tool to sample a neutral point."));
            });
            column->addWidget(picker, 0, Qt::AlignLeft);
        }

        if (kind_ == kCurvesKind) {
            auto* channel = new QComboBox(this);
            channel->addItems({tr("RGB"), tr("Red"), tr("Green"), tr("Blue")});
            column->addWidget(channel);
            auto* editor = new CurveEditor(this);
            editor->setMinimumSize(240, 220);
            const auto channelPoints = [layer](int ch) {
                const LayerItem* l = layer;
                if (!l) return QVector<QPointF>{{0.0, 0.0}, {1.0, 1.0}};
                const QVector<QPointF>* q = &l->adjustmentCurve;
                if (ch == 1) q = &l->adjustmentCurveR;
                if (ch == 2) q = &l->adjustmentCurveG;
                if (ch == 3) q = &l->adjustmentCurveB;
                return q->isEmpty() ? QVector<QPointF>{{0.0, 0.0}, {1.0, 1.0}}
                                    : *q;
            };
            editor->setPoints(channelPoints(0));
            connect(channel, &QComboBox::currentIndexChanged, this,
                    [editor, channelPoints](int ch) {
                        editor->setPoints(channelPoints(ch));
                    });
            editor->onChanged_ = [this, editor, channel] {
                const int ch = channel->currentIndex();
                state_->beginUndoStep();
                const bool ok =
                    ch == 0
                        ? state_->setAdjustmentCurve(editor->points())
                        : state_->setAdjustmentCurveForChannel(ch - 1,
                                                               editor->points());
                if (ok)
                    state_->commitUndoStep(tr("Curves"),
                                           QStringLiteral("adjustments"));
                else
                    state_->discardUndoStep();
            };
            column->addWidget(editor);
            return;
        }
        const QVector<AdjustmentParamDesc> descs =
            adjustmentParamDescs(kind_);
        if (descs.isEmpty() || !layer) {
            auto* note = new QLabel(tr("No live controls for this adjustment yet."),
                                    this);
            note->setWordWrap(true);
            column->addWidget(note);
            return;
        }
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 4, 0, 0);
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(5);
        grid->setColumnStretch(1, 1);
        int gridRow = 0;
        for (int pi = 0; pi < descs.size(); ++pi) {
            const AdjustmentParamDesc& dd = descs[pi];
            QString section;
            if (kind_ == static_cast<int>(
                            pittore::compute::AdjustmentKind::ColorBalance) &&
                pi % 3 == 0)
                section = pi == 0 ? tr("Shadows")
                          : pi == 3 ? tr("Midtones")
                                    : tr("Highlights");
            if (kind_ == static_cast<int>(
                            pittore::compute::AdjustmentKind::ChannelMixer) &&
                (pi == 0 || pi == 3 || pi == 6 || pi == 9))
                section = pi == 0 ? tr("Red Output")
                          : pi == 3 ? tr("Green Output")
                          : pi == 6 ? tr("Blue Output")
                                    : tr("Constants");
            if (kind_ == static_cast<int>(
                            pittore::compute::AdjustmentKind::BlackWhite) &&
                pi == 0)
                section = tr("Channel Weights");
            if (!section.isEmpty()) {
                auto* head = new QLabel(section, this);
                QFont hf = head->font();
                hf.setBold(true);
                hf.setPixelSize(11);
                head->setFont(hf);
                head->setStyleSheet(QStringLiteral("color:#b9b9b9;"));
                grid->addWidget(head, gridRow++, 0, 1, 3);
            }
            const double factor = factorFor(kind_, pi);
            const QString suffix = suffixFor(kind_, pi);
            auto* label = new QLabel(dd.label, this);
            auto* slider = new QSlider(Qt::Horizontal, this);
            const int span = dd.sliderMax - dd.sliderMin;
            slider->setRange(0, span > 0 ? span : 1);
            slider->setStyleSheet(trackStyle(trackFor(kind_, pi)));
            auto* spin = new QDoubleSpinBox(this);
            spin->setRange(dd.realMin * factor, dd.realMax * factor);
            spin->setDecimals(dd.realMax - dd.realMin > 100.0 ? 0 : 1);
            spin->setSingleStep((dd.realMax - dd.realMin) * factor / 100.0);
            spin->setSuffix(suffix);
            spin->setFixedWidth(92);
            const double cur = layer->adjustmentParams[pi];
            {
                const QSignalBlocker b1(slider), b2(spin);
                const int pos = qBound(
                    0,
                    static_cast<int>(std::lround(
                        (cur - dd.realMin) /
                        (dd.realMax - dd.realMin) * span)),
                    span);
                slider->setValue(pos);
                spin->setValue(cur * factor);
            }
            connect(slider, &QSlider::sliderPressed, this, [this, slider] {
                if (!state_->undoStepActive()) {
                    state_->beginUndoStep();
                    drag_ = true;
                    dragStart_ = slider->value();
                }
            });
            connect(slider, &QSlider::sliderReleased, this,
                    [this, slider] {
                        if (!drag_) return;
                        drag_ = false;
                        if (slider->value() == dragStart_)
                            state_->discardUndoStep();
                        else
                            state_->commitUndoStep(
                                tr("Adjustment"),
                                QStringLiteral("adjustments"));
                    });
            connect(slider, &QSlider::valueChanged, this,
                    [this, pi, dd, span, factor, spin](int v) {
                        if (span <= 0) return;
                        const double real =
                            dd.realMin +
                            (dd.realMax - dd.realMin) * v / span;
                        const QSignalBlocker block(spin);
                        spin->setValue(real * factor);
                        state_->setAdjustmentParamAt(
                            layerIndex_, pi, static_cast<float>(real));
                    });
            connect(spin, &QDoubleSpinBox::valueChanged, this,
                    [this, pi, dd, factor, slider](double v) {
                        const double real = v / factor;
                        const QSignalBlocker block(slider);
                        const int span = dd.sliderMax - dd.sliderMin;
                        slider->setValue(qBound(
                            0,
                            static_cast<int>(std::lround(
                                (real - dd.realMin) /
                                (dd.realMax - dd.realMin) * span)),
                            span));
                        state_->beginUndoStep();
                        if (state_->setAdjustmentParamAt(
                                layerIndex_, pi, static_cast<float>(real)))
                            state_->commitUndoStep(
                                tr("Adjustment"),
                                QStringLiteral("adjustments"));
                        else
                            state_->discardUndoStep();
                    });
            grid->addWidget(label, gridRow, 0);
            grid->addWidget(slider, gridRow, 1);
            grid->addWidget(spin, gridRow, 2);
            ++gridRow;
        }
        column->addLayout(grid);
    }

    AppState* state_ = nullptr;
    int layerIndex_ = -1;
    int kind_ = -1;
    QComboBox* opacity_ = nullptr;
    QComboBox* blend_ = nullptr;
    bool dragging_ = false;
    QPoint dragOffset_;
    bool drag_ = false;
    int dragStart_ = 0;
};

}  // namespace

void showAdjustmentDialog(AppState* state, int layerIndex, QWidget* parent) {
    if (!state || layerIndex < 0) return;
    QWidget* root = parent ? parent->window() : nullptr;
    if (root) {
        const auto existing = root->findChildren<QDialog*>(
            QStringLiteral("AdjustmentDialog"));
        for (QDialog* d : existing) {
            if (d->property("layerIndex").toInt() == layerIndex) {
                d->show();
                d->raise();
                d->activateWindow();
                return;
            }
        }
    }
    auto* dialog = new AdjustmentDialog(state, layerIndex, root);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void showRecolourDialog(AppState* state, int layerIndex, QWidget* parent) {
    showAdjustmentDialog(state, layerIndex, parent);
}

}  // namespace pittore::ui
