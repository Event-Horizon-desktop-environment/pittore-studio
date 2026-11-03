#include "ui/panels.h"

#include <QComboBox>
#include <QGridLayout>
#include <QImage>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "engine/compute/adjust.h"
#include "ui/adjustment_presets.h"
#include "ui/icons.h"
#include "ui/panels/shared/panel_helpers.h"

namespace pittore::ui {
namespace {

struct AdjustmentEntry {
    const char* name;
    const char* icon;
    int kind;          // -1 when the engine has no live implementation yet
    const char* detail;
    bool quick;
};

const AdjustmentEntry kAdjustments[] = {
    {"Levels", "properties",
     static_cast<int>(pittore::compute::AdjustmentKind::Levels),
     "Set black, white and gamma points.", true},
    {"White Balance", "sampler",
     static_cast<int>(pittore::compute::AdjustmentKind::WhiteBalance),
     "Neutralise a colour cast with temperature and tint.", true},
    {"HSL", "swatches",
     static_cast<int>(pittore::compute::AdjustmentKind::HueSaturation),
     "Shift hue, saturation and lightness across the stack.", true},
    {"Recolour", "recolor",
     static_cast<int>(pittore::compute::AdjustmentKind::Recolour),
     "Replace hue and saturation while keeping the pixel lightness.", true},
    {"Black & White", "channels",
     static_cast<int>(pittore::compute::AdjustmentKind::BlackWhite),
     "Mix the colour channels into a monochrome image.", true},
    {"Brightness / Contrast", "adjustments",
     static_cast<int>(
         pittore::compute::AdjustmentKind::BrightnessContrast),
     "Lift or crush tone and contrast.", true},
    {"Posterise", "count",
     static_cast<int>(pittore::compute::AdjustmentKind::Posterize),
     "Reduce the image to a fixed number of tone steps.", true},
    {"Vibrance", "color",
     static_cast<int>(pittore::compute::AdjustmentKind::Vibrance),
     "Boost muted colours without clipping saturated ones.", true},
    {"Exposure", "dodge",
     static_cast<int>(pittore::compute::AdjustmentKind::Exposure),
     "Scale scene exposure in stops.", true},
    {"Threshold", "sharpen",
     static_cast<int>(pittore::compute::AdjustmentKind::Threshold),
     "Split the image into pure black and white at a threshold.", false},
    {"Curves", "paths",
     static_cast<int>(pittore::compute::AdjustmentKind::Curves),
     "Map input tone to output tone with an editable curve.", true},
    {"Channel Mixer", "mixer",
     static_cast<int>(pittore::compute::AdjustmentKind::ChannelMixer),
     "Build each output channel from the input channels.", false},
    {"Gradient Map", "gradient", -1,
     "Map luminance through a colour gradient.", false},
    {"Selective Colour", "wand", -1,
     "Adjust individual colour ranges.", false},
    {"Colour Balance", "histogram",
     static_cast<int>(pittore::compute::AdjustmentKind::ColorBalance),
     "Shift shadows, midtones and highlights along colour axes.", true},
    {"Invert", "eraser-magic",
     static_cast<int>(pittore::compute::AdjustmentKind::Invert),
     "Invert every colour channel.", false},
    {"Soft Proof", "proof", -1,
     "Preview the document through an output profile.", false},
    {"LUT", "layers", -1, "Apply a 3D colour lookup table.", false},
    {"Lens Filter", "gradient", -1,
     "Warm or cool the image like a camera filter.", false},
    {"Split Toning", "swatches", -1,
     "Tint shadows and highlights independently.", false},
    {"OCIO", "settings", -1,
     "Apply an OpenColorIO display transform.", false},
    {"Normals", "channels", -1,
     "Generate or adjust surface normal maps.", false},
    {"Tone Compression", "histogram", -1,
     "Compress highlight and shadow detail.", false},
    {"Tone Stretch", "histogram", -1,
     "Stretch tonal detail for inspection.", false},
};

QImage profilePreview(const QImage& source, int kind, const float* params) {
    QImage base = source;
    if (base.isNull()) {
        base = QImage(144, 96, QImage::Format_RGB32);
        for (int y = 0; y < base.height(); ++y) {
            const int v = 40 + y * 160 / base.height();
            for (int x = 0; x < base.width(); ++x) {
                const int u = 70 + x * 150 / base.width();
                base.setPixel(x, y, qRgb(u, v, 120));
            }
        }
    }
    base = base.scaled(144, 96, Qt::KeepAspectRatioByExpanding,
                       Qt::SmoothTransformation)
               .copy(0, 0, 144, 96)
               .convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < base.height(); ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(base.scanLine(y));
        for (int x = 0; x < base.width(); ++x) {
            float r = qRed(row[x]) / 255.0f;
            float g = qGreen(row[x]) / 255.0f;
            float b = qBlue(row[x]) / 255.0f;
            float or_ = r, og = g, ob = b;
            pittore::compute::adjust::apply(
                static_cast<pittore::compute::AdjustmentKind>(kind), params,
                nullptr, r, g, b, or_, og, ob);
            row[x] = qRgb(qBound(0, qRound(or_ * 255.0f), 255),
                          qBound(0, qRound(og * 255.0f), 255),
                          qBound(0, qRound(ob * 255.0f), 255));
        }
    }
    return base;
}

QString panelStyle() {
    return QStringLiteral(
        "QWidget#AdjustmentsRoot { background:#262626; }"
        "QComboBox#AdjustmentMode { background:#3a3a3a; color:#e8e8e8;"
        "  border:1px solid #4a4a4a; border-radius:4px; padding:4px 8px;"
        "  min-height:22px; font-size:12px; }"
        "QComboBox#AdjustmentMode::drop-down { border:0; width:22px; }"
        "QComboBox#AdjustmentMode QAbstractItemView { background:#2b2b2b;"
        "  color:#e8e8e8; selection-background-color:#3d6fb6;"
        "  border:1px solid #4a4a4a; }"
        "QScrollArea { background:transparent; border:0; }"
        "QScrollBar:vertical { background:transparent; width:10px; }"
        "QScrollBar::handle:vertical { background:#555; border-radius:5px;"
        "  min-height:24px; }"
        "QFrame#AdjustmentRow { background:#3a3a3a; border:1px solid #4a4a4a;"
        "  border-radius:4px; }"
        "QFrame#AdjustmentRow:hover { background:#454545; }"
        "QFrame#AdjustmentRow[unsupported=\"true\"] { background:#303030;"
        "  border-color:#3d3d3d; }"
        "QLabel#AdjustmentTitle { color:#e4e4e4; font-size:12px; }"
        "QLabel#AdjustmentTitle[unsupported=\"true\"] { color:#8a8a8a; }"
        "QLabel#AdjustmentDetail { color:#aaaaaa; font-size:11px; }"
        "QLabel#AdjustmentChevron { color:#bdbdbd; font-size:12px; }"
        "QToolButton#AdjustmentChevron { background:transparent; border:0;"
        "  color:#bdbdbd; font-size:12px; padding:0; }"
        "QToolButton#ProfileTile { background:#333333; border:1px solid #454545;"
        "  border-radius:3px; color:#d8d8d8; font-size:10px; padding:2px; }"
        "QToolButton#ProfileTile:hover { background:#404040;"
        "  border-color:#5a5a5a; }");
}

class AdjustmentRow final : public QFrame {
  public:
    AdjustmentRow(const AdjustmentEntry& entry, AppState* state,
                  QWidget* parent)
        : QFrame(parent), state_(state), kind_(entry.kind) {
        setObjectName(QStringLiteral("AdjustmentRow"));
        const bool supported = kind_ >= 0;
        setProperty("unsupported", supported ? "false" : "true");
        setAttribute(Qt::WA_Hover);
        setCursor(supported ? Qt::PointingHandCursor : Qt::ArrowCursor);
        setToolTip(supported ? tr(entry.detail)
                             : tr("Not implemented in Pittore yet."));

        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);

