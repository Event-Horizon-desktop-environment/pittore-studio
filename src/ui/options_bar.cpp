#include "ui/options_bar.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGuiApplication>
#include <QWindow>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QGridLayout>
#include <QInputDialog>
#include <QToolButton>
#include <QWidgetAction>

#include <cmath>

#include <algorithm>
#include <vector>

#include "ui/brushes/brush_library.h"
#include "ui/brushes/brush_preview.h"
#include "ui/brushes/sensor_drives_dialog.h"
#include "ui/brush_popup_scale.h"
#include "ui/dpi_pixmap.h"
#include "ui/flow_layout.h"
#include "ui/icons.h"
#include "ui/panels/shared/panel_helpers.h"
#include "ui/font_preview.h"
#include "ui/tools/log/tool_log.h"

namespace pittore::ui {
namespace {

// Flat colour chip that opens the picker.
class ColorChip final : public QToolButton {
  public:
    ColorChip(QColor initial, QWidget* parent) : QToolButton(parent), color_(initial) {
        setFixedSize(34, 20);
        setCursor(Qt::PointingHandCursor);
    }

    QColor color() const { return color_; }
    void setColor(QColor c) {
        color_ = c;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = rect().adjusted(1, 1, -1, -1);
        if (color_.alpha() < 255) {
            // Checkerboard behind translucent fills.
            for (int y = 0; y < r.height(); y += 5)
                for (int x = 0; x < r.width(); x += 5)
                    p.fillRect(QRectF(r.left() + x, r.top() + y, 5, 5),
                               ((x / 5 + y / 5) % 2) ? QColor(0x99, 0x99, 0x99)
                                                     : QColor(0xcc, 0xcc, 0xcc));
        }
        p.fillRect(r, color_);
        p.setPen(QPen(QColor(0, 0, 0, 120), 1));
        p.drawRect(r);
    }

