#pragma once
// Vector/Pixel/Draw/Color persona tabs: isolated persona model.
//
// Pure QtCore mapping only — no AppState/ToolsPanel/MainWindow dependency, so
// this unit is testable without the widget stack and Pixel behaviour cannot
// regress through it. See vector.md §6 for the full spec.
#include <QString>
#include <QStringList>

#include <vector>

#include "ui/tools/ids/tool_ids.h"

namespace pittore::ui {

// Pixel = photo work (retouch, selections, adjustments, type masks).
// Vector = Designer-persona subset (curves, shapes, fills, text).
// Draw = freehand painting (brushes, eraser, smudge, tone paint, fills).
// Color = grade, balance and scopes (levels/curves/HSL/LUT live, realtime).
// Artboard = document structure and layout (create/size/reorder boards,
//            board properties, per-board export) plus the vector work a
//            layout needs: Pen/Node/Rectangle drawing, Place, Vector Crop,
//            Fill, Transparency and the colour picker. Paint, selection and
//            retouch groups stay hidden, so nothing here can touch pixels.
enum class Persona { Pixel = 0, Vector = 1, Draw = 2, Color = 3, Artboard = 4 };

QString personaName(Persona p);  // "Pixel" / "Vector" / "Draw" / "Color" / "Artboard"
QString personaId(Persona p);    // "pixel" / "vector" / "draw" / "color" / "artboard"
bool personaFromId(const QString& id, Persona* out);

// Group leaders visible in the Vector tab (existing ToolIds only; vector-only
// tools like Node/Contour/Knife arrive as new ToolIds in a later phase).
std::vector<ToolId> vectorVisibleLeaders();
// Group leaders hidden in the Vector tab (= all leaders minus visible).
std::vector<ToolId> vectorHiddenLeaders();
// Vector-exclusive leaders: hidden in Pixel (and Draw) so no vector tool
// ever shows outside the Vector tab.
std::vector<ToolId> pixelHiddenLeaders();
// Group leaders visible in the Draw tab: the paint family plus shared
// utilities (Move/Eyedropper/Gradient/Hand/Zoom also show elsewhere).
std::vector<ToolId> drawVisibleLeaders();
// Group leaders hidden in the Draw tab (= all leaders minus visible).
std::vector<ToolId> drawHiddenLeaders();
// Group leaders visible in the Color tab: grade-only (eyedropper sampling +
// Dodge tone brush + Gradient map/vignette + shared Move/Hand/Zoom). Retouch,
// paint, vector, select and type groups stay hidden so Color reads as
// "grade, don't retouch" (Resolve Color-page logic).
std::vector<ToolId> colorVisibleLeaders();
// Group leaders hidden in the Color tab (= all leaders minus visible).
std::vector<ToolId> colorHiddenLeaders();
// Group leaders visible in the Artboard tab: Move (which hosts the Artboard
// tool), the vector set a layout needs — Pen, Node, Rectangle, Place, Vector
// Crop, Fill, Transparency — the Eyedropper colour picker, and the shared
// Hand/Zoom navigation. Paint, selection and retouch stay hidden, so no tool
// here can alter a layer's pixels.
std::vector<ToolId> artboardVisibleLeaders();
// Group leaders hidden in the Artboard tab (= all leaders minus visible).
std::vector<ToolId> artboardHiddenLeaders();
// ToolsPanel::setHiddenTools payload for a persona.
QStringList hiddenToolsFor(Persona p);

// Panel ids from allPanels(). stroke/appearance join vectorPanels() once those
// panels exist (Phase 2); until then the list is existing panels only.
QStringList vectorPanels();
// Vector-exclusive panels: hidden when leaving Vector.
QStringList vectorOnlyPanels();
// Vector-exclusive panels: hidden when leaving Vector.
QStringList vectorOnlyPanels();
// Hidden on Vector, restored on Pixel when they were visible before.
QStringList pixelOnlyPanels();
// Essentials-equivalent fallback when no Pixel layout was captured yet.
QStringList pixelFallbackPanels();
// Canvas-first paint layout for the Draw tab.
QStringList drawPanels();
// Color-first grade layout for the Color tab (existing panels only,
// scopes-first: histogram/info input, adjustments/properties grade,
// layers/channels context).
QStringList colorPanels();
// Color-exclusive panels: hidden when leaving Color (empty for now —
// histogram/info are shared with Photography; revisit if a vectorscope lands).
QStringList colorOnlyPanels();
// Fallback when entering Color with no captured layout yet.
QStringList colorFallbackPanels();
// Artboard-first layout: the Artboards panel leads, then the board context.
QStringList artboardPanels();
// Artboard-exclusive panels: hidden when leaving Artboard.
QStringList artboardOnlyPanels();
// Fallback when entering Artboard with no captured layout yet.
QStringList artboardFallbackPanels();

}  // namespace pittore::ui
