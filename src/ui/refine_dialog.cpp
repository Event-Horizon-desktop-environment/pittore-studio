#include "ui/refine_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QApplication>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/ai/bg_remove.h"
#include "ui/ai_models.h"
#include "ui/app_state.h"
#include "ui/selection_mask.h"
#include "ui/theme.h"

namespace pittore::ui {

RefineDialog::RefineDialog(AppState* state, DocumentItem* doc, QWidget* parent)
    : QDialog(parent), state_(state), doc_(doc) {
    setWindowTitle(tr("Refine Selection"));
    resize(860, 640);

    inputMask_ = selectionAsMask(*doc_);
    compositeFull_ = doc_->composite;
    QRect bbox = selectionMaskBbox(inputMask_);
    // Pad for the widest operator (border band / feather / brush) so edges
    // never touch the work-rect boundary mid-recompute.
    const int pad = 64;
    workRect_ = QRect(QPoint(0, 0), doc_->size)
                    .intersected(bbox.adjusted(-pad, -pad, pad, pad));
    if (workRect_.isEmpty()) workRect_ = bbox;
    baseCrop_ = inputMask_.copy(workRect_);
    QImage comp = compositeFull_.convertToFormat(QImage::Format_ARGB32);
    // Interactive proxy: longest side capped so every live pass is instant.
    // Full resolution is only ever computed once, on Apply. The cap sits
    // above typical hair-strand scale so zoomed previews don't turn to
    // blocks before Apply re-solves at full resolution.
    static constexpr int kProxyMax = 1024;
    proxyScale_ = std::min(1.0, kProxyMax / double(std::max(
                                        1, std::max(workRect_.width(),
                                                    workRect_.height()))));
    const QSize proxySize(qMax(1, int(workRect_.width() * proxyScale_ + 0.5)),
                          qMax(1, int(workRect_.height() * proxyScale_ + 0.5)));
    baseProxy_ = baseCrop_.scaled(proxySize, Qt::IgnoreAspectRatio,
                                  Qt::SmoothTransformation);
    compProxy_ = comp.copy(workRect_).scaled(proxySize, Qt::IgnoreAspectRatio,
                                             Qt::SmoothTransformation);
    refinedProxy_ = baseProxy_;

    auto* root = new QHBoxLayout(this);

    // --- preview ---------------------------------------------------------
    preview_ = new QLabel(this);
    preview_->setObjectName(QStringLiteral("refine.preview"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(480, 480);
    preview_->setMouseTracking(true);
    preview_->installEventFilter(this);
    preview_->setFrameShape(QFrame::Box);
    root->addWidget(preview_, 1);

    // --- controls --------------------------------------------------------
    auto* side = new QWidget(this);
    auto* col = new QVBoxLayout(side);
    col->setSpacing(8);

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel(tr("Preview"), side));
    previewCombo_ = new QComboBox(side);
    previewCombo_->addItems({tr("Overlay"), tr("Black matte"),
                             tr("White matte"), tr("Black and white"),
                             tr("Transparent")});
    topRow->addWidget(previewCombo_, 1);
    col->addLayout(topRow);

    matteEdgesCheck_ = new QCheckBox(tr("Matte edges"), side);
    matteEdgesCheck_->setChecked(true);
    matteEdgesCheck_->setToolTip(
        tr("Snap the matte to detected image edges inside the border band."));
    col->addWidget(matteEdgesCheck_);

    smartRadiusCheck_ = new QCheckBox(tr("Smart radius"), side);
    smartRadiusCheck_->setChecked(true);
    smartRadiusCheck_->setToolTip(
        tr("Adapt the border band to each edge: tight on hard boundaries, "
           "wide on soft hair and fur, so one border value serves both."));
    col->addWidget(smartRadiusCheck_);

    enhanceButton_ = new QPushButton(tr("Enhance Edges (AI)"), side);
    enhanceButton_->setObjectName(QStringLiteral("refine.enhance"));
    enhanceButton_->setToolTip(
        tr("Re-estimate hair and soft edges with the local portrait-matting "
           "AI inside the edge band. Falls back to the colour snap when no "
           "AI model is installed."));
    col->addWidget(enhanceButton_);
    enhanceStatus_ = new QLabel(side);
    enhanceStatus_->setObjectName(QStringLiteral("refine.enhanceStatus"));
    enhanceStatus_->setWordWrap(true);
    col->addWidget(enhanceStatus_);
    connect(enhanceButton_, &QPushButton::clicked, this,
            &RefineDialog::enhanceEdgesWithAi);

    auto sliderRow = [&](const QString& label, int max, const QString& tip,
                         QSlider*& slider, QLabel*& value) {
        auto* row = new QHBoxLayout;
        auto* name = new QLabel(label, side);
        name->setMinimumWidth(92);
        name->setToolTip(tip);
        row->addWidget(name);
        slider = new QSlider(Qt::Horizontal, side);
        slider->setRange(0, max);
        slider->setValue(0);
        row->addWidget(slider, 1);
        value = new QLabel(side);
        value->setMinimumWidth(48);
        row->addWidget(value);
        col->addLayout(row);
    };
    sliderRow(tr("Border width"), 100,
              tr("Refinement band around the edge, in percent of half the "
                 "selection's smaller side."),
              borderSlider_, borderValue_);
    borderSlider_->setValue(10);
    sliderRow(tr("Smooth"), 100,
              tr("Even out the outline (~1 px per pass)."), smoothSlider_,
              smoothValue_);
    smoothSlider_->setValue(6);
    sliderRow(tr("Feather"), 100,
              tr("Soften the transition, in pixels."), featherSlider_,
              featherValue_);
    sliderRow(tr("Shift"), 100,
              tr("Displace the edge after feathering (contract -, expand +)."),
              shiftSlider_, shiftValue_);
    shiftSlider_->setRange(-100, 100);
    shiftSlider_->setValue(0);
    sliderRow(tr("Ramp"), 100, tr("Harden the edge contrast (+), or choke "
                                "the edge outward (−)."),
              rampSlider_, rampValue_);
    rampSlider_->setRange(-100, 100);
    rampSlider_->setValue(0);

    auto* brushLabel = new QLabel(tr("Adjustment Brush"), side);
    col->addWidget(brushLabel);
    auto* modes = new QHBoxLayout;
    const char* names[] = {"Matte", "Foreground", "Background", "Feather"};
    for (int i = 0; i < 4; ++i) {
        auto* b = new QPushButton(tr(names[i]), side);
        b->setCheckable(true);
        b->setChecked(i == 0);
        brushModes_.append(b);
        modes->addWidget(b);
    }
    auto brushGroup = [&](int i) {
        for (int k = 0; k < brushModes_.size(); ++k)
            brushModes_[k]->setChecked(k == i);
    };
    for (int i = 0; i < 4; ++i) {
        connect(brushModes_[i], &QPushButton::clicked, this,
                [brushGroup, i] { brushGroup(i); });
    }
    col->addLayout(modes);
    brushSizeCombo_ = new QComboBox(side);
    for (int s : {10, 20, 50, 100, 200})
        brushSizeCombo_->addItem(tr("%1 px").arg(s), s);
    brushSizeCombo_->setCurrentIndex(brushSizeCombo_->findData(50));
    col->addWidget(brushSizeCombo_);

    col->addWidget(new QLabel(tr("Output"), side));
    outputCombo_ = new QComboBox(side);
    rebuildOutputList();
    col->addWidget(outputCombo_);
    decontamCheck_ = new QCheckBox(tr("Color decontamination"), side);
    decontamCheck_->setToolTip(
        tr("Unmix background fringe from edge pixels into a new layer."));
    col->addWidget(decontamCheck_);

    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Apply | QDialogButtonBox::Cancel, side);
    col->addStretch(1);
    col->addWidget(buttons_);
    root->addWidget(side);

    // --- wiring ----------------------------------------------------------
    // conventional: dragging a slider shows the value live and the border
    // band overlay; the expensive recompute runs on RELEASE, and brush
    // strokes stamp the proxy instantly per move with a full replay behind.
    auto touch = [this] {
        recompute();
        refreshPreview();
    };
    connect(previewCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPreview(); });
    connect(matteEdgesCheck_, &QCheckBox::toggled, this, touch);
    connect(smartRadiusCheck_, &QCheckBox::toggled, this, touch);
    auto sliderTouch = [this](QSlider* s, QLabel* v, const QString& suffix) {
        connect(s, &QSlider::valueChanged, this, [this, s, v, suffix] {
            v->setText(QStringLiteral("%1 %2").arg(s->value()).arg(suffix));
            if (recomputeTimer_) recomputeTimer_->start();
        });
        connect(s, &QSlider::sliderReleased, this, [this] {
            if (recomputeTimer_) recomputeTimer_->stop();
            recompute();
            refreshPreview();
        });
        v->setText(QStringLiteral("%1 %2").arg(s->value()).arg(suffix));
    };
    sliderTouch(borderSlider_, borderValue_, QStringLiteral("%"));
    sliderTouch(smoothSlider_, smoothValue_, QStringLiteral("px"));
    sliderTouch(featherSlider_, featherValue_, QStringLiteral("px"));
    sliderTouch(shiftSlider_, shiftValue_, QStringLiteral("%"));
    sliderTouch(rampSlider_, rampValue_, QStringLiteral("%"));
    connect(brushSizeCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshPreview(); });
    connect(decontamCheck_, &QCheckBox::toggled, this,
            [this](bool) { rebuildOutputList(); });
    connect(buttons_, &QDialogButtonBox::clicked, this, [this](QAbstractButton* b) {
        if (!b || !buttons_) return;
        const auto role = buttons_->buttonRole(b);
        if (role == QDialogButtonBox::ApplyRole) {
            result_.accepted = true;
            // Full-resolution result: same pipeline, replayed 1:1.
            const QImage full = recomputeFull();
            QImage out = inputMask_;
            QPainter p(&out);
            p.drawImage(workRect_.topLeft(), full);
            p.end();
            result_.mask = out;
            const int sel = outputCombo_->currentIndex();
            if (decontamCheck_->isChecked()) {
                result_.output =
                    sel == 1
                        ? RefineResult::Output::NewDecontaminatedLayerWithMask
                        : RefineResult::Output::NewDecontaminatedLayer;
            } else {
                result_.output = sel == 1 ? RefineResult::Output::LayerMask
                                          : RefineResult::Output::Selection;
            }
            accept();
        } else if (role == QDialogButtonBox::RejectRole) {
            reject();
        }
    });

    recomputeTimer_ = new QTimer(this);
    recomputeTimer_->setSingleShot(true);
    recomputeTimer_->setInterval(180);
    connect(recomputeTimer_, &QTimer::timeout, this, [this] {
        recompute();
        refreshPreview();
    });

    recompute();
    refreshPreview();
}


