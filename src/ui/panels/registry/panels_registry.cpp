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
#include "ui/panels/registry/panel_creators.h"

namespace pittore::ui {

QWidget* makePanelFooter(AppState* state, const QVector<QPair<QString, QString>>& buttons,
                         QWidget* parent, std::function<void(const QString&)> onClick) {
    const ThemeColors c = colorsFor(state->theme());
    auto* footer = new QWidget(parent);
    footer->setFixedHeight(26);
    footer->setStyleSheet(QStringLiteral("background: %1; border-top: 1px solid %2;")
                              .arg(c.chromeAlt.name(), cssColor(c.divider)));
    auto* row = new QHBoxLayout(footer);
    row->setContentsMargins(8, 2, 8, 2);
    row->setSpacing(4);
    row->addStretch(1);
    for (const auto& entry : buttons) {
        QToolButton* button = footerButton(state, entry.first, entry.second, footer);
        const QString id = entry.first;
        QObject::connect(button, &QToolButton::clicked, footer,
                         [onClick, id] { if (onClick) onClick(id); });
        row->addWidget(button);
    }
    row->addStretch(1);
    return footer;
}

const QVector<PanelInfo>& allPanels() {
    static const QVector<PanelInfo> panels = {
        {QStringLiteral("layers"), QObject::tr("Layers"), QStringLiteral("layers"),
         QStringLiteral("right"), true,
         [](AppState* s, QWidget* p) -> QWidget* { return createLayersPanel(s, p); }},
        {QStringLiteral("channels"), QObject::tr("Channels"), QStringLiteral("channels"),
         QStringLiteral("right"), true,
         [](AppState* s, QWidget* p) -> QWidget* { return createChannelsPanel(s, p); }},
        {QStringLiteral("paths"), QObject::tr("Paths"), QStringLiteral("paths"),
         QStringLiteral("right"), true,
         [](AppState* s, QWidget* p) -> QWidget* { return createPathsPanel(s, p); }},
        {QStringLiteral("properties"), QObject::tr("Properties"), QStringLiteral("properties"),
         QStringLiteral("right-secondary"), true,
         [](AppState* s, QWidget* p) -> QWidget* { return createPropertiesPanel(s, p); }},
        {QStringLiteral("adjustments"), QObject::tr("Adjustments"), QStringLiteral("adjustments"),
         QStringLiteral("right-secondary"), true,
         [](AppState* s, QWidget* p) -> QWidget* { return createAdjustmentsPanel(s, p); }},
        {QStringLiteral("color"), QObject::tr("Color"), QStringLiteral("color"),
         QStringLiteral("right-top"), true,
         [](AppState* s, QWidget* p) -> QWidget* { return createColorPanel(s, p); }},
        {QStringLiteral("swatches"), QObject::tr("Swatches"), QStringLiteral("swatches"),
         QStringLiteral("right-top"), true,
         [](AppState* s, QWidget* p) -> QWidget* { return createSwatchesPanel(s, p); }},
        {QStringLiteral("stroke"), QObject::tr("Stroke"), QStringLiteral("stroke"),
         QStringLiteral("right-top"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createStrokePanel(s, p); }},
        {QStringLiteral("appearance"), QObject::tr("Appearance"),
         QStringLiteral("appearance"), QStringLiteral("right-top"), false,
         [](AppState* s, QWidget* p) -> QWidget* {
             return createAppearancePanel(s, p);
         }},
        {QStringLiteral("history"), QObject::tr("History"), QStringLiteral("history"),
         QStringLiteral("right-top"), true,
         [](AppState* s, QWidget* p) -> QWidget* { return createHistoryPanel(s, p); }},
        {QStringLiteral("histogram"), QObject::tr("Histogram"), QStringLiteral("histogram"),
         QStringLiteral("right-top"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createHistogramPanel(s, p); }},
        {QStringLiteral("navigator"), QObject::tr("Navigator"), QStringLiteral("navigator"),
         QStringLiteral("right-top"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createNavigatorPanel(s, p); }},
        {QStringLiteral("info"), QObject::tr("Info"), QStringLiteral("info"),
         QStringLiteral("right-top"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createInfoPanel(s, p); }},
        {QStringLiteral("brushes"), QObject::tr("Brushes"), QStringLiteral("brushes"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createBrushesPanel(s, p); }},
        {QStringLiteral("brushpreview"), QObject::tr("Brush Preview"), QStringLiteral("eye"),
         QStringLiteral("right-secondary"), true,
         [](AppState* s, QWidget* p) -> QWidget* { return createBrushPreviewPanel(s, p); }},
        {QStringLiteral("character"), QObject::tr("Character"), QStringLiteral("character"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createCharacterPanel(s, p); }, true},
        {QStringLiteral("paragraph"), QObject::tr("Paragraph"), QStringLiteral("paragraph"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createParagraphPanel(s, p); }, true},
        {QStringLiteral("actions"), QObject::tr("Actions"), QStringLiteral("actions"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createActionsPanel(s, p); }},
        {QStringLiteral("libraries"), QObject::tr("Libraries"), QStringLiteral("libraries"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createLibrariesPanel(s, p); }},
        {QStringLiteral("aimodels"), QObject::tr("AI Models"), QStringLiteral("sparkle"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createAiModelsPanel(s, p); }},
        {QStringLiteral("align"), QObject::tr("Align & Distribute"), QStringLiteral("align"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createAlignPanel(s, p); }},
        {QStringLiteral("transform"), QObject::tr("Transform"), QStringLiteral("transform"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createTransformPanel(s, p); }},
        {QStringLiteral("xml"), QObject::tr("XML Editor"), QStringLiteral("xml"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createXmlPanel(s, p); }},
        {QStringLiteral("objectprops"), QObject::tr("Object Properties"), QStringLiteral("object"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createObjectPropsPanel(s, p); }},
        {QStringLiteral("filtereditor"), QObject::tr("Filter Editor"), QStringLiteral("filter"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createFilterEditorPanel(s, p); }},
        {QStringLiteral("symbols"), QObject::tr("Symbols & Clones"), QStringLiteral("symbol"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createSymbolsPanel(s, p); }},
        {QStringLiteral("docprops"), QObject::tr("Document Properties"), QStringLiteral("doc"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createDocPropsPanel(s, p); }},
        {QStringLiteral("trace"), QObject::tr("Trace Bitmap"), QStringLiteral("trace"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createTracePanel(s, p); }},
        {QStringLiteral("extensions"), QObject::tr("Extensions"), QStringLiteral("extension"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createExtensionsPanel(s, p); }},
        {QStringLiteral("pages"), QObject::tr("Pages"), QStringLiteral("pages"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createPagesPanel(s, p); }},
        {QStringLiteral("artboards"), QObject::tr("Artboards"), QStringLiteral("pages"),
         QStringLiteral("left"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createArtboardsPanel(s, p); }},
        {QStringLiteral("markers"), QObject::tr("Markers"), QStringLiteral("marker"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createMarkersPanel(s, p); }},
        {QStringLiteral("lpe"), QObject::tr("Path Effects"), QStringLiteral("lpe"),
         QStringLiteral("right-secondary"), false,
         [](AppState* s, QWidget* p) -> QWidget* { return createLpePanel(s, p); }},
    };
    return panels;
}

const PanelInfo* panelInfo(const QString& id) {
    for (const PanelInfo& info : allPanels())
        if (info.id == id) return &info;
    return nullptr;
}

}  // namespace pittore::ui