  private:
    QColor color_;
};

// Logarithmic brush-size mapping shared by the preset popup: slider
// 0..1000 -> 1..5000 px, so small brushes get fine steps while the full
// range stays reachable.
inline int brushSizeToSlider(int px) {
    return qBound(0, int(std::round(1000.0 * std::log(std::max(1, px)) /
                                    std::log(5000.0))),
                  1000);
}
inline int brushSliderToSize(int v) {
    return qMax(1, int(std::round(std::pow(5000.0, v / 1000.0))));
}

// One popup slider row: label + groove + value box driving one option.
// Size uses a logarithmic slider (see brushSizeToSlider); the rest are
// linear. The spin box mirrors the slider both ways.
struct BrushSliderRow {
    QSlider* slider = nullptr;
    QSpinBox* spin = nullptr;
    QLabel* label = nullptr;
    QString name;
    QByteArray optionId;
    bool logScale = false;
};

// One popup combo row: label + widget + the option it drives.
struct BrushComboRow {
    QComboBox* combo = nullptr;
    QByteArray optionId;
    QVariant fallback;
};

// Transparency grid brush, generated in code (no assets): theme-aware
// pair that reads on dark and light themes alike.
inline QBrush checkerBrush(const QPalette& pal, int cell = 12) {
    const QColor base = pal.color(QPalette::Base);
    const QColor lite =
        base.value() < 128 ? base.lighter(135) : base.darker(115);
    QImage tile(cell * 2, cell * 2, QImage::Format_ARGB32);
    tile.fill(lite);
    QPainter p(&tile);
    p.fillRect(0, 0, cell, cell, base);
    p.fillRect(cell, cell, cell, cell, base);
    p.end();
    return QBrush(tile);
}

// Refit a QMenu around its QWidgetAction content. A visible QMenu caches
// action geometry and ignores adjustSize(), so growing content (expanded
// sections) would clip instead of expanding down. Measure the panel layout
// directly, resize explicitly, and slide up only if the screen runs out.
inline void refitBrushMenu(QMenu* menu, QWidget* panel,
                           const QSize& chrome) {
    if (!menu || !panel || !panel->layout()) return;
    panel->layout()->activate();
    const QSize need = panel->layout()->totalSizeHint();
    menu->resize(menu->width(), need.height() + chrome.height());
    if (QScreen* screen = menu->screen()) {
        const QRect avail = screen->availableGeometry();
        QRect g = menu->geometry();
        if (g.bottom() > avail.bottom())
            g.moveBottom(qMax(avail.top(), avail.bottom()));
        if (g.top() < avail.top()) g.moveTop(avail.top());
        if (g != menu->geometry()) menu->setGeometry(g);
    }
}

// Brush preset well: size/hardness popup shared by painting tools.
class BrushPresetButton final : public QToolButton {
  public:
    BrushPresetButton(AppState* state, QWidget* parent) : QToolButton(parent), state_(state) {
        setFixedSize(92, 22);
        setPopupMode(QToolButton::InstantPopup);
        setToolTip(QObject::tr("Brush preset: size, hardness and tip"));

        auto* menu = new QMenu(this);
        auto* panel = new QWidget(menu);
        brushMenu_ = menu;
        brushPanel_ = panel;
        // Popup frame: title row, three content columns, footer actions.
        auto* outer = new QVBoxLayout(panel);
        outer->setContentsMargins(16, 14, 16, 14);
        outer->setSpacing(12);
        // Title: dialog name left, live preset name right.
        auto* titleRow = new QHBoxLayout();
        titleRow->setContentsMargins(0, 0, 0, 0);
        auto* titleLabel = new QLabel(QObject::tr("Brush settings"), panel);
        QFont titleFont = titleLabel->font();
        titleFont.setBold(true);
        titleFont.setPixelSize(titleFont.pixelSize() + 1);
        titleLabel->setFont(titleFont);
        titleRow->addWidget(titleLabel);
        titleRow->addStretch(1);
        titlePreset_ = new QLabel(panel);
        titlePreset_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        titleRow->addWidget(titlePreset_);
        outer->addLayout(titleRow);
        // Three columns: core settings, advanced settings, and the brush
        // gallery (live dab previews to pick from). Two settings columns
        // keep every expanded row on screen without any scrolling, so all
        // targets stay tablet-sized.
        auto* panes = new QHBoxLayout();
        panes->setContentsMargins(0, 0, 0, 0);
        panes->setSpacing(16);
        outer->addLayout(panes);
        auto divider = [&]() {
            // Crisp 1 px rule, no frame bevel or palette tint. Kept
            // translucent white so it reads as a hairline, not a bar.
            auto* line = new QFrame(panel);
            line->setFrameShape(QFrame::NoFrame);
            line->setFixedWidth(1);
            line->setStyleSheet(QStringLiteral(
                "QFrame { background: #585c62; border: none; }"));
            panes->addWidget(line);
        };
        auto* leftA = new QWidget(panel);
        auto* formA = new QVBoxLayout(leftA);
        formA->setContentsMargins(0, 0, 0, 0);
        formA->setSpacing(4);
        auto* leftB = new QWidget(panel);
        auto* formB = new QVBoxLayout(leftB);
        formB->setContentsMargins(0, 0, 0, 0);
        formB->setSpacing(4);
        // Fixed controls width: slider rows need ~80 (label) + 10 + 48
        // (value) at minimum, so the fixed gallery column can never
        // squeeze them into overlap no matter how the menu sizes itself.
        leftA->setFixedWidth(250);
        leftB->setFixedWidth(260);
        paneA_ = leftA;
        paneB_ = leftB;
        panes->addWidget(leftA);
        divider();
        panes->addWidget(leftB);
        divider();
        // Theme-derived paint: grooves, boxes and pills follow the palette
        // instead of hardcoded shades, so the popup survives theme changes.
        const QPalette pal = panel->palette();
        const QString grooveBg =
            pal.color(QPalette::Window).darker(115).name();
        const QString boxBg = pal.color(QPalette::Base).name();
        // Neutral gray derived from the window tone (never the tinted Mid role).
        const QString boxBorder =
            pal.color(QPalette::Window).lighter(170).name();
        const QString accent = pal.color(QPalette::Highlight).name();
        const QString dimText =
            pal.color(QPalette::PlaceholderText).name();

        // Menu chrome (frame + margins) measured once while both size
        // hints are fresh; per-toggle deltas come from the panel layout.
        // Captured by value: toggles fire long after this returns.
        const QSize menuChrome =
            (menu->sizeHint() - panel->sizeHint()).expandedTo(QSize(0, 0));

        // Label + groove + value box: the groove fills in accent to the
        // handle, the box edits the same value directly. Compact 14 px
        // handles stay pen-usable; spin boxes in the bar keep precise
        // entry. Sections below collapse the advanced rows away.
        const QString sliderCss = QStringLiteral(
            "QSlider::groove:horizontal { height: 4px; background: %1; "
            "border-radius: 2px; }"
            "QSlider::sub-page:horizontal { background: %2; "
            "border-radius: 2px; }"
            "QSlider::add-page:horizontal { background: %1; "
            "border-radius: 2px; }"
            "QSlider::handle:horizontal { background: white; border: none; "
            "width: 12px; height: 12px; margin: -4px 0; border-radius: 6px; }")
            .arg(grooveBg, accent);
        const QString spinCss = QStringLiteral(
            "QSpinBox { background: %1; border: 1px solid %2; "
            "border-radius: 5px; padding: 2px 6px; min-height: 22px; }")
            .arg(boxBg, boxBorder);
        QVBoxLayout* form = formA;
        QVBoxLayout* section = formA;
        auto beginSection = [&](const QString& title, bool expanded,
                                QVBoxLayout* target) {
            form = target;
            auto* header = new QToolButton(panel);
            header->setText(title);
            header->setCheckable(true);
            header->setChecked(expanded);
            header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            header->setStyleSheet(QStringLiteral(
                "QToolButton { border: none; font-weight: 600; "
                "padding: 6px 0px 4px 0px; background: transparent; }"
                "QToolButton:checked { background: transparent; }"
                "QToolButton:hover { background: transparent; }"));
            form->addWidget(header);
            auto* body = new QWidget(panel);
            auto* layout = new QVBoxLayout(body);
            layout->setContentsMargins(4, 0, 0, 8);
            layout->setSpacing(2);
            body->setVisible(expanded);
            form->addWidget(body);
            QObject::connect(header, &QToolButton::toggled, panel,
                             [header, body, panel, menu, menuChrome](bool on) {
                                 body->setVisible(on);
                                 header->setArrowType(on ? Qt::DownArrow
                                                        : Qt::RightArrow);
                                 refitBrushMenu(menu, panel, menuChrome);
                             });
            section = layout;
        };
        auto addSlider = [&](const QString& name, int min, int max, int value,
                             const char* optionId, bool logScale = false) {
            auto* row = new QWidget(panel);
            row->setFixedHeight(26);
            popupRows_.push_back({row, 26});
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(10);
            auto* label = new QLabel(name, panel);
            label->setFixedWidth(80);
            popupLabels_.push_back(label);
            label->setStyleSheet(QStringLiteral("color: %1;").arg(dimText));
            auto* slider = new QSlider(Qt::Horizontal, panel);
            slider->setRange(logScale ? 0 : min, logScale ? 1000 : max);
            slider->setValue(logScale ? brushSizeToSlider(value) : value);
            slider->setMinimumWidth(60);
            slider->setSizePolicy(QSizePolicy::Expanding,
                                  QSizePolicy::Fixed);
            slider->setStyleSheet(sliderCss);
            slider->setProperty("optionId", QByteArray(optionId));
            auto* spin = new QSpinBox(panel);
            spin->setRange(logScale ? 1 : min, logScale ? 5000 : max);
            spin->setValue(logScale ? qBound(1, value, 5000)
                                    : qBound(min, value, max));
            spin->setFixedWidth(48);
            popupSpins_.push_back(spin);
            spin->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            spin->setStyleSheet(spinCss);
            QObject::connect(slider, &QSlider::valueChanged, panel,
                             [this, spin, optionId, logScale](int v) {
                                 const int shown =
                                     logScale ? brushSliderToSize(v) : v;
                                 spin->blockSignals(true);
                                 spin->setValue(shown);
                                 spin->blockSignals(false);
                                 state_->setOption(state_->activeTool(),
                                                   QString::fromUtf8(optionId),
                                                   shown);
                                 refreshPreview();
                                 updateLabel();
                             });
            QObject::connect(
                spin,
                static_cast<void (QSpinBox::*)(int)>(
                    &QSpinBox::valueChanged),
                panel, [this, slider, optionId, logScale](int v) {
                    slider->blockSignals(true);
                    slider->setValue(logScale ? brushSizeToSlider(v) : v);
                    slider->blockSignals(false);
                    state_->setOption(state_->activeTool(),
                                      QString::fromUtf8(optionId), v);
                    refreshPreview();
                    updateLabel();
                });
            layout->addWidget(label);
            layout->addWidget(slider, 1);
            layout->addWidget(spin);
            section->addWidget(row);
            sliders_.push_back({slider, spin, label, name,
                                QByteArray(optionId), logScale});
        };
        // Muted subsection labels inside Dynamics (Response / Variation /
        // Engine): grouping only, never collapsible.
        auto addSubhead = [&](const QString& title) {
            auto* sub = new QLabel(title, panel);
            sub->setIndent(0);
            sub->setMargin(0);
            sub->setStyleSheet(
                QStringLiteral("color: %1; padding: 10px 0px 2px 0px;")
                    .arg(dimText));
            section->addWidget(sub);
        };

        beginSection(QObject::tr("Brush"), true, formA);
        addSlider(QObject::tr("Size"), 1, 5000, 64, "brush_size", true);        // 50 matches the soft-round default.
        addSlider(QObject::tr("Opacity"), 1, 100, 100, "opacity");
        addSlider(QObject::tr("Hardness"), 0, 100, 50, "brush_hardness");
        beginSection(QObject::tr("Tip"), true, formA);
        addSlider(QObject::tr("Angle"), -180, 180, 0, "brush_angle");
        addSlider(QObject::tr("Roundness"), 1, 100, 100, "brush_roundness");
        addSlider(QObject::tr("Spacing"), 1, 200, 15, "brush_spacing");
        addSlider(QObject::tr("Scatter"), 0, 500, 0, "brush_scatter");
        addSlider(QObject::tr("Density"), 1, 100, 100, "brush_density");
        addSlider(QObject::tr("Spikes"), 0, 12, 0, "brush_spikes");
        addSlider(QObject::tr("Fade split"), -100, 100, 0,
                  "brush_fade_aniso");
        addSlider(QObject::tr("Sharpness"), 0, 100, 0, "brush_sharpness");
        addSlider(QObject::tr("Soften edge"), 0, 100, 0, "brush_soften");
        addSlider(QObject::tr("Tip neutral"), 0, 100, 50,
                  "brush_tip_neutral");
        addSlider(QObject::tr("Tip brightness"), -100, 100, 0,
                  "brush_tip_brightness");
        addSlider(QObject::tr("Tip contrast"), 0, 400, 100,
                  "brush_tip_contrast");
        beginSection(QObject::tr("Dynamics"), true, formB);
        addSlider(QObject::tr("Tilt"), 0, 100, 100, "brush_tilt_master");
        addSlider(QObject::tr("Tilt size"), 0, 100, 0, "brush_tilt_size");
        addSlider(QObject::tr("Tilt opacity"), 0, 100, 0,
                  "brush_tilt_opacity");
        addSlider(QObject::tr("Tilt X size"), 0, 100, 0,
                  "brush_tiltx_size");
        addSlider(QObject::tr("Tilt Y size"), 0, 100, 0,
                  "brush_tilty_size");
        addSubhead(QObject::tr("Response"));
        addSlider(QObject::tr("Fade"), 0, 2000, 0, "brush_fade");
        addSlider(QObject::tr("Darken"), 0, 100, 0, "brush_darken");
        addSlider(QObject::tr("Speed size"), 0, 100, 0,
                  "brush_speed_size");
        addSlider(QObject::tr("Time fade"), 0, 60, 0, "brush_timefade");
        addSlider(QObject::tr("Perspective"), 0, 100, 0,
                  "brush_perspective");
        addSlider(QObject::tr("Gradient length"), 0, 2000, 500,
                  "brush_gradient_len");
        addSlider(QObject::tr("Smudge color"), 0, 100, 0,
                  "smudge_color_rate");
        addSlider(QObject::tr("Smear length"), 0, 200, 100,
                  "smudge_length");
        addSubhead(QObject::tr("Variation"));
        addSlider(QObject::tr("Hue jitter"), 0, 180, 0,
                  "brush_hue_jitter");
        addSlider(QObject::tr("Sat jitter"), 0, 100, 0,
                  "brush_sat_jitter");
        addSlider(QObject::tr("Val jitter"), 0, 100, 0,
                  "brush_val_jitter");
        addSlider(QObject::tr("Fuzzy size"), 0, 100, 0,
                  "brush_fuzzy_size");
        addSlider(QObject::tr("Fuzzy opacity"), 0, 100, 0,
                  "brush_fuzzy_opacity");
        addSubhead(QObject::tr("Engine"));

        // Combo rows share one look: fixed label, full-width field.
        // Only the height is styled so the platform draws its native
        // frame + drop arrow (a full custom skin drops the arrow). Built
        // by comboCssFor() so applyPopupScale can shrink it with the rows
        // (a fixed 24px box outgrows a shrunken row and overlaps below).
        const QString comboCss = comboCssFor(22);
        popupComboMinH_ = 22;
        auto* tipRow = new QWidget(panel);
        tipRow->setFixedHeight(28);
        popupRows_.push_back({tipRow, 28});
        auto* tipLayout = new QHBoxLayout(tipRow);
        tipLayout->setContentsMargins(0, 0, 0, 0);
        tipLayout->setSpacing(10);
        auto* tipLabel = new QLabel(QObject::tr("Tip"), tipRow);
        tipLabel->setFixedWidth(80);
        popupLabels_.push_back(tipLabel);
        tipLabel->setStyleSheet(QStringLiteral("color: %1;").arg(dimText));
        tipLayout->addWidget(tipLabel);
        tipCombo_ = new QComboBox(tipRow);
        tipCombo_->addItem(QObject::tr("Round"), 0);
        tipCombo_->addItem(QObject::tr("Square"), 1);
        tipCombo_->setStyleSheet(comboCss);
        tipLayout->addWidget(tipCombo_, 1);
        section->addWidget(tipRow);
        QObject::connect(
            tipCombo_,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            panel, [this](int row) {
                state_->setOption(state_->activeTool(),
                                  QStringLiteral("brush_tip"),
                                  tipCombo_->itemData(row).toInt());
                refreshPreview();
            });

        auto addCombo = [&](const QString& name, const QStringList& items,
                              const QVariantList& data, const char* optionId,
                              const QVariant& fallback) {
            auto* row = new QWidget(panel);
            row->setFixedHeight(28);
            popupRows_.push_back({row, 28});
            auto* layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(10);
            auto* label = new QLabel(name, row);
            label->setFixedWidth(80);
            popupLabels_.push_back(label);
            label->setStyleSheet(QStringLiteral("color: %1;").arg(dimText));
            layout->addWidget(label);
            auto* combo = new QComboBox(row);
            for (int i = 0; i < items.size(); ++i)
                combo->addItem(items[i],
                               i < data.size() ? data[i] : QVariant(i));
            combo->setStyleSheet(comboCss);
            layout->addWidget(combo, 1);
            section->addWidget(row);
            QObject::connect(
                combo,
                static_cast<void (QComboBox::*)(int)>(
                    &QComboBox::currentIndexChanged),
                panel, [this, combo, optionId](int r) {
                    state_->setOption(state_->activeTool(),
                                      QString::fromUtf8(optionId),
                                      combo->itemData(r));
                    refreshPreview();
                });
            combos_.push_back({combo, optionId, fallback});
        };

        // Tip rotation source: off, stylus lean, stroke direction, pressure,
        // barrel rotation, or per-dab random. Replaces the old tilt toggle.
        addCombo(QObject::tr("Rotation"),
                 {QObject::tr("Off"), QObject::tr("Tilt"),
                  QObject::tr("Drawing angle"), QObject::tr("Pressure"),
                  QObject::tr("Stylus rotation"), QObject::tr("Fuzzy")},
                 {}, "brush_rotation", 0);
        // Dab color source: plain foreground, random hue per dab, or a
        // pressure mix toward the background.
        addCombo(QObject::tr("Color"),
                 {QObject::tr("Foreground"), QObject::tr("Random hue"),
                  QObject::tr("FG/BG pressure mix"),
                  QObject::tr("Stroke gradient")},
                 {}, "brush_source", 0);
        // Stroke opacity model: wash caps the whole stroke, buildup works
        // per dab.
        addCombo(QObject::tr("Painting"),
                 {QObject::tr("Wash"), QObject::tr("Build-up")},
                 {QStringLiteral("wash"), QStringLiteral("buildup")},
                 "brush_painting_mode", QStringLiteral("wash"));
        // Stabilizer feel: classic ease, speed-weighted lag, or whole-
        // pixel snap for crisp pixel-art lines.
        addCombo(QObject::tr("Stabilizer"),
                 {QObject::tr("Classic"), QObject::tr("Weighted"),
                  QObject::tr("Pixel snap")},
                 {}, "smoothing_mode", 0);
        // Smudge stroke shape: shape-preserving dulling or trailing smear.
        addCombo(QObject::tr("Smudge"),
                 {QObject::tr("Dulling"), QObject::tr("Smear")},
                 {}, "smudge_mode", 0);
        // Tip filter tier: smooth bilinear stamps or crisp draft
        // nearest-neighbour (pixel-art tips, cheaper per texel).
        addCombo(QObject::tr("Tip filter"),
                 {QObject::tr("Draft"), QObject::tr("Smooth")},
                 {}, "brush_tip_filter", 1);
        // Tip edge curve: linear ramp or gaussian bell (normalized).
        addCombo(QObject::tr("Tip fade"),
                 {QObject::tr("Linear"), QObject::tr("Gaussian")},
                 {}, "brush_falloff", 0);
        // Stamp color mode: foreground tint, raw color, lightness-mapped
        // foreground, or tip-lightness gradient (background to foreground).
        addCombo(QObject::tr("Stamp"),
                 {QObject::tr("Mask"), QObject::tr("Color"),
                  QObject::tr("Lightness"), QObject::tr("Gradient")},
                 {}, "brush_stamp_mode", 0);
        // Paper-grain combine: multiply (soft), subtract (harsh), darken
        // (minimum), plus overlay/dodge/burn/height (original shapes).
        addCombo(QObject::tr("Grain"),
                 {QObject::tr("Multiply"), QObject::tr("Subtract"),
                  QObject::tr("Darken"), QObject::tr("Overlay"),
                  QObject::tr("Dodge"), QObject::tr("Burn"),
                  QObject::tr("Height")},
                 {}, "brush_texture_mode", 0);
        previewLabel_ = new QLabel(panel);
        previewLabel_->setFixedHeight(72);
        previewLabel_->setAlignment(Qt::AlignCenter);
        previewLabel_->setAutoFillBackground(true);
        {
            QPalette pp = previewLabel_->palette();
            pp.setBrush(QPalette::Window, checkerBrush(panel->palette()));
            previewLabel_->setPalette(pp);
        }
        formA->addWidget(previewLabel_);
        beginSection(QObject::tr("Setup"), true, formA);

        // Pill-switch list, one toggle per row: small toggles driving tool
        // options directly (imports and presets may also set them). Text
        // left, pill right. Single column: a two-up grid clips the long
        // labels ("Isotropic spacing") at compact pane widths.
        const QString switchCss = QStringLiteral(
            "QCheckBox { spacing: 4px; }"
            "QCheckBox::indicator { width: 24px; height: 14px; "
            "border-radius: 7px; background: %1; }"
            "QCheckBox::indicator:checked { background: %2; }")
            .arg(boxBorder, accent);
        auto* setupGrid = new QGridLayout();
        setupGrid->setContentsMargins(0, 0, 0, 0);
        setupGrid->setHorizontalSpacing(8);
        setupGrid->setVerticalSpacing(6);
        setupGrid->setColumnStretch(0, 1);
        section->addLayout(setupGrid);
        auto addSwitch = [&](const QString& name, const char* optionId,
                             int row) {
            auto* check = new QCheckBox(name, panel);
            check->setLayoutDirection(Qt::RightToLeft);
            check->setStyleSheet(switchCss);
            setupGrid->addWidget(check, row, 0,
                                 Qt::AlignLeft | Qt::AlignVCenter);
            QObject::connect(check, &QCheckBox::toggled, panel,
                             [this, optionId](bool on) {
                                 state_->setOption(state_->activeTool(),
                                                   QString::fromUtf8(optionId),
                                                   on);
                                 refreshPreview();
                             });
            checks_.push_back({check, optionId});
        };
        addSwitch(QObject::tr("Mirror X"), "brush_flip_x", 0);
        addSwitch(QObject::tr("Mirror Y"), "brush_flip_y", 1);
        addSwitch(QObject::tr("Isotropic spacing"), "brush_spacing_isotropic",
                  2);
        addSwitch(QObject::tr("Tangential flow"), "brush_tangential_flow",
                  3);
        addSwitch(QObject::tr("Pressure hold"), "brush_pressure_in", 4);
        addSwitch(QObject::tr("Texture pressure"), "brush_texture_pressure",
                  5);
        addSwitch(QObject::tr("Mask pressure"), "brush_mask_pressure", 6);
        addSwitch(QObject::tr("Smudge pressure"), "brush_smudge_pressure",
                  7);
        addSwitch(QObject::tr("Lock size"), "lock_size", 8);
        addSwitch(QObject::tr("Lock opacity"), "lock_opacity", 9);
        addSwitch(QObject::tr("Lock texture"), "lock_texture", 10);
        addSwitch(QObject::tr("Eraser"), "brush_erase_blend", 11);
        addSwitch(QObject::tr("Soft grain"), "brush_texture_soft", 12);
        addSwitch(QObject::tr("Grain auto-invert eraser"),
                   "brush_texture_auto_invert_eraser", 13);
        addSwitch(QObject::tr("Auto spacing"), "brush_spacing_auto", 14);
        // Pack both settings columns to the top: without a bottom spacer
        // the taller column stretches its neighbour and the rows spread
        // apart to fill the height. Spare space pools invisibly below.
        formA->addStretch(1);
        formB->addStretch(1);

        // Right column: the brush gallery. Every preset renders its dab
        // with the real kernel, so what you see is what the stroke puts
        // down; clicking one applies its full state on the left.
        auto* rightPane = new QWidget(panel);
        auto* rightForm = new QVBoxLayout(rightPane);
        rightForm->setContentsMargins(0, 0, 0, 0);
        rightForm->setSpacing(6);
        auto* galleryHead = new QHBoxLayout();
        galleryHead->setContentsMargins(0, 0, 0, 0);
        auto* galleryTitle = new QLabel(QObject::tr("Brushes"), rightPane);
        QFont galleryFont = galleryTitle->font();
        galleryFont.setBold(true);
        galleryTitle->setFont(galleryFont);
        galleryHead->addWidget(galleryTitle);
        galleryHead->addStretch(1);
        galleryCount_ = new QLabel(rightPane);
        galleryCount_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        galleryHead->addWidget(galleryCount_);
        rightForm->addLayout(galleryHead);
        gallery_ = new QListWidget(rightPane);
        gallery_->setViewMode(QListWidget::IconMode);
        gallery_->setIconSize(QSize(60, 60));
        gallery_->setMovement(QListWidget::Static);
        gallery_->setResizeMode(QListWidget::Adjust);
        // Fixed grid cells keep rows even; names elide only when a
        // custom preset truly overflows its cell. Transparent thumbs over
        // a coded checkerboard read as transparency, like the mock.
        gallery_->setUniformItemSizes(true);
        gallery_->setGridSize(QSize(96, 90));
        gallery_->setWordWrap(false);
        gallery_->setTextElideMode(Qt::ElideRight);
        gallery_->setSpacing(6);
        gallery_->setFixedWidth(300);
        gallery_->setStyleSheet(QStringLiteral(
            "QListWidget { border: none; }"
            "QListWidget::item { border: 2px solid transparent; "
            "border-radius: 6px; }"
            "QListWidget::item:selected { border: 2px solid %1; "
            "background: transparent; }")
            .arg(accent));
        {
            QPalette gp = gallery_->palette();
            gp.setBrush(QPalette::Base, checkerBrush(panel->palette()));
            gallery_->setPalette(gp);
            gallery_->viewport()->setPalette(gp);
        }
        rightForm->addWidget(gallery_, 1);
        panes->addWidget(rightPane);
        QObject::connect(
            gallery_, &QListWidget::itemClicked, panel,
            [this](QListWidgetItem* item) {
                if (!item) return;
                const int idx = item->data(Qt::UserRole).toInt();
                if (idx < 0 || idx >= (int)presetList_.size()) return;
                brushlibrary::applyPreset(state_, presetList_.at(idx));
                appliedPreset_ = presetList_.at(idx).name;
                refreshFromTool();
            });

        // Footer: reset the tool to its schema defaults, or snapshot the
        // live options as a named custom preset.
        auto* footer = new QHBoxLayout();
        footer->setContentsMargins(0, 0, 0, 0);
        footer->addStretch(1);
        auto* resetButton = new QPushButton(QObject::tr("Reset to defaults"),
                                            panel);
        resetButton->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2; "
            "border-radius: 5px; padding: 6px 16px; }"
            "QPushButton:hover { border-color: %3; }")
            .arg(boxBg, boxBorder, accent));
        footer->addWidget(resetButton);
        auto* drivesButton = new QPushButton(QObject::tr("Drives…"), panel);
        drivesButton->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 1px solid %2; "
            "border-radius: 5px; padding: 6px 16px; }"
            "QPushButton:hover { border-color: %3; }")
            .arg(boxBg, boxBorder, accent));
        footer->addWidget(drivesButton);
        QObject::connect(drivesButton, &QPushButton::clicked, panel,
                         [this, panel] {
            auto* dlg = new sensordrive::SensorDrivesDialog(
                state_, state_->activeTool(), panel);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            dlg->onChanged_ = [this] { refreshPreview(); };
            connect(dlg, &QDialog::finished, panel,
                    [this] { refreshFromTool(); });
            dlg->open();
        });
        QObject::connect(resetButton, &QPushButton::clicked, panel, [this] {
            brushlibrary::resetBrushToolToDefaults(state_,
                                                   state_->activeTool());
            refreshFromTool();
        });
        auto* saveButton = new QPushButton(QObject::tr("Save as preset"),
                                           panel);
        saveButton->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; color: white; border: none; "
            "border-radius: 6px; padding: 5px 14px; font-weight: bold; }")
            .arg(accent));
        footer->addWidget(saveButton);
        QObject::connect(saveButton, &QPushButton::clicked, panel,
                         [this] { saveBrushPresetFromTool(); });
        outer->addLayout(footer);

        auto* action = new QWidgetAction(menu);
        action->setDefaultWidget(panel);
        menu->addAction(action);
        setMenu(menu);
        QObject::connect(menu, &QMenu::aboutToShow, panel, [this] {
            fitBrushPopup();
            refreshFromTool();
        });
        QObject::connect(state_, &AppState::toolChanged, panel,
                         [this] {
                             appliedPreset_.clear();
                             refreshFromTool();
                         });
        // Brackets, Alt+Right-drag and the HUD write the same options behind
        // the popup's back: mirror them in the button readout immediately.
        QObject::connect(
            state_, &AppState::optionChanged, this,
            [this](ToolId tool, const QString& id, const QVariant&) {
                if (tool != state_->activeTool()) return;
                if (id != QLatin1String("brush_size") &&
                    id != QLatin1String("brush_hardness"))
                    return;
                updateLabel();
            });
        refreshFromTool();
    }

    // Fit-to-screen: measure the reference-size panel against the showing
    // screen and shrink fixed widths/heights/grid cells uniformly so the
    // whole popup fits without clipping. Big screens compute exactly 1.0
    // (mock 1:1, no scrolling); small screens floor at kMinPopupScale.
    // Text sizes never change. No screen (headless test) = no scaling.
    static QString comboCssFor(int minH) {
        return QStringLiteral("QComboBox { min-height: %1px; }").arg(minH);
    }
    void fitBrushPopup() {
        if (!brushMenu_ || !brushPanel_ || !brushPanel_->layout()) return;
        QScreen* screen = nullptr;
        if (QWidget* win = window())
            if (QWindow* h = win->windowHandle()) screen = h->screen();
        if (!screen) screen = brushMenu_->screen();
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        // Measure at reference size first so the scale is always derived
        // from the mock, never from a previously scaled layout.
        const double saved = popupScale_;
        if (saved != 1.0) applyPopupScale(1.0);
        brushPanel_->layout()->activate();
        const QSize need =
            brushPanel_->layout()->totalSizeHint() + menuChromeSize();
        const QRect avail = screen->availableGeometry();
        const double s = brushPopupScaleFor(double(need.width()),
                                           double(need.height()),
                                           double(avail.width()),
                                           double(avail.height()));
        applyPopupScale(s);
    }

    QSize menuChromeSize() const {
        if (!brushMenu_ || !brushPanel_) return QSize(0, 0);
        return (brushMenu_->sizeHint() - brushPanel_->sizeHint())
            .expandedTo(QSize(0, 0));
    }

    void applyPopupScale(double s) {
        s = std::clamp(s, kMinPopupScale, 1.0);
        popupScale_ = s;
        if (paneA_) paneA_->setFixedWidth(std::max(100, int(250 * s)));
        if (paneB_) paneB_->setFixedWidth(std::max(100, int(260 * s)));
        for (auto [row, base] : popupRows_)
            if (row) row->setFixedHeight(std::max(20, int(base * s)));
        for (QLabel* l : popupLabels_)
            if (l) l->setFixedWidth(std::max(40, int(80 * s)));
        for (QSpinBox* sp : popupSpins_)
            if (sp) sp->setFixedWidth(std::max(32, int(48 * s)));
        // Combo boxes shrink with their rows (native frame included).
        popupComboMinH_ = std::max(16, int(22 * s));
        const QString css = comboCssFor(popupComboMinH_);
        if (tipCombo_) tipCombo_->setStyleSheet(css);
        for (const BrushComboRow& row : combos_)
            if (row.combo) row.combo->setStyleSheet(css);
        if (previewLabel_)
            previewLabel_->setFixedHeight(std::max(40, int(72 * s)));
        if (gallery_) {
            gallery_->setFixedWidth(std::max(140, int(300 * s)));
            gallery_->setGridSize(
                QSize(std::max(56, int(96 * s)), std::max(52, int(90 * s))));
        }
        if (brushPanel_ && brushPanel_->layout())
            brushPanel_->layout()->activate();
    }

    void refreshFromTool() {
        // Sliders initialise from the active tool's stored options (they used
        // to reset to hardcoded defaults on every construction).
        const ToolId tool = state_->activeTool();
        refreshGalleryList();
        auto stored = [&](const char* id, int fallback) {
            const QVariant v = state_->option(tool, QString::fromUtf8(id));
            return v.isValid() ? v.toInt() : fallback;
        };
        for (const BrushSliderRow& row : sliders_) {
            if (!row.slider || !row.label) continue;
            int fallback = 0;
            const QString key = QString::fromUtf8(row.optionId);
            for (const auto& kv : brushlibrary::brushPopupDefaults()) {
                if (QString::fromUtf8(kv.first) == key &&
                    kv.second.canConvert<int>()) {
                    fallback = kv.second.toInt();
                    break;
                }
            }
            const int value = stored(row.optionId.constData(), fallback);
            row.slider->blockSignals(true);
            row.slider->setValue(row.logScale ? brushSizeToSlider(value) : value);
            row.slider->blockSignals(false);
            if (row.spin) {
                row.spin->blockSignals(true);
                row.spin->setValue(row.logScale
                                       ? qBound(1, value, 5000)
                                       : qBound(row.slider->minimum(), value,
                                               row.slider->maximum()));
                row.spin->blockSignals(false);
            }
        }
        if (tipCombo_) {
            tipCombo_->blockSignals(true);
            tipCombo_->setCurrentIndex(stored("brush_tip", 0) == 1 ? 1 : 0);
            tipCombo_->blockSignals(false);
        }
        for (const BrushComboRow& row : combos_) {
            if (!row.combo) continue;
            const QVariant v =
                state_->option(tool, QString::fromUtf8(row.optionId));
            const QVariant want = v.isValid() ? v : row.fallback;
            int at = row.combo->findData(want);
            if (at < 0) at = 0;
            row.combo->blockSignals(true);
            row.combo->setCurrentIndex(at);
            row.combo->blockSignals(false);
        }
        for (const BrushCheckRow& row : checks_) {
            if (!row.box) continue;
            const QVariant v =
                state_->option(tool, QString::fromUtf8(row.optionId));
            row.box->blockSignals(true);
            row.box->setChecked(v.isValid() && v.toBool());
            row.box->blockSignals(false);
        }
        refreshPreview();
        updateLabel();
        updateTitle();
    }

    // Title preset name: the applied preset, else the active library name,
    // else Custom.
    void updateTitle() {
        if (!titlePreset_) return;
        QString name = appliedPreset_;
        if (name.isEmpty()) name = state_->activeBrushPresetName();
        if (name.isEmpty()) name = tr("Custom");
        titlePreset_->setText(name);
    }

    // Reset every value option of the active tool to its schema default.
    // Structural rows (labels, separators, buttons, the preset well) carry
    // no value and are skipped.
    void resetBrushToolToDefaults() {
        const ToolId tool = state_->activeTool();
        for (const OptionSpec& spec : optionsFor(tool)) {
            switch (spec.kind) {
                case OptionKind::Combo:
                case OptionKind::Spin:
                case OptionKind::Slider:
                case OptionKind::Check:
                case OptionKind::ToggleGroup:
                case OptionKind::ColorWell:
                case OptionKind::Text:
                    break;
                default:
                    continue;
            }
            if (!spec.defaultValue.isValid()) continue;
            state_->setOption(tool, QString::fromUtf8(spec.id),
                              spec.defaultValue);
        }
    }

    // Snapshot the live tool options as a named custom preset.
    void saveBrushPresetFromTool() {
        const ToolId tool = state_->activeTool();
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Save brush preset"), tr("Name:"),
            QLineEdit::Normal,
            appliedPreset_.isEmpty() ? state_->activeBrushPresetName()
                                     : appliedPreset_,
            &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        brushlibrary::BrushPreset p =
            brushlibrary::captureBrushState(state_, tool);
        p.name = name.trimmed();
        p.factory = false;
        auto customs = brushlibrary::loadCustomPresets();
        customs.erase(std::remove_if(customs.begin(), customs.end(),
                                     [&](const brushlibrary::BrushPreset& q) {
                                         return !q.factory && q.name == p.name;
                                     }),
                      customs.end());
        customs.push_back(p);
        brushlibrary::saveCustomPresets(customs);
        appliedPreset_ = p.name;
        refreshFromTool();
    }

    // Rebuilds the gallery from the library (factory + customs, so
    // imports appear without reopening anything) and reselects the last
    // applied preset when it is still there.
    void refreshGalleryList() {
        if (!gallery_) return;
        presetList_ = brushlibrary::factoryBrushPresets();
        for (auto& p : brushlibrary::loadCustomPresets())
            presetList_.push_back(std::move(p));
        gallery_->blockSignals(true);
        gallery_->clear();
        auto thumbFor = [&](const brushlibrary::BrushPreset& p) {
            namespace bp = pittore::ui::brushpreview;
            const pittore::compute::StampTip* stamp =
                p.isStamp() ? state_->brushStamp(p.stampId) : nullptr;
            if (stamp)
                return bp::tipThumbStamp(
                    *stamp, p.stampMode, 56, true, state_->background(),
                    p.tipNeutral, p.tipBrightness, p.tipContrast);
            bp::AutoParams a;
            a.ratio = std::clamp(p.roundness / 100.0, 0.01, 1.0);
            a.angleDeg = p.angle;
            a.square = p.tip == 1;
            a.hardness = std::clamp(p.hardness / 100.0, 0.0, 1.0);
            a.spacingPct = std::clamp(p.spacing, 1.0, 200.0);
            a.spikes = std::clamp(p.spikes, 0, 12);
            a.fadeAniso = std::clamp(p.fadeAniso, -100.0, 100.0);
            a.falloff = std::clamp(p.falloff, 0, 1);
            a.sharpness = std::clamp(p.sharpness, 0.0, 100.0);
            a.soften = std::clamp(p.soften, 0.0, 100.0);
            a.spacingAuto = p.spacingAuto;
            return bp::tipThumbAuto(a, 56, true);
        };
        QListWidgetItem* select = nullptr;
        for (std::size_t i = 0; i < presetList_.size(); ++i) {
            const auto& p = presetList_[i];
            auto* item = new QListWidgetItem(
                QIcon(pixmapForWidget(thumbFor(p), gallery_)), p.name,
                gallery_);
            item->setData(Qt::UserRole, int(i));
            item->setToolTip(p.name);
            item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
            // Full-cell hint: with uniform sizes the view would otherwise
            // lock every label to the icon-width text box and elide names
            // that fit the cell easily ("Soft Ro..."). Scaled with the
            // popup on small screens (matches the grid in applyPopupScale).
            item->setSizeHint(QSize(std::max(56, int(96 * popupScale_)),
                                    std::max(52, int(90 * popupScale_))));
            if (!appliedPreset_.isEmpty() && p.name == appliedPreset_)
                select = item;
        }
        if (galleryCount_)
            galleryCount_->setText(QString::number(presetList_.size()));
        gallery_->blockSignals(false);
        if (select) gallery_->setCurrentItem(select);
    }

    // Scratch-stroke preview for the popup, painted with the real kernels
    // from the active tool's live options (stamp when one is assigned).
    void refreshPreview() {
        namespace bp = pittore::ui::brushpreview;
        if (!previewLabel_) return;
        const ToolId tool = state_->activeTool();
        auto dbl = [&](const char* id, double fallback) {
            const QVariant v = state_->option(tool, QString::fromUtf8(id));
            return v.isValid() ? v.toDouble() : fallback;
        };
        const QString stampId =
            state_->option(tool, QStringLiteral("brush_stamp")).toString();
        const pittore::compute::StampTip* stamp =
            stampId.isEmpty() ? nullptr : state_->brushStamp(stampId);
        QImage img;
        bp::AutoParams pr;
        pr.ratio = std::clamp(dbl("brush_roundness", 100.0) / 100.0, 0.01, 1.0);
        pr.angleDeg = std::clamp(dbl("brush_angle", 0.0), -180.0, 180.0);
        pr.square = dbl("brush_tip", 0.0) == 1.0;
        pr.hardness =
            std::clamp(dbl("brush_hardness", 50.0) / 100.0, 0.0, 1.0);
        pr.spacingPct = std::clamp(dbl("brush_spacing", 15.0), 1.0, 200.0);
        {
            const QVariant psv =
                state_->option(tool, QStringLiteral("pressure_size"));
            pr.pressureSize = !psv.isValid() || psv.toBool();
            const QVariant pov =
                state_->option(tool, QStringLiteral("pressure_opacity"));
            pr.pressureOpacity = !pov.isValid() || pov.toBool();
        }
        pr.opacity = std::clamp(dbl("opacity", 100.0) / 100.0, 0.0, 1.0);
        pr.flowPct = std::clamp(dbl("flow", 100.0) / 100.0, 0.0, 1.0);
        pr.sizeCurve =
            state_->option(tool, QStringLiteral("brush_size_curve")).toString();
        pr.opacityCurve =
            state_->option(tool, QStringLiteral("brush_opacity_curve"))
                .toString();
        pr.flowCurve =
            state_->option(tool, QStringLiteral("brush_flow_curve")).toString();
        pr.spikes = std::clamp(
            state_->option(tool, QStringLiteral("brush_spikes")).toInt(), 0,
            12);
        pr.fadeAniso = std::clamp(
            state_->option(tool, QStringLiteral("brush_fade_aniso"))
                .toDouble(),
            -100.0, 100.0);
        pr.falloff = std::clamp(
            state_->option(tool, QStringLiteral("brush_falloff")).toInt(), 0,
            1);
        pr.sharpness = std::clamp(
            state_->option(tool, QStringLiteral("brush_sharpness")).toDouble(),
            0.0, 100.0);
        pr.soften = std::clamp(
            state_->option(tool, QStringLiteral("brush_soften")).toDouble(),
            0.0, 100.0);
        pr.spacingAuto =
            state_->option(tool, QStringLiteral("brush_spacing_auto"))
                .toBool();
        if (stamp) {
            const int mode = qBound(
                0,
                state_->option(tool, QStringLiteral("brush_stamp_mode"))
                    .toInt(),
                3);
            img = bp::strokePreviewStamp(
                *stamp, mode, state_->foreground(), pr.angleDeg, 220, 72,
                nullptr, pr, false, state_->background(),
                dbl("brush_tip_neutral", 50.0),
                dbl("brush_tip_brightness", 0.0),
                dbl("brush_tip_contrast", 100.0));
        } else {
            img = bp::strokePreviewAuto(pr, state_->foreground(), 220, 72,
                                        nullptr, false);
        }
        previewLabel_->setPixmap(pixmapForWidget(img, previewLabel_));
    }

    void updateLabel() {        const ToolId tool = state_->activeTool();
        const int size = state_->option(tool, QStringLiteral("brush_size"))
                             .toInt();
        const QVariant hardOpt =
            state_->option(tool, QStringLiteral("brush_hardness"));
        const int hard = hardOpt.isValid()
                             ? hardOpt.toInt()
                             : (tool == ToolId::Pencil ? 100 : 50);
        // conventional readout: size first, hardness after — readable at a
        // glance without opening the popup.
        setText(QStringLiteral(" %1 · %2% ")
                    .arg(size > 0 ? size : 64)
                    .arg(hard));
    }

  private:
    AppState* state_;
    std::vector<BrushSliderRow> sliders_;
    QComboBox* tipCombo_ = nullptr;
    QLabel* previewLabel_ = nullptr;
    QListWidget* gallery_ = nullptr;
    QLabel* galleryCount_ = nullptr;
    // Fit-to-screen handles (reference metrics in the constructor calls).
    QMenu* brushMenu_ = nullptr;
    QWidget* brushPanel_ = nullptr;
    QWidget* paneA_ = nullptr;
    QWidget* paneB_ = nullptr;
    std::vector<std::pair<QWidget*, int>> popupRows_;  // row + base height
    std::vector<QLabel*> popupLabels_;                 // base width 80
    std::vector<QSpinBox*> popupSpins_;                // base width 48
    int popupComboMinH_ = 22;  // combo box min-height at scale 1.0
    double popupScale_ = 1.0;
    QLabel* titlePreset_ = nullptr;
    std::vector<BrushComboRow> combos_;
    struct BrushCheckRow {
        QCheckBox* box = nullptr;
        QByteArray optionId;
    };
    std::vector<BrushCheckRow> checks_;
    std::vector<brushlibrary::BrushPreset> presetList_;
    QString appliedPreset_;
};

}  // namespace

