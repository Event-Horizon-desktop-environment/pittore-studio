#include "ui/panels.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/app_state.h"
#include "ui/panels/shared/panel_helpers.h"

namespace pittore::ui {
namespace {

class ParagraphPanel final : public QWidget {
  public:
    ParagraphPanel(AppState* state, QWidget* parent)
        : QWidget(parent), state_(state) {
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

        column->addWidget(sectionLabel(tr("Alignment"), state_, body));
        auto* alignRow = new QHBoxLayout;
        alignRow->setSpacing(4);
        group_ = new QButtonGroup(this);
        group_->setExclusive(true);
        const char* labels[] = {"Left", "Center", "Right", "Justify"};
        for (int i = 0; i < 4; ++i) {
            auto* b = new QToolButton(body);
            b->setText(tr(labels[i]));
            b->setCheckable(true);
            b->setAutoRaise(true);
            b->setToolButtonStyle(Qt::ToolButtonTextOnly);
            b->setMinimumWidth(58);
            b->setObjectName(QStringLiteral("paragraph.align%1").arg(i));
            group_->addButton(b, i);
            alignRow->addWidget(b);
        }
        alignRow->addStretch(1);
        column->addLayout(alignRow);
        connect(group_, QOverload<int>::of(&QButtonGroup::idClicked), this,
                [this](int id) {
                    if (!updating_)
                        state_->applyCharacterOption(QStringLiteral("align"), id);
                });

        column->addWidget(sectionLabel(tr("Indents"), state_, body));
        auto* indentForm = new QFormLayout;
        indentForm->setSpacing(6);
        indentStart_ = addSpin(indentForm, tr("Indent left"),
                               QStringLiteral("paragraph.indentStart"));
        indentEnd_ = addSpin(indentForm, tr("Indent right"),
                             QStringLiteral("paragraph.indentEnd"));
        firstLine_ = addSpin(indentForm, tr("First line indent"),
                             QStringLiteral("paragraph.firstLine"));
        column->addLayout(indentForm);

        column->addWidget(sectionLabel(tr("Spacing"), state_, body));
        auto* spaceForm = new QFormLayout;
        spaceForm->setSpacing(6);
        spaceBefore_ = addSpin(spaceForm, tr("Space before"),
                               QStringLiteral("paragraph.spaceBefore"));
        spaceAfter_ = addSpin(spaceForm, tr("Space after"),
                              QStringLiteral("paragraph.spaceAfter"));
        column->addLayout(spaceForm);

        column->addWidget(sectionLabel(tr("Frame insets"), state_, body));
        auto* insetForm = new QFormLayout;
        insetForm->setSpacing(6);
        insetLeft_ = addSpin(insetForm, tr("Left"),
                             QStringLiteral("paragraph.insetLeft"));
        insetRight_ = addSpin(insetForm, tr("Right"),
                              QStringLiteral("paragraph.insetRight"));
        insetTop_ = addSpin(insetForm, tr("Top"),
                            QStringLiteral("paragraph.insetTop"));
        insetBottom_ = addSpin(insetForm, tr("Bottom"),
                               QStringLiteral("paragraph.insetBottom"));
        column->addLayout(insetForm);

        auto* vAlignForm = new QFormLayout;
        vAlignForm->setSpacing(6);
        vAlign_ = new QComboBox(body);
        vAlign_->addItem(tr("Top"), 0);
        vAlign_->addItem(tr("Centre"), 1);
        vAlign_->addItem(tr("Bottom"), 2);
        vAlign_->setObjectName(QStringLiteral("paragraph.verticalAlign"));
        vAlignForm->addRow(tr("Vertical align"), vAlign_);
        column->addLayout(vAlignForm);
        connect(vAlign_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) {
                    if (!updating_)
                        state_->applyCharacterOption(
                            QStringLiteral("verticalAlign"),
                            vAlign_->currentData());
                });

        column->addWidget(sectionLabel(tr("Frame columns"), state_, body));
        auto* frameForm = new QFormLayout;
        frameForm->setSpacing(6);
        columns_ = new QSpinBox(body);
        columns_->setRange(1, 16);
        columns_->setObjectName(QStringLiteral("paragraph.columns"));
        frameForm->addRow(tr("Columns"), columns_);
        gutter_ = new QDoubleSpinBox(body);
        gutter_->setRange(0.0, 1000.0);
        gutter_->setDecimals(1);
        gutter_->setSuffix(QStringLiteral(" px"));
        gutter_->setObjectName(QStringLiteral("paragraph.columnGutter"));
        frameForm->addRow(tr("Gutter"), gutter_);
        autoFit_ = new QCheckBox(tr("Auto-fit frame"), body);
        autoFit_->setObjectName(QStringLiteral("paragraph.autoFit"));
        frameForm->addRow(QString(), autoFit_);
        column->addLayout(frameForm);
        connect(columns_, QOverload<int>::of(&QSpinBox::valueChanged), this,
                [this](int v) {
                    if (!updating_)
                        state_->applyCharacterOption(QStringLiteral("columns"), v);
                });
        connect(gutter_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) {
                    if (!updating_)
                        state_->applyCharacterOption(
                            QStringLiteral("columnGutter"), v);
                });
        connect(autoFit_, &QCheckBox::toggled, this, [this](bool on) {
            if (!updating_)
                state_->applyCharacterOption(QStringLiteral("autoFit"), on);
        });