void RefineDialog::rebuildOutputList() {
    const QSignalBlocker block(outputCombo_);
    outputCombo_->clear();
    if (decontamCheck_ && decontamCheck_->isChecked()) {
        outputCombo_->addItems({tr("New decontaminated layer"),
                                tr("New decontaminated layer with mask")});
    } else {
        outputCombo_->addItems({tr("Selection"), tr("Mask")});
    }
}


int refineBorderPx(const QRect& bbox, int percent) {
    if (percent <= 0) return 0;
    const int m = std::min(bbox.width(), bbox.height());
    return std::clamp(int(percent / 100.0 * m / 2.0 + 0.5), 1, 256);
}


void RefineDialog::recompute() {
    // Live pass at proxy scale: gather full-res params, scale the spatial
    // ones down, replay the strokes. The border band is relative to the
    // SELECTION's smaller side (per the tooltip), not the document.
    const QRect selBox = selectionMaskBbox(inputMask_);
    const int bandFull = refineBorderPx(
        selBox.isEmpty() ? workRect_ : selBox, borderSlider_->value());
    const int band =
        (matteEdgesCheck_->isChecked() && bandFull > 0)
            ? std::max(1, int(bandFull * proxyScale_ + 0.5))
            : 0;
    const int passes = std::clamp((smoothSlider_->value() + 1) / 3, 0, 10);
    const int feather =
        featherSlider_->value() > 0
            ? std::max(1, int(featherSlider_->value() * proxyScale_ + 0.5))
            : 0;
    const int shift =
        int(shiftSlider_->value() / 100.0 * 24 * proxyScale_);
    refinedProxy_ = runPipeline(baseProxy_, compProxy_, band, passes, feather,
                                shift, rampSlider_->value(), strokes_,
                                smartRadiusCheck_->isChecked(), proxyScale_);
}


