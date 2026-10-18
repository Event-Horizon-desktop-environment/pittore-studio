#pragma once
// Factory for every panel. The registry (panels_registry.cpp) builds the
// Window-menu table from these; panel classes stay file-local to their own TU.
#include <QWidget>

namespace pittore::ui {

class AppState;

QWidget* createLayersPanel(AppState* state, QWidget* parent);
QWidget* createChannelsPanel(AppState* state, QWidget* parent);
QWidget* createPathsPanel(AppState* state, QWidget* parent);
QWidget* createAdjustmentsPanel(AppState* state, QWidget* parent);
QWidget* createPropertiesPanel(AppState* state, QWidget* parent);
QWidget* createColorPanel(AppState* state, QWidget* parent);
QWidget* createSwatchesPanel(AppState* state, QWidget* parent);
// Vector persona panels (own folder: ui/persona). Declared here so the
// registry stays a one-line-per-panel table; implementations never touch it.
QWidget* createStrokePanel(AppState* state, QWidget* parent);
QWidget* createAppearancePanel(AppState* state, QWidget* parent);
QWidget* createHistoryPanel(AppState* state, QWidget* parent);
QWidget* createInfoPanel(AppState* state, QWidget* parent);
QWidget* createNavigatorPanel(AppState* state, QWidget* parent);
QWidget* createHistogramPanel(AppState* state, QWidget* parent);
QWidget* createBrushesPanel(AppState* state, QWidget* parent);
QWidget* createBrushPreviewPanel(AppState* state, QWidget* parent);
QWidget* createCharacterPanel(AppState* state, QWidget* parent);
QWidget* createParagraphPanel(AppState* state, QWidget* parent);
QWidget* createActionsPanel(AppState* state, QWidget* parent);
QWidget* createLibrariesPanel(AppState* state, QWidget* parent);
QWidget* createAiModelsPanel(AppState* state, QWidget* parent);
QWidget* createAlignPanel(AppState* state, QWidget* parent);
QWidget* createTransformPanel(AppState* state, QWidget* parent);
QWidget* createXmlPanel(AppState* state, QWidget* parent);
QWidget* createObjectPropsPanel(AppState* state, QWidget* parent);
QWidget* createFilterEditorPanel(AppState* state, QWidget* parent);
QWidget* createSymbolsPanel(AppState* state, QWidget* parent);
QWidget* createDocPropsPanel(AppState* state, QWidget* parent);
QWidget* createTracePanel(AppState* state, QWidget* parent);
QWidget* createExtensionsPanel(AppState* state, QWidget* parent);
QWidget* createArtboardsPanel(AppState* state, QWidget* parent);

QWidget* createPagesPanel(AppState* state, QWidget* parent);
QWidget* createMarkersPanel(AppState* state, QWidget* parent);
QWidget* createLpePanel(AppState* state, QWidget* parent);

}  // namespace pittore::ui
