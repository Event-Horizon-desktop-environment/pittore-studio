#pragma once

#include <QWidget>

namespace pittore::ui {

class AppState;

// Hue / Saturation / Lightness controls for a Recolour adjustment layer.
// The three sliders carry live colour ramps and are paired with numeric
// fields. "Attach to layer below" toggles the layer's clipping mask so the
// adjustment only grades its base layer instead of the whole stack below it.
QWidget* createRecolourControls(AppState* state, QWidget* parent);

}  // namespace pittore::ui