QImage RefineDialog::recomputeFull() const {
    // Apply-time pass at full work-rect resolution, same pipeline.
    // Selection-relative border band, matching the live pass.
    const QRect selBox = selectionMaskBbox(inputMask_);
    const int band =
        matteEdgesCheck_->isChecked()
            ? refineBorderPx(selBox.isEmpty() ? workRect_ : selBox,
                             borderSlider_->value())
            : 0;
    const int passes = std::clamp((smoothSlider_->value() + 1) / 3, 0, 10);
    const int feather = featherSlider_->value();
    const int shift = int(shiftSlider_->value() / 100.0 * 24);
    const QImage compFull = compositeFull_.copy(workRect_).convertToFormat(
        QImage::Format_ARGB32);
    return runPipeline(baseCrop_, compFull, band, passes, feather, shift,
                       rampSlider_->value(), strokes_,
                       smartRadiusCheck_->isChecked(), 1.0);
}


QImage RefineDialog::runPipeline(const QImage& base, const QImage& comp,
                                 int bandPx, int smoothPasses, int featherPx,
                                 int shiftPx, int rampPct,
                                 const QVector<Stroke>& strokes,
                                 bool smartRadius, double coordScale) const {
    if (base.isNull()) return base;
    QImage work = selectionMaskRemoveSpeckles(
        base, std::max(8, int(32 * coordScale * coordScale + 0.5)));
    // Decay broad AI wash far from the subject before solving: grow-only
    // rules otherwise preserve a ghost band in empty backdrop. Support is
    // clamped tight so a wide border band can't protect distant wash: only
    // the immediate edge transition is exempt.
    work = selectionMaskDecayWash(work, comp,
                                  std::clamp(bandPx, 8, 48));
    // Edge snap solves against the colour guide: hair vs backdrop separates
    // on colour, not just tone. Smart radius runs a half-width band too and
    // lets the wide result through only where the mask edge is soft — but
    // needs room for two bands, so a tiny border (narrow < 2px) takes the
    // plain snap instead (a half-width pass there is too tight to reach
    // anything, and a hard edge is all it would ever see anyway).
    if (bandPx > 0) {
        work = (smartRadius && bandPx >= 4)
                   ? selectionMaskSnapSmart(comp, work, bandPx / 2, bandPx, 3)
                   : selectionMaskSnapToEdges(comp, work, bandPx, 3);
    }
    if (smoothPasses > 0) work = selectionMaskSmooth(work, smoothPasses);
    if (featherPx > 0) work = selectionMaskFeather(work, featherPx);
    if (shiftPx != 0) work = selectionMaskGrow(work, shiftPx);
    if (rampPct > 0) {
        work = selectionMaskRamp(work, 1.0 + 3.0 * rampPct / 100.0);
    } else if (rampPct < 0) {
        // Negative ramp chokes the edge outward: grow the matte softly.
        const int growPx =
            std::max(1, int(-rampPct / 100.0 * 24 * coordScale + 0.5));
        work = selectionMaskGrow(work, growPx);
    }
    // Replay adjustment-brush strokes on top: user paint always wins. Matte
    // strokes re-solve against the color guide; fg/bg force coverage.
    for (const Stroke& s : strokes) {
        const QPointF p(s.pos.x() * coordScale, s.pos.y() * coordScale);
        const double r = std::max(1.0, s.radius * coordScale);
        if (s.mode == 3) {
            work = selectionMaskFeatherStamp(work, p, r);
        } else if (s.mode == 0) {
            work = selectionMaskMatteSolve(comp, work, p, r);
        } else {
            const int value = s.mode == 1 ? 255 : 0;
            work = selectionMaskBrushStamp(work, p, r, 0.0, value);
        }
    }
    return work;
}


