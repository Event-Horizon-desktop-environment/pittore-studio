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
#include <QPushButton>
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
#include "engine/filter/registry/filter_registry.h"
#include "ui/curve_editor.h"
#include "ui/panels/shared/panel_helpers.h"
#include "ui/persona/vector_edit.h"

namespace pittore::ui {
namespace {


// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------
class PropertiesPanel final : public QWidget {
  public:
    PropertiesPanel(AppState* state, QWidget* parent) : QWidget(parent), state_(state) {
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(12, 12, 12, 12);
        column->setSpacing(8);

        title_ = sectionLabel(tr("No layer selected"), state_, this);
        column->addWidget(title_);

        form_ = new QFormLayout;
        form_->setLabelAlignment(Qt::AlignRight);
        form_->setSpacing(6);
        column->addLayout(form_);
        column->addStretch(1);

        connect(state_, &AppState::layersChanged, this, [this] { rebuild(); });
        connect(state_, &AppState::activeDocumentChanged, this, [this] { rebuild(); });
        connect(state_, &AppState::historyChanged, this, [this] { rebuild(); });
        connect(state_, &AppState::activeLayerChanged, this, [this] { rebuild(); });
        rebuild();
    }

  private:
    static QColor artColor(const std::uint8_t rgba[4]) {
        return QColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    static void setArtColor(std::uint8_t rgba[4], const QColor& c) {
        rgba[0] = static_cast<std::uint8_t>(c.red());
        rgba[1] = static_cast<std::uint8_t>(c.green());
        rgba[2] = static_cast<std::uint8_t>(c.blue());
        rgba[3] = static_cast<std::uint8_t>(c.alpha());
    }

    // The active layer's art paint (copied: the dialog below is modal and the
    // layer may change under it). False when it has no vector geometry. Read
    // at click time, not rebuild time.
    bool activeArtPaint(int* indexOut, pittore::vector::ArtPaint* paintOut) {
        DocumentItem* d = state_->activeDocument();
        if (!d) return false;
        const int i = d->activeLayer;
        if (i < 0 || i >= d->layers.size() || !d->layers[i].art ||
            d->layers[i].art->isEmpty())
            return false;
        if (indexOut) *indexOut = i;
        if (paintOut) *paintOut = d->layers[i].art->paint;
        return true;
    }

    void pickArtColor(bool fill) {
        pittore::vector::ArtPaint paint;
        int index = -1;
        if (!activeArtPaint(&index, &paint)) return;
        const QColor start = artColor(fill ? paint.fill : paint.stroke);
        const QColor picked = QColorDialog::getColor(
            start, this, fill ? tr("Fill Color") : tr("Stroke Color"),
            QColorDialog::ShowAlphaChannel);
        if (!picked.isValid()) return;
        if (fill) {
            paint.hasFill = true;
            setArtColor(paint.fill, picked);
        } else {
            paint.hasStroke = true;
            setArtColor(paint.stroke, picked);
        }
        state_->applyVectorPaint(index, paint, -1.0,
                                 fill ? tr("Fill Color") : tr("Stroke Color"));
    }

  private:
    void rebuild() {
        while (form_->rowCount() > 0) form_->removeRow(0);

        DocumentItem* d = state_->activeDocument();
        LayerItem* layer = state_->activeLayer();
        if (!d || !layer) {
            title_->setText(tr("No layer selected"));
            return;
        }

        title_->setText(tr("%1 properties").arg(layer->name));

        auto* w = new QSpinBox(this);
        w->setRange(1, 300000);
        w->setValue(d->size.width());
        w->setSuffix(QStringLiteral(" px"));
        form_->addRow(tr("Width"), w);

        auto* h = new QSpinBox(this);
        h->setRange(1, 300000);
        h->setValue(d->size.height());
        h->setSuffix(QStringLiteral(" px"));
        form_->addRow(tr("Height"), h);

        auto* res = new QSpinBox(this);
        res->setRange(1, 10000);
        res->setValue(d->dpi);
        res->setSuffix(QStringLiteral(" ppi"));
        form_->addRow(tr("Resolution"), res);

        auto* mode = new QComboBox(this);
        mode->addItems({QStringLiteral("RGB/8"), QStringLiteral("RGB/16"),
                        QStringLiteral("RGB/32"), QStringLiteral("Grayscale/8"),
                        QStringLiteral("CMYK/8"), QStringLiteral("Lab/16")});
        mode->setCurrentText(d->colorMode);
        form_->addRow(tr("Mode"), mode);

        if (layer->art && !layer->art->isEmpty()) {
            // Vector appearance for retained-geometry layers: the same paint
            // the Stroke/Appearance panels edit (own folder: applyVectorPaint),
            // one undo step per committed change.
            const auto& paint = layer->art->paint;
            auto* fill = new QPushButton(this);
            fill->setCursor(Qt::PointingHandCursor);
            fill->setStyleSheet(
                QStringLiteral("background: %1; border: 1px solid #888;")
                    .arg(artColor(paint.fill).name(QColor::HexArgb)));
            fill->setText(artColor(paint.fill).name(QColor::HexRgb).toUpper());
            connect(fill, &QPushButton::clicked, this,
                    [this] { pickArtColor(true); });
            form_->addRow(tr("Fill"), fill);

            auto* stroke = new QPushButton(this);
            stroke->setCursor(Qt::PointingHandCursor);
            stroke->setStyleSheet(
                QStringLiteral("background: %1; border: 1px solid #888;")
                    .arg(artColor(paint.stroke).name(QColor::HexArgb)));
            stroke->setText(
                artColor(paint.stroke).name(QColor::HexRgb).toUpper());
            connect(stroke, &QPushButton::clicked, this,
                    [this] { pickArtColor(false); });
            form_->addRow(tr("Stroke"), stroke);

            auto* width = new QDoubleSpinBox(this);
            width->setRange(0.01, 1000.0);
            width->setSingleStep(0.5);
            width->setDecimals(2);
            width->setSuffix(tr(" px"));
            width->setKeyboardTracking(false);
            width->setValue(paint.strokeWidth);
            connect(width, &QDoubleSpinBox::editingFinished, this, [this, width] {
                pittore::vector::ArtPaint next;
                int index = -1;
                if (!activeArtPaint(&index, &next)) return;
                next.strokeWidth = width->value();
                state_->applyVectorPaint(index, next, -1.0, tr("Stroke Width"));
            });
            form_->addRow(tr("Width"), width);
        }

        if (layer->kind == LayerItem::Kind::Adjustment &&
            layer->adjustmentKind != 0) {
            // Header: name + Reset + press-hold compare (before/after).
            // Compare hides the row with no undo step and restores on
            // release (AppState::begin/endAdjustmentPreview); Reset restores
            // creation defaults with the same write-through contract as the
            // slider rows below.
            auto* head = new QWidget(this);
            auto* hrow = new QHBoxLayout(head);
            hrow->setContentsMargins(0, 0, 0, 0);
            hrow->setSpacing(8);
            hrow->addWidget(sectionLabel(
                adjustmentKindName(layer->adjustmentKind), state_, head));
            hrow->addStretch(1);
            auto* reset = new QPushButton(tr("Reset"), head);
            reset->setToolTip(tr("Reset this adjustment to its defaults."));
            reset->setAutoDefault(false);
            connect(reset, &QPushButton::clicked, this, [this] {
                state_->beginUndoStep();
                if (state_->resetAdjustmentToDefaults())
                    state_->commitUndoStep(tr("Reset Adjustments"),
                                           QStringLiteral("adjustments"));
                else
                    state_->discardUndoStep();
            });
            hrow->addWidget(reset);
            auto* compare = footerButton(
                state_, QStringLiteral("eye"),
                tr("Hold to preview without this adjustment."), head);
            connect(compare, &QToolButton::pressed, this,
                    [this] { state_->beginAdjustmentPreview(); });
            connect(compare, &QToolButton::released, this,
                    [this] { state_->endAdjustmentPreview(); });
            hrow->addWidget(compare);
            form_->addRow(head);
            // Live parameter rows, in param-index order. Slider drags write
            // straight through to the layer and recomposite immediately;
            // the drag holds one undo step (begin on press, commit on
            // release) so Ctrl+Z undoes the whole drag at once.
            const QVector<AdjustmentParamDesc> descs =
                adjustmentParamDescs(layer->adjustmentKind);
            for (int pi = 0; pi < descs.size(); ++pi) {
                const AdjustmentParamDesc& dd = descs[pi];
                auto* slider = new QSlider(Qt::Horizontal, this);
                const int span = dd.sliderMax - dd.sliderMin;
                slider->setRange(0, span > 0 ? span : 1);
                const double cur = layer->adjustmentParams[pi];
                const int pos = qBound(
                    0,
                    static_cast<int>(std::lround(
                        (cur - dd.realMin) / (dd.realMax - dd.realMin) * span)),
                    span);
                {
                    const QSignalBlocker block(slider);
                    slider->setValue(pos);
                }
                slider->setToolTip(
                    QStringLiteral("%1 (%2–%3)").arg(dd.label).arg(dd.realMin).arg(dd.realMax));
                connect(slider, &QSlider::sliderPressed, this, [this, slider] {
                    if (state_->activeLayer() && !state_->undoStepActive()) {
                        state_->beginUndoStep();
                        sliderGesture_ = true;
                        sliderStart_ = slider->value();
                    }
                });
                connect(slider, &QSlider::sliderReleased, this, [this, slider] {
                    if (sliderGesture_) {
                        sliderGesture_ = false;
                        if (slider->value() == sliderStart_)
                            state_->discardUndoStep();
                        else
                            state_->commitUndoStep(tr("Adjustments"),
                                                   QStringLiteral("adjustments"));
                    }
                });
                connect(slider, &QSlider::valueChanged, this,
                        [this, pi, dd, span](int v) {
                            const double real =
                                dd.realMin +
                                (dd.realMax - dd.realMin) * v / span;
                            if (state_->activeLayer())
                                state_->setAdjustmentParam(
                                    pi, static_cast<float>(real));
                        });
                form_->addRow(dd.label, slider);
            }
            if (layer->adjustmentKind ==
                static_cast<int>(
                    pittore::compute::AdjustmentKind::Curves)) {
                auto* channel = new QComboBox(this);
                channel->addItems({tr("RGB"), tr("Red"), tr("Green"),
                                   tr("Blue")});
                form_->addRow(tr("Channel"), channel);
                auto* editor = new CurveEditor(this);
                editor->setMinimumSize(170, 170);
                editor->setSizePolicy(QSizePolicy::Expanding,
                                      QSizePolicy::Fixed);
                const auto channelPoints = [this](int ch) {
                    const LayerItem* l = state_->activeLayer();
                    if (!l) return QVector<QPointF>{{0.0, 0.0}, {1.0, 1.0}};
                    const QVector<QPointF>* q = &l->adjustmentCurve;
                    if (ch == 1) q = &l->adjustmentCurveR;
                    if (ch == 2) q = &l->adjustmentCurveG;
                    if (ch == 3) q = &l->adjustmentCurveB;
                    return q->isEmpty() ? QVector<QPointF>{{0.0, 0.0},
                                                          {1.0, 1.0}}
                                        : *q;
                };
                editor->setPoints(channelPoints(0));
                connect(channel, &QComboBox::currentIndexChanged, this,
                        [this, editor, channelPoints](int ch) {
                            editor->setPoints(channelPoints(ch));
                        });
                editor->onChanged_ = [this, editor, channel] {
                    if (!state_->activeLayer()) return;
                    const int ch = channel->currentIndex();
                    state_->beginUndoStep();
                    const bool ok =
                        ch == 0 ? state_->setAdjustmentCurve(editor->points())
                                : state_->setAdjustmentCurveForChannel(ch - 1, editor->points());
                    if (ok)
                        state_->commitUndoStep(tr("Adjustments"),
                                               QStringLiteral("adjustments"));
                    else
                        state_->discardUndoStep();
                };
                form_->addRow(tr("Curve"), editor);
            }
        } else if (layer->kind == LayerItem::Kind::Adjustment) {
            auto* note = new QLabel(
                tr("Legacy adjustment (no live parameters)."), this);
            note->setWordWrap(true);
            form_->addRow(note);
        }
        if (layer->hasMask) {
            auto* density = new QSlider(Qt::Horizontal, this);
            density->setRange(0, 100);
            density->setValue(qRound(layer->maskDensity * 100));
            connect(density, &QSlider::sliderPressed, this, [this, density] {
                if (state_->activeLayer() && !state_->undoStepActive()) {
                    state_->beginUndoStep();
                    sliderGesture_ = true;
                    sliderStart_ = density->value();
                }
            });
            connect(density, &QSlider::sliderReleased, this, [this, density] {
                if (sliderGesture_) {
                    sliderGesture_ = false;
                    if (density->value() == sliderStart_)
                        state_->discardUndoStep();
                    else
                        state_->commitUndoStep(tr("Mask Density"),
                                               QStringLiteral("mask"));
                }
            });
            connect(density, &QSlider::valueChanged, this,
                    [this](int v) {
                        if (state_->activeLayer())
                            state_->setLayerMaskDensity(v / 100.0f);
                    });
            form_->addRow(tr("Density"), density);

            auto* feather = new QDoubleSpinBox(this);
            feather->setRange(0, 1000);
            feather->setValue(layer->maskFeather);
            feather->setSuffix(QStringLiteral(" px"));
            connect(feather, &QDoubleSpinBox::valueChanged, this,
                    [this](double v) {
                        if (!state_->activeLayer()) return;
                        state_->beginUndoStep();
                        if (state_->setLayerMaskFeather(static_cast<float>(v)))
                            state_->commitUndoStep(tr("Mask Feather"),
                                                   QStringLiteral("mask"));
                        else
                            state_->discardUndoStep();
                    });
            form_->addRow(tr("Feather"), feather);
        }
        if (layer->hasLiveFilter) {
            const pittore::filter::FilterDef* def =
                pittore::filter::findFilter(
                    layer->liveFilterId.toStdString());
            auto* name = new QLabel(
                def ? QString::fromUtf8(def->name) : layer->liveFilterId,
                this);
            form_->addRow(tr("Live filter"), name);
            auto* enabled = new QCheckBox(tr("Enabled"), this);
            enabled->setChecked(layer->liveFilterEnabled);
            connect(enabled, &QCheckBox::toggled, this, [this](bool on) {
                if (state_->activeLayer())
                    state_->setLiveFilterEnabled(on);
            });
            form_->addRow(QString(), enabled);
            if (def) {
                for (std::size_t pi = 0; pi < def->params.size(); ++pi) {
                    const pittore::filter::FilterParam& fp =
                        def->params[pi];
                    const double cur =
                        pi < layer->liveFilterParams.size()
                            ? layer->liveFilterParams[pi]
                            : fp.def;
                    if (fp.kind == 2) {
                        auto* check = new QCheckBox(
                            QString::fromUtf8(fp.label), this);
                        check->setChecked(cur != 0.0);
                        connect(check, &QCheckBox::toggled, this,
                                [this, pi](bool on) {
                                    if (state_->activeLayer())
                                        state_->setLiveFilterParam(
                                            static_cast<int>(pi),
                                            on ? 1.0 : 0.0);
                                });
                        form_->addRow(QString(), check);
                    } else if (fp.kind == 1 && !fp.choices.empty()) {
                        auto* combo = new QComboBox(this);
                        for (const char* c : fp.choices)
                            combo->addItem(QString::fromUtf8(c));
                        combo->setCurrentIndex(qBound(
                            0, static_cast<int>(std::lround(cur)),
                            combo->count() - 1));
                        connect(combo, &QComboBox::currentIndexChanged, this,
                                [this, pi](int v) {
                                    if (state_->activeLayer())
                                        state_->setLiveFilterParam(
                                            static_cast<int>(pi),
                                            static_cast<double>(v));
                                });
                        form_->addRow(QString::fromUtf8(fp.label), combo);
                    } else {
                        const int span = 1000;
                        auto* slider = new QSlider(Qt::Horizontal, this);
                        slider->setRange(0, span);
                        slider->setValue(qBound(
                            0,
                            static_cast<int>(std::lround(
                                (cur - fp.min) /
                                (fp.max - fp.min) * span)),
                            span));
                        slider->setToolTip(
                            QStringLiteral("%1 (%2–%3%4)")
                                .arg(QString::fromUtf8(fp.label))
                                .arg(fp.min)
                                .arg(fp.max)
                                .arg(QString::fromUtf8(fp.suffix)));
                        connect(slider, &QSlider::valueChanged, this,
                                [this, pi, fp, span](int v) {
                                    const double real =
                                        fp.min +
                                        (fp.max - fp.min) * v / span;
                                    if (state_->activeLayer())
                                        state_->setLiveFilterParam(
                                            static_cast<int>(pi), real);
                                });
                        form_->addRow(QString::fromUtf8(fp.label), slider);
                    }
                }
            }
            auto* remove = new QPushButton(tr("Remove live filter"), this);
            connect(remove, &QPushButton::clicked, this, [this] {
                if (state_->activeLayer()) state_->removeLiveFilter();
            });
            form_->addRow(QString(), remove);
        }
    }

    AppState* state_;
    QLabel* title_ = nullptr;
    QFormLayout* form_ = nullptr;
    // True while a slider drag holds an open undo step (adjustment and
    // mask-density rows), and the dragged slider's start position: a press
    // without movement discards instead of logging a no-op step. One drag
    // commits as one Ctrl+Z step.
    bool sliderGesture_ = false;
    int sliderStart_ = 0;
};

}  // namespace

QWidget* createPropertiesPanel(AppState* state, QWidget* parent) {
    return new PropertiesPanel(state, parent);
}

}  // namespace pittore::ui
