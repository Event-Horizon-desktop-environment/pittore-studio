#include "ui/panels/shared/panel_helpers.h"

#include <QComboBox>
#include <QCompleter>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QSignalBlocker>
#include <QPainter>
#include <QPen>
#include <QToolButton>

#include "ui/app_state.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace pittore::ui {

QToolButton* footerButton(AppState* state, const QString& iconKey, const QString& tip,
                          QWidget* parent) {
    const ThemeColors c = colorsFor(state->theme());
    auto* button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setIcon(chromeIcon(iconKey, c.textDim, c.text));
    button->setIconSize(QSize(17, 17));
    button->setToolTip(tip);
    button->setFixedSize(24, 22);
    return button;
}

QLabel* sectionLabel(const QString& text, AppState* state, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(colorsFor(state->theme()).textDim.name()));
    return label;
}

// A colour well for the Character panel. An invalid or fully transparent
// colour is drawn as a crossed-out checkerboard: "inherit the fill" for a
// decoration, "no highlight" for the background.
QPixmap colorSwatchPixmap(const QColor& c, qreal dpr) {
    const qreal useDpr = dpr > 0.0 ? dpr : 1.0;
    QPixmap pm(qMax(1, qRound(18.0 * useDpr)), qMax(1, qRound(18.0 * useDpr)));
    pm.setDevicePixelRatio(useDpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    if (!c.isValid() || c.alpha() == 0) {
        p.fillRect(0, 0, 18, 18, QColor(255, 255, 255));
        p.fillRect(0, 0, 9, 9, QColor(190, 190, 190));
        p.fillRect(9, 9, 9, 9, QColor(190, 190, 190));
        p.setPen(QPen(QColor(210, 70, 70), 1.4));
        p.drawLine(3, 15, 15, 3);
    } else {
        p.fillRect(0, 0, 18, 18, c);
    }
    p.setPen(QColor(0, 0, 0, 110));
    p.drawRect(0, 0, 17, 17);
    p.end();
    return pm;
}

void paintColorSwatch(QToolButton* button, const QColor& c) {
    // Ship 1x and 2x bakes so fractional scales pick (or downscale) a
    // crisp variant instead of stretching the 1x one.
    QIcon icon;
    icon.addPixmap(colorSwatchPixmap(c, 1.0));
    icon.addPixmap(colorSwatchPixmap(c, 2.0));
    button->setIcon(icon);
    button->setIconSize(QSize(18, 18));
}


void makeFamilyComboSearchable(QComboBox* box) {
    if (!box) return;
    box->setEditable(true);
    box->setInsertPolicy(QComboBox::NoInsert);
    if (QCompleter* completer = box->completer()) {
        completer->setCompletionMode(QCompleter::PopupCompletion);
        completer->setFilterMode(Qt::MatchContains);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
    }
    // Enter resolves the typed text: exact match first (case-insensitive),
    // then first contains match; anything else reverts to the current item.
    // setCurrentIndex reuses the caller's existing currentIndexChanged commit
    // path, so popup picks and typed commits behave identically.
    QObject::connect(box->lineEdit(), &QLineEdit::returnPressed, box,
                     [box] {
                         const QString text =
                             box->lineEdit()->text().trimmed();
                         int hit = -1;
                         if (!text.isEmpty()) {
                             // MatchExactly is case-sensitive; a bare
                             // MatchFixedString/MatchContains match
                             // case-insensitively.
                             hit = box->findText(text, Qt::MatchExactly);
                             if (hit < 0)
                                 hit = box->findText(text, Qt::MatchFixedString);
                             if (hit < 0)
                                 hit = box->findText(text, Qt::MatchContains);
                         }
                         if (hit < 0) {
                             const QSignalBlocker block(box->lineEdit());
                             box->lineEdit()->setText(
                                 box->itemText(box->currentIndex()));
                         } else if (hit != box->currentIndex()) {
                             box->setCurrentIndex(hit);
                         } else {
                             // Canonical casing (e.g. typed "inter" resolved
                             // exactly): normalize the displayed text.
                             const QSignalBlocker block(box->lineEdit());
                             box->lineEdit()->setText(box->itemText(hit));
                         }
                     });
}

}  // namespace pittore::ui