void RefineDialog::enhanceEdgesWithAi() {
    if (baseCrop_.isNull() || workRect_.isEmpty()) return;
    if (!enhanceStatus_ || !enhanceButton_) return;
    // Hair band: the current border band, at least 16 px so flyaways are in
    // range even when the Border slider sits low.
    const QRect selBox = selectionMaskBbox(inputMask_);
    const int borderBand = refineBorderPx(
        selBox.isEmpty() ? workRect_ : selBox,
        borderSlider_ ? borderSlider_->value() : 10);
    const int hairBand = std::max(16, borderBand);
    const QImage compFull =
        compositeFull_.copy(workRect_).convertToFormat(QImage::Format_ARGB32);
    if (compFull.isNull() || compFull.size() != baseCrop_.size()) {
        enhanceStatus_->setText(tr("Enhance Edges: no image under the selection."));
        return;
    }

    // Preferences ▸ Machine Learning pick first (pair/SAM models need
    // prompts and cannot re-estimate hair on their own, so the shared picker
    // only ever returns installed single-file models).
    const QString modelId = bestEnhanceModelId(
        state_ ? state_->settings().enhanceModel : QString());

    // No local AI available: classical colour-snap fallback so the button
    // never dead-ends (same affinity model as Matte edges, wider band — but
    // still tight on hard edges via the smart blend).
    if (modelId.isEmpty() || !pittore::ai::onnx_available()) {
        const QImage snapped =
            selectionMaskSnapSmart(compFull, baseCrop_, std::max(1, hairBand / 2),
                                   hairBand, 3);
        baseCrop_ = snapped;
        const QSize proxySize(qMax(1, int(workRect_.width() * proxyScale_ + 0.5)),
                              qMax(1, int(workRect_.height() * proxyScale_ + 0.5)));
        baseProxy_ = baseCrop_.scaled(proxySize, Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
        recompute();
        refreshPreview();
        enhanceStatus_->setText(
            modelId.isEmpty()
                ? tr("Enhance Edges: no AI model installed — used local edge "
                     "snap. Install BiRefNet Portrait in Preferences / AI "
                     "Models for hair matting.")
                : tr("Enhance Edges: this build links no ONNX Runtime — used "
                     "local edge snap."));
        return;
    }

    const AiModel* model = aiModel(modelId);
    if (!model) return;
    const int w = compFull.width(), h = compFull.height();
    std::vector<std::uint8_t> rgba(std::size_t(w) * h * 4u);
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(compFull.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = row[x];
            std::uint8_t* d = &rgba[(std::size_t(y) * w + x) * 4u];
            d[0] = static_cast<std::uint8_t>(qRed(px));
            d[1] = static_cast<std::uint8_t>(qGreen(px));
            d[2] = static_cast<std::uint8_t>(qBlue(px));
            d[3] = static_cast<std::uint8_t>(qAlpha(px));
        }
    }

    enhanceButton_->setEnabled(false);
    enhanceStatus_->setText(tr("Enhance Edges: running %1…").arg(model->name));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents();
    bool cursorSet = true;
    pittore::ai::SegmentResult seg;
    try {
        seg = pittore::ai::segment_rgba8(rgba.data(), w, h,
                                         aiModelPath(modelId).toStdString(),
                                         model->inputSize);
    } catch (const std::bad_alloc&) {
        seg.ok = false;
        seg.error = "out of memory";
    } catch (const std::exception& e) {
        seg.ok = false;
        try {
            seg.error = e.what();
        } catch (...) {
            seg.error = "unknown error";
        }
    }
    if (cursorSet) QApplication::restoreOverrideCursor();
    enhanceButton_->setEnabled(true);
    if (!seg.ok || seg.alpha.size() != std::size_t(w) * h) {
        enhanceStatus_->setText(
            tr("Enhance Edges failed: %1")
                .arg(seg.error.empty() ? tr("bad mask")
                                       : QString::fromStdString(seg.error)));
        return;
    }
    QImage aiMask(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* row = aiMask.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const float v = seg.alpha[std::size_t(y) * w + x];
            row[x] = static_cast<uchar>(
                std::clamp(int(v * 255.0f + 0.5f), 0, 255));
        }
    }
    baseCrop_ = selectionMaskBlendAiHair(baseCrop_, aiMask, hairBand);
    const QSize proxySize(qMax(1, int(workRect_.width() * proxyScale_ + 0.5)),
                          qMax(1, int(workRect_.height() * proxyScale_ + 0.5)));
    baseProxy_ = baseCrop_.scaled(proxySize, Qt::IgnoreAspectRatio,
                                  Qt::SmoothTransformation);
    recompute();
    refreshPreview();
    enhanceStatus_->setText(tr("Enhanced hair edges with %1.").arg(model->name));
}


