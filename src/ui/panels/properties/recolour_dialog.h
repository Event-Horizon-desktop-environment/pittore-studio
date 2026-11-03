#pragma once

class QWidget;

namespace pittore::ui {

class AppState;

// Opens (or raises) the Affinity-style floating panel for an adjustment
// layer. Levels/White Balance/HSL get their characteristic visuals; every
// other live adjustment gets labelled slider+spin rows.
void showAdjustmentDialog(AppState* state, int layerIndex, QWidget* parent);

// Kept for the existing Recolour call sites.
void showRecolourDialog(AppState* state, int layerIndex, QWidget* parent);

}  // namespace pittore::ui