namespace {

// Dab thumbnail for the badge from the tool's live options: hose first
// cell, stamp, else the auto tip (smudge shapers included).
QImage brushBadgeThumb(AppState* state, ToolId tool) {
    namespace bp = pittore::ui::brushpreview;
    const QString hoseId =
        state->option(tool, QStringLiteral("brush_hose")).toString();
    const auto* hose =
        hoseId.isEmpty() ? nullptr : state->brushHose(hoseId);
    if (hose && !hose->cells.empty())
        return bp::tipThumbStamp(hose->cells.front().tip, 0);
    const QString stampId =
        state->option(tool, QStringLiteral("brush_stamp")).toString();
    const auto* stamp =
        stampId.isEmpty() ? nullptr : state->brushStamp(stampId);
    auto dbl = [&](const char* id, double fallback) {
        const QVariant v = state->option(tool, QString::fromUtf8(id));
        return v.isValid() ? v.toDouble() : fallback;
    };
    if (stamp)
        return bp::tipThumbStamp(
            *stamp,
            qBound(0,
                   state->option(tool, QStringLiteral("brush_stamp_mode"))
                       .toInt(),
                   3),
            56, true, state->background(),
            dbl("brush_tip_neutral", 50.0),
            dbl("brush_tip_brightness", 0.0),
            dbl("brush_tip_contrast", 100.0));
    bp::AutoParams a;
    a.ratio = std::clamp(dbl("brush_roundness", 100.0) / 100.0, 0.01, 1.0);
    a.angleDeg = std::clamp(dbl("brush_angle", 0.0), -180.0, 180.0);
    a.square = dbl("brush_tip", 0.0) == 1.0;
    a.hardness = std::clamp(dbl("brush_hardness", 50.0) / 100.0, 0.0, 1.0);
    a.spacingPct = std::clamp(dbl("brush_spacing", 15.0), 1.0, 200.0);
    a.spikes = std::clamp(
        state->option(tool, QStringLiteral("brush_spikes")).toInt(), 0, 12);
    a.fadeAniso = std::clamp(
        state->option(tool, QStringLiteral("brush_fade_aniso")).toDouble(),
        -100.0, 100.0);
    a.falloff = std::clamp(
        state->option(tool, QStringLiteral("brush_falloff")).toInt(), 0, 1);
    a.sharpness = std::clamp(
        state->option(tool, QStringLiteral("brush_sharpness")).toDouble(),
        0.0, 100.0);
    a.soften = std::clamp(
        state->option(tool, QStringLiteral("brush_soften")).toDouble(), 0.0,
        100.0);
    a.spacingAuto =
        state->option(tool, QStringLiteral("brush_spacing_auto")).toBool();
    return bp::tipThumbAuto(a);
}

}  // namespace