QPoint RefineDialog::clampViewOff(QPoint off, const QSize& box,
                                    const QSize& pm) {
    // Image always covers the label: center small pixmaps, clamp panning of
    // large ones so the view can never get lost off-image.
    int x, y;
    if (pm.width() <= box.width())
        x = (box.width() - pm.width()) / 2;
    else
        x = std::clamp(off.x(), box.width() - pm.width(), 0);
    if (pm.height() <= box.height())
        y = (box.height() - pm.height()) / 2;
    else
        y = std::clamp(off.y(), box.height() - pm.height(), 0);
    return {x, y};
}


void RefineDialog::zoomAt(const QPointF& labelPos, double factor) {
    if (!(factor > 0.0) || !std::isfinite(factor) || previewPx_.isEmpty())
        return;
    const QSize box = preview_ ? preview_->size() : QSize();
    if (box.isEmpty()) return;
    const double next = std::clamp(viewZoom_ * factor, 0.25, 8.0);
    if (qFuzzyCompare(next, viewZoom_)) return;
    // Keep the image point under the cursor stable across the zoom.
    const double kOld =
        previewPx_.width() / double(std::max(1, refinedProxy_.width()));
    const double imgX = (labelPos.x() - previewOff_.x()) / kOld;
    const double imgY = (labelPos.y() - previewOff_.y()) / kOld;
    viewZoom_ = next;
    refreshPreview();
    if (previewPx_.isEmpty()) return;
    const double kNew =
        previewPx_.width() / double(std::max(1, refinedProxy_.width()));
    previewOff_ = clampViewOff(
        QPoint(int(labelPos.x() - imgX * kNew + 0.5),
               int(labelPos.y() - imgY * kNew + 0.5)),
        box, previewPx_);
    refreshPreview();
}