        auto* head = new QHBoxLayout;
        head->setContentsMargins(6, 5, 8, 5);
        head->setSpacing(6);
        chevron_ = new QToolButton(this);
        chevron_->setObjectName(QStringLiteral("AdjustmentChevron"));
        chevron_->setText(QStringLiteral("\u25B8"));
        chevron_->setFixedSize(16, 18);
        chevron_->setCursor(Qt::PointingHandCursor);
        head->addWidget(chevron_);

        auto* icon = new QLabel(this);
        const ThemeColors c = colorsFor(state_->theme());
        QIcon ico = chromeIcon(QString::fromUtf8(entry.icon), c.text,
                               c.accentText);
        if (ico.isNull())
            ico = chromeIcon(QStringLiteral("adjustments"), c.text,
                             c.accentText);
        icon->setPixmap(ico.pixmap(16, 16));
        icon->setFixedSize(18, 18);
        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        head->addWidget(icon);

        auto* title = new QLabel(tr(entry.name), this);
        title->setObjectName(QStringLiteral("AdjustmentTitle"));
        title->setProperty("unsupported", supported ? "false" : "true");
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        head->addWidget(title, 1);
        outer->addLayout(head);

        auto* body = new QWidget(this);
        auto* bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 0, 0);
        bodyLayout->setSpacing(2);
        detail_ = new QLabel(tr(entry.detail), body);
        detail_->setObjectName(QStringLiteral("AdjustmentDetail"));
        detail_->setWordWrap(true);
        detail_->setContentsMargins(8, 0, 4, 4);
        bodyLayout->addWidget(detail_);