OptionsBar::OptionsBar(AppState* state, QWidget* parent) : QFrame(parent), state_(state) {
    setObjectName(QStringLiteral("optionsBar"));
    setFrameShape(QFrame::NoFrame);
    // No fixed height: the row wraps onto extra lines when the window is
    // narrow, so the bar's height follows its width (issue #2). One row
    // still lands at the old fixed 44 px.
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    row_ = new FlowLayout(this, 10);
    row_->setContentsMargins(12, 6, 12, 6);

    connect(state_, &AppState::toolChanged, this, [this] { rebuild(); });
    // No document = no-op strip: options act on layers/pixels, so the whole
    // row dims instead of looking live while clicks do nothing.
    connect(state_, &AppState::activeDocumentChanged, this, [this] { rebuild(); });
    connect(state_, &AppState::documentsChanged, this, [this] { rebuild(); });
    // Mirrored layer edits update the widget in place, no rebuild (keeps focus).
    // The badge wears the live brush, so brush-affecting edits refresh it.
    connect(state_, &AppState::optionChanged, this,
            [this](ToolId tool, const QString& id, const QVariant& value) {
        if (tool != state_->activeTool()) return;
        syncOption(id, value);
        updateBadge();
    });
    connect(state_, &AppState::themeChanged, this, [this] {
        applyTheme();
        rebuild();
    });

    applyTheme();
    rebuild();
}