QPointF RefineDialog::labelToWork(const QPointF& labelPos) const {
    // 0-based proxy crop coords via the current zoom/pan offset.
    if (previewPx_.isEmpty() || refinedProxy_.isNull())
        return QPointF(-1, -1);
    const double kx =
        previewPx_.width() / double(std::max(1, refinedProxy_.width()));
    const double ky =
        previewPx_.height() / double(std::max(1, refinedProxy_.height()));
    if (kx <= 0.0 || ky <= 0.0) return QPointF(-1, -1);
    return QPointF((labelPos.x() - previewOff_.x()) / kx,
                   (labelPos.y() - previewOff_.y()) / ky);
}


void RefineDialog::paintAt(const QPointF& labelPos) {
    const QPointF pp = labelToWork(labelPos);   // 0-based proxy crop coords
    if (pp.x() < 0 || pp.y() < 0 || pp.x() >= refinedProxy_.width() ||
        pp.y() >= refinedProxy_.height())
        return;
    int mode = 0;
    for (int i = 0; i < brushModes_.size(); ++i) {
        if (brushModes_[i]->isChecked()) {
            mode = i;
            break;
        }
    }
    // Stamp the live proxy directly (instant); every dab is also stored in
    // full crop coords so slider replays and the Apply-time result match the
    // preview exactly.
    const double radius =
        std::max(1.0, brushSizeCombo_->currentData().toDouble() / 2.0 *
                          proxyScale_);
    const double inv = proxyScale_ > 0.0 ? 1.0 / proxyScale_ : 1.0;
    const double radiusFull =
        brushSizeCombo_->currentData().toDouble() / 2.0;
    auto stampOne = [&](const QPointF& at) {
        if (mode == 3) {
            refinedProxy_ =
                selectionMaskFeatherStamp(refinedProxy_, at, radius);
        } else if (mode == 0) {
            // Matte: re-solve the disc against the color guide instead of
            // painting flat gray.
            refinedProxy_ =
                selectionMaskMatteSolve(compProxy_, refinedProxy_, at, radius);
        } else {
            const int value = mode == 1 ? 255 : 0;
            refinedProxy_ = selectionMaskBrushStamp(refinedProxy_, at, radius,
                                                    0.0, value);
        }
        strokes_.append({QPointF(at.x() * inv, at.y() * inv), radiusFull,
                         mode});
    };
    // Gap-fill against the previous dab of THIS press only (haveLastDab_
    // resets on press, so separate strokes never connect across the image).
    if (painting_ && haveLastDab_) {
        const QPointF prev = lastDab_;
        const double dist = QLineF(prev, pp).length();
        const double step = std::max(1.0, radius * 0.5);
        if (dist > step) {
            const int n = int(dist / step);
            for (int i = 1; i <= n; ++i) {
                const double t = double(i) / (n + 1);
                stampOne(prev + (pp - prev) * t);
            }
        }
    }
    stampOne(pp);
    haveLastDab_ = true;
    lastDab_ = pp;
    pressDots_.append({pp, radius});
    refreshPreview();
}


