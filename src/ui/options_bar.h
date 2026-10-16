#pragma once
#include <QFrame>
#include <QHash>

#include "ui/app_state.h"
#include "ui/tool_registry.h"

class QToolButton;

namespace pittore::ui {

class FlowLayout;

// The options bar: settings for the active tool, rebuilt on tool change.
// Widgets come from OptionSpec rows, so new tools get a bar for free.
class OptionsBar final : public QFrame {
    Q_OBJECT

  public:
    explicit OptionsBar(AppState* state, QWidget* parent = nullptr);

    QSize sizeHint() const override;
    // The row wraps when the window is narrow, so the bar is taller the
    // less width it gets; the parent layout needs both hooks.
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;

  signals:
    // Bar buttons are commands; the main window runs them (Select Subject, …).
    void commandTriggered(ToolId tool, const QString& id);

  private:
    void rebuild();
    void applyTheme();
    QWidget* buildWidget(const OptionSpec& spec);
    QWidget* buildWidgetRaw(const OptionSpec& spec);
    void syncOption(const QString& id, const QVariant& value);
    // Badge shows the live brush (dab + preset name) for paint tools, the
    // plain tool icon otherwise. Refreshed on rebuild and option changes.
    void updateBadge();

    AppState* state_;
    FlowLayout* row_ = nullptr;
    QToolButton* toolBadge_ = nullptr;
};

}  // namespace pittore::ui