        auto* tabsForm = new QFormLayout;
        tabsForm->setSpacing(6);
        tabInterval_ = new QDoubleSpinBox(body);
        tabInterval_->setRange(1.0, 2000.0);
        tabInterval_->setDecimals(1);
        tabInterval_->setSuffix(QStringLiteral(" px"));
        tabInterval_->setObjectName(QStringLiteral("paragraph.tabInterval"));
        tabsForm->addRow(tr("Tab interval"), tabInterval_);
        column->addLayout(tabsForm);
        connect(tabInterval_, &QDoubleSpinBox::valueChanged, this,
                [this](double v) {
                    if (!updating_)
                        state_->applyCharacterOption(
                            QStringLiteral("tabInterval"), v);
                });

        noBreak_ = new QCheckBox(tr("No break"), body);
        noBreak_->setObjectName(QStringLiteral("paragraph.noBreak"));
        noBreak_->setToolTip(tr("Keep this paragraph on one line."));
        column->addWidget(noBreak_);
        connect(noBreak_, &QCheckBox::toggled, this, [this](bool on) {
            if (!updating_)
                state_->applyCharacterOption(QStringLiteral("noBreak"), on);
        });

        hyphenate_ = new QCheckBox(tr("Hyphenate"), body);
        hyphenate_->setObjectName(QStringLiteral("paragraph.hyphenate"));
        column->addWidget(hyphenate_);
        connect(hyphenate_, &QCheckBox::toggled, this, [this](bool on) {
            if (!updating_)
                state_->applyCharacterOption(QStringLiteral("hyphenate"), on);
        });
        column->addStretch(1);

        connect(state_, &AppState::activeLayerChanged, this, [this] { refresh(); });
        connect(state_, &AppState::layersChanged, this, [this] { refresh(); });
        connect(state_, &AppState::activeDocumentChanged, this,
                [this](DocumentItem*) { refresh(); });
        refresh();
    }

  private:
    QDoubleSpinBox* addSpin(QFormLayout* form, const QString& label,
                            const QString& objectName) {
        auto* spin = new QDoubleSpinBox(this);
        spin->setRange(-10000.0, 10000.0);
        spin->setDecimals(1);
        spin->setSuffix(QStringLiteral(" px"));
        spin->setObjectName(objectName);
        form->addRow(label, spin);
        connect(spin, &QDoubleSpinBox::valueChanged, this,
                [this, objectName](double v) {
                    if (!updating_)
                        state_->applyCharacterOption(
                            objectName.mid(objectName.indexOf('.') + 1), v);
                });
        return spin;
    }

    void refresh() {
        updating_ = true;
        const TextItem t = state_->activeTextSpec();
        if (QAbstractButton* b = group_->button(qBound(0, t.align, 3)))
            b->setChecked(true);
        indentStart_->setValue(t.indentStart);
        indentEnd_->setValue(t.indentEnd);
        firstLine_->setValue(t.firstLineIndent);
        spaceBefore_->setValue(t.spaceBefore);
        spaceAfter_->setValue(t.spaceAfter);
        insetLeft_->setValue(t.insetLeft);
        insetRight_->setValue(t.insetRight);
        insetTop_->setValue(t.insetTop);
        insetBottom_->setValue(t.insetBottom);
        vAlign_->setCurrentIndex(qBound(0, t.verticalAlign, 2));
        columns_->setValue(qBound(1, t.columns, 16));
        gutter_->setValue(t.columnGutter);
        autoFit_->setChecked(t.autoFit);
        tabInterval_->setValue(t.tabInterval);
        noBreak_->setChecked(t.noBreak);
        hyphenate_->setChecked(t.hyphenate);
        listType_->setCurrentIndex(qBound(0, t.listType, 2));
        listStart_->setValue(t.listStart);
        listIndent_->setValue(t.listIndent);
        updating_ = false;
    }

    AppState* state_ = nullptr;
    QButtonGroup* group_ = nullptr;
    QDoubleSpinBox* indentStart_ = nullptr;
    QDoubleSpinBox* indentEnd_ = nullptr;
    QDoubleSpinBox* firstLine_ = nullptr;
    QDoubleSpinBox* spaceBefore_ = nullptr;
    QDoubleSpinBox* spaceAfter_ = nullptr;
    QDoubleSpinBox* insetLeft_ = nullptr;
    QDoubleSpinBox* insetRight_ = nullptr;
    QDoubleSpinBox* insetTop_ = nullptr;
    QDoubleSpinBox* insetBottom_ = nullptr;
    QComboBox* vAlign_ = nullptr;
    QSpinBox* columns_ = nullptr;
    QDoubleSpinBox* gutter_ = nullptr;
    QCheckBox* autoFit_ = nullptr;
    QDoubleSpinBox* tabInterval_ = nullptr;
    QCheckBox* noBreak_ = nullptr;
    QCheckBox* hyphenate_ = nullptr;
    QCheckBox* keepLines_ = nullptr;
    QComboBox* listType_ = nullptr;
    QSpinBox* listStart_ = nullptr;
    QDoubleSpinBox* listIndent_ = nullptr;
    bool updating_ = false;
};

}  // namespace

QWidget* createParagraphPanel(AppState* state, QWidget* parent) {
    return new ParagraphPanel(state, parent);
}

}  // namespace pittore::ui
