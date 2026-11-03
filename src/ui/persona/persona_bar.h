#pragma once
// Segmented [Vector|Pixel|Draw|Color] switcher (segmented persona toolbar).
//
// Emits on click; PersonaManager owns the state, the bar only reflects it, so
// the widget stays stateless and safe to rebuild on theme change.
#include <QFrame>

#include "ui/persona/persona.h"

class QButtonGroup;
class QToolButton;

namespace pittore::ui {

class AppState;

class PersonaBar final : public QFrame {
    Q_OBJECT

  public:
    explicit PersonaBar(AppState* state, QWidget* parent = nullptr);

    void setPersona(Persona p);

  signals:
    void personaSelected(Persona p);

  private:
    void applyTheme();
    int tabletBarHeight() const;

    AppState* state_;
    QToolButton* vectorButton_ = nullptr;
    QToolButton* pixelButton_ = nullptr;
    QToolButton* drawButton_ = nullptr;
    QToolButton* colorButton_ = nullptr;
    QToolButton* artboardButton_ = nullptr;
    QButtonGroup* group_ = nullptr;
};

}  // namespace pittore::ui