QSize OptionsBar::sizeHint() const { return {600, 34}; }

bool OptionsBar::hasHeightForWidth() const { return row_->hasHeightForWidth(); }

int OptionsBar::heightForWidth(int w) const { return row_->heightForWidth(w); }

void OptionsBar::updateBadge() {
    if (!toolBadge_) return;
    const ToolId tool = state_->activeTool();
    const ThemeColors c = colorsFor(state_->theme());
    if (!isPaintTool(tool)) {
        toolBadge_->setIcon(toolIcon(tool, c.text, c.accentText));
        toolBadge_->setIconSize(QSize(20, 20));
        toolBadge_->setText(QString());
        toolBadge_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        toolBadge_->setToolTip(toolName(tool) + tr(" — tool presets"));
        return;
    }
    // Paint tools wear the live brush: dab thumbnail + preset name.
    toolBadge_->setIcon(QIcon(pixmapForWidget(brushBadgeThumb(state_, tool),
                                              toolBadge_)));
    toolBadge_->setIconSize(QSize(22, 22));
    QString name = state_->activeBrushPresetName();
    if (name.isEmpty()) name = tr("Custom");
    toolBadge_->setText(
        toolBadge_->fontMetrics().elidedText(name, Qt::ElideRight, 150));
    toolBadge_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolBadge_->setToolTip(toolName(tool) + QStringLiteral(" — ") + name);
}