        int presetCount = 0;
        const AdjustmentPreset* presets = adjustmentPresets(kind_, &presetCount);
        if (presets && presetCount > 0) {
            auto* grid = new QGridLayout;
            grid->setContentsMargins(6, 0, 4, 8);
            grid->setHorizontalSpacing(4);
            grid->setVerticalSpacing(6);
            DocumentItem* doc = state_->activeDocument();
            const QImage source = doc ? doc->composite : QImage();
            for (int pi = 0; pi < presetCount; ++pi) {
                const AdjustmentPreset& preset = presets[pi];
                auto* tile = new QToolButton(body);
                tile->setObjectName(QStringLiteral("ProfileTile"));
                tile->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
                tile->setIcon(QIcon(QPixmap::fromImage(
                    profilePreview(source, kind_, preset.params))));
                tile->setIconSize(QSize(68, 44));
                QString label = tr(preset.name);
                if (label.size() > 12) {
                    const int split = label.lastIndexOf(QLatin1Char(' '), 12);
                    if (split > 0)
                        label = label.left(split) + QLatin1Char('\n') +
                                label.mid(split + 1);
                }
                tile->setText(label);
                tile->setToolTip(tr(preset.name));
                QFont tileFont = tile->font();
                tileFont.setPixelSize(10);
                tile->setFont(tileFont);
                tile->setFixedSize(118, 86);
                tile->setCursor(Qt::PointingHandCursor);
                const float* params = preset.params;
                connect(tile, &QToolButton::clicked, this, [this, params] {
                    state_->addAdjustmentLayer(kind_);
                    DocumentItem* d = state_->activeDocument();
                    if (!d || d->activeLayer < 0 ||
                        d->activeLayer >= d->layers.size())
                        return;
                    state_->setAdjustmentParams(d->activeLayer, params);
                });
                profileTiles_.append(qMakePair(tile, &preset));
                grid->addWidget(tile, pi / 2, pi % 2);
            }
            bodyLayout->addLayout(grid);
        }
        body->hide();
        outer->addWidget(body);

        connect(chevron_, &QToolButton::clicked, this, [this, body] {
            expanded_ = !expanded_;
            if (expanded_) refreshPreviews();
            body->setVisible(expanded_);
            chevron_->setText(expanded_ ? QStringLiteral("\u25BE")
                                        : QStringLiteral("\u25B8"));
        });
    }

  protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QFrame::mouseReleaseEvent(event);
            return;
        }
        if (kind_ < 0) {
            state_->setStatusHint(
                tr("That adjustment is not implemented in Pittore yet."));
            return;
        }
        state_->addAdjustmentLayer(kind_);
    }

  private:
    void refreshPreviews() {
        DocumentItem* doc = state_->activeDocument();
        const QImage source = doc ? doc->composite : QImage();
        for (const auto& entry : profileTiles_)
            entry.first->setIcon(QIcon(QPixmap::fromImage(
                profilePreview(source, kind_, entry.second->params))));
    }

    AppState* state_ = nullptr;
    int kind_ = -1;
    QVector<QPair<QToolButton*, const AdjustmentPreset*>> profileTiles_;
    bool expanded_ = false;
    QToolButton* chevron_ = nullptr;
    QLabel* detail_ = nullptr;
};

class AdjustmentsPanel final : public QWidget {
  public:
    AdjustmentsPanel(AppState* state, QWidget* parent)
        : QWidget(parent), state_(state) {
        setObjectName(QStringLiteral("AdjustmentsRoot"));
        setStyleSheet(panelStyle());
        setMinimumWidth(320);
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(6, 6, 6, 6);
        column->setSpacing(6);

        mode_ = new QComboBox(this);
        mode_->setObjectName(QStringLiteral("AdjustmentMode"));
        mode_->addItem(tr("Adjustments"));
        mode_->addItem(tr("Quick Adjustments"));
        column->addWidget(mode_);

        scroll_ = new QScrollArea(this);
        scroll_->setWidgetResizable(true);
        scroll_->setFrameShape(QFrame::NoFrame);
        scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll_->setMinimumWidth(300);
        list_ = new QWidget(scroll_);
        listLayout_ = new QVBoxLayout(list_);
        listLayout_->setContentsMargins(0, 4, 2, 4);
        listLayout_->setSpacing(6);
        scroll_->setWidget(list_);
        column->addWidget(scroll_, 1);

        connect(mode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int index) { rebuild(index == 1); });
        rebuild(false);
    }

  private:
    void rebuild(bool quick) {
        while (QLayoutItem* item = listLayout_->takeAt(0)) {
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }
        for (const AdjustmentEntry& entry : kAdjustments) {
            if (quick && !entry.quick) continue;
            listLayout_->addWidget(new AdjustmentRow(entry, state_, list_));
        }
        listLayout_->addStretch(1);
    }

    AppState* state_ = nullptr;
    QComboBox* mode_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QWidget* list_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;
};

}  // namespace

QWidget* createAdjustmentsPanel(AppState* state, QWidget* parent) {
    return new AdjustmentsPanel(state, parent);
}

}  // namespace pittore::ui
