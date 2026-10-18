#include "ui/persona/persona_bar.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QToolButton>

#include "ui/app_state.h"
#include "ui/theme.h"

namespace pittore::ui {

PersonaBar::PersonaBar(AppState* state, QWidget* parent) : QFrame(parent), state_(state) {
    setObjectName(QStringLiteral("personaBar"));
    setFrameShape(QFrame::NoFrame);
    setFixedHeight(tabletBarHeight());

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(12, 3, 12, 3);
    row->setSpacing(4);

    group_ = new QButtonGroup(this);
    group_->setExclusive(true);

    vectorButton_ = new QToolButton(this);
    vectorButton_->setText(tr("Vector"));
    vectorButton_->setToolTip(tr("Vector persona — curves, shapes, fills and text"));
    vectorButton_->setCheckable(true);
    vectorButton_->setCursor(Qt::PointingHandCursor);

    pixelButton_ = new QToolButton(this);
    pixelButton_->setText(tr("Pixel"));
    pixelButton_->setToolTip(tr("Pixel persona — retouch, selections and adjustments"));
    pixelButton_->setCheckable(true);
    pixelButton_->setCursor(Qt::PointingHandCursor);

    drawButton_ = new QToolButton(this);
    drawButton_->setText(tr("Draw"));
    drawButton_->setToolTip(tr("Draw persona — freehand painting, brushes and tablet"));
    drawButton_->setCheckable(true);
    drawButton_->setCursor(Qt::PointingHandCursor);

    colorButton_ = new QToolButton(this);
    colorButton_->setText(tr("Color"));
    colorButton_->setToolTip(tr("Color persona — grade, balance and scopes"));
    colorButton_->setCheckable(true);
    colorButton_->setCursor(Qt::PointingHandCursor);

    group_->addButton(vectorButton_, static_cast<int>(Persona::Vector));
    group_->addButton(pixelButton_, static_cast<int>(Persona::Pixel));
    group_->addButton(drawButton_, static_cast<int>(Persona::Draw));
    group_->addButton(colorButton_, static_cast<int>(Persona::Color));

    artboardButton_ = new QToolButton(this);
    artboardButton_->setText(tr("Artboard"));
    artboardButton_->setToolTip(
        tr("Artboard persona — create, size, reorder and export artboards"));
    artboardButton_->setCheckable(true);
    artboardButton_->setCursor(Qt::PointingHandCursor);
    group_->addButton(artboardButton_, static_cast<int>(Persona::Artboard));

    row->addWidget(vectorButton_);
    row->addWidget(pixelButton_);
    row->addWidget(drawButton_);
    row->addWidget(colorButton_);
    row->addWidget(artboardButton_);
    row->addStretch(1);

    connect(vectorButton_, &QToolButton::clicked, this,
            [this] { emit personaSelected(Persona::Vector); });
    connect(pixelButton_, &QToolButton::clicked, this,
            [this] { emit personaSelected(Persona::Pixel); });
    connect(drawButton_, &QToolButton::clicked, this,
            [this] { emit personaSelected(Persona::Draw); });
    connect(colorButton_, &QToolButton::clicked, this,
            [this] { emit personaSelected(Persona::Color); });
    connect(artboardButton_, &QToolButton::clicked, this,
            [this] { emit personaSelected(Persona::Artboard); });
    connect(state_, &AppState::themeChanged, this, [this] { applyTheme(); });
    connect(state_, &AppState::settingsChanged, this, [this] {
        setFixedHeight(tabletBarHeight());
        applyTheme();
    });

    pixelButton_->setChecked(true);
    applyTheme();
}

void PersonaBar::setPersona(Persona p) {
    if (p == Persona::Vector)
        vectorButton_->setChecked(true);
    else if (p == Persona::Draw)
        drawButton_->setChecked(true);
    else if (p == Persona::Color)
        colorButton_->setChecked(true);
    else if (p == Persona::Artboard)
        artboardButton_->setChecked(true);
    else
        pixelButton_->setChecked(true);
}

void PersonaBar::applyTheme() {
    const ThemeColors c = colorsFor(state_->theme());
    setStyleSheet(
        QStringLiteral("#personaBar { background: %1; border-bottom: 1px solid %2; }")
            .arg(c.chrome.name(), cssColor(c.divider)));
    // Tablet mode gets roomier pills to match the tool strip.
    const bool tablet = state_->settings().tabletMode;
    const QString button =
        QStringLiteral(
            "QToolButton { padding: %1; border-radius: 9px; color: %2; }"
            "QToolButton:checked { background: %3; color: %4; }"
            "QToolButton:hover:!checked { background: %5; }")
            .arg(tablet ? QStringLiteral("6px 18px") : QStringLiteral("3px 14px"))
            .arg(c.text.name(), c.accent.name(), c.accentText.name(), c.hover.name());
    vectorButton_->setStyleSheet(button);
    pixelButton_->setStyleSheet(button);
    drawButton_->setStyleSheet(button);
    colorButton_->setStyleSheet(button);
}

int PersonaBar::tabletBarHeight() const {
    return state_->settings().tabletMode ? 40 : 30;
}

}  // namespace pittore::ui