void OptionsBar::applyTheme() {
    const ThemeColors c = colorsFor(state_->theme());
    setStyleSheet(QStringLiteral("#optionsBar { background: %1; border-bottom: 1px solid %2; }")
                      .arg(c.chrome.name(), cssColor(c.divider)));
}

// Stable name so live option changes can find the widget again.
QWidget* OptionsBar::buildWidget(const OptionSpec& spec) {
    QWidget* w = buildWidgetRaw(spec);
    if (w) w->setObjectName(QStringLiteral("opt/") + QString::fromUtf8(spec.id));
    return w;
}

void OptionsBar::syncOption(const QString& id, const QVariant& value) {
    QWidget* w = findChild<QWidget*>(QStringLiteral("opt/") + id);
    if (!w) return;
    // Don't echo programmatic writes back into setOption.
    const QSignalBlocker block(w);
    if (auto* box = qobject_cast<QComboBox*>(w)) {
        box->setCurrentIndex(value.toInt());
    } else if (auto* spin = qobject_cast<QDoubleSpinBox*>(w)) {
        spin->setValue(value.toDouble());
    } else if (auto* slider = qobject_cast<QSlider*>(w)) {
        slider->setValue(value.toInt());
    } else if (auto* check = qobject_cast<QCheckBox*>(w)) {
        check->setChecked(value.toBool());
    } else if (auto* edit = qobject_cast<QLineEdit*>(w)) {
        edit->setText(value.toString());
    } else if (w->property("optionKind").toInt() == int(OptionKind::ColorWell)) {
        static_cast<ColorChip*>(w)->setColor(value.value<QColor>());
    } else if (w->property("toggleGroup").toBool()) {
        const QList<QToolButton*> buttons = w->findChildren<QToolButton*>();
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setChecked(i == value.toInt());
    }
}

