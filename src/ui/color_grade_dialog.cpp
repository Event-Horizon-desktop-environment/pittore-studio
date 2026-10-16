#include "ui/color_grade_dialog.h"
#include "ui/dpi_pixmap.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <chrono>
#include <cmath>

#include "engine/compute/adjust.h"
#include "engine/core/log.h"
#include "ui/app_state.h"

namespace pittore::ui {
namespace {

enum Track { TrackRamp, TrackRainbow, TrackTemp, TrackTintGM };

struct RowDef {
    pittore::compute::AdjustmentKind kind;
    int pi;
    const char* label;
    const char* suffix;
    double factor;
    double neutral;
    Track track;
};

const RowDef kExposureRows[] = {
    {pittore::compute::AdjustmentKind::Exposure, 0, "Exposure", "", 1.0, 0.0,
     TrackRamp},
    {pittore::compute::AdjustmentKind::Levels, 0, "Blackpoint", " %", 100.0,
     0.0, TrackRamp},
    {pittore::compute::AdjustmentKind::BrightnessContrast, 0, "Brightness",
     " %", 100.0, 0.0, TrackRamp},
};

const RowDef kEnhanceRows[] = {
    {pittore::compute::AdjustmentKind::BrightnessContrast, 1, "Contrast",
     " %", 100.0, 0.0, TrackRamp},
    {pittore::compute::AdjustmentKind::HueSaturation, 1, "Saturation", " %",
     100.0, 0.0, TrackRainbow},
    {pittore::compute::AdjustmentKind::Vibrance, 0, "Vibrance", " %", 100.0,
     0.0, TrackRainbow},
};

const RowDef kWhiteBalanceRows[] = {
    {pittore::compute::AdjustmentKind::WhiteBalance, 0, "Temperature", " K",
     1.0, 6500.0, TrackTemp},
    {pittore::compute::AdjustmentKind::WhiteBalance, 1, "Tint", " %", 100.0,
     0.0, TrackTintGM},
};

struct PresetItem {
    pittore::compute::AdjustmentKind kind;
    int pi;
    float value;
};

const PresetItem kPunchy[] = {
    {pittore::compute::AdjustmentKind::Exposure, 0, 0.3f},
    {pittore::compute::AdjustmentKind::BrightnessContrast, 1, 0.25f},
    {pittore::compute::AdjustmentKind::Vibrance, 0, 0.3f},
    {pittore::compute::AdjustmentKind::HueSaturation, 1, 0.1f},
};

const PresetItem kWarm[] = {
    {pittore::compute::AdjustmentKind::WhiteBalance, 0, 7200.0f},
    {pittore::compute::AdjustmentKind::WhiteBalance, 1, 0.15f},
    {pittore::compute::AdjustmentKind::HueSaturation, 1, 0.1f},
    {pittore::compute::AdjustmentKind::Exposure, 0, 0.1f},
};

QString trackCss(Track track) {
    QString stops;
    switch (track) {
        case TrackRainbow:
            stops = QStringLiteral(
                "stop:0 #ff0000, stop:0.17 #ffff00, stop:0.33 #00ff00, "
                "stop:0.5 #00ffff, stop:0.67 #0000ff, stop:0.83 #ff00ff, "
                "stop:1 #ff0000");
            break;
        case TrackTemp:
            stops = QStringLiteral(
                "stop:0 #27408b, stop:0.55 #e8e0d0, stop:1 #ff9a2e");
            break;
        case TrackTintGM:
            stops = QStringLiteral(
                "stop:0 #3fae5a, stop:0.5 #808080, stop:1 #c05ab0");
            break;
        case TrackRamp:
        default:
            stops = QStringLiteral("stop:0 #000000, stop:1 #ffffff");
            break;
    }
    return QStringLiteral(
               "QSlider::groove:horizontal {height:6px; border-radius:3px; "
               "background: qlineargradient(x1:0,y1:0,x2:1,y2:0, %1);} "
               "QSlider::handle:horizontal {width:14px; margin:-5px 0; "
               "border-radius:7px; background:#e8e8e8; border:1px solid "
               "#777;}")
        .arg(stops);
}

int decimalsFor(double span) {
    if (span > 100.0) return 0;
    if (span > 2.0) return 1;
    return 2;
}

// Live-update cadence (writes and preview share it): 30Hz keeps drags
// feeling instant while a storm still coalesces instead of queueing —
// trailing work never piles up because each flush stops with an empty map.
constexpr int kLiveMs = 33;

}  // namespace

ColorGradeDialog::ColorGradeDialog(AppState* state, QWidget* parent)
    : QDialog(parent), state_(state) {
    setWindowTitle(tr("Color Grading"));
    setMinimumSize(1000, 620);
    if (const QSettings settings(QStringLiteral("PittoreStudio"),
                                 QStringLiteral("painter"));
        settings.contains(QStringLiteral("ColorGrade/geometry")))
        restoreGeometry(
            settings.value(QStringLiteral("ColorGrade/geometry")).toByteArray());
    else if (const QSettings legacy(QStringLiteral("InfinityPhoto"),
                                    QStringLiteral("InfinityPhoto"));
             legacy.contains(QStringLiteral("ColorGrade/geometry")))
        restoreGeometry(
            legacy.value(QStringLiteral("ColorGrade/geometry")).toByteArray());

    DocumentItem* doc = state_->activeDocument();
    origActive_ = doc ? doc->activeLayer : -1;

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    // Preview column: live composite plus a hold-for-before button showing
    // the image as opened (no engine round trip, just the stashed pixmap).
    auto* left = new QVBoxLayout;
    preview_ = new QLabel(this);
    preview_->setObjectName(QStringLiteral("GradePreview"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(480, 340);
    left->addWidget(preview_, 1);
    auto* beforeHold = new QPushButton(tr("Hold for Before"), this);
    beforeHold->setToolTip(
        tr("Hold to compare against the image as this window opened."));
    connect(beforeHold, &QPushButton::pressed, this, [this] {
        if (!before_.isNull()) preview_->setPixmap(before_);
    });
    connect(beforeHold, &QPushButton::released, this,
            [this] { renderPreview(); });
    left->addWidget(beforeHold);
    root->addLayout(left, 1);

    auto* right = new QVBoxLayout;
    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel(tr("Preset:"), this));
    auto* preset = new QComboBox(this);
    preset->addItems({tr("Default"), tr("Punchy"), tr("Warm")});
    connect(preset, &QComboBox::activated, this,
            [this](int index) { applyPreset(index); });
    presetRow->addWidget(preset, 1);
    right->addLayout(presetRow);

    const struct {
        const char* name;
        const RowDef* rows;
        int count;
    } sections[] = {
        {"Exposure", kExposureRows, 3},
        {"Enhance", kEnhanceRows, 3},
        {"White Balance", kWhiteBalanceRows, 2},
    };
    auto* form = new QVBoxLayout;
    form->setSpacing(2);
    for (const auto& section : sections) {
        auto* header = new QWidget(this);
        auto* hrow = new QHBoxLayout(header);
        hrow->setContentsMargins(0, 6, 0, 2);
        auto* toggle = new QToolButton(header);
        toggle->setText(tr(section.name));
        toggle->setCheckable(true);
        toggle->setChecked(true);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setArrowType(Qt::DownArrow);
        auto* reset = new QPushButton(tr("Reset"), header);
        reset->setObjectName(QStringLiteral("SectionReset:") +
                             QString::fromUtf8(section.name));
        reset->setToolTip(tr("Reset this section to defaults."));
        reset->setAutoDefault(false);
        hrow->addWidget(toggle);
        hrow->addStretch(1);
        hrow->addWidget(reset);
        form->addWidget(header);

        auto* box = new QWidget(this);
        auto* grid = new QFormLayout(box);
        grid->setSpacing(4);
        QVector<int> sectionKinds;
        for (int r = 0; r < section.count; ++r) {
            const RowDef& def = section.rows[r];
            const QVector<AdjustmentParamDesc> descs =
                adjustmentParamDescs(static_cast<int>(def.kind));
            if (def.pi < 0 || def.pi >= descs.size()) continue;
            const AdjustmentParamDesc& dd = descs[def.pi];
            const int span = dd.sliderMax - dd.sliderMin;
            const double realSpan = dd.realMax - dd.realMin;
            const double dispSpan = realSpan * def.factor;
            Row row;
            row.kind = static_cast<int>(def.kind);
            row.pi = def.pi;
            row.label = QString::fromUtf8(def.label);
            row.suffix = QString::fromUtf8(def.suffix);
            row.factor = def.factor;
            row.sliderMin = dd.sliderMin;
            row.sliderMax = dd.sliderMax;
            row.realMin = dd.realMin;
            row.realMax = dd.realMax;
            row.neutral = def.neutral;
            row.trackCss = trackCss(def.track);
            auto* slider = new QSlider(Qt::Horizontal, this);
            slider->setObjectName(QStringLiteral("GradeSlider:") +
                                  row.label);
            slider->setRange(0, span > 0 ? span : 1);
            slider->setStyleSheet(row.trackCss);
            auto* spin = new QDoubleSpinBox(this);
            spin->setRange(dd.realMin * def.factor, dd.realMax * def.factor);
            spin->setDecimals(decimalsFor(dispSpan));
            spin->setSingleStep(dispSpan / 100.0);
            spin->setSuffix(row.suffix);
            spin->setFixedWidth(92);
            // Untouched sections show neutrals until their first write.
            {
                const QSignalBlocker b1(slider), b2(spin);
                slider->setValue(qBound(
                    0,
                    static_cast<int>(std::lround(
                        (def.neutral - dd.realMin) / realSpan * span)),
                    span));
                spin->setValue(def.neutral * def.factor);
            }
            connect(slider, &QSlider::sliderReleased, this,
                    [this] { flushWrites(); });
            connect(
                slider, &QSlider::valueChanged, this,
                [this, spin, row, realSpan, span](int v) mutable {
                    const double real =
                        row.realMin + realSpan * v / span;
                    const QSignalBlocker block(spin);
                    spin->setValue(real * row.factor);
                    requestWrite(row.kind, row.pi,
                                 static_cast<float>(real));
                });
            connect(
                spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, slider, row, realSpan, span](double disp) mutable {
                    const double real = disp / row.factor;
                    const QSignalBlocker block(slider);
                    slider->setValue(qBound(
                        0,
                        static_cast<int>(std::lround(
                            (real - row.realMin) / realSpan * span)),
                        span));
                    requestWrite(row.kind, row.pi,
                                 static_cast<float>(real));
                });
            auto* cell = new QWidget(this);
            auto* cellLayout = new QHBoxLayout(cell);
            cellLayout->setContentsMargins(0, 0, 0, 0);
            cellLayout->setSpacing(6);
            cellLayout->addWidget(slider, 1);
            cellLayout->addWidget(spin);
            grid->addRow(row.label, cell);
            row.slider = slider;
            row.spin = spin;
            rows_.push_back(row);
            if (!sectionKinds.contains(row.kind))
                sectionKinds.push_back(row.kind);
        }
        form->addWidget(box);
        connect(toggle, &QToolButton::toggled, box, &QWidget::setVisible);
        connect(toggle, &QToolButton::toggled, this, [toggle](bool on) {
            toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        });
        connect(reset, &QPushButton::clicked, this,
                [this, sectionKinds] { resetSection(sectionKinds); });
    }
    right->addLayout(form);
    right->addStretch(1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { reject(); });
    right->addWidget(buttons);
    root->addLayout(right, 1);

    writeTimer_ = new QTimer(this);
    writeTimer_->setInterval(kLiveMs);
    connect(writeTimer_, &QTimer::timeout, this, [this] {
        if (pending_.isEmpty()) {
            writeTimer_->stop();
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const auto batch = pending_;
        pending_.clear();
        for (auto it = batch.constBegin(); it != batch.constEnd(); ++it)
            writeThrough(it.key().first, it.key().second, it.value());
        // End-to-end tick: param writes + LUT + full rebuilds + synchronous
        // emit handlers (panel rows, canvas refresh scheduling). Slow ticks
        // always report so grade-dialog drag attribution is visible.
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        if (pittore::core::log::strokeTrace() ||
            ms >= pittore::core::log::slowEventMs())
            PITTORE_LOG("[grade] tick params=%d ms=%.2f",
                         static_cast<int>(batch.size()), ms);
    });

    previewTimer_ = new QTimer(this);
    previewTimer_->setInterval(kLiveMs);
    connect(previewTimer_, &QTimer::timeout, this, [this] {
        if (!previewPending_) {
            previewTimer_->stop();
            return;
        }
        previewPending_ = false;
        renderPreview();
    });
    connect(state_, &AppState::documentModified, this,
            [this] { schedulePreview(); });

    syncRows();
    renderPreview();
    before_ = preview_->pixmap(Qt::ReturnByValue);
}

int ColorGradeDialog::findLayer(int kind) const {
    DocumentItem* d = state_->activeDocument();
    if (!d) return -1;
    for (int i = 0; i < d->layers.size(); ++i) {
        const LayerItem& l = d->layers[i];
        if (l.kind == LayerItem::Kind::Adjustment && l.adjustmentKind == kind)
            return i;
    }
    return -1;
}

int ColorGradeDialog::ensureLayer(int kind) {
    const int found = findLayer(kind);
    if (found >= 0) return found;
    // Inserting shifts layer indices, which pending writes key on: flush
    // first so the trailing batch always resolves against fresh indices.
    flushWrites();
    if (!state_->addAdjustmentLayer(kind)) return -1;
    DocumentItem* d = state_->activeDocument();
    return d ? d->activeLayer : -1;
}

void ColorGradeDialog::beginGradeGesture() {
    if (gradeGesture_ || !state_->activeDocument()) return;
    state_->beginUndoStep();
    gradeGesture_ = true;
}

void ColorGradeDialog::writeThrough(int layer, int pi, float value) {
    state_->setAdjustmentParamAt(layer, pi, value);
}

void ColorGradeDialog::requestWrite(int kind, int pi, float value) {
    beginGradeGesture();
    const int index = ensureLayer(kind);
    if (index < 0) return;
    if (!writeTimer_->isActive()) {
        writeThrough(index, pi, value);
        writeTimer_->start();
    } else {
        pending_.insert({index, pi}, value);
    }
}

void ColorGradeDialog::flushWrites() {
    if (pending_.isEmpty()) {
        writeTimer_->stop();
        return;
    }
    const auto batch = pending_;
    pending_.clear();
    writeTimer_->stop();
    for (auto it = batch.constBegin(); it != batch.constEnd(); ++it)
        writeThrough(it.key().first, it.key().second, it.value());
}

void ColorGradeDialog::syncRows() {
    for (Row& row : rows_) {
        const int index = findLayer(row.kind);
        const double real =
            index >= 0
                ? state_->activeDocument()->layers[index].adjustmentParams[row.pi]
                : row.neutral;
        const int span = row.sliderMax - row.sliderMin > 0
                             ? row.sliderMax - row.sliderMin
                             : 1;
        const double realSpan = row.realMax - row.realMin;
        const QSignalBlocker b1(row.slider), b2(row.spin);
        row.slider->setValue(qBound(
            0,
            static_cast<int>(std::lround((real - row.realMin) / realSpan *
                                         span)),
            span));
        row.spin->setValue(real * row.factor);
    }
}

void ColorGradeDialog::applyPreset(int preset) {
    const PresetItem* items = nullptr;
    int count = 0;
    if (preset == 1) {
        items = kPunchy;
        count = 4;
    } else if (preset == 2) {
        items = kWarm;
        count = 4;
    } else {
        // Default: neutralize every row through the normal write path so
        // layers are ensured inside the session step like any other edit.
        flushWrites();
        beginGradeGesture();
        for (const Row& row : rows_) {
            const int index = ensureLayer(row.kind);
            if (index < 0) continue;
            writeThrough(index, row.pi, static_cast<float>(row.neutral));
        }
        syncRows();
        return;
    }
    flushWrites();
    beginGradeGesture();
    for (int i = 0; i < count; ++i) {
        const int kind = static_cast<int>(items[i].kind);
        const int index = ensureLayer(kind);
        if (index < 0) continue;
        writeThrough(index, items[i].pi, items[i].value);
    }
    syncRows();
}

void ColorGradeDialog::resetSection(const QVector<int>& kinds) {
    flushWrites();
    beginGradeGesture();
    for (int kind : kinds) {
        const int index = findLayer(kind);
        if (index < 0) continue;
        state_->resetAdjustmentAt(index);
    }
    syncRows();
}

void ColorGradeDialog::schedulePreview() {
    if (!previewTimer_->isActive()) {
        renderPreview();
        previewTimer_->start();
    } else {
        previewPending_ = true;
    }
}

void ColorGradeDialog::renderPreview() {
    if (!preview_) return;
    DocumentItem* d = state_->activeDocument();
    if (!d || d->composite.isNull()) {
        preview_->clear();
        return;
    }
    // Fast (nearest) downscale: at preview size it is visually identical to
    // smooth and an order of magnitude cheaper on large composites, so the
    // preview never dominates the tick that just rebuilt the composite.
    const auto pt0 = std::chrono::steady_clock::now();
    preview_->setPixmap(pixmapForWidget(d->composite.scaled(
        QSize(520, 360), Qt::KeepAspectRatio, Qt::FastTransformation),
        preview_));
    const double pms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - pt0)
                           .count();
    if (pms >= pittore::core::log::slowEventMs())
        PITTORE_LOG("[grade] preview scale ms=%.2f", pms);
}

void ColorGradeDialog::accept() {
    flushWrites();
    if (gradeGesture_) {
        gradeGesture_ = false;
        state_->commitUndoStep(tr("Color Grade"), QStringLiteral("adjustments"));
    }
    QSettings(QStringLiteral("PittoreStudio"), QStringLiteral("painter"))
        .setValue(QStringLiteral("ColorGrade/geometry"), saveGeometry());
    if (DocumentItem* d = state_->activeDocument()) {
        const int count = static_cast<int>(d->layers.size());
        state_->setActiveLayerIndex(origActive_ >= 0 && origActive_ < count
                                        ? origActive_
                                        : d->activeLayer);
    }
    QDialog::accept();
}

void ColorGradeDialog::reject() {
    // Drop anything throttled but never written, then rewind the whole
    // session (edits and created layers) like an undo of one step.
    pending_.clear();
    writeTimer_->stop();
    if (gradeGesture_) {
        gradeGesture_ = false;
        state_->cancelUndoStep();
    }
    QSettings(QStringLiteral("PittoreStudio"), QStringLiteral("painter"))
        .setValue(QStringLiteral("ColorGrade/geometry"), saveGeometry());
    if (DocumentItem* d = state_->activeDocument()) {
        const int count = static_cast<int>(d->layers.size());
        state_->setActiveLayerIndex(origActive_ >= 0 && origActive_ < count
                                        ? origActive_
                                        : d->activeLayer);
    }
    QDialog::reject();
}

}  // namespace pittore::ui