bool RefineDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == preview_) {
        switch (event->type()) {
            case QEvent::MouseButtonPress: {
                auto* e = static_cast<QMouseEvent*>(event);
                if (e->button() == Qt::LeftButton) {
                    painting_ = true;
                    haveLastDab_ = false;
                    paintAt(e->position());
                    return true;
                }
                if (e->button() == Qt::MiddleButton) {
                    panDragging_ = true;
                    panStart_ = e->position();
                    panOffStart_ = previewOff_;
                    return true;
                }
                break;
            }
            case QEvent::MouseMove: {
                auto* e = static_cast<QMouseEvent*>(event);
                hoverLabel_ = e->position();
                hoverValid_ = true;
                if (panDragging_) {
                    const QPointF d = e->position() - panStart_;
                    previewOff_ = clampViewOff(
                        panOffStart_ +
                            QPoint(int(d.x() + 0.5), int(d.y() + 0.5)),
                        preview_ ? preview_->size() : QSize(), previewPx_);
                    refreshPreview();
                    return true;
                }
                if (painting_) paintAt(e->position());
                else refreshPreview();
                return true;
            }
            case QEvent::MouseButtonRelease: {
                auto* e = static_cast<QMouseEvent*>(event);
                if (panDragging_ && e->button() == Qt::MiddleButton) {
                    panDragging_ = false;
                    return true;
                }
                if (painting_ && e->button() == Qt::LeftButton) {
                    painting_ = false;
                    pressDots_.clear();
                    // conventional: the stroke lands, then the pipeline
                    // re-solves once (snapped/smoothed/feathered/ramped over
                    // the new paint) instead of once per mousemove.
                    recompute();
                    refreshPreview();
                    return true;
                }
                break;
            }
            case QEvent::Wheel: {
                auto* e = static_cast<QWheelEvent*>(event);
                zoomAt(e->position(),
                       std::pow(1.0015, e->angleDelta().y()));
                return true;
            }
            case QEvent::Leave:
                if (hoverValid_) {
                    hoverValid_ = false;
                    refreshPreview();
                }
                break;
            default:
                break;
        }
    }
    return QDialog::eventFilter(watched, event);
}


void RefineDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    refreshPreview();
}


QImage RefineDialog::renderPreview() const {
    if (refinedProxy_.isNull() || compProxy_.isNull()) return {};
    const int mode = previewCombo_ ? previewCombo_->currentIndex() : 0;
    const int w = refinedProxy_.width(), h = refinedProxy_.height();
    QImage out(w, h, QImage::Format_ARGB32);
    // Checkerboard for the Transparent mode.
    QImage board(w, h, QImage::Format_ARGB32);
    {
        QPainter bp(&board);
        constexpr int cell = 8;
        for (int y = 0; y < h; y += cell) {
            for (int x = 0; x < w; x += cell) {
                const bool alt = ((x / cell) + (y / cell)) % 2 == 0;
                bp.fillRect(x, y, std::min(cell, w - x), std::min(cell, h - y),
                            alt ? QColor(0x2a, 0x2c, 0x2f)
                                : QColor(0x24, 0x26, 0x28));
            }
        }
    }
    for (int y = 0; y < h; ++y) {
        const QRgb* crow =
            reinterpret_cast<const QRgb*>(compProxy_.constScanLine(y));
        const uchar* mrow = refinedProxy_.constScanLine(y);
        QRgb* orow = reinterpret_cast<QRgb*>(out.scanLine(y));
        const QRgb* brow =
            reinterpret_cast<const QRgb*>(board.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const double cov = mrow[x] / 255.0;
            const QRgb c = crow[x];
            switch (mode) {
                case 1: {   // Black matte
                    orow[x] = qRgb(int(qRed(c) * cov + 0.5),
                                   int(qGreen(c) * cov + 0.5),
                                   int(qBlue(c) * cov + 0.5));
                    break;
                }
                case 2: {   // White matte
                    orow[x] = qRgb(
                        int(qRed(c) * cov + 255 * (1.0 - cov) + 0.5),
                        int(qGreen(c) * cov + 255 * (1.0 - cov) + 0.5),
                        int(qBlue(c) * cov + 255 * (1.0 - cov) + 0.5));
                    break;
                }
                case 3: {   // Black and white: the matte itself
                    const int v = mrow[x];
                    orow[x] = qRgb(v, v, v);
                    break;
                }
                case 4: {   // Transparent: composite over checkerboard
                    const QRgb b = brow[x];
                    orow[x] = qRgb(int(qRed(c) * cov + qRed(b) * (1.0 - cov) +
                                       0.5),
                                   int(qGreen(c) * cov + qGreen(b) * (1.0 - cov) +
                                       0.5),
                                   int(qBlue(c) * cov + qBlue(b) * (1.0 - cov) +
                                       0.5));
                    break;
                }
                default: {   // Overlay: red rubylith on the deselected
                    const double k = 1.0 - cov;
                    orow[x] = qRgb(
                        int(qRed(c) * cov + 255 * k * 0.6 + 0.5),
                        int(qGreen(c) * cov + 0 * k + 0.5),
                        int(qBlue(c) * cov + 20 * k + 0.5));
                    break;
                }
            }
        }
    }
    return out;
}