QWidget* OptionsBar::buildWidgetRaw(const OptionSpec& spec) {
    const ToolId tool = state_->activeTool();
    const QString id = QString::fromUtf8(spec.id);
    const ThemeColors c = colorsFor(state_->theme());

    switch (spec.kind) {
        case OptionKind::Separator: {
            auto* line = new QFrame(this);
            line->setFrameShape(QFrame::NoFrame);
            line->setFixedSize(1, 22);
            line->setStyleSheet(QStringLiteral("background: %1;").arg(cssColor(c.divider)));
            return line;
        }

        case OptionKind::Label:
            return new QLabel(QString::fromUtf8(spec.label), this);

        case OptionKind::Combo: {
            auto* box = new QComboBox(this);
            for (const char* item : spec.items) box->addItem(QString::fromUtf8(item));
            const QVariant stored = state_->option(tool, id);
            box->setCurrentIndex(stored.isValid() ? stored.toInt() : spec.defaultValue.toInt());
            if (spec.width) box->setFixedWidth(spec.width);
            // The Type tools' family list is hundreds of entries: make it
            // searchable (type-to-filter popup + Enter-to-resolve) and render
            // each family in its own face per Settings > Interface.
            if (id == QLatin1String("family")) {
                makeFamilyComboSearchable(box);
                applyFamilyPreview(box, [state = state_] {
                    return state->settings().fontPreviewSize;
                });
            }
            connect(box, &QComboBox::currentIndexChanged, this,
                    [this, tool, id](int index) { state_->setOption(tool, id, index); });
            return box;
        }

        case OptionKind::Spin: {
            auto* spin = new QDoubleSpinBox(this);
            spin->setRange(spec.min, spec.max);
            spin->setSingleStep(spec.step);
            spin->setDecimals(spec.step < 1.0 ? 1 : 0);
            spin->setSuffix(QString::fromUtf8(spec.suffix));
            spin->setKeyboardTracking(false);
            const QVariant stored = state_->option(tool, id);
            spin->setValue(stored.isValid() ? stored.toDouble() : spec.defaultValue.toDouble());
            spin->setFixedWidth(spec.width ? spec.width : 80);
            connect(spin, &QDoubleSpinBox::valueChanged, this,
                    [this, tool, id](double v) { state_->setOption(tool, id, v); });
            return spin;
        }

        case OptionKind::Slider: {
            auto* slider = new QSlider(Qt::Horizontal, this);
            slider->setRange(int(spec.min), int(spec.max));
            slider->setFixedWidth(spec.width ? spec.width : 90);
            const QVariant stored = state_->option(tool, id);
            slider->setValue(stored.isValid() ? stored.toInt() : spec.defaultValue.toInt());
            connect(slider, &QSlider::valueChanged, this,
                    [this, tool, id](int v) { state_->setOption(tool, id, v); });
            return slider;
        }

        case OptionKind::Check: {
            auto* box = new QCheckBox(QString::fromUtf8(spec.label), this);
            const QVariant stored = state_->option(tool, id);
            box->setChecked(stored.isValid() ? stored.toBool() : spec.defaultValue.toBool());
            connect(box, &QCheckBox::toggled, this,
                    [this, tool, id](bool on) { state_->setOption(tool, id, on); });
            return box;
        }

        case OptionKind::ToggleGroup: {
            auto* host = new QWidget(this);
            host->setProperty("toggleGroup", true);
            auto* line = new QHBoxLayout(host);
            line->setContentsMargins(0, 0, 0, 0);
            line->setSpacing(1);
            const QVariant stored = state_->option(tool, id);
            const int current = stored.isValid() ? stored.toInt() : spec.defaultValue.toInt();
            for (std::size_t i = 0; i < spec.items.size(); ++i) {
                auto* button = new QToolButton(host);
                button->setText(QString::fromUtf8(spec.items[i]));
                button->setCheckable(true);
                button->setChecked(int(i) == current);
                button->setAutoExclusive(true);
                button->setStyleSheet(QStringLiteral("padding: 4px 12px;"));
                connect(button, &QToolButton::clicked, this,
                        [this, tool, id, i] { state_->setOption(tool, id, int(i)); });
                line->addWidget(button);
            }
            return host;
        }

        case OptionKind::Button: {
            auto* button = new QPushButton(QString::fromUtf8(spec.label), this);
            button->setStyleSheet(QStringLiteral("padding: 5px 14px;"));
            connect(button, &QPushButton::clicked, this,
                    [this, tool, id] { emit commandTriggered(tool, id); });
            return button;
        }

        case OptionKind::ColorWell: {
            const QVariant stored = state_->option(tool, id);
            QColor initial = stored.isValid() ? stored.value<QColor>()
                                              : (id == QLatin1String("stroke")
                                                     ? QColor(Qt::transparent)
                                                     : state_->foreground());
            auto* chip = new ColorChip(initial, this);
            chip->setProperty("optionKind", int(OptionKind::ColorWell));
            connect(chip, &QToolButton::clicked, this, [this, chip, tool, id] {
                const QColor picked = QColorDialog::getColor(
                    chip->color(), this, QString(), QColorDialog::ShowAlphaChannel);
                if (!picked.isValid()) return;
                chip->setColor(picked);
                state_->setOption(tool, id, picked);
            });
            return chip;
        }

        case OptionKind::BrushPreset:
            return new BrushPresetButton(state_, this);

        case OptionKind::Text: {
            auto* edit = new QLineEdit(this);
            edit->setPlaceholderText(QString::fromUtf8(spec.label));
            const QVariant stored = state_->option(tool, id);
            edit->setText(stored.isValid() ? stored.toString() : spec.defaultValue.toString());
            edit->setFixedWidth(spec.width ? spec.width : 140);
            connect(edit, &QLineEdit::textChanged, this,
                    [this, tool, id](const QString& text) { state_->setOption(tool, id, text); });
            return edit;
        }
    }
    return nullptr;
}

