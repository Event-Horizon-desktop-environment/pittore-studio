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
#include "ui/font_preview.h"

namespace pittore::ui {
namespace {


class CharacterPanel final : public QWidget {
  public:
    CharacterPanel(AppState* state, QWidget* parent) : QWidget(parent), state_(state) {
        for (const std::string& f : typeFontFamilies())
            families_ << QString::fromStdString(f);

        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        auto* scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* body = new QWidget;
        auto* column = new QVBoxLayout(body);
        column->setContentsMargins(12, 12, 12, 12);
        column->setSpacing(8);
        scroll->setWidget(body);
        outer->addWidget(scroll);

        // --- font ---------------------------------------------------------
        column->addWidget(sectionLabel(tr("Font"), state_, body));
        auto* fontForm = new QFormLayout;
        fontForm->setSpacing(6);
        fontForm->setContentsMargins(0, 0, 0, 0);

        collection_ = new QComboBox(body);
        collection_->addItem(tr("All Fonts"));
        collection_->setToolTip(tr("Font collections are not available yet."));
        fontForm->addRow(tr("Collection"), collection_);

        family_ = new QComboBox(body);
        family_->addItems(families_);
        makeFamilyComboSearchable(family_);
        applyFamilyPreview(family_, [state = state_] {
            return state->settings().fontPreviewSize;
        });
        fontForm->addRow(tr("Family"), family_);

        style_ = new QComboBox(body);
        style_->addItem(tr("Regular"), 0);
        style_->addItem(tr("Italic"), 1);
        style_->addItem(tr("Bold"), 4);
        style_->addItem(tr("Bold Italic"), 6);
        fontForm->addRow(tr("Style"), style_);

        size_ = new QDoubleSpinBox(body);
        size_->setRange(1, 1296);
        size_->setValue(36);
        size_->setDecimals(1);
        size_->setSuffix(QStringLiteral(" pt"));
        fontForm->addRow(tr("Size"), size_);

        variations_ = new QToolButton(body);
        variations_->setText(tr("Variations\u2026"));
        variations_->setEnabled(false);
        variations_->setToolTip(tr("Variable-font axes are not editable yet."));
        fontForm->addRow(QString(), variations_);

        fontColor_ = new QToolButton(body);
        fontColor_->setToolTip(tr("Fill colour"));
        fontForm->addRow(tr("Fill"), fontColor_);

        bgColor_ = new QToolButton(body);
        bgColor_->setToolTip(tr("Highlight behind the text (Alt-click to clear)."));
        fontForm->addRow(tr("Background"), bgColor_);

        textStyle_ = new QComboBox(body);
        textStyle_->addItem(tr("None"));
        textStyle_->setEnabled(false);
        textStyle_->setToolTip(tr("Named text styles are not available yet."));
        fontForm->addRow(tr("Text Style"), textStyle_);
        column->addLayout(fontForm);

        // --- decorations ---------------------------------------------------
        column->addWidget(sectionLabel(tr("Decorations"), state_, body));
        auto* decoForm = new QFormLayout;
        decoForm->setSpacing(6);
        decoForm->setContentsMargins(0, 0, 0, 0);

        underline_ = new QComboBox(body);
        underline_->addItem(tr("None"), 0);
        underline_->addItem(tr("Single"), 1);
        underline_->addItem(tr("Double"), 2);
        decoForm->addRow(tr("Underline"), underline_);

        underlineColor_ = new QToolButton(body);
        underlineColor_->setToolTip(tr("Underline colour (Alt-click to inherit)."));
        decoForm->addRow(QString(), underlineColor_);

        strike_ = new QComboBox(body);
        strike_->addItem(tr("None"), 0);
        strike_->addItem(tr("Single"), 1);
        strike_->addItem(tr("Double"), 2);
        decoForm->addRow(tr("Strikethrough"), strike_);

        strikeColor_ = new QToolButton(body);
        strikeColor_->setToolTip(tr("Strikethrough colour (Alt-click to inherit)."));
        decoForm->addRow(QString(), strikeColor_);
        column->addLayout(decoForm);

        // --- position & transform ------------------------------------------
        column->addWidget(sectionLabel(tr("Position & Transform"), state_, body));
        auto* posForm = new QFormLayout;
        posForm->setSpacing(6);
        posForm->setContentsMargins(0, 0, 0, 0);

        tracking_ = new QDoubleSpinBox(body);
        tracking_->setRange(-1000, 1000);
        tracking_->setDecimals(1);
        tracking_->setSuffix(QStringLiteral(" px"));
        posForm->addRow(tr("Tracking"), tracking_);

        leading_ = new QDoubleSpinBox(body);
        leading_->setRange(0.1, 10.0);
        leading_->setDecimals(2);
        leading_->setSingleStep(0.1);
        leading_->setSuffix(QStringLiteral(" \u00D7"));
        posForm->addRow(tr("Leading"), leading_);

        baseline_ = new QDoubleSpinBox(body);
        baseline_->setRange(-2000, 2000);
        baseline_->setDecimals(1);
        baseline_->setSuffix(QStringLiteral(" px"));
        posForm->addRow(tr("Baseline"), baseline_);

        hScale_ = new QSpinBox(body);
        hScale_->setRange(1, 1000);
        hScale_->setSuffix(QStringLiteral("%"));
        posForm->addRow(tr("Horizontal"), hScale_);

        vScale_ = new QSpinBox(body);
        vScale_->setRange(1, 1000);
        vScale_->setSuffix(QStringLiteral("%"));
        posForm->addRow(tr("Vertical"), vScale_);

        superSub_ = new QComboBox(body);
        superSub_->addItem(tr("None"), 0);
        superSub_->addItem(tr("Superscript"), 1);
        superSub_->addItem(tr("Subscript"), -1);
        posForm->addRow(tr("Position"), superSub_);

        rotation_ = new QDoubleSpinBox(body);
        rotation_->setRange(-3600, 3600);
        rotation_->setDecimals(1);
        rotation_->setSuffix(QStringLiteral(" \u00B0"));
        posForm->addRow(tr("Rotation"), rotation_);

        shear_ = new QDoubleSpinBox(body);
        shear_->setRange(-89, 89);
        shear_->setDecimals(1);
        shear_->setSuffix(QStringLiteral(" \u00B0"));
        posForm->addRow(tr("Shear"), shear_);

        noBreak_ = new QCheckBox(tr("No break"), body);
        noBreak_->setEnabled(false);
        noBreak_->setToolTip(tr("No-break runs are not available yet."));
        posForm->addRow(QString(), noBreak_);
        column->addLayout(posForm);

        // --- typography ----------------------------------------------------
        column->addWidget(sectionLabel(tr("Typography"), state_, body));
        allCaps_ = new QCheckBox(tr("All caps"), body);
        kerning_ = new QCheckBox(tr("Kerning (metrics)"), body);
        column->addWidget(allCaps_);
        column->addWidget(kerning_);

        auto* featuresGrid = new QGridLayout;
        featuresGrid->setContentsMargins(0, 0, 0, 0);
        featuresGrid->setHorizontalSpacing(12);
        featuresGrid->setVerticalSpacing(6);
        const std::pair<const char*, const char*> featureList[] = {
            {"ot:liga", QT_TR_NOOP("Ligatures")},
            {"ot:calt", QT_TR_NOOP("Contextual alternates")},
            {"ot:smcp", QT_TR_NOOP("Small caps")},
            {"ot:c2sc", QT_TR_NOOP("All small caps")},
            {"ot:sups", QT_TR_NOOP("Superscript")},
            {"ot:subs", QT_TR_NOOP("Subscript")},
            {"ot:frac", QT_TR_NOOP("Fractions")},
            {"ot:ordn", QT_TR_NOOP("Ordinals")},
            {"ot:swsh", QT_TR_NOOP("Swash")},
            {"ot:ss01", QT_TR_NOOP("Stylistic set 1")},
            {"ot:ss02", QT_TR_NOOP("Stylistic set 2")},
            {"ot:ss03", QT_TR_NOOP("Stylistic set 3")},
        };
        int fi = 0;
        for (const auto& f : featureList) {
            const QString id = QString::fromLatin1(f.first);
            auto* box = new QCheckBox(tr(f.second), body);
            features_.insert(id, box);
            connect(box, &QCheckBox::toggled, this, [this, id](bool on) {
                apply(id, on);
            });
            featuresGrid->addWidget(box, fi / 2, fi % 2);
            ++fi;
        }
        column->addLayout(featuresGrid);

        // --- language ------------------------------------------------------
        column->addWidget(sectionLabel(tr("Language"), state_, body));
        language_ = new QComboBox(body);
        const std::pair<const char*, const char*> langs[] = {
            {"en", "English"},   {"en-GB", "English (UK)"},
            {"en-US", "English (US)"}, {"de", "German"},
            {"fr", "French"},    {"es", "Spanish"},
            {"it", "Italian"},   {"pt", "Portuguese"},
            {"nl", "Dutch"},     {"sv", "Swedish"},
            {"pl", "Polish"},    {"cs", "Czech"},
            {"ru", "Russian"},   {"ja", "Japanese"},
            {"zh", "Chinese"},   {"ko", "Korean"},
            {"ar", "Arabic"},    {"he", "Hebrew"},
        };
        for (const auto& l : langs)
            language_->addItem(tr(l.second), QString::fromLatin1(l.first));
        column->addWidget(language_);
        column->addStretch(1);

        // Stable object names so tests can drive the panel.
        collection_->setObjectName(QStringLiteral("character.collection"));
        family_->setObjectName(QStringLiteral("character.family"));
        style_->setObjectName(QStringLiteral("character.style"));
        size_->setObjectName(QStringLiteral("character.size"));
        fontColor_->setObjectName(QStringLiteral("character.fill"));
        bgColor_->setObjectName(QStringLiteral("character.background"));
        underline_->setObjectName(QStringLiteral("character.underline"));
        underlineColor_->setObjectName(QStringLiteral("character.underlineColor"));
        strike_->setObjectName(QStringLiteral("character.strike"));
        strikeColor_->setObjectName(QStringLiteral("character.strikeColor"));
        tracking_->setObjectName(QStringLiteral("character.tracking"));
        leading_->setObjectName(QStringLiteral("character.leading"));
        baseline_->setObjectName(QStringLiteral("character.baseline"));
        hScale_->setObjectName(QStringLiteral("character.hScale"));
        vScale_->setObjectName(QStringLiteral("character.vScale"));
        rotation_->setObjectName(QStringLiteral("character.rotation"));
        shear_->setObjectName(QStringLiteral("character.shear"));
        language_->setObjectName(QStringLiteral("character.language"));
        superSub_->setObjectName(QStringLiteral("character.superSub"));
        allCaps_->setObjectName(QStringLiteral("character.allCaps"));
        kerning_->setObjectName(QStringLiteral("character.kerning"));
        for (auto it = features_.begin(); it != features_.end(); ++it)
            it.value()->setObjectName(QStringLiteral("character.") + it.key());

        // Every control routes through apply(), which writes the option to the
        // active text layer (one undo step) or the Type insertion defaults.
        connect(family_, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i >= 0) apply(QStringLiteral("family"), i);
        });
        connect(style_, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i >= 0) apply(QStringLiteral("style"), style_->itemData(i).toInt());
        });
        connect(size_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) { apply(QStringLiteral("size"), v); });
        connect(tracking_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) { apply(QStringLiteral("tracking"), v); });
        connect(leading_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) { apply(QStringLiteral("leading"), v); });
        connect(baseline_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) { apply(QStringLiteral("baselineShift"), v); });
        connect(hScale_, &QSpinBox::valueChanged, this,
                [this](int v) { apply(QStringLiteral("hScale"), v); });
        connect(vScale_, &QSpinBox::valueChanged, this,
                [this](int v) { apply(QStringLiteral("vScale"), v); });
        connect(underline_, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i >= 0) apply(QStringLiteral("underline"), underline_->itemData(i).toInt());
        });
        connect(strike_, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i >= 0) apply(QStringLiteral("strike"), strike_->itemData(i).toInt());
        });
        connect(superSub_, &QComboBox::currentIndexChanged, this, [this](int i) {
            if (i >= 0) apply(QStringLiteral("superSub"), superSub_->itemData(i).toInt());
        });
        connect(allCaps_, &QCheckBox::toggled, this,
                [this](bool on) { apply(QStringLiteral("allCaps"), on); });
        connect(kerning_, &QCheckBox::toggled, this,
                [this](bool on) { apply(QStringLiteral("kerning"), on); });
        connect(rotation_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) { apply(QStringLiteral("rotation"), v); });
        connect(shear_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) { apply(QStringLiteral("shear"), v); });
        connect(language_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) {
                    if (!updating_)
                        apply(QStringLiteral("language"),
                              language_->currentData());
                });
        connect(fontColor_, &QToolButton::clicked, this, [this] {
            pickColor(QStringLiteral("color"), tr("Fill Colour"));
        });
        connect(bgColor_, &QToolButton::clicked, this, [this] {
            pickColor(QStringLiteral("backgroundColor"), tr("Background Colour"));
        });
        connect(underlineColor_, &QToolButton::clicked, this, [this] {
            pickColor(QStringLiteral("underlineColor"), tr("Underline Colour"));
        });
        connect(strikeColor_, &QToolButton::clicked, this, [this] {
            pickColor(QStringLiteral("strikeColor"), tr("Strikethrough Colour"));
        });

        connect(state_, &AppState::activeLayerChanged, this, [this] { refresh(); });
        connect(state_, &AppState::layersChanged, this, [this] { refresh(); });
        connect(state_, &AppState::activeDocumentChanged, this,
                [this](DocumentItem*) { refresh(); });
        refresh();
    }

  private:
    // One user edit. Guarded against the refresh churn so setting a control
    // programmatically never writes back to the document.
    void apply(const QString& id, const QVariant& value) {
        if (updating_) return;
        state_->applyCharacterOption(id, value);
        refresh();
    }

    void pickColor(const QString& id, const QString& title) {
        const TextItem t = state_->activeTextSpec();
        QColor current;
        if (id == QStringLiteral("color")) current = t.color;
        else if (id == QStringLiteral("backgroundColor")) current = t.backgroundColor;
        else if (id == QStringLiteral("underlineColor")) current = t.underlineColor;
        else if (id == QStringLiteral("strikeColor")) current = t.strikeColor;
        if (!current.isValid()) current = t.color;
        // Alt-click clears: an invalid colour means inherit / no highlight.
        if (QApplication::keyboardModifiers() & Qt::AltModifier) {
            apply(id, QColor());
            return;
        }
        const QColor c =
            QColorDialog::getColor(current, this, title, QColorDialog::ShowAlphaChannel);
        if (c.isValid()) apply(id, c);
    }

    void refresh() {
        updating_ = true;
        const TextItem t = state_->activeTextSpec();
        setEnabled(state_->activeDocument() != nullptr);

        const int famIndex = families_.indexOf(t.family);
        if (famIndex >= 0) family_->setCurrentIndex(famIndex);
        const int styleValue = t.bold && t.italic ? 6 : t.bold ? 4 : t.italic ? 1 : 0;
        style_->setCurrentIndex(style_->findData(styleValue));
        size_->setValue(t.size);
        tracking_->setValue(t.tracking);
        leading_->setValue(t.lineHeight);
        baseline_->setValue(t.baselineShift);
        hScale_->setValue(qRound(t.hScale));
        vScale_->setValue(qRound(t.vScale));
        rotation_->setValue(t.rotation);
        shear_->setValue(t.shear);
        {
            const int li = language_->findData(t.language);
            if (li >= 0) language_->setCurrentIndex(li);
        }
        underline_->setCurrentIndex(underline_->findData(t.underline));
        strike_->setCurrentIndex(strike_->findData(t.strike));
        superSub_->setCurrentIndex(superSub_->findData(t.superSub));
        allCaps_->setChecked(t.allCaps);
        kerning_->setChecked(t.kerning);

        static const QHash<QString, unsigned> bits = {
            {QStringLiteral("ot:liga"), pittore::text::OTF_Liga},
            {QStringLiteral("ot:calt"), pittore::text::OTF_Calt},
            {QStringLiteral("ot:smcp"), pittore::text::OTF_Smcp},
            {QStringLiteral("ot:c2sc"), pittore::text::OTF_C2sc},
            {QStringLiteral("ot:sups"), pittore::text::OTF_Sups},
            {QStringLiteral("ot:subs"), pittore::text::OTF_Subs},
            {QStringLiteral("ot:frac"), pittore::text::OTF_Frac},
            {QStringLiteral("ot:ordn"), pittore::text::OTF_Ordn},
            {QStringLiteral("ot:swsh"), pittore::text::OTF_Swsh},
            {QStringLiteral("ot:ss01"), pittore::text::OTF_Ss01},
            {QStringLiteral("ot:ss02"), pittore::text::OTF_Ss02},
            {QStringLiteral("ot:ss03"), pittore::text::OTF_Ss03},
        };
        for (auto it = features_.begin(); it != features_.end(); ++it)
            it.value()->setChecked((t.otFeatures & bits.value(it.key(), 0u)) != 0);

        paintColorSwatch(fontColor_, t.color);
        paintColorSwatch(bgColor_, t.backgroundColor);
        paintColorSwatch(underlineColor_, t.underlineColor);
        paintColorSwatch(strikeColor_, t.strikeColor);
        updating_ = false;
    }

    AppState* state_;
    bool updating_ = false;
    QStringList families_;
    QComboBox* collection_ = nullptr;
    QComboBox* family_ = nullptr;
    QComboBox* style_ = nullptr;
    QDoubleSpinBox* size_ = nullptr;
    QToolButton* variations_ = nullptr;
    QToolButton* fontColor_ = nullptr;
    QToolButton* bgColor_ = nullptr;
    QComboBox* textStyle_ = nullptr;
    QComboBox* underline_ = nullptr;
    QToolButton* underlineColor_ = nullptr;
    QComboBox* strike_ = nullptr;
    QToolButton* strikeColor_ = nullptr;
    QDoubleSpinBox* rotation_ = nullptr;
    QDoubleSpinBox* shear_ = nullptr;
    QComboBox* language_ = nullptr;
    QDoubleSpinBox* tracking_ = nullptr;
    QDoubleSpinBox* leading_ = nullptr;
    QDoubleSpinBox* baseline_ = nullptr;
    QSpinBox* hScale_ = nullptr;
    QSpinBox* vScale_ = nullptr;
    QComboBox* superSub_ = nullptr;
    QCheckBox* noBreak_ = nullptr;
    QCheckBox* allCaps_ = nullptr;
    QCheckBox* kerning_ = nullptr;
    QHash<QString, QCheckBox*> features_;
};

}  // namespace

QWidget* createCharacterPanel(AppState* state, QWidget* parent) {
    return new CharacterPanel(state, parent);
}

}  // namespace pittore::ui
