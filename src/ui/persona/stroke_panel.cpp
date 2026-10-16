#include "ui/persona/stroke_panel.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include "ui/app_state.h"
#include "ui/panels/registry/panel_creators.h"
#include "ui/persona/vector_edit.h"

namespace pittore::ui {
namespace {

QColor paintColor(const std::uint8_t rgba[4]) {
    return QColor(rgba[0], rgba[1], rgba[2], rgba[3]);
}

void setPaintColor(std::uint8_t rgba[4], const QColor& c) {
    rgba[0] = static_cast<std::uint8_t>(c.red());
    rgba[1] = static_cast<std::uint8_t>(c.green());
    rgba[2] = static_cast<std::uint8_t>(c.blue());
    rgba[3] = static_cast<std::uint8_t>(c.alpha());
}

void styleColorButton(QPushButton* button, const QColor& c) {
    button->setStyleSheet(QStringLiteral("background: %1; border: 1px solid #888;")
                              .arg(c.name(QColor::HexArgb)));
    button->setText(c.name(QColor::HexRgb).toUpper());
}

}  // namespace

StrokePanel::StrokePanel(AppState* state, QWidget* parent)
    : QWidget(parent), state_(state) {
    // Scrollable body: the form is taller than most docks, and a plain
    // layout would lift the window's minimum height when the Vector persona
    // shows this panel. The scroll area keeps the dock floor small, so the
    // window stays resizable on small logical screens.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget;
    auto* column = new QVBoxLayout(body);
    column->setContentsMargins(12, 10, 12, 10);
    column->setSpacing(8);
    scroll->setWidget(body);
    outer->addWidget(scroll);

    title_ = new QLabel(tr("No vector layer selected"), body);
    title_->setWordWrap(true);
    column->addWidget(title_);

    auto* form = new QFormLayout();
    form->setSpacing(6);
    column->addLayout(form);

    fillCheck_ = new QCheckBox(tr("Fill"), this);
    fillButton_ = new QPushButton(this);
    fillButton_->setCursor(Qt::PointingHandCursor);
    auto* fillRow = new QWidget(this);
    auto* fillLayout = new QHBoxLayout(fillRow);
    fillLayout->setContentsMargins(0, 0, 0, 0);
    fillLayout->addWidget(fillCheck_);
    fillLayout->addWidget(fillButton_, 1);
    form->addRow(fillRow);

    strokeCheck_ = new QCheckBox(tr("Stroke"), this);
    strokeButton_ = new QPushButton(this);
    strokeButton_->setCursor(Qt::PointingHandCursor);
    auto* strokeRow = new QWidget(this);
    auto* strokeLayout = new QHBoxLayout(strokeRow);
    strokeLayout->setContentsMargins(0, 0, 0, 0);
    strokeLayout->addWidget(strokeCheck_);
    strokeLayout->addWidget(strokeButton_, 1);
    form->addRow(strokeRow);

    widthSpin_ = new QDoubleSpinBox(this);
    widthSpin_->setRange(0.01, 1000.0);
    widthSpin_->setSingleStep(0.5);
    widthSpin_->setDecimals(2);
    widthSpin_->setSuffix(tr(" px"));
    widthSpin_->setKeyboardTracking(false);
    form->addRow(tr("Width:"), widthSpin_);

    profileLabel_ = new QLabel(this);
    profileReset_ = new QPushButton(tr("Reset"), this);
    profileReset_->setCursor(Qt::PointingHandCursor);
    profileReset_->setToolTip(
        tr("Clear the variable-width profile back to a uniform stroke."));
    auto* profileRow = new QWidget(this);
    auto* profileLayout = new QHBoxLayout(profileRow);
    profileLayout->setContentsMargins(0, 0, 0, 0);
    profileLayout->addWidget(profileLabel_, 1);
    profileLayout->addWidget(profileReset_);
    form->addRow(tr("Profile:"), profileRow);

    capCombo_ = new QComboBox(this);
    capCombo_->addItems({tr("Butt cap"), tr("Round cap"), tr("Square cap")});
    form->addRow(tr("Cap:"), capCombo_);

    joinCombo_ = new QComboBox(this);
    joinCombo_->addItems({tr("Miter join"), tr("Round join"), tr("Bevel join")});
    form->addRow(tr("Join:"), joinCombo_);

    dashCombo_ = new QComboBox(this);
    dashCombo_->addItems({tr("Solid"), tr("Dashed"), tr("Dotted"),
                          tr("Dash-Dot"), tr("Custom")});
    form->addRow(tr("Dash:"), dashCombo_);

    dashOnSpin_ = new QDoubleSpinBox(this);
    dashOnSpin_->setRange(0.0, 1000.0);
    dashOnSpin_->setSingleStep(0.5);
    dashOnSpin_->setDecimals(2);
    dashOnSpin_->setSuffix(tr(" ×w"));
    dashOnSpin_->setKeyboardTracking(false);
    dashOnSpin_->setToolTip(tr("Dash length, in stroke widths (0 = dot)."));
    form->addRow(tr("Dash on:"), dashOnSpin_);

    dashOffSpin_ = new QDoubleSpinBox(this);
    dashOffSpin_->setRange(0.0, 1000.0);
    dashOffSpin_->setSingleStep(0.5);
    dashOffSpin_->setDecimals(2);
    dashOffSpin_->setSuffix(tr(" ×w"));
    dashOffSpin_->setKeyboardTracking(false);
    dashOffSpin_->setToolTip(tr("Gap length, in stroke widths."));
    form->addRow(tr("Dash off:"), dashOffSpin_);

    dashOffsetSpin_ = new QDoubleSpinBox(this);
    dashOffsetSpin_->setRange(-1000.0, 1000.0);
    dashOffsetSpin_->setSingleStep(0.5);
    dashOffsetSpin_->setDecimals(2);
    dashOffsetSpin_->setSuffix(tr(" ×w"));
    dashOffsetSpin_->setKeyboardTracking(false);
    form->addRow(tr("Dash offset:"), dashOffsetSpin_);

    opacitySlider_ = new QSlider(Qt::Horizontal, this);
    opacitySlider_->setRange(1, 100);
    opacityLabel_ = new QLabel(QStringLiteral("100%"), this);
    auto* opacityRow = new QWidget(this);
    auto* opacityLayout = new QHBoxLayout(opacityRow);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    opacityLayout->addWidget(opacitySlider_, 1);
    opacityLayout->addWidget(opacityLabel_);
    form->addRow(tr("Opacity:"), opacityRow);

    auto* note = new QLabel(tr("Align — planned."), this);
    note->setWordWrap(true);
    note->setEnabled(false);
    column->addWidget(note);
    column->addStretch(1);

    // One undo step per discrete gesture (never per slider tick).
    connect(fillButton_, &QPushButton::clicked, this, [this] { pickColor(true); });
    connect(strokeButton_, &QPushButton::clicked, this, [this] { pickColor(false); });
    connect(fillCheck_, &QCheckBox::toggled, this, [this](bool on) {
        if (layerIndex_ < 0) return;
        DocumentItem* d = state_->activeDocument();
        if (!d || !d->layers[layerIndex_].art) return;
        auto paint = d->layers[layerIndex_].art->paint;
        paint.hasFill = on;
        if (!on && !paint.hasStroke) {
            // A coat of no paint renders nothing; keep the stroke instead.
            paint.hasStroke = true;
            const QSignalBlocker block(strokeCheck_);
            strokeCheck_->setChecked(true);
        }
        commit(paint, d->layers[layerIndex_].art->opacity, tr("Fill"));
    });
    connect(strokeCheck_, &QCheckBox::toggled, this, [this](bool on) {
        if (layerIndex_ < 0) return;
        DocumentItem* d = state_->activeDocument();
        if (!d || !d->layers[layerIndex_].art) return;
        auto paint = d->layers[layerIndex_].art->paint;
        paint.hasStroke = on;
        if (!on && !paint.hasFill) {
            paint.hasFill = true;
            const QSignalBlocker block(fillCheck_);
            fillCheck_->setChecked(true);
        }
        commit(paint, d->layers[layerIndex_].art->opacity, tr("Stroke"));
    });
    connect(widthSpin_, &QDoubleSpinBox::editingFinished, this, [this] {
        if (layerIndex_ < 0) return;
        DocumentItem* d = state_->activeDocument();
        if (!d || !d->layers[layerIndex_].art) return;
        auto paint = d->layers[layerIndex_].art->paint;
        paint.strokeWidth = widthSpin_->value();
        commit(paint, d->layers[layerIndex_].art->opacity, tr("Stroke Width"));
    });
    connect(profileReset_, &QPushButton::clicked, this, [this] {
        state_->resetStrokeProfile();
    });
    connect(capCombo_, &QComboBox::activated, this, [this](int index) {
        if (layerIndex_ < 0) return;
        DocumentItem* d = state_->activeDocument();
        if (!d || !d->layers[layerIndex_].art) return;
        auto paint = d->layers[layerIndex_].art->paint;
        paint.cap = index;
        commit(paint, d->layers[layerIndex_].art->opacity, tr("Line Cap"));
    });
    connect(joinCombo_, &QComboBox::activated, this, [this](int index) {
        if (layerIndex_ < 0) return;
        DocumentItem* d = state_->activeDocument();
        if (!d || !d->layers[layerIndex_].art) return;
        auto paint = d->layers[layerIndex_].art->paint;
        paint.join = index;
        commit(paint, d->layers[layerIndex_].art->opacity, tr("Line Join"));
    });
    // Dash presets write the spins, then commit; Custom commits the spins
    // as-is. Lengths are stroke-width multiples; on=0 draws dots (needs a
    // round cap, which Dotted sets for free).
    auto commitDash = [this](const QString& undoName) {
        if (layerIndex_ < 0) return;
        DocumentItem* d = state_->activeDocument();
        if (!d || !d->layers[layerIndex_].art) return;
        auto paint = d->layers[layerIndex_].art->paint;
        const int preset = dashCombo_->currentIndex();
        paint.hasDash = preset > 0;
        paint.dash.clear();
        if (preset > 0) {
            const float on =
                static_cast<float>(qMax(0.0, dashOnSpin_->value()));
            const float off =
                static_cast<float>(qMax(0.0, dashOffSpin_->value()));
            paint.dash.push_back(on);
            paint.dash.push_back(off);
            if (preset == 3) {  // Dash-Dot appends the dot run.
                paint.dash.push_back(0.0f);
                paint.dash.push_back(off);
            }
            if (preset == 2 || preset == 3) paint.cap = 1;  // dots need round
        }
        paint.dashOffset = static_cast<float>(dashOffsetSpin_->value());
        commit(paint, d->layers[layerIndex_].art->opacity, undoName);
    };
    connect(dashCombo_, &QComboBox::activated, this, [this, commitDash](int index) {
        const QSignalBlocker b0(dashOnSpin_), b1(dashOffSpin_);
        if (index == 1) {  // Dashed
            dashOnSpin_->setValue(4.0);
            dashOffSpin_->setValue(2.0);
        } else if (index == 2) {  // Dotted
            dashOnSpin_->setValue(0.0);
            dashOffSpin_->setValue(2.0);
        }
        commitDash(tr("Dash"));
    });
    connect(dashOnSpin_, &QDoubleSpinBox::editingFinished, this,
            [commitDash] { commitDash(tr("Dash")); });
    connect(dashOffSpin_, &QDoubleSpinBox::editingFinished, this,
            [commitDash] { commitDash(tr("Dash")); });
    connect(dashOffsetSpin_, &QDoubleSpinBox::editingFinished, this,
            [commitDash] { commitDash(tr("Dash Offset")); });
    connect(opacitySlider_, &QSlider::valueChanged, this, [this](int v) {
        opacityLabel_->setText(QStringLiteral("%1%").arg(v));
    });
    connect(opacitySlider_, &QSlider::sliderReleased, this, [this] {
        if (layerIndex_ < 0) return;
        DocumentItem* d = state_->activeDocument();
        if (!d || !d->layers[layerIndex_].art) return;
        commit(d->layers[layerIndex_].art->paint,
               opacitySlider_->value() / 100.0, tr("Opacity"));
    });

    connect(state_, &AppState::layersChanged, this, [this] { rebuild(); });
    connect(state_, &AppState::historyChanged, this, [this] { rebuild(); });
    connect(state_, &AppState::activeLayerChanged, this, [this] { rebuild(); });
    connect(state_, &AppState::activeDocumentChanged, this, [this] { rebuild(); });

    rebuild();
}

void StrokePanel::commit(const pittore::vector::ArtPaint& paint, double opacity,
                         const QString& undoName) {
    if (layerIndex_ < 0) return;
    state_->applyVectorPaint(layerIndex_, paint, opacity, undoName);
}

void StrokePanel::pickColor(bool fill) {
    if (layerIndex_ < 0) return;
    DocumentItem* d = state_->activeDocument();
    if (!d || !d->layers[layerIndex_].art) return;
    const auto& paint = d->layers[layerIndex_].art->paint;
    const QColor start = paintColor(fill ? paint.fill : paint.stroke);
    const QColor picked = QColorDialog::getColor(
        start, this, fill ? tr("Fill Color") : tr("Stroke Color"),
        QColorDialog::ShowAlphaChannel);
    if (!picked.isValid()) return;
    auto next = paint;
    if (fill) {
        next.hasFill = true;
        setPaintColor(next.fill, picked);
        const QSignalBlocker block(fillCheck_);
        fillCheck_->setChecked(true);
    } else {
        next.hasStroke = true;
        setPaintColor(next.stroke, picked);
        const QSignalBlocker block(strokeCheck_);
        strokeCheck_->setChecked(true);
    }
    commit(next, d->layers[layerIndex_].art->opacity,
           fill ? tr("Fill Color") : tr("Stroke Color"));
}

void StrokePanel::rebuild() {
    layerIndex_ = vectorEditableLayer(state_);
    DocumentItem* d = state_->activeDocument();
    const pittore::vector::ArtNode* art =
        (d && layerIndex_ >= 0) ? d->layers[layerIndex_].art.get() : nullptr;

    const QSignalBlocker b0(fillCheck_), b1(strokeCheck_), b2(widthSpin_),
        b3(capCombo_), b4(joinCombo_), b5(opacitySlider_), b6(dashCombo_),
        b7(dashOnSpin_), b8(dashOffSpin_), b9(dashOffsetSpin_);
    const bool enabled = art != nullptr;
    title_->setText(art ? d->layers[layerIndex_].name
                        : tr("No vector layer selected"));
    fillCheck_->setEnabled(enabled);
    fillButton_->setEnabled(enabled);
    strokeCheck_->setEnabled(enabled);
    strokeButton_->setEnabled(enabled);
    widthSpin_->setEnabled(enabled);
    capCombo_->setEnabled(enabled);
    joinCombo_->setEnabled(enabled);
    profileLabel_->setEnabled(enabled);
    profileReset_->setEnabled(false);  // re-armed below when profiled
    dashCombo_->setEnabled(enabled);
    dashOnSpin_->setEnabled(enabled);
    dashOffSpin_->setEnabled(enabled);
    dashOffsetSpin_->setEnabled(enabled);
    opacitySlider_->setEnabled(enabled);
    if (!art) return;

    const auto& paint = art->paint;
    fillCheck_->setChecked(paint.hasFill);
    strokeCheck_->setChecked(paint.hasStroke);
    styleColorButton(fillButton_, paintColor(paint.fill));
    styleColorButton(strokeButton_, paintColor(paint.stroke));
    widthSpin_->setValue(paint.strokeWidth);
    capCombo_->setCurrentIndex(qBound(0, paint.cap, 2));
    joinCombo_->setCurrentIndex(qBound(0, paint.join, 2));
    const bool profiled = paint.hasStroke && paint.hasProfile &&
                          !paint.profile.empty();
    profileLabel_->setText(
        profiled ? tr("%1 points").arg(paint.profile.size()) : tr("Uniform"));
    profileReset_->setEnabled(profiled);
    int dashPreset = 0;
    double dashOn = 4.0, dashOff = 2.0;
    if (paint.hasDash && paint.dash.size() >= 2) {
        dashOn = paint.dash[0];
        dashOff = paint.dash[1];
        if (dashOn <= 0.0)
            dashPreset = 2;  // Dotted
        else if (paint.dash.size() == 2)
            dashPreset = 1;  // Dashed
        else if (paint.dash.size() == 4 && paint.dash[2] <= 0.0)
            dashPreset = 3;  // Dash-Dot
        else
            dashPreset = 4;  // Custom
    } else if (paint.hasDash) {
        dashPreset = 4;
    }
    dashCombo_->setCurrentIndex(dashPreset);
    dashOnSpin_->setValue(dashOn);
    dashOffSpin_->setValue(dashOff);
    dashOffsetSpin_->setValue(paint.dashOffset);
    const int pct = qRound(qBound(0.0, art->opacity, 1.0) * 100.0);
    opacitySlider_->setValue(pct);
    opacityLabel_->setText(QStringLiteral("%1%").arg(pct));
}

QWidget* createStrokePanel(AppState* state, QWidget* parent) {
    return new StrokePanel(state, parent);
}

}  // namespace pittore::ui