void RefineDialog::refreshPreview() {
    if (!preview_) return;
    QImage img = renderPreview();
    const QSize box = preview_->size();
    if (img.isNull() || box.isEmpty()) {
        preview_->clear();
        return;
    }
    // Display size: aspect-fit times the zoom level, then pan offset.
    const double fitK =
        std::min(box.width() / double(std::max(1, img.width())),
                 box.height() / double(std::max(1, img.height())));
    const double k = fitK * viewZoom_;
    const QSize dsp(qMax(1, int(img.width() * k + 0.5)),
                    qMax(1, int(img.height() * k + 0.5)));
    QPixmap pm = QPixmap::fromImage(
        img.scaled(dsp, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    previewPx_ = pm.size();
    previewOff_ = clampViewOff(previewOff_, box, previewPx_);
    // Composite onto a label-sized board at the pan offset (transparent
    // board: every preview mode is already opaque except Transparent, whose
    // checkerboard is baked into the image).
    QPixmap board(box);
    board.fill(Qt::transparent);
    {
        QPainter bp(&board);
        bp.drawPixmap(previewOff_, pm);
        if (hoverValid_) {
            // Brush ring at the hover point; radius scales with the pixmap.
            // Board space == label space, so the label point draws directly.
            const double r =
                brushSizeCombo_->currentData().toDouble() / 2.0 *
                (pm.width() / double(std::max(1, img.width())));
            bp.setPen(QPen(Qt::white, 1));
            bp.drawEllipse(hoverLabel_, r, r);
            bp.setPen(QPen(Qt::black, 1));
            bp.drawEllipse(hoverLabel_, r + 1.0, r + 1.0);
        }
        if (!pressDots_.isEmpty()) {
            // In-progress stroke footprint: one translucent stroked path at
            // true brush diameter (Affinity-style). Per-dab discs are NOT
            // drawn: overlapping translucent fills accumulate into opaque
            // globs wherever the pointer lingers.
            const double kx =
                pm.width() / double(std::max(1, img.width()));
            const double ky =
                pm.height() / double(std::max(1, img.height()));
            auto toBoard = [&](const QPointF& p) {
                return QPointF(previewOff_.x() + p.x() * kx,
                               previewOff_.y() + p.y() * ky);
            };
            const double dia =
                2.0 * std::max(1.0, pressDots_.first().radius * kx);
            QPen pen(QColor(150, 0, 12, 115), dia, Qt::SolidLine,
                     Qt::RoundCap, Qt::RoundJoin);
            bp.setPen(pen);
            if (pressDots_.size() == 1) {
                bp.setBrush(QColor(150, 0, 12, 115));
                const QPointF c = toBoard(pressDots_.first().pos);
                const double rx = std::max(1.0, pressDots_.first().radius * kx);
                const double ry = std::max(1.0, pressDots_.first().radius * ky);
                bp.drawEllipse(c, rx, ry);
                bp.setBrush(Qt::NoBrush);
            } else {
                QPainterPath path;
                path.moveTo(toBoard(pressDots_.first().pos));
                for (int i = 1; i < pressDots_.size(); ++i)
                    path.lineTo(toBoard(pressDots_[i].pos));
                bp.drawPath(path);
            }
        }
    }
    preview_->setPixmap(board);
}

}  // namespace pittore::ui