void OptionsBar::rebuild() {
    while (QLayoutItem* item = row_->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();  // hide now; the deferred delete would keep painting
            w->deleteLater();
        }
        delete item;
    }

    const ThemeColors c = colorsFor(state_->theme());
    const ToolId tool = state_->activeTool();
    tool_log(tool, "options-bar/rebuild", "row cleared");

    // Tool icon + presets, where the preset well belongs.
    tool_log(tool, "options-bar/badge", "building icon + preset menu");
    auto* badge = new QToolButton(this);
    badge->setIcon(toolIcon(tool, c.text, c.accentText));
    badge->setIconSize(QSize(20, 20));
    badge->setAutoRaise(true);
    badge->setPopupMode(QToolButton::InstantPopup);
    badge->setToolTip(toolName(tool) + tr(" — tool presets"));
    auto* presets = new QMenu(badge);
    presets->addAction(tr("Reset Tool"));
    presets->addAction(tr("Reset All Tools"));
    presets->addSeparator();
    presets->addAction(tr("New Tool Preset…"));
    badge->setMenu(presets);
    row_->addWidget(badge);
    toolBadge_ = badge;
    updateBadge();
    tool_log(tool, "options-bar/badge", "done");

    auto* divider = new QFrame(this);
    divider->setFrameShape(QFrame::NoFrame);
    divider->setFixedSize(1, 22);
    divider->setStyleSheet(QStringLiteral("background: %1;").arg(cssColor(c.divider)));
    row_->addWidget(divider);
    // One line per OptionSpec *before* it is built: each tool's option list is
    // hand-written, so this names the exact option id if a spec is what died.
    const std::vector<OptionSpec> specs = optionsFor(tool);
    tool_logf(tool, "options-bar/optionsFor", "%d specs",
              static_cast<int>(specs.size()));
    for (const OptionSpec& spec : specs) {
        tool_logf(tool, "options-bar/spec", "id=%s kind=%d", spec.id,
                  static_cast<int>(spec.kind));
        if (spec.kind == OptionKind::Combo || spec.kind == OptionKind::Spin ||
            spec.kind == OptionKind::Slider || spec.kind == OptionKind::ColorWell ||
            spec.kind == OptionKind::BrushPreset) {
            const QString label = QString::fromUtf8(spec.label);
            if (!label.isEmpty()) {
                auto* text = new QLabel(label + QStringLiteral(":"), this);
                text->setStyleSheet(QStringLiteral("color: %1;").arg(c.textDim.name()));
                row_->addWidget(text);
            }
        }
        if (QWidget* w = buildWidget(spec)) row_->addWidget(w);
        else
            tool_logf(tool, "options-bar/spec", "id=%s kind=%d produced NO widget",
                      spec.id, static_cast<int>(spec.kind));
    }
    tool_log(tool, "options-bar/rebuild", "row built");
    // Disabled visual language: without a document every option is dimmed so
    // the strip teaches "nothing to act on" instead of looking live.
    setEnabled(state_->activeDocument() != nullptr);
}

}  // namespace pittore::ui
