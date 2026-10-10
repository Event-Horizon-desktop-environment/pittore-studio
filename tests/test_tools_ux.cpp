// UX proof: the real ToolsPanel strip shows the shape flyout with every
// shape kind, and the Vector filter keeps it while dropping pixel groups.
// Runs headless (QT_QPA_PLATFORM=offscreen).
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileInfo>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QTabBar>
#include <QTabletEvent>
#include <QGridLayout>
#include <QMenu>
#include <QToolButton>

#include <cmath>

#include "test_util.h"
#include "engine/compute/brushes/loaders/loaders.h"
#include "engine/compute/factory.h"
#include "engine/io/zip.h"
#include "ui/app_state.h"
#include "ui/brushes/brush_library.h"
#include "ui/brushes/brush_preview.h"
#include "ui/brushes/bundle_import.h"
#include "ui/canvas/shared/canvas_helpers.h"
#include "ui/options_bar.h"
#include "ui/panels/registry/panel_creators.h"
#include "ui/canvas_view.h"
#include "ui/contextual_task_bar.h"
#include "ui/main_window.h"
#include "ui/panels.h"
#include "ui/persona/persona.h"
#include "ui/persona/persona_manager.h"
#include "ui/persona/persona.h"
#include "ui/persona/persona_manager.h"
#include "ui/persona/vector_node.h"
#include "ui/persona/vector_pen.h"
#include "ui/persona/vector_view.h"
#include "ui/tools_panel.h"

using namespace pittore::ui;

namespace {

// The strip button for a group leader: visible ones only (rebuild() hides
// retired buttons immediately and destroys them via deleteLater; footer
// buttons carry no groupLeader at all).
QToolButton* leaderButton(ToolsPanel& tools, ToolId leader) {
    for (QToolButton* b : tools.findChildren<QToolButton*>()) {
        if (!b->isVisible()) continue;
        const QVariant v = b->property("groupLeader");
        if (v.isValid() && v.toInt() == static_cast<int>(leader)) return b;
    }
    return nullptr;
}

// Flush deleteLater() retirements synchronously (deterministic offscreen).
void flushRetirements() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QStringList flyoutNames(QToolButton* button) {
    QStringList names;
    if (!button) return names;
    // Drive the real path: context-menu event → anchored grid popup, found as
    // a top-level window. Closed right after reading.
    const QPoint local = button->rect().center();
    QContextMenuEvent ev(QContextMenuEvent::Mouse, local,
                         button->mapToGlobal(local));
    QApplication::sendEvent(button, &ev);
    QWidget* popup = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (w->objectName() == QStringLiteral("toolFlyout") && w->isVisible()) {
            popup = w;
            break;
        }
    }
    if (!popup) return names;
    // Compactness proof: the grid spans 2 columns, so a 38-tool flyout stays
    // short enough to sit clear of the rulers.
    if (auto* grid = qobject_cast<QGridLayout*>(popup->layout()))
        CHECK(grid->columnCount() == 2);
    for (QToolButton* b : popup->findChildren<QToolButton*>()) names << b->text();
    popup->hide();
    popup->deleteLater();
    flushRetirements();  // destroys it via deleteLater
    return names;
}

bool listed(const QStringList& names, const QString& want) {
    for (const QString& n : names)
        if (n.contains(want)) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    AppState state;
    ToolsPanel tools(&state);
    tools.show();  // offscreen: makes isVisible() report real strip state

    // 1. The shape group sits on the strip…
    QToolButton* shapes = leaderButton(tools, ToolId::Rectangle);
    CHECK(shapes != nullptr);
    if (!shapes) {
        const int rc = pittore_test::failures() == 0 ? 0 : 1;
        std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                    pittore_test::failures());
        return rc;
    }

    // 2. …and its flyout lists the full set: 24 standard shapes, 12 beyond
    // those, plus two holdovers (Line, Custom Shape).
    const QStringList names = flyoutNames(shapes);
    CHECK(names.size() == 38);
    for (const char* want :
         {"Rectangle", "Ellipse", "Rounded Rectangle", "Triangle", "Diamond",
          "Trapezoid", "Polygon", "Star", "Double Star", "Square Star", "Arrow",
          "Donut", "Pie", "Segment", "Crescent", "Cog", "Cloud",
          "Callout Rounded Rectangle", "Callout Ellipse", "Tear", "Heart",
          "Spiral", "QR Code", "Cat", "Hexagon", "Octagon", "Cross",
          "Right Triangle", "Parallelogram", "Chevron", "Double Arrow",
          "Circular Arrow", "Sparkle", "Shield", "Ticket", "Sun", "Line",
          "Custom Shape"})
        CHECK(listed(names, QString::fromUtf8(want)));

    // 3. Vector persona: the shape group stays, pixel groups go.
    tools.setHiddenTools(hiddenToolsFor(Persona::Vector));
    flushRetirements();
    CHECK(leaderButton(tools, ToolId::Rectangle) != nullptr);
    CHECK(leaderButton(tools, ToolId::RectMarquee) == nullptr);
    CHECK(leaderButton(tools, ToolId::Brush) == nullptr);
    CHECK(leaderButton(tools, ToolId::NodeTool) != nullptr);
    CHECK(leaderButton(tools, ToolId::VectorBrushTool) != nullptr);
    CHECK(leaderButton(tools, ToolId::HorizontalType) != nullptr);

    // 4. Back to Pixel: photo bench returns, vector tools stay out.
    tools.setHiddenTools(hiddenToolsFor(Persona::Pixel));
    flushRetirements();
    CHECK(leaderButton(tools, ToolId::Rectangle) == nullptr);
    CHECK(leaderButton(tools, ToolId::NodeTool) == nullptr);
    CHECK(leaderButton(tools, ToolId::VectorBrushTool) == nullptr);
    CHECK(leaderButton(tools, ToolId::Brush) != nullptr);
    CHECK(leaderButton(tools, ToolId::RectMarquee) != nullptr);
    CHECK(leaderButton(tools, ToolId::CloneStamp) != nullptr);
    // Text is shared, not vector-exclusive: the Type group stays in Pixel.
    CHECK(leaderButton(tools, ToolId::HorizontalType) != nullptr);

    // 5. Every shape survives a real press-drag-release on the canvas: one
    // new art layer each (QR Code honestly refuses — no encoder yet).
    {
        DocumentItem* d = state.addDocument(QStringLiteral("shapedrag"),
                                            QSize(200, 150), 300);
        CHECK(d != nullptr);
        if (!d) {
            std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                        pittore_test::failures());
            return 1;
        }
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        app.processEvents();
        QWidget* vp = canvas->viewport();

        auto sendMouse = [&](QEvent::Type type, const QPointF& pos,
                             Qt::MouseButton button, Qt::MouseButtons buttons,
                             Qt::KeyboardModifiers mods = Qt::NoModifier) {
            const QPointF global = vp->mapToGlobal(pos);
            QMouseEvent e(type, pos, global, button, buttons, mods);
            QApplication::sendEvent(vp, &e);
        };

        const QColor ink(200, 30, 30);
        state.setForeground(ink);
        for (ToolId id :
             {ToolId::Rectangle, ToolId::Ellipse, ToolId::RoundedRectangle,
              ToolId::Triangle, ToolId::Diamond, ToolId::Trapezoid,
              ToolId::Polygon, ToolId::Hexagon, ToolId::Octagon, ToolId::Star,
              ToolId::Sparkle, ToolId::DoubleStar, ToolId::SquareStar,
              ToolId::Arrow, ToolId::DoubleArrow, ToolId::Donut, ToolId::Pie,
              ToolId::Segment, ToolId::Crescent, ToolId::Cog, ToolId::Cloud,
              ToolId::CalloutRect, ToolId::CalloutEllipse, ToolId::Tear,
              ToolId::Heart, ToolId::CustomShape, ToolId::Spiral, ToolId::Line,
              ToolId::Cross, ToolId::RightTriangle, ToolId::Parallelogram,
              ToolId::Chevron, ToolId::CircularArrow, ToolId::Shield,
              ToolId::Ticket, ToolId::Sun, ToolId::Cat}) {
            state.setOption(id, QStringLiteral("stroke"), ink);
            state.setActiveTool(id);
            app.processEvents();
            const int before = d->layers.size();
            const QPointF a = canvas->documentToView(QPointF(20, 20));
            const QPointF b = canvas->documentToView(QPointF(100, 80));
            sendMouse(QEvent::MouseButtonPress, a, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
            app.processEvents();  // overlay preview paints mid-drag
            sendMouse(QEvent::MouseButtonRelease, b, Qt::LeftButton,
                      Qt::NoButton);
            app.processEvents();
            CHECK(d->layers.size() == before + 1);
            if (d->layers.size() != before + 1) continue;
            CHECK(d->layers[0].art && !d->layers[0].art->isEmpty());
            // Landing state: Move tool, new shape selected, handles live.
            CHECK(state.activeTool() == ToolId::Move);
            CHECK(state.selectedLayerIndices().contains(0));
        }
        // QR Code refuses with a hint instead of a layer.
        {
            state.setActiveTool(ToolId::QRCode);
            app.processEvents();
            const int before = d->layers.size();
            const QPointF a = canvas->documentToView(QPointF(20, 20));
            const QPointF b = canvas->documentToView(QPointF(100, 80));
            sendMouse(QEvent::MouseButtonPress, a, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, b, Qt::LeftButton,
                      Qt::NoButton);
            app.processEvents();
            CHECK(d->layers.size() == before);
        }
        // Shift constrains the drag to equal proportions (square bounds).
        {
            state.setActiveTool(ToolId::Rectangle);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(20, 20));
            const QPointF b = canvas->documentToView(QPointF(100, 60));
            sendMouse(QEvent::MouseButtonPress, a, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton,
                      Qt::ShiftModifier);
            sendMouse(QEvent::MouseButtonRelease, b, Qt::LeftButton,
                      Qt::NoButton);
            app.processEvents();
            const QRectF bounds = layerBounds(*d, d->layers[0]);
            CHECK(std::abs(bounds.width() - bounds.height()) <= 2.0);
        }
        // Transform handles: corners scale freeform by default, Shift holds
        // proportions — on the just-created shape, still selected under Move.
        {
            CHECK(state.activeTool() == ToolId::Move);
            const int top = 0;
            const QRectF start = layerBounds(*d, d->layers[top]);
            // Freeform corner drag: axes diverge.
            const QPointF h1 = canvas->documentToView(start.topLeft());
            const QPointF m1 = canvas->documentToView(
                QPointF(start.right() - 0.5 * start.width(),
                        start.bottom() - 0.25 * start.height()));
            sendMouse(QEvent::MouseButtonPress, h1, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(QEvent::MouseMove, m1, Qt::NoButton, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, m1, Qt::LeftButton,
                      Qt::NoButton);
            app.processEvents();
            // Post-release the art re-renders with scale folded to 1 (the
            // vector-direct invariant), so aspect is read off bounds, which
            // the release refresh recomputed from geometry.
            {
                const QRectF b = layerBounds(*d, d->layers[top]);
                CHECK(std::abs(b.width() / b.height() - 1.0) > 0.1);
                CHECK(d->layers[top].scaleX == 1.0);
                CHECK(d->layers[top].scaleY == 1.0);
            }
            // Shift corner drag: axes stay locked.
            state.undo();
            app.processEvents();
            const QPointF h2 = canvas->documentToView(
                layerBounds(*d, d->layers[top]).topLeft());
            const QPointF m2 = canvas->documentToView(QPointF(
                layerBounds(*d, d->layers[top]).right() - 20.0,
                layerBounds(*d, d->layers[top]).bottom() - 10.0));
            sendMouse(QEvent::MouseButtonPress, h2, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(QEvent::MouseMove, m2, Qt::NoButton, Qt::LeftButton,
                      Qt::ShiftModifier);
            sendMouse(QEvent::MouseButtonRelease, m2, Qt::LeftButton,
                      Qt::NoButton);
            app.processEvents();
            // Locked proportions survive the release refresh as a square box.
            {
                const QRectF b = layerBounds(*d, d->layers[top]);
                CHECK(std::abs(b.width() - b.height()) <= 3.0);
            }
            state.undo();
        }
        // Alt draws from the centre: the press point stays the middle.
        {
            state.setActiveTool(ToolId::Ellipse);
            app.processEvents();
            const QPointF pressDoc(60, 50);
            const QPointF a = canvas->documentToView(pressDoc);
            const QPointF b = canvas->documentToView(QPointF(80, 60));
            sendMouse(QEvent::MouseButtonPress, a, Qt::LeftButton,
                      Qt::LeftButton);
            sendMouse(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton,
                      Qt::AltModifier);
            sendMouse(QEvent::MouseButtonRelease, b, Qt::LeftButton,
                      Qt::NoButton);
            app.processEvents();
            const QRectF bounds = layerBounds(*d, d->layers[0]);
            const QPointF centre = bounds.center();
            CHECK(std::abs(centre.x() - pressDoc.x()) <= 2.0);
            CHECK(std::abs(centre.y() - pressDoc.y()) <= 2.0);
            // Stroke bleed may grow the trim box; the intended box is covered.
            CHECK(bounds.contains(QRectF(50, 45, 20, 10)));
        }
        // Full window: cycle every shape tool like the crash report's session
        // (tool switch → strip/options/task-bar rebuilds → deferred deletes).
        {
            DocumentItem* winDoc =
                state.addDocument(QStringLiteral("win"), QSize(200, 150), 300);
            CHECK(winDoc != nullptr);
            if (winDoc) {
                MainWindow win(&state);
                win.resize(1100, 800);
                win.show();
                app.processEvents();
                flushRetirements();
                for (ToolId id :
                     {ToolId::Move, ToolId::Rectangle, ToolId::Polygon,
                      ToolId::Star, ToolId::Pen, ToolId::NodeTool,
                      ToolId::Brush, ToolId::Move, ToolId::Polygon,
                      ToolId::Rectangle}) {
                    state.setActiveTool(id);
                    app.processEvents();
                    flushRetirements();
                    app.processEvents();
                }
                CHECK(state.activeTool() == ToolId::Rectangle);
                // The exact user gesture: Vector persona, open the shape
                // flyout, click Polygon in the grid.
                {
                    auto* manager = win.findChild<PersonaManager*>();
                    CHECK(manager != nullptr);
                    if (manager) {
                        manager->setPersona(Persona::Vector);
                        app.processEvents();
                        flushRetirements();
                        app.processEvents();
                    }
                    ToolsPanel* strip =
                        win.findChild<ToolsPanel*>(QStringLiteral("toolsPanel"));
                    CHECK(strip != nullptr);
                    if (strip) {
                        QToolButton* shapes = nullptr;
                        for (QToolButton* b : strip->findChildren<QToolButton*>()) {
                            if (!b->isVisible()) continue;
                            const QVariant v = b->property("groupLeader");
                            if (v.isValid() &&
                                v.toInt() == static_cast<int>(ToolId::Rectangle)) {
                                shapes = b;
                                break;
                            }
                        }
                        CHECK(shapes != nullptr);
                        if (shapes) {
                            const QPoint local = shapes->rect().center();
                            QContextMenuEvent ev(QContextMenuEvent::Mouse, local,
                                                 shapes->mapToGlobal(local));
                            QApplication::sendEvent(shapes, &ev);
                            app.processEvents();
                            QWidget* popup = nullptr;
                            for (QWidget* w : QApplication::topLevelWidgets()) {
                                if (w->objectName() == QStringLiteral("toolFlyout") &&
                                    w->isVisible()) {
                                    popup = w;
                                    break;
                                }
                            }
                            CHECK(popup != nullptr);
                            if (popup) {
                                QToolButton* poly = nullptr;
                                for (QToolButton* b :
                                     popup->findChildren<QToolButton*>()) {
                                    if (b->text().contains(
                                            QStringLiteral("Polygon"))) {
                                        poly = b;
                                        break;
                                    }
                                }
                                CHECK(poly != nullptr);
                                if (poly) {
                                    const QPoint pl =
                                        poly->rect().center();
                                    const QPoint pg =
                                        poly->mapToGlobal(pl);
                                    QMouseEvent press(
                                        QEvent::MouseButtonPress,
                                        QPointF(pl), QPointF(pg),
                                        Qt::LeftButton, Qt::LeftButton,
                                        Qt::NoModifier);
                                    QApplication::sendEvent(poly, &press);
                                    QMouseEvent release(
                                        QEvent::MouseButtonRelease,
                                        QPointF(pl), QPointF(pg),
                                        Qt::LeftButton, Qt::NoButton,
                                        Qt::NoModifier);
                                    QApplication::sendEvent(poly, &release);
                                    app.processEvents();
                                    flushRetirements();
                                    app.processEvents();
                                }
                            }
                            CHECK(state.activeTool() == ToolId::Polygon);
                        }
                    }
                }
            }
        }
    }

    // 6. Segmented view paint: photo/vector interleave in order, edges crisp.
    {
        DocumentItem* d = state.addDocument(QStringLiteral("vpaint"),
                                            QSize(200, 150), 300);
        if (!d) {
            CHECK(false);
        } else {
            QImage photo(200, 150, QImage::Format_ARGB32);
            photo.fill(QColor(255, 0, 0));
            state.placeImageLayer(photo, QStringLiteral("bg"),
                                  QPointF(100, 75), 1.0);
            state.setForeground(QColor(0, 0, 255));
            CHECK(state.addVectorShapeLayer(ToolId::Rectangle,
                                            QRectF(20, 20, 60, 40),
                                            QStringLiteral("VS")));
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();

            auto grabAt = [&](const QPointF& docPos) {
                const QPointF v = canvas->documentToView(docPos);
                const QImage shot =
                    canvas->viewport()->grab().toImage();
                return shot.pixelColor(qBound(0, qRound(v.x()), shot.width() - 1),
                                       qBound(0, qRound(v.y()), shot.height() - 1));
            };
            // Shape on top: blue centre.
            QColor c = grabAt(QPointF(50, 40));
            CHECK(c.blue() > 200 && c.red() < 100);
            CHECK(c.blue() > 200 && c.red() < 100);
            // Crisp edge at 4x: the blue ramp spans a few screen px, not the
            // ~16 a document-resolution bake would smear across.
            {
                const QPointF mid = canvas->documentToView(QPointF(0, 40));
                const QPointF edge = canvas->documentToView(QPointF(20, 40));
                const QImage shot = canvas->viewport()->grab().toImage();
                const int x0 = qRound(mid.x()), x1 = qRound(edge.x()) + 30;
                int ramp = 0;
                for (int x = x0; x <= qMin(x1, shot.width() - 1); ++x) {
                    const int b = shot.pixelColor(
                        x, qBound(0, qRound(mid.y()), shot.height() - 1)).blue();
                    if (b > 16 && b < 235) ++ramp;
                }
                CHECK(ramp <= 10);
            }
            // Photo above the shape covers it: order is honoured.
            state.setActiveLayerIndex(1);
            CHECK(state.bringSelectedLayersToFront());
            app.processEvents();
            c = grabAt(QPointF(50, 40));
            CHECK(c.red() > 200 && c.blue() < 100);
        }
    }

    // 7. Diagnostic: an existing project laid out as a shape over a
    // background. Skips unless PITTORE_SDF_FIXTURE points at a .psc/.ifp.
    {
        const QString sdf = QString::fromLocal8Bit(qgetenv("PITTORE_SDF_FIXTURE"));
        if (sdf.isEmpty() || !QFileInfo::exists(sdf)) {
            std::printf("SKIP: PITTORE_SDF_FIXTURE not set\n");
        } else {
            DocumentItem* d = state.activeDocument();
            QString error;
            CHECK(state.openProject(sdf, &error));
            d = state.activeDocument();
            CHECK(d != nullptr);
            if (d) {
                QWidget window;
                auto* canvas = new CanvasView(&state, &window);
                window.resize(800, 600);
                canvas->setGeometry(0, 0, 800, 600);
                window.show();
                canvas->zoomToFit();
                app.processEvents();
                const QImage shot = canvas->viewport()->grab().toImage();
                shot.save(QDir::tempPath() + QStringLiteral("/pittore-sdfshot.png"));
                int dark = 0;
                for (int y = 0; y < shot.height(); ++y)
                    for (int x = 0; x < shot.width(); ++x) {
                        const QColor px = shot.pixelColor(x, y);
                        if (px.red() < 100 && px.green() < 100 &&
                            px.blue() < 100)
                            ++dark;
                    }
                std::fprintf(stderr, "DBG sdfshot dark=%d shot=%dx%d\n", dark,
                             shot.width(), shot.height());
                CHECK(dark > 1000);
                // Reorder + canvas-drag on the real file (the reported
                // "can't move up or down" path).
                int rectIdx = -1;
                for (int i = 0; i < d->layers.size(); ++i) {
                    if (d->layers[i].name == QStringLiteral("Rectangle"))
                        rectIdx = i;
                }
                CHECK(rectIdx == 0);
                state.setActiveLayerIndex(rectIdx);
                // Locked Background pins the bottom: moving down under it
                // correctly refuses, moving up from the
                // top is a no-op. Both are correct behavior, not stuck UI.
                CHECK(d->layers[1].locked);
                CHECK(!state.moveSelectedLayersInStack(1));
                CHECK(d->layers[0].name == QStringLiteral("Rectangle"));
                CHECK(!state.moveSelectedLayersInStack(-1));
                CHECK(d->layers[0].name == QStringLiteral("Rectangle"));
                // Canvas Move-drag translates the shape.
                state.setActiveTool(ToolId::Move);
                app.processEvents();
                const QPointF before = d->layers[0].offset;
                const QRectF bounds = layerBounds(*d, d->layers[0]);
                const QPointF a =
                    canvas->documentToView(bounds.center());
                const QPointF b = canvas->documentToView(
                    bounds.center() + QPointF(30, 20));
                const QPointF global = canvas->viewport()->mapToGlobal(
                    a.toPoint());
                QMouseEvent press(QEvent::MouseButtonPress, a, global,
                                 Qt::LeftButton, Qt::LeftButton,
                                 Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &press);
                QMouseEvent move(QEvent::MouseMove, b, global,
                                Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &move);
                QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                                    Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &release);
                app.processEvents();
                CHECK(d->layers[0].offset.x() > before.x());
            }
        }
    }

    // 8. Right-rail layout: two tab groups, Layers leading the top one, and
    // later-shown panels tabify into their group instead of stacking.
    {
        MainWindow win(&state);
        win.resize(1100, 800);
        win.show();
        app.processEvents();
        auto dock = [&](const char* id) {
            return win.findChild<QDockWidget*>(QStringLiteral("dock.") +
                                               QString::fromLatin1(id));
        };
        // Leftmost tab of the tab bar holding the given panel title.
        auto firstTab = [&](const QString& title) -> QString {
            for (QTabBar* bar : win.findChildren<QTabBar*>()) {
                for (int i = 0; i < bar->count(); ++i)
                    if (bar->tabText(i) == title) return bar->tabText(0);
            }
            return QString();
        };
        auto* persona =
            win.findChild<PersonaManager*>(QString(), Qt::FindDirectChildrenOnly);
        CHECK(persona != nullptr);
        // Pixel mode: full Essentials rail — two tab groups, Layers leftmost
        // on top, Channels with it, Adjustments in the bottom group below.
        persona->setPersona(Persona::Pixel);
        app.processEvents();
        QDockWidget* layers = dock("layers");
        QDockWidget* channels = dock("channels");
        QDockWidget* adjustments = dock("adjustments");
        QDockWidget* stroke = dock("stroke");
        CHECK(layers && layers->isVisible());
        CHECK(channels && channels->isVisible());
        CHECK(adjustments && adjustments->isVisible());
        CHECK(win.tabifiedDockWidgets(layers).contains(channels));
        CHECK(!win.tabifiedDockWidgets(layers).contains(adjustments));
        CHECK(win.tabifiedDockWidgets(adjustments).contains(
            dock("properties")) == false);  // properties lives on top
        CHECK(firstTab(QStringLiteral("Layers")) ==
              QStringLiteral("Layers"));
        // Geometry is only live for a tab group's current tab — an
        // obscured tab keeps whatever rect it last had — so surface both
        // docks before comparing their groups' positions.
        if (layers) layers->raise();
        if (adjustments) adjustments->raise();
        app.processEvents();
        CHECK(layers->geometry().center().y() <
              adjustments->geometry().center().y());
        // Vector mode: pixel-only panels hide, Stroke/Appearance join the
        // bottom group — no new strip, Layers still leftmost on top.
        persona->setPersona(Persona::Vector);
        app.processEvents();
        layers = dock("layers");
        channels = dock("channels");
        adjustments = dock("adjustments");
        stroke = dock("stroke");
        auto* appearance = dock("appearance");
        CHECK(layers && layers->isVisible());
        CHECK(channels && !channels->isVisible());
        CHECK(adjustments && !adjustments->isVisible());
        CHECK(stroke && stroke->isVisible());
        CHECK(appearance && appearance->isVisible());
        CHECK(win.tabifiedDockWidgets(dock("color")).contains(stroke));
        CHECK(win.tabifiedDockWidgets(dock("color")).contains(appearance));
        CHECK(firstTab(QStringLiteral("Layers")) ==
              QStringLiteral("Layers"));
        // Same live-geometry rule as above: surface both docks first.
        if (layers) layers->raise();
        if (stroke) stroke->raise();
        app.processEvents();
        CHECK(layers->geometry().center().y() <
              stroke->geometry().center().y());
        // Back to Pixel restores the hidden panels into their groups.
        persona->setPersona(Persona::Pixel);
        app.processEvents();
        CHECK(dock("channels")->isVisible());
        CHECK(dock("adjustments")->isVisible());
        CHECK(win.tabifiedDockWidgets(dock("layers")).contains(
            dock("channels")));
        CHECK(firstTab(QStringLiteral("Layers")) ==
              QStringLiteral("Layers"));
    }

    // 9. Node handle drag headless: press the in-handle tip, drag, release
    // commits one history step and moves the control point.
    {
        state.addDocument(QStringLiteral("nodehandle"), QSize(200, 150), 300);
        state.setForeground(QColor(255, 0, 0));
        PenPath path;
        path.addCorner(QPointF(40, 40));
        path.addSmooth(QPointF(80, 40), QPointF(20, 0));
        path.addCorner(QPointF(120, 40));
        CHECK(state.addVectorPathLayer(path.toSegments(PenMode::Bezier),
                                       ToolId::Pen, QStringLiteral("Pen")));
        DocumentItem* d = state.activeDocument();
        int artAt = -1;
        for (int i = 0; i < d->layers.size(); ++i)
            if (d->layers[i].art && !d->layers[i].art->isEmpty()) artAt = i;
        CHECK(artAt >= 0);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::NodeTool);
        app.processEvents();
        // Node → document transform, mirroring the overlay.
        const LayerItem* l = &d->layers[artAt];
        const QTransform docT =
            QTransform(l->art->matrix[0], l->art->matrix[1], l->art->matrix[2],
                       l->art->matrix[3], l->art->matrix[4],
                       l->art->matrix[5]) *
            QTransform().scale(l->scaleX, l->scaleY) *
            QTransform().translate(l->offset.x(), l->offset.y());
        const NodeHandle h =
            nodeHandleAt(*l->art, QPointF(20, 0), 50.0);
        CHECK(h.anchorSeg == 1 && h.side == NodeHandleSide::In);
        const QPointF a = canvas->documentToView(docT.map(h.pos));
        const QPointF b = a + QPointF(30, 12);
        const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
        const float c2xBefore = l->art->segments[1].c2x;
        QMouseEvent press(QEvent::MouseButtonPress, a, global, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &press);
        QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &move);
        QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &release);
        app.processEvents();
        d = state.activeDocument();
        const LayerItem& moved = d->layers[artAt];
        CHECK(moved.art);
        // Dragged right-and-down in view: the in-handle followed in node x.
        CHECK(std::abs(moved.art->segments[1].c2x - c2xBefore) > 0.5);
        state.undo();  // the drag committed exactly one step
        CHECK(std::abs(state.activeDocument()->layers[artAt].art->segments[1]
                           .c2x -
                       c2xBefore) < 1e-6);
    }

    // 10. StrokeWidth headless drag: press the stroke, drag, release shapes
    // the profile in one undo step; undo restores uniform.
    {
        state.addDocument(QStringLiteral("strokeprof"), QSize(200, 150), 300);
        state.setForeground(QColor(255, 0, 0));
        PenPath path;
        path.addCorner(QPointF(20, 50));
        path.addCorner(QPointF(80, 50));
        CHECK(state.addVectorPathLayer(path.toSegments(PenMode::Bezier),
                                       ToolId::Pen, QStringLiteral("SW")));
        DocumentItem* d = state.activeDocument();
        int artAt = -1;
        for (int i = 0; i < d->layers.size(); ++i)
            if (d->layers[i].art && !d->layers[i].art->isEmpty()) artAt = i;
        CHECK(artAt >= 0);
        CHECK(!d->layers[artAt].art->paint.hasProfile);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::StrokeWidthTool);
        app.processEvents();
        const QPointF a = canvas->documentToView(QPointF(50, 50));
        const QPointF b = a + QPointF(80, 0);
        const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, a, global, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &press);
        QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &move);
        QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &release);
        app.processEvents();
        d = state.activeDocument();
        const LayerItem& shaped = d->layers[artAt];
        CHECK(shaped.art && shaped.art->paint.hasProfile);
        CHECK(!shaped.art->paint.profile.empty());
        state.undo();  // one step for the whole drag
        CHECK(!state.activeDocument()->layers[artAt].art->paint.hasProfile);
    }

    // 11. Tablet pressure: a pressure-ramped stylus stroke lays
    // significantly less ink than the same constant-width mouse stroke,
    // and commits exactly once (no mouse-synthesis double).
    {
        auto countRedInk = [&]() {
            DocumentItem* dd = state.activeDocument();
            int ink = 0;
            for (int y = 0; y < dd->composite.height(); ++y)
                for (int x = 0; x < dd->composite.width(); ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if (c.red() > 150 && c.green() < 100) ++ink;
                }
            return ink;
        };
        state.addDocument(QStringLiteral("mousestroke"), QSize(200, 150), 300);
        state.setForeground(QColor(255, 0, 0));
        state.setOption(ToolId::VectorBrushTool, QStringLiteral("brush_width"), 20);
        state.setOption(ToolId::VectorBrushTool, QStringLiteral("controller"), 0);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::VectorBrushTool);
        app.processEvents();
        const QPointF a = canvas->documentToView(QPointF(20, 40));
        const QPointF b = canvas->documentToView(QPointF(80, 40));
        const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
        {
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
        }
        const int mouseInk = countRedInk();
        CHECK(mouseInk > 100);

        // Same geometry via synthetic stylus, pressure ramping 0.1 → 1.0.
        state.addDocument(QStringLiteral("tabletstroke"), QSize(200, 150), 300);
        state.setForeground(QColor(255, 0, 0));
        state.setOption(ToolId::VectorBrushTool, QStringLiteral("brush_width"), 20);
        state.setOption(ToolId::VectorBrushTool, QStringLiteral("controller"), 2);
        auto* canvas2 = new CanvasView(&state, &window);
        canvas2->setGeometry(0, 0, 800, 600);
        canvas2->zoomToFit();
        canvas2->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::VectorBrushTool);
        app.processEvents();
        const QPointF a2 = canvas2->documentToView(QPointF(20, 40));
        const QPointF b2 = canvas2->documentToView(QPointF(80, 40));
        const QPointF global2 = canvas2->viewport()->mapToGlobal(a2.toPoint());
        DocumentItem* dt = state.activeDocument();
        const int layersBefore = (int)dt->layers.size();
        auto tabletAt = [&](QEvent::Type type, const QPointF& p, double pressure,
                            Qt::MouseButtons buttons) {
            // QTabletEvent dereferences its device; app runs carry the real
            // stylus device, headless runs use a stand-in.
            static QPointingDevice stylus;
            QTabletEvent e(type, &stylus, p, global2, pressure, 0, 0, 0.0, 0.0,
                           0, Qt::NoModifier, Qt::LeftButton, buttons);
            QApplication::sendEvent(canvas2->viewport(), &e);
        };
        tabletAt(QEvent::TabletPress, a2, 0.1, Qt::LeftButton);
        for (int i = 1; i <= 10; ++i) {
            const QPointF p = a2 + (b2 - a2) * (i / 10.0);
            tabletAt(QEvent::TabletMove, p, 0.1 + 0.9 * (i / 10.0),
                     Qt::LeftButton);
        }
        tabletAt(QEvent::TabletRelease, b2, 1.0, Qt::NoButton);
        app.processEvents();
        dt = state.activeDocument();
        CHECK((int)dt->layers.size() == layersBefore + 1);  // committed once
        const int tabletInk = countRedInk();
        CHECK(tabletInk > 0);
        CHECK(tabletInk < mouseInk * 0.8);  // ramp averages ~0.55× width
        state.setOption(ToolId::VectorBrushTool, QStringLiteral("controller"), 0);
    }

    // 12. Pixel brush pressure: a ramped stylus stroke lays much less ink
    // than the same constant mouse stroke (size + opacity both respond);
    // opting out of both gates reproduces the mouse stroke exactly.
    {
        auto countDarkInk = [&]() {
            DocumentItem* dd = state.activeDocument();
            int ink = 0;
            for (int y = 0; y < dd->composite.height(); ++y)
                for (int x = 0; x < dd->composite.width(); ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if ((c.red() + c.green() + c.blue()) / 3 < 128) ++ink;
                }
            return ink;
        };
        auto strokeWith = [&](CanvasView* canvas, bool tablet) {
            const QPointF a = canvas->documentToView(QPointF(20, 75));
            const QPointF b = canvas->documentToView(QPointF(80, 75));
            const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
            if (!tablet) {
                QMouseEvent press(QEvent::MouseButtonPress, a, global,
                                 Qt::LeftButton, Qt::LeftButton,
                                 Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &press);
                QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                                Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &move);
                QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                                   Qt::LeftButton, Qt::NoButton,
                                   Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &release);
            } else {
                auto tabletAt = [&](QEvent::Type type, const QPointF& p,
                                    double pressure, Qt::MouseButtons buttons) {
                    static QPointingDevice stylus;
                    QTabletEvent e(type, &stylus, p, global, pressure, 0, 0,
                                   0.0, 0.0, 0, Qt::NoModifier, Qt::LeftButton,
                                   buttons);
                    QApplication::sendEvent(canvas->viewport(), &e);
                };
                tabletAt(QEvent::TabletPress, a, 0.1, Qt::LeftButton);
                for (int i = 1; i <= 10; ++i) {
                    const QPointF p = a + (b - a) * (i / 10.0);
                    tabletAt(QEvent::TabletMove, p, 0.1 + 0.9 * (i / 10.0),
                             Qt::LeftButton);
                }
                tabletAt(QEvent::TabletRelease, b, 1.0, Qt::NoButton);
            }
            app.processEvents();
        };
        state.addDocument(QStringLiteral("pixelmouse"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::Brush);
        app.processEvents();
        strokeWith(canvas, false);
        const int mouseInk = countDarkInk();
        CHECK(mouseInk > 100);

        state.addDocument(QStringLiteral("pixeltablet"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        auto* canvas2 = new CanvasView(&state, &window);
        canvas2->setGeometry(0, 0, 800, 600);
        canvas2->zoomToFit();
        canvas2->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::Brush);
        app.processEvents();
        DocumentItem* dt = state.activeDocument();
        const int layersBefore = (int)dt->layers.size();
        strokeWith(canvas2, true);
        dt = state.activeDocument();
        CHECK((int)dt->layers.size() == layersBefore);  // paints in place
        const int tabletInk = countDarkInk();
        CHECK(tabletInk > 0);
        CHECK(tabletInk < mouseInk * 0.6);
        // Both gates off: the stylus reproduces the mouse stroke exactly
        // (same document, so the reference is airtight).
        state.undo();  // drop the tablet stroke
        strokeWith(canvas2, false);
        const int mouseInk2 = countDarkInk();
        CHECK(mouseInk2 > 100);
        state.undo();
        state.setOption(ToolId::Brush, QStringLiteral("pressure_size"), false);
        state.setOption(ToolId::Brush, QStringLiteral("pressure_opacity"), false);
        strokeWith(canvas2, true);
        CHECK(countDarkInk() == mouseInk2);
        state.setOption(ToolId::Brush, QStringLiteral("pressure_size"), true);
        state.setOption(ToolId::Brush, QStringLiteral("pressure_opacity"), true);
    }

    // 13. Stamp tip end-to-end: a registered solid-square stamp inks through    // the same mouse gesture; a dangling stamp id falls back to the auto tip
    // (stroke still inks, never silently vanishes).
    {
        state.addDocument(QStringLiteral("stampstroke"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        pittore::compute::StampTip tip;
        tip.w = 4;
        tip.h = 4;
        tip.alpha.assign(16, 1.0f);
        tip.spacingPct = 15.0f;
        state.setBrushStamp(QStringLiteral("test-square"), tip);
        CHECK(state.brushStamp(QStringLiteral("test-square")) != nullptr);
        CHECK(state.brushStamp(QStringLiteral("nope")) == nullptr);
        state.setOption(ToolId::Brush, QStringLiteral("brush_stamp"),
                        QStringLiteral("test-square"));
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::Brush);
        app.processEvents();
        const QPointF a = canvas->documentToView(QPointF(20, 75));
        const QPointF b = canvas->documentToView(QPointF(80, 75));
        const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
        auto mouseStroke = [&]() {
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
        };
        auto ink = [&]() {
            DocumentItem* dd = state.activeDocument();
            int n = 0;
            for (int y = 0; y < dd->composite.height(); ++y)
                for (int x = 0; x < dd->composite.width(); ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    // Opaque-dark only: erased (transparent) pixels read as
                    // (0,0,0,0) and must not count as ink.
                    if (c.alpha() > 128 &&
                        (c.red() + c.green() + c.blue()) / 3 < 128)
                        ++n;
                }
            return n;
        };
        mouseStroke();
        CHECK(ink() > 100);
        // Dangling id: falls back to auto tip, still inks.
        state.setOption(ToolId::Brush, QStringLiteral("brush_stamp"),
                        QStringLiteral("missing-file"));
        state.undo();
        mouseStroke();
        CHECK(ink() > 100);
        // Eraser with a stamp removes what the stamp painted.
        state.setOption(ToolId::Brush, QStringLiteral("brush_stamp"),
                        QStringLiteral("test-square"));
        state.undo();
        mouseStroke();
        const int painted = ink();
        CHECK(painted > 100);
        state.setActiveTool(ToolId::Eraser);
        state.setOption(ToolId::Eraser, QStringLiteral("brush_stamp"),
                        QStringLiteral("test-square"));
        state.setOption(ToolId::Eraser, QStringLiteral("opacity"), 100);
        app.processEvents();
        mouseStroke();
        CHECK(ink() < painted);
        state.setOption(ToolId::Brush, QStringLiteral("brush_stamp"), QString());
        state.setOption(ToolId::Eraser, QStringLiteral("brush_stamp"), QString());
        state.setActiveTool(ToolId::Brush);
    }

    // 13b. Spacing stays live for stamps: the preset seeds the option, the
    // slider drives the stride (wide spacing = fewer dabs = less ink).
    {
        state.addDocument(QStringLiteral("stampspacing"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 10);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        state.setOption(ToolId::Brush, QStringLiteral("brush_stamp"),
                        QStringLiteral("test-square"));
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::Brush);
        app.processEvents();
        auto strokeLine = [&]() {
            const QPointF a = canvas->documentToView(QPointF(20, 75));
            const QPointF b = canvas->documentToView(QPointF(120, 75));
            const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
        };
        auto darkInk = [&]() {
            DocumentItem* dd = state.activeDocument();
            int n = 0;
            for (int y = 0; y < dd->composite.height(); ++y)
                for (int x = 0; x < dd->composite.width(); ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if (c.alpha() > 128 &&
                        (c.red() + c.green() + c.blue()) / 3 < 128)
                        ++n;
                }
            return n;
        };
        state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"), 15);
        strokeLine();
        const int dense = darkInk();
        CHECK(dense > 100);
        state.undo();
        state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"), 200);
        strokeLine();
        const int sparse = darkInk();
        CHECK(sparse > 0);
        CHECK(sparse < dense / 2);
        state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"), 15);
        state.setOption(ToolId::Brush, QStringLiteral("brush_stamp"), QString());
    }

    // 14. Preset-bundle import (synthetic in-memory .bundle): auto preset
    // maps tip params + gates, predefined GBR resolves to a stamp, smudge
    // flags approximate, unreferenced tips surface as loose tips.
    {
        namespace bi = pittore::ui::bundleimport;
        auto kppPng = [](const QString& xml) {
            QImage icon(32, 32, QImage::Format_ARGB32);
            icon.fill(Qt::white);
            icon.setText(QStringLiteral("preset"), xml);
            QByteArray out;
            QBuffer buf(&out);
            buf.open(QIODevice::WriteOnly);
            QImageWriter w(&buf, "PNG");
            CHECK(w.write(icon));
            return out;
        };
        const QString autoXml =
            QStringLiteral("<Preset paintopid=\"paintbrush\" name=\"AutoT\">"
                           "<resources></resources>"
                           "<param type=\"internal\" name=\"OpacityValue\">0.8</param>"
                           "<param type=\"internal\" name=\"SizeUseCurve\">false</param>"
                           "<param type=\"internal\" name=\"OpacityUseCurve\">true</param>"
                           "<param type=\"string\" name=\"OpacitySensor\">"
                           "<![CDATA[<params id=\"pressure\">"
                           "<curve>0,0;0.144578,0.0481932;1,1;</curve>"
                           "</params>]]></param>"
                           "<param type=\"internal\" name=\"ScatterUseCurve\">true</param>"
                           "<param type=\"string\" name=\"ScatterSensor\">"
                           "<![CDATA[<params id=\"pressure\"><curve>0,0;1,1;</curve>"
                           "</params>]]></param>"
                           "<param type=\"internal\" name=\"ScatterValue\">0.5</param>"
                           "<param type=\"internal\" name=\"Scattering/AxisX\">true</param>"
                           "<param type=\"internal\" name=\"Scattering/AxisY\">false</param>"
                           "<param type=\"internal\" name=\"PaintOpSettings/isAirbrushing\">"
                           "true</param>"
                           "<param type=\"internal\" name=\"PaintOpSettings/rate\">30</param>"
                           "<param type=\"internal\" name=\"Texture/Pattern/Enabled\">"
                           "true</param>"
                           "<param type=\"internal\" name=\"Texture/Pattern/PatternFileName\">"
                           "loose.png</param>"
                           "<param type=\"internal\" name=\"Texture/Strength/Value\">"
                           "0.9</param>"
                           "<param type=\"string\" name=\"brush_definition\">"
                           "<![CDATA[<Brush type=\"auto_brush\" angle=\"1.5708\" "
                           "spacing=\"0.2\" density=\"0.5\">"
                           "<MaskGenerator type=\"rect\" diameter=\"20\" ratio=\"0.5\" "
                           "hfade=\"0.3\" vfade=\"0.3\"/></Brush>]]></param>"
                           "</Preset>");
        const QString stampXml =
            QStringLiteral("<Preset paintopid=\"paintbrush\" name=\"StampT\">"
                           "<resources></resources>"
                           "<param type=\"string\" name=\"brush_definition\">"
                           "<![CDATA[<Brush type=\"gbr_brush\" filename=\"tip.gbr\" "
                           "angle=\"0\" spacing=\"0.15\" "
                           "ColorAsMask=\"1\"/>]]></param></Preset>");
        const QString smudgeXml =
            QStringLiteral("<Preset paintopid=\"colorsmudge\" name=\"SmT\">"
                           "<resources></resources>"
                           "<param type=\"internal\" name=\"SmudgeRateValue\">0.76</param>"
                           "<param type=\"internal\" name=\"SmudgeRadiusValue\">0.97</param>"
                           "<param type=\"string\" name=\"brush_definition\">"
                           "<![CDATA[<Brush type=\"auto_brush\" angle=\"0\" "
                           "spacing=\"0.1\">"
                           "<MaskGenerator type=\"circle\" diameter=\"30\" ratio=\"1\" "
                           "hfade=\"0.5\" vfade=\"0.5\"/></Brush>]]></param>"
                           "</Preset>");
        // Rotation + color source + grain mode/cutoff on an auto tip.
        const QString rotXml =
            QStringLiteral("<Preset paintopid=\"paintbrush\" name=\"RotT\">"
                           "<resources></resources>"
                           "<param type=\"internal\" name=\"RotationUseCurve\">true</param>"
                           "<param type=\"string\" name=\"RotationSensor\">"
                           "<![CDATA[<!DOCTYPE params> <params id=\"drawingangle\"/>]]></param>"
                           "<param type=\"internal\" name=\"ColorOption/hue\">true</param>"
                           "<param type=\"internal\" name=\"HorizontalMirrorEnabled\">true</param>"
                           "<param type=\"internal\" name=\"CompositeOp\">multiply</param>"                           "<param type=\"internal\" name=\"Texture/Pattern/Enabled\">true</param>"
                           "<param type=\"internal\" name=\"Texture/Pattern/PatternFileName\">"
                           "loose.png</param>"
                           "<param type=\"internal\" name=\"Texture/Pattern/TexturingMode\">1</param>"
                           "<param type=\"internal\" name=\"Texture/Pattern/CutoffPolicy\">1</param>"
                           "<param type=\"internal\" name=\"Texture/Pattern/CutoffLeft\">64</param>"
                           "<param type=\"internal\" name=\"Texture/Pattern/CutoffRight\">192</param>"
                           "<param type=\"string\" name=\"brush_definition\">"
                           "<![CDATA[<Brush type=\"auto_brush\" angle=\"0\" "
                           "spacing=\"0.1\">"
                           "<MaskGenerator type=\"circle\" diameter=\"30\" ratio=\"1\" "
                           "hfade=\"0.5\" vfade=\"0.5\"/></Brush>]]></param>"
                           "</Preset>");
        // Masked twin with an auto tip: noted, not transferred.
        const QString maskXml =
            QStringLiteral("<Preset paintopid=\"paintbrush\" name=\"MaskT\">"
                           "<resources></resources>"
                           "<param type=\"internal\" name=\"MaskingBrush/Enabled\">true</param>"
                           "<param type=\"internal\" name=\"SizeUseCurve\">true</param>"
                           "<param type=\"string\" name=\"SizeSensor\">"
                           "<![CDATA[<params id=\"tiltsensor\">"
                           "<curve>0,0;1,1;</curve>"
                           "</params>]]></param>"
                           "<param type=\"internal\" name=\"SmoothingValue\">0.5</param>"
                           "<param type=\"string\" name=\"MaskingBrush/Preset/brush_definition\">"
                           "<![CDATA[<Brush type=\"auto_brush\" angle=\"0\" "
                           "spacing=\"0.1\">"
                           "<MaskGenerator type=\"circle\" diameter=\"20\" ratio=\"1\" "
                           "hfade=\"0.5\" vfade=\"0.5\"/></Brush>]]></param>"
                           "<param type=\"string\" name=\"brush_definition\">"
                           "<![CDATA[<Brush type=\"auto_brush\" angle=\"0\" "
                           "spacing=\"0.1\">"
                           "<MaskGenerator type=\"circle\" diameter=\"30\" ratio=\"1\" "
                           "hfade=\"0.5\" vfade=\"0.5\"/></Brush>]]></param>"
                           "</Preset>");
        // Minimal 4x4 gray GBR.
        QByteArray gbr;
        {
            auto be32 = [&](std::uint32_t x) {
                gbr.append(char(x >> 24));
                gbr.append(char(x >> 16));
                gbr.append(char(x >> 8));
                gbr.append(char(x));
            };
            be32(28 + 2);
            be32(2);
            be32(4);
            be32(4);
            be32(1);
            gbr.append("GIMP", 4);
            be32(25);
            gbr.append("t", 1);
            gbr.append('\0');
            for (int i = 0; i < 16; ++i) gbr.append(char(200));
        }
        QImage loose(8, 8, QImage::Format_Grayscale8);
        loose.fill(128);
        QByteArray loosePng;
        {
            QBuffer buf(&loosePng);
            buf.open(QIODevice::WriteOnly);
            QImageWriter w(&buf, "PNG");
            CHECK(w.write(loose));
        }
        std::vector<pittore::io::ZipEntry> entries;
        auto addEntry = [&](const char* name, const QByteArray& bytes) {
            pittore::io::ZipEntry e;
            e.name = name;
            e.data.assign(bytes.begin(), bytes.end());
            entries.push_back(std::move(e));
        };
        addEntry("paintoppresets/a.kpp", kppPng(autoXml));
        addEntry("paintoppresets/b.kpp", kppPng(stampXml));
        addEntry("paintoppresets/c.kpp", kppPng(smudgeXml));
        addEntry("paintoppresets/d.kpp", kppPng(rotXml));
        addEntry("paintoppresets/e.kpp", kppPng(maskXml));
        addEntry("brushes/tip.gbr", gbr);
        addEntry("brushes/loose.png", loosePng);
        // Bundle manifest with grouping tags for two presets.
        addEntry("META-INF/manifest.xml",
                 QByteArray("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                            "<manifest:manifest xmlns:manifest=\"urn:x\">"
                            "<manifest:file-entry manifest:full-path=\"/\"/>"
                            "<manifest:file-entry manifest:media-type=\"x\" "
                            "manifest:full-path=\"paintoppresets/a.kpp\">"
                            "<manifest:tags><manifest:tag>SK2</manifest:tag>"
                            "<manifest:tag>Ink</manifest:tag></manifest:tags>"
                            "</manifest:file-entry>"
                            "<manifest:file-entry manifest:media-type=\"x\" "
                            "manifest:full-path=\"paintoppresets/b.kpp\"/>"
                            "</manifest:manifest>"));
        auto packed = pittore::io::zipWrite(entries);
        CHECK(packed.has_value());
        auto unpacked = pittore::io::zipRead(*packed);
        CHECK(unpacked.has_value());
        bi::BundleResult r = bi::importBundleEntries(*unpacked);
        CHECK(r.presets.size() == 5);
        CHECK(r.skippedFiles == 0);
        const bi::BundlePreset& pa = r.presets[0];
        CHECK(pa.name == QStringLiteral("AutoT"));
        CHECK(!pa.isStamp);
        CHECK_NEAR(pa.size, 20.0, 1e-9);
        CHECK_NEAR(pa.roundness, 50.0, 1e-9);
        CHECK(pa.squareTip);
        CHECK_NEAR(pa.hardness, 70.0, 1e-9);
        CHECK_NEAR(pa.angleDeg, 90.0, 1e-3);
        CHECK_NEAR(pa.spacingPct, 20.0, 1e-9);
        CHECK_NEAR(pa.opacity, 80.0, 1e-9);
        CHECK(!pa.pressureSize);
        CHECK(pa.pressureOpacity);
        CHECK(pa.targetTool == QStringLiteral("Brush"));
        CHECK(pa.opacityCurve ==
              QStringLiteral("0.8|0,0;0.144578,0.0481932;1,1;"));
        CHECK(pa.sizeCurve.isEmpty());  // disabled curve transfers nothing
        CHECK_NEAR(pa.scatterPct, 50.0, 1e-9);
        CHECK(pa.scatterX);
        CHECK(!pa.scatterY);
        CHECK_NEAR(pa.densityPct, 50.0, 1e-9);
        CHECK(pa.airbrush);
        CHECK_NEAR(pa.airbrushRate, 30.0, 1e-9);
        CHECK(pa.hasTexture);
        CHECK_NEAR(pa.textureStrength, 90.0, 1e-9);
        CHECK(!pa.texturePng.isEmpty());
        const bi::BundlePreset& pb = r.presets[1];
        CHECK(pb.isStamp);
        CHECK(pb.stampExt == QStringLiteral("gbr"));
        CHECK(pb.stampMode == 0);
        CHECK_NEAR(pb.spacingPct, 15.0, 1e-9);
        CHECK(!pb.stampPng.isEmpty());
        const bi::BundlePreset& pc = r.presets[2];
        CHECK(pc.isSmudge);
        CHECK_NEAR(pc.smudgeRate, 76.0, 1e-9);
        CHECK_NEAR(pc.smudgeRadius, 97.0, 1e-9);
        CHECK(pc.approximateNotes.isEmpty());
        CHECK(r.looseTips.size() == 1);
        CHECK(r.looseTips[0].first == QStringLiteral("loose.png"));
        // Manifest tags land on their presets (a has SK2+Ink, b has none).
        CHECK(r.presets[0].tags.contains(QStringLiteral("SK2")));
        CHECK(r.presets[0].tags.contains(QStringLiteral("Ink")));
        CHECK(r.presets[1].tags.isEmpty());
        // Rotation drawing-angle + hue source + grain mode/cutoff.
        const bi::BundlePreset& pd = r.presets[3];
        CHECK(pd.rotationMode == 2);
        CHECK(pd.sourceMode == 1);
        CHECK(pd.hasTexture);
        CHECK(pd.textureMode == 1);
        CHECK(pd.textureCutoffPolicy == 1);
        CHECK_NEAR(pd.textureCutLo, 64.0 / 255.0, 1e-9);
        CHECK_NEAR(pd.textureCutHi, 192.0 / 255.0, 1e-9);
        CHECK(pd.flipX);
        CHECK(!pd.flipY);
        CHECK(!pd.approximateNotes.isEmpty());  // blend mode noted
        // Masked auto twin: noted, nothing stored. The tilt-driven size
        // curve transfers as a lean sensor drive with its authored shape
        // (pressure gate off, no dormant pressure curve); the smoothing
        // value lands on the stabilizer strength.
        const bi::BundlePreset& pe = r.presets[4];
        CHECK(!pe.hasMask);
        CHECK(!pe.approximateNotes.isEmpty());
        CHECK(!pe.pressureSize);
        CHECK_NEAR(pe.tiltSize, 0.0, 1e-9);
        CHECK(pe.drives.size() == 1);
        CHECK(pe.drives[0].prop == "size");
        CHECK(pe.drives[0].sensor ==
              pittore::ui::sensordrive::Sensor::Lean);
        CHECK_NEAR(pe.drives[0].amount, 100.0, 1e-9);
        CHECK(pe.drives[0].curve == "1|0,0;1,1;");
        CHECK(pe.sizeCurve.isEmpty());
        CHECK_NEAR(pe.smoothing, 50.0, 1e-9);
        for (const QString& note : pe.approximateNotes)
            CHECK(!note.contains(QStringLiteral("tilt size")));
        // Garbage bundle fails cleanly.
        std::vector<std::uint8_t> junk{1, 2, 3, 4};
        CHECK(!pittore::io::zipRead(junk).has_value());
    }

    // 20b. Tilt/wheel sensors with explicit curves become sensor drives
    // (exact shape, no approximation notes); bare mentions keep the
    // linear fallback + note.
    {
        namespace bi = pittore::ui::bundleimport;
        auto kppPng = [](const QString& xml) {
            QImage icon(32, 32, QImage::Format_ARGB32);
            icon.fill(Qt::white);
            icon.setText(QStringLiteral("preset"), xml);
            QByteArray out;
            QBuffer buf(&out);
            buf.open(QIODevice::WriteOnly);
            QImageWriter w(&buf, "PNG");
            CHECK(w.write(icon));
            return out;
        };
        const QMap<QString, QByteArray> noFiles;
        bool ok = false;
        const QString tiltXml =
            QStringLiteral("<Preset paintopid=\"paintbrush\" name=\"TiltOpT\">"
                           "<resources></resources>"
                           "<param type=\"internal\" name=\"OpacityUseCurve\">true</param>"
                           "<param type=\"string\" name=\"OpacitySensor\">"
                           "<![CDATA[<params id=\"tiltelevation\">"
                           "<curve>0,1;1,0;</curve>"
                           "</params>]]></param>"
                           "<param type=\"internal\" name=\"FlowUseCurve\">true</param>"
                           "<param type=\"string\" name=\"FlowSensor\">"
                           "<![CDATA[<params id=\"tangential\">"
                           "<curve>0,0;1,1;</curve>"
                           "</params>]]></param>"
                           "</Preset>");
        const bi::BundlePreset pt =
            bi::parseKpp(QStringLiteral("t.png"), kppPng(tiltXml), noFiles,
                         &ok);
        CHECK(ok);
        CHECK(pt.drives.size() == 2);
        CHECK(pt.drives[0].prop == "opacity");
        CHECK(pt.drives[0].sensor ==
              pittore::ui::sensordrive::Sensor::Lean);
        CHECK_NEAR(pt.drives[0].amount, -100.0, 1e-9);
        CHECK(pt.drives[0].curve == "1|0,1;1,0;");
        CHECK(pt.drives[1].prop == "flow");
        CHECK(pt.drives[1].sensor ==
              pittore::ui::sensordrive::Sensor::Tangential);
        CHECK_NEAR(pt.drives[1].amount, 100.0, 1e-9);
        CHECK(pt.opacityCurve.isEmpty());
        CHECK(!pt.tangentialFlow);
        CHECK_NEAR(pt.tiltOpacity, 0.0, 1e-9);
        for (const QString& note : pt.approximateNotes) {
            CHECK(!note.contains(QStringLiteral("tilt")));
            CHECK(!note.contains(QStringLiteral("wheel")));
        }
        // Bare tilt mention (no curve): linear fallback + note, no drive.
        const QString bareXml =
            QStringLiteral("<Preset paintopid=\"paintbrush\" name=\"BareT\">"
                           "<resources></resources>"
                           "<param type=\"internal\" name=\"SizeUseCurve\">true</param>"
                           "<param type=\"string\" name=\"SizeSensor\">"
                           "<![CDATA[<params id=\"tiltsensor\"/>]]></param>"
                           "</Preset>");
        const bi::BundlePreset pb =
            bi::parseKpp(QStringLiteral("b.png"), kppPng(bareXml), noFiles,
                         &ok);
        CHECK(ok);
        CHECK(pb.drives.empty());
        CHECK_NEAR(pb.tiltSize, 100.0, 1e-9);
        bool foundTiltNote = false;
        for (const QString& note : pb.approximateNotes)
            if (note.contains(QStringLiteral("tilt size"))) foundTiltNote = true;
        CHECK(foundTiltNote);
    }

    // 21. Pressure-curve parse + evaluation.
    {
        namespace bc = pittore::ui::brushcurve;
        bc::Curve empty = bc::parseEncoded(QString());
        CHECK(!empty.has());
        bc::Curve c = bc::parseEncoded(QStringLiteral("1|0,0;0.5,0.25;1,1"));
        CHECK(c.has());
        CHECK_NEAR(c.value, 1.0, 1e-9);
        CHECK_NEAR(bc::eval(c, 0.0), 0.0, 1e-9);
        CHECK_NEAR(bc::eval(c, 0.25), 0.125, 1e-9);
        CHECK_NEAR(bc::eval(c, 0.75), 0.625, 1e-9);
        CHECK_NEAR(bc::eval(c, 1.0), 1.0, 1e-9);
        CHECK_NEAR(bc::eval(c, -1.0), 0.0, 1e-9);  // clamped ends
        CHECK_NEAR(bc::eval(c, 2.0), 1.0, 1e-9);
        bc::Curve v = bc::parseEncoded(QStringLiteral("0.5|0,0;1,1"));
        CHECK_NEAR(bc::eval(v, 1.0), 0.5, 1e-9);  // value scales
        bc::Curve junk =
            bc::parseEncoded(QStringLiteral("abc|junk;1,2,3;0.5,x"));
        CHECK(!junk.has());  // garbage drops out, never crashes
    }

    // 15. Brush preview renderer: dab thumbs and pressure-ramp strokes.
    {
        namespace bp = pittore::ui::brushpreview;
        auto dark = [](const QImage& img, int x, int y) {
            return qGray(img.pixel(x, y)) < 128;
        };
        auto light = [](const QImage& img, int x, int y) {
            return qGray(img.pixel(x, y)) > 200;
        };
        bp::AutoParams a;
        const QImage thumb = bp::tipThumbAuto(a);
        CHECK(thumb.width() == 56 && thumb.height() == 56);
        CHECK(dark(thumb, 28, 28));   // dab core
        CHECK(light(thumb, 2, 2));    // checker corner
        // Square fills the diagonal the round tip leaves empty.
        bp::AutoParams sq = a;
        sq.square = true;
        sq.hardness = 1.0;
        const QImage roundHard = bp::tipThumbAuto(a);
        const QImage sqThumb = bp::tipThumbAuto(sq);
        CHECK(light(roundHard, 12, 12));
        CHECK(dark(sqThumb, 12, 12));
        // Stroke: the swollen middle lays more ink than the faint start.
        const QImage stroke = bp::strokePreviewAuto(a, Qt::black);
        CHECK(stroke.width() == 300 && stroke.height() == 110);
        auto inkIn = [&](int x0, int x1) {
            int n = 0;
            for (int y = 0; y < stroke.height(); ++y)
                for (int x = x0; x < x1; ++x)
                    if (qGray(stroke.pixel(x, y)) < 128) ++n;
            return n;
        };
        CHECK(inkIn(120, 180) > inkIn(8, 30) * 2);
        // Stamp stroke renders its tip ink.
        pittore::compute::StampTip tip;
        tip.w = 4;
        tip.h = 4;
        tip.alpha.assign(16, 1.0f);
        const QImage sThumb = bp::tipThumbStamp(tip, 0);
        CHECK(dark(sThumb, 28, 28));
        const QImage sStroke =
            bp::strokePreviewStamp(tip, 0, Qt::black, 0.0);
        int sInk = 0;
        for (int y = 0; y < sStroke.height(); ++y)
            for (int x = 0; x < sStroke.width(); ++x)
                if (qGray(sStroke.pixel(x, y)) < 128) ++sInk;
        CHECK(sInk > 200);
        // Hose preview cycles its cells.
        pittore::compute::StampTip emptyTip;
        emptyTip.w = 4;
        emptyTip.h = 4;
        emptyTip.alpha.assign(16, 0.0f);
        const QImage hStroke = bp::strokePreviewHose(
            {tip, emptyTip}, QStringLiteral("incremental"), 0, 0, Qt::black,
            0.0);
        int hInk = 0;
        for (int y = 0; y < hStroke.height(); ++y)
            for (int x = 0; x < hStroke.width(); ++x)
                if (qGray(hStroke.pixel(x, y)) < 128) ++hInk;
        CHECK(hInk > 50);
        // Smudge previews drag the seed block into a smear.
        const QImage smAuto = bp::strokePreviewSmudgeAuto(a, 0.8, Qt::black);
        const QImage smStamp =
            bp::strokePreviewSmudgeStamp(tip, 0.8, 0.0, Qt::black);
        auto hasSmear = [](const QImage& img) {
            // Just past the seed block the dragged paint reads mid-gray.
            for (int y = 0; y < img.height(); ++y)
                for (int x = 60; x < 120; ++x) {
                    const int g = qGray(img.pixel(x, y));
                    if (g > 30 && g < 215) return true;
                }
            return false;
        };
        CHECK(hasSmear(smAuto));
        CHECK(hasSmear(smStamp));
    }

    // 16. Hose cycling: a two-cell hose (solid + empty) prints an
    // alternating stroke. Small brush + wide spacing so solid dabs cannot
    // bridge the empty ones: painted runs with regular gaps.
    {
        state.addDocument(QStringLiteral("hosestroke"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 8);
        state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"), 100);
        state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"), 150);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        pittore::compute::brushload::LoadedHose hose;
        hose.name = "test";
        hose.step = 150;
        hose.selection = "incremental";
        hose.ok = true;
        for (int i = 0; i < 2; ++i) {
            pittore::compute::brushload::LoadedTip cell;
            cell.tip.w = 4;
            cell.tip.h = 4;
            cell.tip.spacingPct = 150.0f;
            cell.tip.alpha.assign(16, i == 0 ? 1.0f : 0.0f);
            cell.ok = true;
            hose.cells.push_back(cell);
        }
        state.setBrushHose(QStringLiteral("test-hose"), hose);
        CHECK(state.brushHose(QStringLiteral("test-hose")) != nullptr);
        state.setOption(ToolId::Brush, QStringLiteral("brush_hose"),
                        QStringLiteral("test-hose"));
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::Brush);
        app.processEvents();
        const QPointF a = canvas->documentToView(QPointF(20, 75));
        const QPointF b = canvas->documentToView(QPointF(120, 75));
        const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, a, global, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &press);
        QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &move);
        QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &release);
        app.processEvents();
        DocumentItem* dd = state.activeDocument();
        int runs = 0, ink = 0;
        bool was = false;
        for (int x = 25; x < 115; ++x) {
            bool dark = false;
            for (int y = 70; y < 80; ++y) {
                const QColor c = dd->composite.pixelColor(x, y);
                if (c.alpha() > 128 &&
                    (c.red() + c.green() + c.blue()) / 3 < 128) {
                    dark = true;
                    break;
                }
            }
            if (dark) ++ink;
            if (dark && !was) ++runs;
            was = dark;
        }
        CHECK(ink > 10);   // solid cells laid ink
        CHECK(runs >= 3);  // empty cells cut regular gaps
        state.setOption(ToolId::Brush, QStringLiteral("brush_hose"), QString());
        state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"), 15);
        state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"), 50);
    }

    // 17. Scatter determinism: reseeded strokes replay bit-exactly.
    {
        state.addDocument(QStringLiteral("scatterstroke"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        state.setOption(ToolId::Brush, QStringLiteral("brush_scatter"), 60);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::Brush);
        app.processEvents();
        const QPointF a = canvas->documentToView(QPointF(20, 75));
        const QPointF b = canvas->documentToView(QPointF(120, 75));
        const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
        auto hashComposite = [&]() {
            DocumentItem* dd = state.activeDocument();
            quint64 h = 1469598103934665603ull;
            for (int y = 0; y < dd->composite.height(); ++y)
                for (int x = 0; x < dd->composite.width(); ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    h ^= quint64(c.red() + c.green() * 2 + c.blue() * 4 +
                                 c.alpha() * 8 + x + y * 1000);
                    h *= 1099511628211ull;
                }
            return h;
        };
        auto strokeSeeded = [&](std::uint64_t seed) {
            state.pinStrokeSeed(seed);
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            state.pinStrokeSeed(0);
        };
        strokeSeeded(7);
        const quint64 hashA = hashComposite();
        CHECK(hashA != 1469598103934665603ull);
        state.undo();
        strokeSeeded(7);
        CHECK(hashComposite() == hashA);  // replay is bit-exact
        state.undo();
        strokeSeeded(8);
        CHECK(hashComposite() != hashA);  // ...and seed-sensitive
        state.setOption(ToolId::Brush, QStringLiteral("brush_scatter"), 0);
    }

    // 18. Density gate: 0% lays nothing (empty stroke, no history entry).
    {
        state.addDocument(QStringLiteral("densitystroke"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("brush_density"), 0);
        DocumentItem* dd0 = state.activeDocument();
        const int layersBefore = (int)dd0->layers.size();
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::Brush);
        app.processEvents();
        const QPointF a = canvas->documentToView(QPointF(20, 75));
        const QPointF b = canvas->documentToView(QPointF(120, 75));
        const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, a, global, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &press);
        QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &move);
        QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &release);
        app.processEvents();
        DocumentItem* dd = state.activeDocument();
        int ink = 0;
        for (int y = 0; y < dd->composite.height(); ++y)
            for (int x = 0; x < dd->composite.width(); ++x) {
                const QColor c = dd->composite.pixelColor(x, y);
                if (c.alpha() > 128 &&
                    (c.red() + c.green() + c.blue()) / 3 < 128)
                    ++ink;
            }
        CHECK(ink == 0);
        CHECK((int)dd->layers.size() == layersBefore);
        state.setOption(ToolId::Brush, QStringLiteral("brush_density"), 100);
    }

    // 19. Smudge, grain, airbrush, flow and tilt, end to end.
    {
        namespace bp = pittore::ui::brushpreview;
        // Tilt mapping is pure: upright and near-upright give 0.
        CHECK_NEAR(bp::tiltRotationOffset(0, 0), 0.0, 1e-9);
        CHECK_NEAR(bp::tiltRotationOffset(1, 1), 0.0, 1e-9);
        CHECK_NEAR(bp::tiltRotationOffset(30, 0), 90.0, 1e-9);
        CHECK_NEAR(bp::tiltRotationOffset(-30, 0), -90.0, 1e-9);
        CHECK_NEAR(bp::tiltRotationOffset(0, 30), 0.0, 1e-9);

        auto makeCanvas = [&](QWidget& window) {
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            return canvas;
        };
        auto hStroke = [&](CanvasView* canvas, QPointF a, QPointF b) {
            const QPointF va = canvas->documentToView(a);
            const QPointF vb = canvas->documentToView(b);
            const QPointF global =
                canvas->viewport()->mapToGlobal(va.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, va, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, vb, global, Qt::NoButton,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, vb, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
        };
        auto opaqueDark = [&]() {
            DocumentItem* dd = state.activeDocument();
            int n = 0;
            for (int y = 0; y < dd->composite.height(); ++y)
                for (int x = 0; x < dd->composite.width(); ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if (c.alpha() > 128 &&
                        (c.red() + c.green() + c.blue()) / 3 < 128)
                        ++n;
                }
            return n;
        };

        // (a) Smudge drags paint across an edge.
        state.addDocument(QStringLiteral("smudgestroke"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        {
            QWidget window;
            makeCanvas(window);
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            state.paintDab(QPointF(40, 75), 10, 1.0, 1.0, QColor(0, 0, 0));
            state.flushPaint();
            app.processEvents();
            state.setForeground(QColor(255, 0, 0));
            state.beginStrokeState(ToolId::Brush);
            CHECK(state.smudgeTipDab(QPointF(55, 75), 12, 1.0, 1.0, 0.0,
                                     false, 1.0, 1.0));
            state.flushPaint();
            app.processEvents();
            state.endStrokeState();
            DocumentItem* dd = state.activeDocument();
            CHECK(dd->composite.pixelColor(45, 75).red() > 100);
            CHECK(dd->composite.pixelColor(65, 75).green() < 200);
        }

        // (b) Grain thins the stroke.
        state.addDocument(QStringLiteral("grainstroke"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        {
            AppState::PatternGray checker;
            checker.w = 2;
            checker.h = 2;
            checker.gray = {0.0f, 1.0f, 1.0f, 0.0f};
            state.setBrushPattern(QStringLiteral("checker"), checker);
            CHECK(state.brushPattern(QStringLiteral("checker")) != nullptr);
            state.setOption(ToolId::Brush, QStringLiteral("brush_texture"),
                            QStringLiteral("checker"));
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_texture_strength"), 100);
            QWidget window;
            auto* canvas = makeCanvas(window);
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            hStroke(canvas, QPointF(20, 75), QPointF(120, 75));
            const int grainy = opaqueDark();
            CHECK(grainy > 0);
            state.undo();
            state.setOption(ToolId::Brush, QStringLiteral("brush_texture"),
                            QString());
            hStroke(canvas, QPointF(20, 75), QPointF(120, 75));
            const int full = opaqueDark();
            CHECK(full > grainy * 1.2);
        }

        // (c) Airbrush ticks lay dabs while held.
        state.addDocument(QStringLiteral("airbrushstroke"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("airbrush"), true);
        state.setOption(ToolId::Brush, QStringLiteral("airbrush_rate"), 100);
        {
            QWidget window;
            auto* canvas = makeCanvas(window);
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const QPointF va = canvas->documentToView(QPointF(60, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(va.toPoint());
            // A move first: arms cursor tracking (airbrush ticks paint at
            // the cursor).
            QMouseEvent hover(QEvent::MouseMove, va, global, Qt::NoButton,
                             Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &hover);
            QMouseEvent press(QEvent::MouseButtonPress, va, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            const int inkPress = opaqueDark();
            canvas->airbrushTick();
            canvas->airbrushTick();
            canvas->airbrushTick();
            app.processEvents();
            CHECK(opaqueDark() > inkPress);
            QMouseEvent release(QEvent::MouseButtonRelease, va, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            state.setOption(ToolId::Brush, QStringLiteral("airbrush"), false);
        }

        // (d) Flow scales the stroke's darkness.
        state.addDocument(QStringLiteral("flowstroke"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        {
            QWidget window;
            auto* canvas = makeCanvas(window);
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            auto avgLuma = [&]() {
                DocumentItem* dd = state.activeDocument();
                double sum = 0;
                int n = 0;
                for (int y = 60; y < 90; ++y)
                    for (int x = 20; x < 120; ++x) {
                        const QColor c = dd->composite.pixelColor(x, y);
                        if (c.alpha() > 128) {
                            sum += (c.red() + c.green() + c.blue()) / 3.0;
                            ++n;
                        }
                    }
                return n > 0 ? sum / n : 255.0;
            };
            state.setOption(ToolId::Brush, QStringLiteral("flow"), 100);
            hStroke(canvas, QPointF(20, 75), QPointF(120, 75));
            const double full = avgLuma();
            state.undo();
            state.setOption(ToolId::Brush, QStringLiteral("flow"), 30);
            hStroke(canvas, QPointF(20, 75), QPointF(120, 75));
            CHECK(avgLuma() > full + 30.0);
            state.setOption(ToolId::Brush, QStringLiteral("flow"), 100);
        }

        // (e) Tilt rotates a flat tip 90 degrees.
        state.addDocument(QStringLiteral("tiltstroke"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("brush_roundness"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("tilt_rotation"), true);
        {
            QWidget window;
            auto* canvas = makeCanvas(window);
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            auto bounds = [&]() {
                DocumentItem* dd = state.activeDocument();
                int x0 = 1 << 30, x1 = -1, y0 = 1 << 30, y1 = -1;
                for (int y = 0; y < dd->composite.height(); ++y)
                    for (int x = 0; x < dd->composite.width(); ++x) {
                        const QColor c = dd->composite.pixelColor(x, y);
                        if (c.alpha() > 128 &&
                            (c.red() + c.green() + c.blue()) / 3 < 128) {
                            x0 = std::min(x0, x);
                            x1 = std::max(x1, x);
                            y0 = std::min(y0, y);
                            y1 = std::max(y1, y);
                        }
                    }
                return std::make_tuple(x1 - x0, y1 - y0);
            };
            auto tabletStroke = [&](double xt, double yt) {
                const QPointF a = canvas->documentToView(QPointF(20, 75));
                const QPointF b = canvas->documentToView(QPointF(120, 75));
                const QPointF global =
                    canvas->viewport()->mapToGlobal(a.toPoint());
                auto tabletAt = [&](QEvent::Type type, const QPointF& p,
                                    Qt::MouseButtons buttons) {
                    static QPointingDevice stylus;
                    QTabletEvent e(type, &stylus, p, global, 1.0, xt, yt, 0.0,
                                   0.0, 0, Qt::NoModifier, Qt::LeftButton,
                                   buttons);
                    QApplication::sendEvent(canvas->viewport(), &e);
                };
                tabletAt(QEvent::TabletPress, a, Qt::LeftButton);
                tabletAt(QEvent::TabletMove, b, Qt::LeftButton);
                tabletAt(QEvent::TabletRelease, b, Qt::NoButton);
                app.processEvents();
            };
            tabletStroke(0, 0);
            const auto [wPlain, hPlain] = bounds();
            CHECK(wPlain > hPlain + 20);  // flat tip lies horizontal
            state.undo();
            tabletStroke(30, 0);
            const auto [wTilt, hTilt] = bounds();
            CHECK(hTilt > hPlain + 5);  // ...and stands up when leaned on
            state.setOption(ToolId::Brush, QStringLiteral("tilt_rotation"),
                            false);
            state.setOption(ToolId::Brush, QStringLiteral("brush_roundness"),
                            100);
        }
    }

    // 20. Brushes panel boots a seeded library: stamps, patterns and hoses
    // register, rows gain icons, and the scratch strip renders.
    {
        namespace bp = pittore::ui::brushpreview;
        const QString home =
            QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        QDir().mkpath(home + QStringLiteral("/PittoreStudio/brushes/patterns"));
        auto stampPng = [](int v) {
            QImage img(8, 8, QImage::Format_Grayscale8);
            img.fill(v);
            return img;
        };
        CHECK(stampPng(200).save(
            home + QStringLiteral("/PittoreStudio/brushes/cell0.png"), "PNG"));
        CHECK(stampPng(100).save(
            home + QStringLiteral("/PittoreStudio/brushes/cell1.png"), "PNG"));
        CHECK(stampPng(160).save(
            home + QStringLiteral("/PittoreStudio/brushes/solo.png"), "PNG"));
        CHECK(stampPng(128).save(
            home + QStringLiteral("/PittoreStudio/brushes/patterns/grain.png"),
            "PNG"));
        QJsonArray lib;
        auto preset = [&](const QString& name, const QJsonObject& extra) {
            QJsonObject o{{"name", name}};
            for (auto it = extra.begin(); it != extra.end(); ++it)
                o.insert(it.key(), it.value());
            lib.push_back(o);
        };
        preset(QStringLiteral("Auto"),
               QJsonObject{{"size", 40},
                           {"roundness", 50},
                           {"pressure_size", true}});
        preset(QStringLiteral("Stamp"),
               QJsonObject{{"tipKind", "stamp"},
                           {"stampId", "solo.png"},
                           {"stampMode", 0}});
        preset(QStringLiteral("Hoser"),
               QJsonObject{{"hose", "hose-t20"},
                           {"hose_cells", QJsonArray{"cell0.png", "cell1.png"}},
                           {"hose_selection", "incremental"},
                           {"tipKind", "stamp"},
                           {"stampId", "cell0.png"},
                           {"tags", QJsonArray{"SK2"}}});
        preset(QStringLiteral("Smudger"),
               QJsonObject{{"engine", "smudge"},
                           {"smudge_rate", 80},
                           {"tipKind", "stamp"},
                           {"stampId", "solo.png"},
                           {"tags", QJsonArray{"SK2", "Soft"}}});
        preset(QStringLiteral("Grainy"),
               QJsonObject{{"texture", "grain.png"},
                           {"texture_strength", 90}});
        QFile jf(home + QStringLiteral("/PittoreStudio/brushes.json"));
        CHECK(jf.open(QIODevice::WriteOnly | QIODevice::Truncate));
        jf.write(QJsonDocument(lib).toJson(QJsonDocument::Compact));
        jf.close();
        QWidget window;
        QWidget* panel =
            pittore::ui::createBrushesPanel(&state, &window);
        CHECK(panel != nullptr);
        window.show();
        app.processEvents();
        app.processEvents();
        auto* tree = panel->findChild<QTreeWidget*>();
        CHECK(tree != nullptr);
        CHECK(tree->topLevelItemCount() == 2);
        int leaves = 0;
        for (int g = 0; g < 2; ++g)
            leaves += tree->topLevelItem(g)->childCount();
        CHECK(leaves == 8 + 5);  // factory set + seeded customs
        CHECK(state.brushStamp(QStringLiteral("solo.png")) != nullptr);
        CHECK(state.brushStamp(QStringLiteral("cell0.png")) != nullptr);
        CHECK(state.brushHose(QStringLiteral("hose-t20")) != nullptr);
        CHECK(state.brushHose(QStringLiteral("hose-t20"))->cells.size() == 2);
        CHECK(state.brushPattern(QStringLiteral("grain.png")) != nullptr);
        auto* preview =
            panel->findChild<QWidget*>(QStringLiteral("brushPreview"));
        CHECK(preview != nullptr);
        CHECK(preview->height() > 0);
        // Tag filter: SK2 shows its two customs, hides the rest + factory.
        auto* tagFilter =
            panel->findChild<QComboBox*>(QStringLiteral("brushTagFilter"));
        CHECK(tagFilter != nullptr);
        CHECK(tagFilter->findText(QStringLiteral("SK2")) >= 0);
        CHECK(tagFilter->findText(QStringLiteral("Soft")) >= 0);
        auto leafShown = [&](const QString& name) {
            const auto found =
                tree->findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
            CHECK(!found.isEmpty());
            return !found.front()->isHidden();
        };
        tagFilter->setCurrentText(QStringLiteral("SK2"));
        app.processEvents();
        CHECK(leafShown(QStringLiteral("Hoser")));
        CHECK(leafShown(QStringLiteral("Smudger")));
        CHECK(!leafShown(QStringLiteral("Auto")));
        CHECK(!leafShown(QStringLiteral("Soft Round")));
        tagFilter->setCurrentText(QStringLiteral("Soft"));
        app.processEvents();
        CHECK(!leafShown(QStringLiteral("Hoser")));
        CHECK(leafShown(QStringLiteral("Smudger")));
        tagFilter->setCurrentIndex(0);  // All tags
        app.processEvents();
        CHECK(leafShown(QStringLiteral("Auto")));
        CHECK(leafShown(QStringLiteral("Soft Round")));
        // Leave no trace for other runs sharing the sandbox home.
        QFile::remove(home + QStringLiteral("/PittoreStudio/brushes.json"));
        QFile::remove(home + QStringLiteral("/PittoreStudio/brushes/cell0.png"));
        QFile::remove(home + QStringLiteral("/PittoreStudio/brushes/cell1.png"));
        QFile::remove(home + QStringLiteral("/PittoreStudio/brushes/solo.png"));
        QFile::remove(home +
                      QStringLiteral("/PittoreStudio/brushes/patterns/grain.png"));
    }

    // 22. Authored opacity curve: the same pressure ramp lays much less ink
    // than the built-in linear response (concave author curve).
    {
        state.addDocument(QStringLiteral("curvestroke"), QSize(200, 150), 300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::Brush);
        app.processEvents();
        auto rampStroke = [&]() {
            const QPointF a = canvas->documentToView(QPointF(20, 75));
            const QPointF b = canvas->documentToView(QPointF(120, 75));
            const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
            auto tabletAt = [&](QEvent::Type type, const QPointF& p,
                                double pressure, Qt::MouseButtons buttons) {
                static QPointingDevice stylus;
                QTabletEvent e(type, &stylus, p, global, pressure, 0, 0, 0.0,
                               0.0, 0, Qt::NoModifier, Qt::LeftButton, buttons);
                QApplication::sendEvent(canvas->viewport(), &e);
            };
            tabletAt(QEvent::TabletPress, a, 0.1, Qt::LeftButton);
            for (int i = 1; i <= 10; ++i) {
                const QPointF p = a + (b - a) * (i / 10.0);
                tabletAt(QEvent::TabletMove, p, 0.1 + 0.9 * (i / 10.0),
                         Qt::LeftButton);
            }
            tabletAt(QEvent::TabletRelease, b, 1.0, Qt::NoButton);
            app.processEvents();
        };
        auto darkInk = [&](int x0, int x1) {
            DocumentItem* dd = state.activeDocument();
            int n = 0;
            for (int y = 0; y < dd->composite.height(); ++y)
                for (int x = x0; x < x1; ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if (c.alpha() > 128 &&
                        (c.red() + c.green() + c.blue()) / 3 < 128)
                        ++n;
                }
            return n;
        };
        rampStroke();
        // The light-pressure first third inks under the linear response...
        const int linearInk = darkInk(20, 55);
        CHECK(linearInk > 50);
        state.undo();
        // Pencil-style concave curve: light touches nearly vanish.
        state.setOption(ToolId::Brush, QStringLiteral("brush_opacity_curve"),
                        QStringLiteral("1|0,0;0.144578,0.0481932;1,1;"));
        rampStroke();
        // ...while the full stroke still lands (heavy end commits).
        CHECK(darkInk(20, 120) > 0);
        const int curveInk = darkInk(20, 55);
        CHECK(curveInk < linearInk);
        state.undo();
        // Single-dab readout (no build-up): the same 0.3 press lands
        // lighter under the concave curve than under linear.
        auto pressGray = [&](double pressure) {
            const QPointF a = canvas->documentToView(QPointF(60, 75));
            const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
            auto tabletAt = [&](QEvent::Type type, double p,
                                Qt::MouseButtons buttons) {
                static QPointingDevice stylus;
                QTabletEvent e(type, &stylus, a, global, p, 0, 0, 0.0, 0.0, 0,
                               Qt::NoModifier, Qt::LeftButton, buttons);
                QApplication::sendEvent(canvas->viewport(), &e);
            };
            tabletAt(QEvent::TabletPress, pressure, Qt::LeftButton);
            tabletAt(QEvent::TabletRelease, pressure, Qt::NoButton);
            app.processEvents();
            DocumentItem* dd = state.activeDocument();
            const QColor c = dd->composite.pixelColor(60, 75);
            return (c.red() + c.green() + c.blue()) / 3.0;
        };
        state.setOption(ToolId::Brush, QStringLiteral("brush_opacity_curve"),
                        QString());
        const double linearGray = pressGray(0.3);
        state.undo();
        state.setOption(ToolId::Brush, QStringLiteral("brush_opacity_curve"),
                        QStringLiteral("1|0,0;0.144578,0.0481932;1,1;"));
        const double curveGray = pressGray(0.3);
        CHECK(curveGray > linearGray + 10.0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_opacity_curve"),
                        QString());
    }

    // 23. Shared preset library: factory set, JSON round trip, and full
    // apply (the same call the panel and the options-bar picker share).
    {
        namespace bl = pittore::ui::brushlibrary;
        const auto factory = bl::factoryBrushPresets();
        CHECK(factory.size() == 8);
        CHECK(factory.front().name == QStringLiteral("Soft Round"));
        bl::BrushPreset p;
        p.name = QStringLiteral("RoundTrip");
        p.size = 42.0;
        p.roundness = 33.0;
        p.sizeCurve = QStringLiteral("1|0,0.2;1,1");
        p.opacityCurve = QStringLiteral("0.8|0,0;1,1");
        p.engine = QStringLiteral("smudge");
        p.smudgeRate = 76.0;
        p.scatterPct = 25.0;
        p.tiltRotation = true;
        p.textureFile = QStringLiteral("grain.png");
        p.hoseId = QStringLiteral("hose-x");
        p.hoseCells = QStringList{"a.png", "b.png"};
        p.hoseSelection = QStringLiteral("random");
        p.tags = QStringList{"SK2", "Ink"};
        p.paintingMode = QStringLiteral("buildup");
        p.rotationMode = 2;
        p.rotationCurve = QStringLiteral("1|0,0;1,1");
        p.sourceMode = 1;
        p.spacingIsotropic = true;
        p.maskStamp = QStringLiteral("mask.png");
        p.maskMode = 1;
        p.maskRatio = 80.0;
        p.maskAngle = 30.0;
        p.textureMode = 1;
        p.textureCutoffPolicy = 1;
        p.textureCutLo = 0.1;
        p.textureCutHi = 0.9;
        p.flipX = true;
        p.tiltSize = 60.0;
        p.tiltOpacity = 40.0;
        p.tangentialFlow = true;
        p.smoothing = 25.0;
        p.smoothingMode = 2;
        p.tipFilter = 0;
        p.fadeLen = 300.0;
        p.darkenPct = 50.0;
        p.hueJitter = 30.0;
        p.pressureIn = true;
        p.speedSize = 40.0;
        p.tiltXSize = 30.0;
        p.texturePressure = true;
        p.maskPressure = true;
        p.smudgePressure = true;
        p.gradientLen = 240.0;
        p.tiltYSize = 20.0;
        p.timeFade = 5.0;
        p.fuzzySize = 25.0;
        p.fuzzyOpacity = 15.0;
        p.perspective = 60.0;
        p.vpX = 100.0;
        p.vpY = 75.0;
        bool ok = false;
        bl::BrushPreset q =
            bl::BrushPreset::fromJson(p.toJson(), &ok);
        CHECK(ok);
        CHECK(q.name == p.name);
        CHECK_NEAR(q.size, 42.0, 1e-9);
        CHECK_NEAR(q.roundness, 33.0, 1e-9);
        CHECK(q.sizeCurve == p.sizeCurve);
        CHECK(q.opacityCurve == p.opacityCurve);
        CHECK(q.engine == QStringLiteral("smudge"));
        CHECK_NEAR(q.smudgeRate, 76.0, 1e-9);
        CHECK_NEAR(q.scatterPct, 25.0, 1e-9);
        CHECK(q.tiltRotation);
        CHECK(q.textureFile == QStringLiteral("grain.png"));
        CHECK(q.hoseCells.size() == 2);
        CHECK(q.hoseSelection == QStringLiteral("random"));
        CHECK(q.tags.contains(QStringLiteral("SK2")));
        CHECK(q.tags.size() == 2);
        CHECK(q.paintingMode == QStringLiteral("buildup"));
        CHECK(q.rotationMode == 2);
        CHECK(q.rotationCurve == QStringLiteral("1|0,0;1,1"));
        CHECK(q.sourceMode == 1);
        CHECK(q.spacingIsotropic);
        CHECK(q.maskStamp == QStringLiteral("mask.png"));
        CHECK(q.maskMode == 1);
        CHECK_NEAR(q.maskRatio, 80.0, 1e-9);
        CHECK_NEAR(q.maskAngle, 30.0, 1e-9);
        CHECK(q.textureMode == 1);
        CHECK(q.textureCutoffPolicy == 1);
        CHECK_NEAR(q.textureCutLo, 0.1, 1e-9);
        CHECK_NEAR(q.textureCutHi, 0.9, 1e-9);
        CHECK(q.flipX);
        CHECK(!q.flipY);
        CHECK_NEAR(q.tiltSize, 60.0, 1e-9);
        CHECK_NEAR(q.tiltOpacity, 40.0, 1e-9);
        CHECK(q.tangentialFlow);
        CHECK_NEAR(q.smoothing, 25.0, 1e-9);
        CHECK(q.smoothingMode == 2);
        CHECK(q.tipFilter == 0);
        CHECK_NEAR(q.fadeLen, 300.0, 1e-9);
        CHECK_NEAR(q.darkenPct, 50.0, 1e-9);
        CHECK_NEAR(q.hueJitter, 30.0, 1e-9);
        CHECK(q.pressureIn);
        CHECK_NEAR(q.speedSize, 40.0, 1e-9);
        CHECK(q.texturePressure);
        CHECK(q.maskPressure);
        CHECK(q.smudgePressure);
        CHECK_NEAR(q.gradientLen, 240.0, 1e-9);
        CHECK_NEAR(q.tiltXSize, 30.0, 1e-9);
        CHECK_NEAR(q.tiltYSize, 20.0, 1e-9);
        CHECK_NEAR(q.timeFade, 5.0, 1e-9);
        CHECK_NEAR(q.fuzzySize, 25.0, 1e-9);
        CHECK_NEAR(q.fuzzyOpacity, 15.0, 1e-9);
        CHECK_NEAR(q.perspective, 60.0, 1e-9);
        CHECK_NEAR(q.vpX, 100.0, 1e-9);
        CHECK_NEAR(q.vpY, 75.0, 1e-9);
        // Factory presets carry no extras (clean JSON for built-ins).
        CHECK(!factory.front().toJson().contains(QStringLiteral("engine")));
        // Apply writes the whole known state, including curves and tilt.
        state.setActiveTool(ToolId::Brush);
        CHECK(bl::applyPreset(&state, q) ==
              bl::ApplyResult::Applied);
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("brush_size"))
                       .toDouble(),
                   42.0, 1e-9);
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_size_curve"))
                  .toString() == p.sizeCurve);
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_opacity_curve"))
                  .toString() == p.opacityCurve);
        CHECK(state.option(ToolId::Brush, QStringLiteral("tilt_rotation"))
                  .toBool());
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_engine"))
                  .toString() == QStringLiteral("smudge"));
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_hose"))
                  .toString() == QStringLiteral("hose-x"));
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_painting_mode"))
                  .toString() == QStringLiteral("buildup"));
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_rotation"))
                  .toInt() == 2);
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_rotation_curve"))
                  .toString() == QStringLiteral("1|0,0;1,1"));
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_source"))
                  .toInt() == 1);
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_spacing_isotropic"))
                  .toBool());
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_mask_stamp"))
                  .toString() == QStringLiteral("mask.png"));
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_mask_mode"))
                  .toInt() == 1);
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_texture_mode"))
                  .toInt() == 1);
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_texture_cutoff_policy"))
                  .toInt() == 1);
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_flip_x"))
                  .toBool());
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_tilt_size"))
                       .toDouble(),
                   60.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_tilt_opacity"))
                       .toDouble(),
                   40.0, 1e-9);
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_tangential_flow"))
                  .toBool());
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("smoothing"))
                       .toDouble(),
                   25.0, 1e-9);
        CHECK(state.option(ToolId::Brush, QStringLiteral("smoothing_mode"))
                  .toInt() == 2);
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_tip_filter"))
                  .toInt() == 0);
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("brush_fade"))
                       .toDouble(),
                   300.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("brush_darken"))
                       .toDouble(),
                   50.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_hue_jitter"))
                       .toDouble(),
                   30.0, 1e-9);
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_pressure_in"))
                  .toBool());
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_speed_size"))
                       .toDouble(),
                   40.0, 1e-9);
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_texture_pressure"))
                  .toBool());
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_mask_pressure"))
                  .toBool());
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_smudge_pressure"))
                  .toBool());
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_gradient_len"))
                       .toDouble(),
                   240.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_tiltx_size"))
                       .toDouble(),
                   30.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_tilty_size"))
                       .toDouble(),
                   20.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_timefade"))
                       .toDouble(),
                   5.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_fuzzy_size"))
                       .toDouble(),
                   25.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_fuzzy_opacity"))
                       .toDouble(),
                   15.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_perspective"))
                       .toDouble(),
                   60.0, 1e-9);
        // Missing stamp reports, but still applies (auto-tip fallback).
        bl::BrushPreset missing = q;
        missing.tipKind = QStringLiteral("stamp");
        missing.stampId = QStringLiteral("no-such-file.png");
        CHECK(bl::applyPreset(&state, missing) ==
              bl::ApplyResult::StampMissing);
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_size"))
                  .toDouble() == 42.0);
        // Reset stroke-shaping leftovers so later sections see defaults.
        state.setOption(ToolId::Brush, QStringLiteral("brush_fade"), 0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_darken"), 0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_hue_jitter"),
                        0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_pressure_in"),
                        false);
        state.setOption(ToolId::Brush, QStringLiteral("brush_speed_size"),
                        0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tiltx_size"),
                        0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilty_size"),
                        0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_timefade"), 0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_fuzzy_size"),
                        0);
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_fuzzy_opacity"), 0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_perspective"),
                        0);
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_texture_pressure"), false);
        state.setOption(ToolId::Brush, QStringLiteral("brush_mask_pressure"),
                        false);
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_smudge_pressure"), false);
        state.setOption(ToolId::Brush, QStringLiteral("brush_gradient_len"),
                        500);
        state.setOption(ToolId::Brush, QStringLiteral("brush_engine"),
                        QString());
        state.setOption(ToolId::Brush, QStringLiteral("brush_scatter"), 0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_rotation"), 0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_source"), 0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_mask_stamp"),
                        QString());
        state.setOption(ToolId::Brush, QStringLiteral("brush_flip_x"),
                        false);
        state.setOption(ToolId::Brush, QStringLiteral("brush_flip_y"),
                        false);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilt_size"),
                        0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilt_opacity"),
                        0);
        state.setOption(ToolId::Brush, QStringLiteral("tilt_rotation"),
                        false);
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_tangential_flow"), false);
        state.setOption(ToolId::Brush, QStringLiteral("smoothing"), 0);
        state.setOption(ToolId::Brush, QStringLiteral("smoothing_mode"),
                        0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tip_filter"),
                        1);
        state.setOption(ToolId::Brush, QStringLiteral("brush_size_curve"),
                        QString());
        state.setOption(ToolId::Brush, QStringLiteral("brush_opacity_curve"),
                        QString());
        state.setOption(ToolId::Brush, QStringLiteral("brush_flow_curve"),
                        QString());
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_rotation_curve"), QString());
    }

    // 24. Brush Preview panel (right side): name follows applied presets,
    // readout tracks live options, scratch strip renders.
    {
        // Deterministic baseline (earlier sections leave options behind).
        state.setActiveTool(ToolId::Brush);
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 64);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        QWidget window;
        QWidget* panel =
            pittore::ui::createBrushPreviewPanel(&state, &window);
        CHECK(panel != nullptr);
        window.show();
        app.processEvents();
        auto* nameLabel =
            panel->findChild<QLabel*>(QStringLiteral("brushPreviewName"));
        CHECK(nameLabel != nullptr);
        // Name tracks the last applied preset through shared state.
        CHECK(nameLabel->text() == state.activeBrushPresetName() ||
              nameLabel->text() == QStringLiteral("Custom brush"));
        auto* readout =
            panel->findChild<QLabel*>(QStringLiteral("brushPreviewReadout"));
        CHECK(readout != nullptr);
        CHECK(readout->text().contains(QStringLiteral("64 px")));
        namespace bl = pittore::ui::brushlibrary;
        bl::BrushPreset p;
        p.name = QStringLiteral("PreviewProbe");
        p.size = 48.0;
        p.opacity = 80.0;
        CHECK(bl::applyPreset(&state, p) == bl::ApplyResult::Applied);
        CHECK(state.activeBrushPresetName() == QStringLiteral("PreviewProbe"));
        app.processEvents();
        CHECK(nameLabel->text() == QStringLiteral("PreviewProbe"));
        CHECK(readout->text().contains(QStringLiteral("48 px")));
        CHECK(readout->text().contains(QStringLiteral("80% opacity")));
        auto* thumb =
            panel->findChild<QLabel*>(QStringLiteral("brushPreviewThumb"));
        CHECK(thumb != nullptr);
        CHECK(!thumb->pixmap(Qt::ReturnByValue).isNull());
    }

    // 25. Options bar carries the brush well for paint tools (the popup
    // hosts settings + gallery now).
    {
        state.setActiveTool(ToolId::Brush);
        QWidget window;
        auto* bar = new OptionsBar(&state, &window);
        window.show();
        app.processEvents();
        bool found = false;
        for (auto* button : bar->findChildren<QToolButton*>()) {
            if (button->toolTip() ==
                QStringLiteral("Brush preset: size, hardness and tip")) {
                found = true;
                break;
            }
        }
        CHECK(found);
        // The badge wears the applied brush: dab + preset name.
        namespace bl = pittore::ui::brushlibrary;
        bl::BrushPreset probe;
        probe.name = QStringLiteral("BadgeProbe");
        probe.size = 48.0;
        CHECK(bl::applyPreset(&state, probe) == bl::ApplyResult::Applied);
        app.processEvents();
        QToolButton* badge = nullptr;
        for (auto* button : bar->findChildren<QToolButton*>()) {
            if (button->toolTip().contains(QStringLiteral("BadgeProbe"))) {
                badge = button;
                break;
            }
        }
        CHECK(badge != nullptr);
        CHECK(badge->text() == QStringLiteral("BadgeProbe"));
        // Other tools keep the plain tool icon (no text).
        state.setActiveTool(ToolId::Move);
        app.processEvents();
        bool badgePlain = false;
        for (auto* button : bar->findChildren<QToolButton*>()) {
            if (button->toolTip().contains(
                    QStringLiteral("tool presets"))) {
                badgePlain = button->text().isEmpty();
                break;
            }
        }
        CHECK(badgePlain);
        state.setActiveTool(ToolId::Brush);
        // The brush gallery is a thumbnail grid (no ragged wrap):
        // IconMode, fixed cells, no word wrap, factory set present.
        auto* gallery = bar->findChild<QListWidget*>();
        CHECK(gallery != nullptr);
        CHECK(gallery->viewMode() == QListView::IconMode);
        CHECK(gallery->gridSize().width() > 0);
        CHECK(!gallery->wordWrap());
        CHECK(gallery->count() >= 8);  // factory set, no customs here
    }

    // 26. Wash vs buildup at 43% opacity: overlapping dabs stay capped in
    // wash mode but pile toward black in buildup mode.
    {
        auto ghostStroke = [&](const QString& docName, const QString& mode) {
            state.addDocument(docName, QSize(200, 150), 300);
            state.setForeground(QColor(0, 0, 0));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 40);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 43);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"), mode);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            // One pass: dense overlap builds up within a single stroke.
            const QPointF a = canvas->documentToView(QPointF(40, 75));
            const QPointF b = canvas->documentToView(QPointF(160, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            DocumentItem* dd = state.activeDocument();
            double sum = 0;
            int n = 0;
            for (int y = 65; y < 85; ++y)
                for (int x = 80; x < 120; ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if (c.alpha() > 128) {
                        sum += (c.red() + c.green() + c.blue()) / 3.0;
                        ++n;
                    }
                }
            return n > 0 ? sum / n : 255.0;
        };
        const double washGray =
            ghostStroke(QStringLiteral("washmode"), QStringLiteral("wash"));
        const double buildGray = ghostStroke(QStringLiteral("buildmode"),
                                             QStringLiteral("buildup"));
        // Wash holds the storyboard gray (~43% ink over paper) while the
        // same single pass piles toward black in buildup mode.
        CHECK(washGray > 120.0);
        CHECK(buildGray < 60.0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_painting_mode"),
                        QStringLiteral("wash"));
    }

    // 27. Full stylus ground: lean math, wheel math, tilt-thickened
    // strokes, wheel-throttled flow, and eraser-end auto-switch.
    {
        // Pure response math (no device needed).
        CHECK_NEAR(CanvasView::tiltLean(0, 0), 0.0, 1e-9);
        CHECK_NEAR(CanvasView::tiltLean(60, 0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::tiltLean(0, -60), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::tiltLean(60, 60), 1.0, 1e-9);  // clamped
        CHECK_NEAR(CanvasView::tiltLean(30, 0), 0.5, 1e-9);
        CHECK_NEAR(CanvasView::tiltSizeFactor(0.0, 100.0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::tiltSizeFactor(1.0, 100.0), 2.0, 1e-9);
        CHECK_NEAR(CanvasView::tiltSizeFactor(0.5, 0.0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::tiltOpacityFactor(0.0, 100.0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::tiltOpacityFactor(1.0, 100.0), 0.0, 1e-9);
        CHECK_NEAR(CanvasView::tiltOpacityFactor(1.0, 0.0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::tangentialFlowFactor(1.0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::tangentialFlowFactor(0.0), 0.15, 1e-9);

        // Tilt-thickened dab: full lean at tilt_size 100 doubles the
        // diameter, so one leaned dab spans twice the width of an
        // upright one. Wheel flow: the same dab at wheel 0 lands at
        // 0.15 flow (faint center) vs opaque at wheel 1.
        auto dabAt = [&](const QString& docName, double xt, double tangent,
                         bool wheelOn) {
            state.addDocument(docName, QSize(200, 150), 300);
            state.setForeground(QColor(0, 0, 0));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 24);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"),
                            QStringLiteral("buildup"));
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_tangential_flow"), wheelOn);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(100, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            static QPointingDevice stylus;
            auto tabletAt = [&](QEvent::Type type, Qt::MouseButtons buttons) {
                QTabletEvent e(type, &stylus, a, global, 1.0, xt, 0, tangent,
                               0.0, 0, Qt::NoModifier, Qt::LeftButton,
                               buttons);
                QApplication::sendEvent(canvas->viewport(), &e);
            };
            tabletAt(QEvent::TabletPress, Qt::LeftButton);
            tabletAt(QEvent::TabletRelease, Qt::NoButton);
            app.processEvents();
            DocumentItem* dd = state.activeDocument();
            // Darkness, not alpha: the paper itself is opaque white.
            int x0 = 200, x1 = -1;
            for (int x = 0; x < 200; ++x)
                for (int y = 0; y < 150; ++y) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if ((c.red() + c.green() + c.blue()) / 3 < 128) {
                        x0 = std::min(x0, x);
                        x1 = std::max(x1, x);
                    }
                }
            const QColor center = dd->composite.pixelColor(100, 75);
            const int centerGray =
                (center.red() + center.green() + center.blue()) / 3;
            return std::make_pair(x1 >= x0 ? x1 - x0 + 1 : 0, centerGray);
        };
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilt_size"),
                        100);
        const auto [wUpright, aUpright] =
            dabAt(QStringLiteral("stylusup"), 0, 1.0, false);
        const auto [wLeaned, aLeaned] =
            dabAt(QStringLiteral("styluslean"), 60, 1.0, false);
        CHECK(wUpright >= 20);       // the dab landed
        CHECK(wLeaned > wUpright + 10);  // doubled diameter shows
        CHECK(aUpright < 50);            // opaque black center
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilt_size"), 0);
        // Wheel flow: same upright dab, wheel 1 vs wheel 0.
        const auto [wFull, aFull] =
            dabAt(QStringLiteral("styluswheel1"), 0, 1.0, true);
        const auto [wEmpty, aEmpty] =
            dabAt(QStringLiteral("styluswheel0"), 0, 0.0, true);
        CHECK(aFull < 50);    // full wheel: black center
        CHECK(aEmpty > 150);  // empty wheel: faint gray center
        CHECK(wEmpty < wFull);
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_tangential_flow"), false);

        // Eraser end on the Brush: same tool kept, erase blend flipped
        // for the stroke, cleared on release.
        {
            state.addDocument(QStringLiteral("styluseraser"), QSize(200, 150),
                              300);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            QPointingDevice eraserDev(
                QStringLiteral("eraser"), 2,
                QInputDevice::DeviceType::Stylus,
                QPointingDevice::PointerType::Eraser,
                QPointingDevice::Capabilities(), 1, 1);
            const QPointF a = canvas->documentToView(QPointF(100, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            QTabletEvent press(QEvent::TabletPress, &eraserDev, a, global,
                               1.0, 0, 0, 0.0, 0.0, 0, Qt::NoModifier,
                               Qt::LeftButton, Qt::LeftButton);
            QApplication::sendEvent(canvas->viewport(), &press);
            app.processEvents();
            CHECK(state.activeTool() == ToolId::Brush);  // tip kept
            CHECK(state.option(ToolId::Brush,
                               QStringLiteral("brush_erase_blend"))
                      .toBool());
            QTabletEvent release(QEvent::TabletRelease, &eraserDev, a, global,
                                 1.0, 0, 0, 0.0, 0.0, 0, Qt::NoModifier,
                                 Qt::LeftButton, Qt::NoButton);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            CHECK(state.activeTool() == ToolId::Brush);
            CHECK(!state.option(ToolId::Brush,
                                QStringLiteral("brush_erase_blend"))
                       .toBool());
        }
    }

    // 28. Layers-panel opacity slider: stylus-sized slider drives the
    // active layer, staying in sync with the precise spinbox both ways.
    {
        state.addDocument(QStringLiteral("layeropacity"), QSize(200, 150),
                          300);
        QWidget window;
        QWidget* panel = createLayersPanel(&state, &window);
        panel->show();
        app.processEvents();
        auto sliders = panel->findChildren<QSlider*>();
        CHECK(!sliders.isEmpty());
        QSlider* opacitySlider = sliders.front();
        CHECK(opacitySlider->maximum() == 100);
        opacitySlider->setValue(40);
        app.processEvents();
        CHECK(state.activeLayer() != nullptr);
        CHECK(state.activeLayer()->opacity == 40);
        // Spinbox follows the slider...
        auto spins = panel->findChildren<QSpinBox*>();
        bool spinShows40 = false;
        for (QSpinBox* s : spins) {
            if (s->suffix() == QStringLiteral("%") && s->value() == 40) {
                spinShows40 = true;
                // ...and driving the spinbox moves the slider back.
                s->setValue(75);
                app.processEvents();
                CHECK(opacitySlider->value() == 75);
                CHECK(state.activeLayer()->opacity == 75);
                break;
            }
        }
        CHECK(spinShows40);
    }

    // 29. Zoomed display quality: replicate the view's 8x smooth upscale
    // headless and profile it. A hard dab edge must spread over several
    // screen px (nearest would print full 255 steps); a soft fade is
    // profiled for flat-band runs.
    {
        state.addDocument(QStringLiteral("zoomquality"), QSize(200, 150),
                          300);
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                        100);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_painting_mode"),
                        QStringLiteral("buildup"));
        state.setActiveTool(ToolId::Brush);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(1.0);
        app.processEvents();
        // One hard dab at doc center via a press-release (no move).
        const QPointF a = canvas->documentToView(QPointF(100, 75));
        const QPointF global =
            canvas->viewport()->mapToGlobal(a.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, a, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &press);
        QMouseEvent release(QEvent::MouseButtonRelease, a, global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &release);
        app.processEvents();
        DocumentItem* dd = state.activeDocument();
        // Same upscale the view does: smooth 8x of the composite.
        QImage big(1600, 1200, QImage::Format_ARGB32);
        big.fill(Qt::white);
        {
            QPainter p(&big);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(QRectF(0, 0, 1600, 1200), dd->composite);
        }
        // Hard-edge profile across the dab (row = doc y75 * 8).
        int maxStep = 0;
        for (int x = 1; x < 1600; ++x) {
            const int v0 = qGray(big.pixel(x - 1, 600));
            const int v1 = qGray(big.pixel(x, 600));
            maxStep = std::max(maxStep, std::abs(v1 - v0));
        }
        std::printf("[zoomq] hard-edge max adjacent step at 8x: %d\n",
                    maxStep);
        CHECK(maxStep < 128);  // nearest-neighbor would print 255
    }

    // 30. No view ghosting: after an incremental (sub-rect) stroke at high
    // zoom, every inked composite pixel must show on screen. Missing ink
    // (paper-white view over solid composite ink) is the dashed-stroke
    // ghost: data painted, screen never told. Runs on the CPU backend and,
    // when one builds, the GPU backend (the frame-buffer ghost report came
    // from a CUDA run).
    {
        auto runGhost = [&](const QString& docName,
                            pittore::compute::ComputeBackend* be,
                            bool tablet) {
            state.addDocument(docName, QSize(200, 150), 300);
            DocumentItem* dd = state.activeDocument();
            if (be) dd->setBackend(be);
            state.setForeground(QColor(0, 0, 0));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 48);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            90);
            state.setOption(ToolId::Brush, QStringLiteral("brush_angle"),
                            45);
            state.setOption(ToolId::Brush, QStringLiteral("brush_roundness"),
                            60);
            state.setOption(ToolId::Brush, QStringLiteral("brush_tip"), 1);
            state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"),
                            10);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"),
                            QStringLiteral("buildup"));
            state.setActiveTool(ToolId::Brush);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(6.82);
            app.processEvents();
            // Fast swooping stroke: coarse jumps like a quick stylus gesture.
            const QPointF global =
                canvas->viewport()->mapToGlobal(QPoint(10, 10));
            auto viewAt = [&](const QPointF& docPt) {
                return canvas->documentToView(docPt);
            };
            const QPointF pts[] = {
                QPointF(20, 130), QPointF(60, 90), QPointF(100, 110),
                QPointF(140, 60), QPointF(180, 100),
            };
            // Mouse or a light-touch stylus gesture (pressure ramps up,
            // like a real stroke start). Tablet input synthesizes the
            // same mouse gesture through the stylus latches.
            static QPointingDevice stylus;
            auto sendAt = [&](QEvent::Type type, const QPointF& view,
                              double pressure, Qt::MouseButtons buttons,
                              Qt::MouseButton button) {
                if (!tablet) {
                    QMouseEvent e(type, view, global, button, buttons,
                                  Qt::NoModifier);
                    QApplication::sendEvent(canvas->viewport(), &e);
                    return;
                }
                QTabletEvent e(type, &stylus, view, global, pressure, 0, 0,
                               0.0, 0.0, 0, Qt::NoModifier, Qt::LeftButton,
                               buttons);
                QApplication::sendEvent(canvas->viewport(), &e);
            };
            {
                sendAt(tablet ? QEvent::TabletPress
                              : QEvent::MouseButtonPress,
                       viewAt(pts[0]), 0.05, Qt::LeftButton, Qt::LeftButton);
                app.processEvents();
                for (int i = 1; i < 5; ++i) {
                    const double pr =
                        tablet ? 0.05 + 0.95 * (i / 4.0) : 1.0;
                    sendAt(tablet ? QEvent::TabletMove : QEvent::MouseMove,
                           viewAt(pts[i]), pr, Qt::LeftButton, Qt::NoButton);
                    app.processEvents();
                }
                sendAt(tablet ? QEvent::TabletRelease
                              : QEvent::MouseButtonRelease,
                       viewAt(pts[4]), tablet ? 1.0 : 1.0, Qt::NoButton,
                       Qt::LeftButton);
                app.processEvents();
            }
            // Incremental view vs forced full repaint of the same canvas:
            // identical paint code, so any difference is pure update loss
            // (no reference-alignment wobble possible).
            const QImage incr = canvas->viewport()->grab().toImage();
            canvas->refresh();
            app.processEvents();
            const QImage full = canvas->viewport()->grab().toImage();
            CHECK(incr.size() == full.size());
            // Lost ink: paper on the incremental frame, solid ink after the
            // full repaint. (Extra dark pixels are the brush ring; ignored.)
            int missing = 0;
            int ink = 0;
            int x0 = full.width(), x1 = -1, y0 = full.height(), y1 = -1;
            for (int y = 0; y < full.height(); ++y) {
                for (int x = 0; x < full.width(); ++x) {
                    if (qGray(full.pixel(x, y)) < 100) {
                        ++ink;
                        if (qGray(incr.pixel(x, y)) > 200) {
                            ++missing;
                            x0 = std::min(x0, x);
                            x1 = std::max(x1, x);
                            y0 = std::min(y0, y);
                            y1 = std::max(y1, y);
                        }
                    }
                }
            }
            return std::make_tuple(ink, missing, x0, x1, y0, y1);
        };
        {
            const auto [ink, missing, x0, x1, y0, y1] =
                runGhost(QStringLiteral("viewghost-cpu"), nullptr, false);
            std::printf("[viewq] cpu-mouse ink=%d missing=%d bbox x=(%d,%d) "
                        "y=(%d,%d)\n",
                        ink, missing, x0, x1, y0, y1);
            CHECK(ink > 1000);  // the stroke really landed
            CHECK(missing == 0);
        }
        {
            const auto [ink, missing, x0, x1, y0, y1] =
                runGhost(QStringLiteral("viewghost-cpu-tab"), nullptr, true);
            std::printf("[viewq] cpu-tablet ink=%d missing=%d bbox "
                        "x=(%d,%d) y=(%d,%d)\n",
                        ink, missing, x0, x1, y0, y1);
            CHECK(ink > 1000);
            CHECK(missing == 0);
        }
        if (auto gpu = pittore::compute::make_backend(
                pittore::compute::BackendType::CUDA)) {
            const auto [ink, missing, x0, x1, y0, y1] =
                runGhost(QStringLiteral("viewghost-gpu"), gpu.get(), false);
            std::printf("[viewq] gpu-mouse ink=%d missing=%d bbox x=(%d,%d) "
                        "y=(%d,%d)\n",
                        ink, missing, x0, x1, y0, y1);
            CHECK(ink > 1000);
            CHECK(missing == 0);
            const auto [ink2, missing2, a0, a1, b0, b1] =
                runGhost(QStringLiteral("viewghost-gpu-tab"), gpu.get(),
                         true);
            std::printf("[viewq] gpu-tablet ink=%d missing=%d bbox "
                        "x=(%d,%d) y=(%d,%d)\n",
                        ink2, missing2, a0, a1, b0, b1);
            CHECK(ink2 > 1000);
            CHECK(missing2 == 0);
        } else {
            std::printf("[viewq] gpu backend unavailable, skipped\n");
        }
    }

    // 31. Multi-stroke accumulation on the GPU backend: dense small moves
    // (like a real stylus), hover relocations between strokes, then the
    // incremental frame vs a forced full repaint. Catches ghosts that
    // need more than one stroke to appear (stale scratch, dirty-union,
    // or handoff state carried across strokes).
    {
        auto gpu = pittore::compute::make_backend(
            pittore::compute::BackendType::CUDA);
        state.addDocument(QStringLiteral("viewghost-multi"), QSize(200, 150),
                          300);
        DocumentItem* dd = state.activeDocument();
        if (gpu) dd->setBackend(gpu.get());
        std::printf("[viewq] multi backend=%s\n",
                    gpu ? "gpu" : "cpu-fallback");
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 40);
        state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"), 90);
        state.setOption(ToolId::Brush, QStringLiteral("brush_angle"), 45);
        state.setOption(ToolId::Brush, QStringLiteral("brush_roundness"), 60);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tip"), 1);
        state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"), 10);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
        state.setOption(ToolId::Brush,
                        QStringLiteral("brush_painting_mode"),
                        QStringLiteral("buildup"));
        state.setActiveTool(ToolId::Brush);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(6.82);
        app.processEvents();
        static QPointingDevice stylus2;
        const QPointF global =
            canvas->viewport()->mapToGlobal(QPoint(10, 10));
        auto viewAt = [&](const QPointF& docPt) {
            return canvas->documentToView(docPt);
        };
        // One dense stroke: press + N small tablet moves + release, then
        // a hover relocation (ring follows, away from the ink).
        auto denseStroke = [&](const std::vector<QPointF>& path,
                               double p0, double p1) {
            QTabletEvent press(QEvent::TabletPress, &stylus2,
                               viewAt(path.front()), global, p0, 0, 0, 0.0,
                               0.0, 0, Qt::NoModifier, Qt::LeftButton,
                               Qt::LeftButton);
            QApplication::sendEvent(canvas->viewport(), &press);
            app.processEvents();
            for (std::size_t i = 1; i < path.size(); ++i) {
                const double t = double(i) / double(path.size() - 1);
                QTabletEvent move(QEvent::TabletMove, &stylus2,
                                  viewAt(path[i]), global,
                                  p0 + (p1 - p0) * t, 0, 0, 0.0, 0.0, 0,
                                  Qt::NoModifier, Qt::LeftButton,
                                  Qt::LeftButton);
                QApplication::sendEvent(canvas->viewport(), &move);
                app.processEvents();
            }
            QTabletEvent release(QEvent::TabletRelease, &stylus2,
                                 viewAt(path.back()), global, p1, 0, 0, 0.0,
                                 0.0, 0, Qt::NoModifier, Qt::LeftButton,
                                 Qt::NoButton);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
        };
        auto hoverAt = [&](const QPointF& docPt) {
            QMouseEvent move(QEvent::MouseMove, viewAt(docPt), global,
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            app.processEvents();
        };
        // Stroke 1: dense zigzag fill (the black-blob equivalent).
        {
            std::vector<QPointF> path;
            for (int i = 0; i <= 24; ++i)
                path.emplace_back(80 + (i % 2) * 40, 40 + i * 3);
            denseStroke(path, 0.8, 0.8);
        }
        hoverAt(QPointF(10, 140));
        // Stroke 2: swooping light-touch curve (the slash equivalent).
        {
            std::vector<QPointF> path;
            for (int i = 0; i <= 24; ++i) {
                const double t = double(i) / 24.0;
                path.emplace_back(20 + t * 160,
                                  120 - std::sin(t * 6.2831) * 40 - t * 30);
            }
            denseStroke(path, 0.1, 0.9);
        }
        hoverAt(QPointF(190, 10));
        // Stroke 3: second curve crossing stroke 2.
        {
            std::vector<QPointF> path;
            for (int i = 0; i <= 24; ++i) {
                const double t = double(i) / 24.0;
                path.emplace_back(20 + t * 160,
                                  30 + std::sin(t * 6.2831) * 35 + t * 40);
            }
            denseStroke(path, 0.5, 1.0);
        }
        // Park the ring far from all ink, then compare frames.
        hoverAt(QPointF(195, 145));
        const QPointF ringView = viewAt(QPointF(195, 145));
        const QImage incr = canvas->viewport()->grab().toImage();
        canvas->refresh();
        app.processEvents();
        const QImage full = canvas->viewport()->grab().toImage();
        CHECK(incr.size() == full.size());
        // Missing ink (paper over solid ink) AND added ink (dark over
        // paper, ring zone excluded): either direction is a ghost.
        int missing = 0, added = 0, ink = 0;
        for (int y = 0; y < full.height(); ++y) {
            for (int x = 0; x < full.width(); ++x) {
                const bool nearRing =
                    std::hypot(x - ringView.x(), y - ringView.y()) < 80.0;
                const int gf = qGray(full.pixel(x, y));
                const int gi = qGray(incr.pixel(x, y));
                if (gf < 100) {
                    ++ink;
                    if (gi > 200) ++missing;
                } else if (gf > 200 && gi < 100 && !nearRing) {
                    ++added;
                }
            }
        }
        std::printf("[viewq] multi ink=%d missing=%d added=%d\n", ink,
                    missing, added);
        CHECK(ink > 5000);
        CHECK(missing == 0);
        CHECK(added == 0);
    }

    // 32. Speckled-mask ants read as dash confetti at high zoom (and
    // vanish on deselect): blank doc, noisy mask selection, no paint at
    // all. Overlay-only pixels (dark screen over clean composite, ring
    // zone excluded) spike with the selection and return to the exact
    // baseline without it — the headless Ctrl+D experiment.
    {
        state.addDocument(QStringLiteral("antsconfetti"), QSize(200, 150),
                          300);
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 10);
        state.setActiveTool(ToolId::Brush);
        // Deterministic speckle: isolated dots + a few short streaks.
        QImage mask(200, 150, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 0; y < 150; ++y) {
            uchar* row = mask.scanLine(y);
            for (int x = 0; x < 200; ++x) {
                if ((x * 7 + y * 13) % 17 == 0) row[x] = 255;
                if ((x + 2 * y) % 61 == 0) row[x] = 255;
            }
        }
        // Selection goes on after the baseline grab below, so the task
        // bar caught in the grab cancels out of every comparison.
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(6.82);
        app.processEvents();
        // Park the ring in the corner, then count overlay-only pixels.
        // Baseline with no selection: the floating task bar is a child
        // widget caught in the grab, so it counts here too and must
        // cancel out of every comparison.
        const QPointF global =
            canvas->viewport()->mapToGlobal(QPoint(10, 10));
        auto viewAt = [&](const QPointF& docPt) {
            return canvas->documentToView(docPt);
        };
        auto hoverAt = [&](const QPointF& docPt) {
            QMouseEvent move(QEvent::MouseMove, viewAt(docPt), global,
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            app.processEvents();
        };
        hoverAt(QPointF(195, 145));
        const QPointF ringView = viewAt(QPointF(195, 145));
        auto overlayPixels = [&]() {
            const QImage seen = canvas->viewport()->grab().toImage();
            int n = 0;
            for (int y = 0; y < seen.height(); ++y) {
                for (int x = 0; x < seen.width(); ++x) {
                    if (std::hypot(x - ringView.x(), y - ringView.y()) <
                        80.0)
                        continue;
                    if (qGray(seen.pixel(x, y)) < 200) ++n;
                }
            }
            return n;
        };
        const int baseline = overlayPixels();
        state.setSelectionMask(mask);
        app.processEvents();
        CHECK(!state.activeDocument()->selection.isEmpty());
        const int withSelection = overlayPixels();
        std::printf("[viewq] ants overlay px baseline=%d selected=%d\n",
                    baseline, withSelection);
        CHECK(withSelection > baseline + 50);  // dash confetti draws
        // Deselect: the confetti must go with it (the Ctrl+D experiment).
        state.setSelectionMask(QImage());
        app.processEvents();
        CHECK(state.activeDocument()->selection.isEmpty());
        const int deselected = overlayPixels();
        std::printf("[viewq] ants overlay px after deselect: %d\n",
                    deselected);
        CHECK(deselected == baseline);
    }

    // 33. Extras hides the selection edges but keeps the brush cursor:
    // speckled selection with Extras off draws no ants (clean view, live
    // selection), while the ring still tracks the pointer.
    {
        state.addDocument(QStringLiteral("extrasants"), QSize(200, 150),
                          300);
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 10);
        state.setActiveTool(ToolId::Brush);
        QImage mask(200, 150, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 0; y < 150; ++y) {
            uchar* row = mask.scanLine(y);
            for (int x = 0; x < 200; ++x) {
                if ((x * 7 + y * 13) % 17 == 0) row[x] = 255;
                if ((x + 2 * y) % 61 == 0) row[x] = 255;
            }
        }
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(6.82);
        app.processEvents();
        const QPointF global =
            canvas->viewport()->mapToGlobal(QPoint(10, 10));
        auto viewAt = [&](const QPointF& docPt) {
            return canvas->documentToView(docPt);
        };
        QMouseEvent hover(QEvent::MouseMove, viewAt(QPointF(100, 75)),
                          global, Qt::NoButton, Qt::NoButton,
                          Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &hover);
        app.processEvents();
        // Ring parked center-screen (visible); the floating task bar
        // band is excluded from every count (it is chrome, and it
        // restyles itself when a selection exists).
        const QPointF ringView = viewAt(QPointF(100, 75));
        auto darkOutsideRing = [&]() {
            const QImage seen = canvas->viewport()->grab().toImage();
            int n = 0;
            for (int y = 0; y < seen.height() - 70; ++y) {
                for (int x = 0; x < seen.width(); ++x) {
                    if (std::hypot(x - ringView.x(), y - ringView.y()) <
                        80.0)
                        continue;
                    if (qGray(seen.pixel(x, y)) < 200) ++n;
                }
            }
            return n;
        };
        auto ringDrawn = [&]() {
            const QImage seen = canvas->viewport()->grab().toImage();
            for (int y = 0; y < seen.height() - 70; ++y) {
                for (int x = 0; x < seen.width(); ++x) {
                    if (std::hypot(x - ringView.x(), y - ringView.y()) <
                        80.0) {
                        if (qGray(seen.pixel(x, y)) < 200) return true;
                    }
                }
            }
            return false;
        };
        const int baseline = darkOutsideRing();
        std::printf("[viewq] extras stages baseline=%d ringview=(%.0f,%.0f)\n",
                    baseline, ringView.x(), ringView.y());
        CHECK(ringDrawn());  // cursor tracks with Extras on
        state.setSelectionMask(mask);
        app.processEvents();
        CHECK(!state.activeDocument()->selection.isEmpty());
        CHECK(darkOutsideRing() > baseline + 50);  // ants on: confetti
        // Extras off: edges gone, selection live, cursor alive.
        canvas->setExtrasVisible(false);
        app.processEvents();
        {
            const int off = darkOutsideRing();
            std::printf("[viewq] extras off dark=%d (baseline=%d)\n", off,
                        baseline);
        }
        CHECK(darkOutsideRing() == baseline);
        CHECK(ringDrawn());
        CHECK(!state.activeDocument()->selection.isEmpty());
        // And back on: the edges return.
        canvas->setExtrasVisible(true);
        app.processEvents();
        CHECK(darkOutsideRing() > baseline + 50);
    }

    // 34. New stylus responses: fade taper, darken-to-black, PressureIn
    // hold, and the speed-size curve math.
    {
        // Pure math first.
        CHECK_NEAR(CanvasView::speedSizeFactor(0.0, 100.0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::speedSizeFactor(1.0, 100.0), 0.25, 1e-9);
        CHECK_NEAR(CanvasView::speedSizeFactor(0.5, 0.0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::speedSizeFactor(2.0, 100.0), 0.25, 1e-9);

        auto darkBand = [&](DocumentItem* dd, int x0, int x1) {
            int w = 0;
            for (int x = x0; x <= x1; ++x) {
                for (int y = 55; y <= 95; ++y) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if ((c.red() + c.green() + c.blue()) / 3 < 128) {
                        ++w;
                        break;
                    }
                }
            }
            return w;
        };
        // Fade: a 120 px line with a 120 px taper carries ink at the
        // head and nothing at the tail.
        {
            state.addDocument(QStringLiteral("fadetaper"), QSize(200, 150),
                              300);
            state.setForeground(QColor(0, 0, 0));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 40);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"),
                            15);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"),
                            QStringLiteral("buildup"));
            state.setOption(ToolId::Brush, QStringLiteral("brush_fade"),
                            120);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(40, 75));
            const QPointF b = canvas->documentToView(QPointF(160, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            DocumentItem* dd = state.activeDocument();
            const int head = darkBand(dd, 40, 70);
            const int tail = darkBand(dd, 130, 160);
            CHECK(head > 20);
            CHECK(tail * 4 < head);
            state.setOption(ToolId::Brush, QStringLiteral("brush_fade"), 0);
        }
        // Darken: full darken at full pressure paints black from red.
        {
            state.addDocument(QStringLiteral("darkendab"), QSize(200, 150),
                              300);
            state.setForeground(QColor(200, 50, 50));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 40);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"),
                            QStringLiteral("buildup"));
            state.setOption(ToolId::Brush, QStringLiteral("brush_darken"),
                            100);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(100, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent release(QEvent::MouseButtonRelease, a, global,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            const QColor c =
                state.activeDocument()->composite.pixelColor(100, 75);
            CHECK(c.red() < 60 && c.green() < 40 && c.blue() < 40);
            state.setOption(ToolId::Brush, QStringLiteral("brush_darken"),
                            0);
            state.setForeground(QColor(0, 0, 0));
        }
        // PressureIn: a 1.0→0.2 ramp keeps full width with hold on,
        // tapers thin with hold off (hold tracks the running maximum).
        auto rampEndWidth = [&](const QString& docName, bool hold) {
            state.addDocument(docName, QSize(200, 150), 300);
            state.setForeground(QColor(0, 0, 0));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 30);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"),
                            15);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"),
                            QStringLiteral("buildup"));
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_pressure_in"), hold);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(40, 75));
            const QPointF b = canvas->documentToView(QPointF(160, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            static QPointingDevice stylus;
            auto tabletAt = [&](QEvent::Type type, const QPointF& p,
                                double pressure, Qt::MouseButtons buttons) {
                QTabletEvent e(type, &stylus, p, global, pressure, 0, 0,
                               0.0, 0.0, 0, Qt::NoModifier, Qt::LeftButton,
                               buttons);
                QApplication::sendEvent(canvas->viewport(), &e);
            };
            tabletAt(QEvent::TabletPress, a, 1.0, Qt::LeftButton);
            for (int i = 1; i <= 5; ++i) {
                const QPointF p = a + (b - a) * (i / 5.0);
                tabletAt(QEvent::TabletMove, p, 1.0 - 0.8 * (i / 5.0),
                         Qt::LeftButton);
            }
            tabletAt(QEvent::TabletRelease, b, 0.2, Qt::NoButton);
            app.processEvents();
            // Vertical ink extent at x=145 (dab diameter, not horizontal
            // reach: faint dabs still pile opaque along the stroke in
            // buildup, but stay thin).
            DocumentItem* dd = state.activeDocument();
            int w = 0;
            for (int y = 55; y <= 95; ++y) {
                const QColor c = dd->composite.pixelColor(145, y);
                if ((c.red() + c.green() + c.blue()) / 3 < 128) ++w;
            }
            return w;
        };
        const int holdW =
            rampEndWidth(QStringLiteral("pressurehold"), true);
        const int liveW =
            rampEndWidth(QStringLiteral("pressurelive"), false);
        CHECK(holdW > liveW + 10);
        state.setOption(ToolId::Brush, QStringLiteral("brush_pressure_in"),
                        false);

        // Pure sensor math (no device needed).
        CHECK_NEAR(CanvasView::fuzzyFactor(0.5, 100.0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::fuzzyFactor(0.0, 100.0), 0.0, 1e-9);
        CHECK_NEAR(CanvasView::fuzzyFactor(1.0, 100.0), 2.0, 1e-9);
        CHECK_NEAR(CanvasView::fuzzyFactor(0.3, 0.0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::hash01(12345),
                   CanvasView::hash01(12345), 1e-12);
        CHECK(CanvasView::hash01(12345) >= 0.0);
        CHECK(CanvasView::hash01(12345) < 1.0);
        CHECK_NEAR(CanvasView::timeFadeFactorMs(0, 5.0), 1.0, 1e-9);
        CHECK_NEAR(CanvasView::timeFadeFactorMs(2500, 5.0), 0.5, 1e-9);
        CHECK_NEAR(CanvasView::timeFadeFactorMs(5000, 5.0), 0.0, 1e-9);
        CHECK_NEAR(CanvasView::timeFadeFactorMs(9000, 5.0), 0.0, 1e-9);
        CHECK_NEAR(CanvasView::timeFadeFactorMs(9999, 0.0), 1.0, 1e-9);

        // Tilt-axis split: X lean widens only with tilt-x amount (Y same).
        auto leanDabWidth = [&](const QString& docName, double xt,
                                double yt) {
            state.addDocument(docName, QSize(200, 150), 300);
            state.setForeground(QColor(0, 0, 0));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 24);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"),
                            QStringLiteral("buildup"));
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(100, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            static QPointingDevice stylus;
            auto tabletAt = [&](QEvent::Type type, Qt::MouseButtons buttons) {
                QTabletEvent e(type, &stylus, a, global, 1.0, xt, yt, 0.0,
                               0.0, 0, Qt::NoModifier, Qt::LeftButton,
                               buttons);
                QApplication::sendEvent(canvas->viewport(), &e);
            };
            tabletAt(QEvent::TabletPress, Qt::LeftButton);
            tabletAt(QEvent::TabletRelease, Qt::NoButton);
            app.processEvents();
            DocumentItem* dd = state.activeDocument();
            int x0 = 200, x1 = -1;
            for (int x = 0; x < 200; ++x)
                for (int y = 0; y < 150; ++y) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if ((c.red() + c.green() + c.blue()) / 3 < 128) {
                        x0 = std::min(x0, x);
                        x1 = std::max(x1, x);
                    }
                }
            return x1 >= x0 ? x1 - x0 + 1 : 0;
        };
        state.setOption(ToolId::Brush, QStringLiteral("brush_tiltx_size"),
                        100);
        CHECK(leanDabWidth(QStringLiteral("tiltxup"), 0, 0) >= 20);
        CHECK(leanDabWidth(QStringLiteral("tiltxlean"), 60, 0) >= 40);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tiltx_size"),
                        0);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilty_size"),
                        100);
        CHECK(leanDabWidth(QStringLiteral("tiltylean"), 0, 60) >= 40);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilty_size"),
                        0);

        // Time fade: a stroke 10 s after press (deterministic event
        // timestamps) lays only its press dab.
        {
            state.addDocument(QStringLiteral("timefadetaper"),
                              QSize(200, 150), 300);
            state.setForeground(QColor(0, 0, 0));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 40);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"),
                            15);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"),
                            QStringLiteral("buildup"));
            state.setOption(ToolId::Brush, QStringLiteral("brush_timefade"),
                            6);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(40, 75));
            const QPointF b = canvas->documentToView(QPointF(160, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            press.setTimestamp(1000000);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            move.setTimestamp(1000000 + 10000);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            release.setTimestamp(1000000 + 10000);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            CHECK(darkBand(state.activeDocument(), 40, 160) < 45);
            state.setOption(ToolId::Brush, QStringLiteral("brush_timefade"),
                            0);
        }

        // Fuzzy-stroke clock: a pinned seed scales the whole dab
        // deterministically (seed 7 lands off-center here).
        {
            auto fuzzyDabWidth = [&](const QString& docName, double amt) {
                state.addDocument(docName, QSize(200, 150), 300);
                state.setForeground(QColor(0, 0, 0));
                state.setOption(ToolId::Brush, QStringLiteral("brush_size"),
                                40);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_hardness"), 100);
                state.setOption(ToolId::Brush, QStringLiteral("opacity"),
                                100);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_painting_mode"),
                                QStringLiteral("buildup"));
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_fuzzy_size"), amt);
                state.pinStrokeSeed(7);
                QWidget window;
                auto* canvas = new CanvasView(&state, &window);
                window.resize(800, 600);
                canvas->setGeometry(0, 0, 800, 600);
                window.show();
                canvas->zoomToFit();
                canvas->setZoom(4.0);
                app.processEvents();
                state.setActiveTool(ToolId::Brush);
                app.processEvents();
                const QPointF a = canvas->documentToView(QPointF(100, 75));
                const QPointF global =
                    canvas->viewport()->mapToGlobal(a.toPoint());
                QMouseEvent press(QEvent::MouseButtonPress, a, global,
                                  Qt::LeftButton, Qt::LeftButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &press);
                QMouseEvent release(QEvent::MouseButtonRelease, a, global,
                                    Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &release);
                app.processEvents();
                state.pinStrokeSeed(0);
                DocumentItem* dd = state.activeDocument();
                int x0 = 200, x1 = -1;
                for (int x = 0; x < 200; ++x)
                    for (int y = 0; y < 150; ++y) {
                        const QColor c = dd->composite.pixelColor(x, y);
                        if ((c.red() + c.green() + c.blue()) / 3 < 128) {
                            x0 = std::min(x0, x);
                            x1 = std::max(x1, x);
                        }
                    }
                return x1 >= x0 ? x1 - x0 + 1 : 0;
            };
            const int plain = fuzzyDabWidth(QStringLiteral("fuzzyoff"), 0);
            const int fuzzy =
                fuzzyDabWidth(QStringLiteral("fuzzyon"), 100);
            CHECK(plain >= 35);
            CHECK(std::abs(fuzzy - plain) > 5);
            state.setOption(ToolId::Brush, QStringLiteral("brush_fuzzy_size"),
                            0);
        }

        // Perspective: VP pinned to the corner shrinks near dabs.
        {
            auto vpDabWidth = [&](const QString& docName, double x) {
                state.addDocument(docName, QSize(200, 150), 300);
                state.setForeground(QColor(0, 0, 0));
                state.setOption(ToolId::Brush, QStringLiteral("brush_size"),
                                48);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_hardness"), 100);
                state.setOption(ToolId::Brush, QStringLiteral("opacity"),
                                100);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_painting_mode"),
                                QStringLiteral("buildup"));
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_perspective"), 100);
                state.setOption(ToolId::Brush, QStringLiteral("brush_vp_x"),
                                0);
                state.setOption(ToolId::Brush, QStringLiteral("brush_vp_y"),
                                0);
                QWidget window;
                auto* canvas = new CanvasView(&state, &window);
                window.resize(800, 600);
                canvas->setGeometry(0, 0, 800, 600);
                window.show();
                canvas->zoomToFit();
                canvas->setZoom(4.0);
                app.processEvents();
                state.setActiveTool(ToolId::Brush);
                app.processEvents();
                const QPointF a = canvas->documentToView(QPointF(x, 75));
                const QPointF global =
                    canvas->viewport()->mapToGlobal(a.toPoint());
                QMouseEvent press(QEvent::MouseButtonPress, a, global,
                                  Qt::LeftButton, Qt::LeftButton,
                                  Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &press);
                QMouseEvent release(QEvent::MouseButtonRelease, a, global,
                                    Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &release);
                app.processEvents();
                DocumentItem* dd = state.activeDocument();
                int x0 = 200, x1 = -1;
                for (int xx = 0; xx < 200; ++xx)
                    for (int y = 0; y < 150; ++y) {
                        const QColor c = dd->composite.pixelColor(xx, y);
                        if ((c.red() + c.green() + c.blue()) / 3 < 128) {
                            x0 = std::min(x0, xx);
                            x1 = std::max(x1, xx);
                        }
                    }
                return x1 >= x0 ? x1 - x0 + 1 : 0;
            };
            const int far = vpDabWidth(QStringLiteral("vpfar"), 160);
            const int near = vpDabWidth(QStringLiteral("vpnear"), 10);
            CHECK(far >= 28);
            CHECK(far > near + 12);
            state.setOption(ToolId::Brush, QStringLiteral("brush_perspective"),
                            0);
            state.setOption(ToolId::Brush, QStringLiteral("brush_vp_x"), -1);
            state.setOption(ToolId::Brush, QStringLiteral("brush_vp_y"), -1);
        }

        // Stroke gradient: FG red bleeding to BG blue over 120 px.
        {
            state.addDocument(QStringLiteral("strokegradient"),
                              QSize(200, 150), 300);
            state.setForeground(QColor(200, 50, 50));
            state.setBackground(QColor(50, 50, 200));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 40);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"),
                            15);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"),
                            QStringLiteral("buildup"));
            state.setOption(ToolId::Brush, QStringLiteral("brush_source"),
                            3);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_gradient_len"), 120);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(40, 75));
            const QPointF b = canvas->documentToView(QPointF(160, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            DocumentItem* dd = state.activeDocument();
            const QColor head = dd->composite.pixelColor(50, 75);
            const QColor tail = dd->composite.pixelColor(150, 75);
            CHECK(head.red() > 120 && head.red() > head.blue());
            CHECK(tail.blue() > 120 && tail.blue() > tail.red());
            state.setOption(ToolId::Brush, QStringLiteral("brush_source"),
                            0);
            state.setForeground(QColor(0, 0, 0));
            state.setBackground(QColor(255, 255, 255));
        }

        // Texture pressure: grain thins with the dab at low pressure.
        {
            auto grainyInk = [&](const QString& docName, bool toggle) {
                state.addDocument(docName, QSize(200, 150), 300);
                state.setForeground(QColor(0, 0, 0));
                state.setOption(ToolId::Brush, QStringLiteral("brush_size"),
                                30);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_hardness"), 100);
                state.setOption(ToolId::Brush, QStringLiteral("opacity"),
                                100);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_painting_mode"),
                                QStringLiteral("buildup"));
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_texture_pressure"),
                                toggle);
                QWidget window;
                auto* canvas = new CanvasView(&state, &window);
                window.resize(800, 600);
                canvas->setGeometry(0, 0, 800, 600);
                window.show();
                canvas->zoomToFit();
                canvas->setZoom(4.0);
                app.processEvents();
                state.setActiveTool(ToolId::Brush);
                app.processEvents();
                const QPointF a = canvas->documentToView(QPointF(40, 75));
                const QPointF b = canvas->documentToView(QPointF(160, 75));
                const QPointF global =
                    canvas->viewport()->mapToGlobal(a.toPoint());
                static QPointingDevice stylus;
                auto tabletAt = [&](QEvent::Type type, const QPointF& p,
                                    Qt::MouseButtons buttons) {
                    QTabletEvent e(type, &stylus, p, global, 0.3, 0, 0,
                                   0.0, 0.0, 0, Qt::NoModifier,
                                   Qt::LeftButton, buttons);
                    QApplication::sendEvent(canvas->viewport(), &e);
                };
                tabletAt(QEvent::TabletPress, a, Qt::LeftButton);
                tabletAt(QEvent::TabletMove, b, Qt::LeftButton);
                tabletAt(QEvent::TabletRelease, b, Qt::NoButton);
                app.processEvents();
                DocumentItem* dd = state.activeDocument();
                int ink = 0;
                for (int y = 0; y < 150; ++y)
                    for (int x = 0; x < 200; ++x) {
                        const QColor c = dd->composite.pixelColor(x, y);
                        if ((c.red() + c.green() + c.blue()) / 3 < 128)
                            ++ink;
                    }
                return ink;
            };
            AppState::PatternGray checker;
            checker.w = 2;
            checker.h = 2;
            checker.gray = {0.0f, 1.0f, 1.0f, 0.0f};
            state.setBrushPattern(QStringLiteral("checker2"), checker);
            state.setOption(ToolId::Brush, QStringLiteral("brush_texture"),
                            QStringLiteral("checker2"));
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_texture_strength"), 100);
            const int fullGrain =
                grainyInk(QStringLiteral("grainpressoff"), false);
            const int thinGrain =
                grainyInk(QStringLiteral("grainpresson"), true);
            CHECK(fullGrain > 100);
            CHECK(thinGrain > fullGrain);  // less breakup at low pressure
            state.setOption(ToolId::Brush, QStringLiteral("brush_texture"),
                            QString());
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_texture_pressure"), false);
        }

        // Mask pressure: the mask window shrinks with the dab.
        {
            pittore::compute::StampTip solid;
            solid.w = 8;
            solid.h = 8;
            solid.color = false;
            solid.alpha.assign(64, 1.0f);
            state.setBrushStamp(QStringLiteral("testmask8"), solid);
            CHECK(state.brushStamp(QStringLiteral("testmask8")) != nullptr);
            auto maskInk = [&](const QString& docName, bool toggle) {
                state.addDocument(docName, QSize(200, 150), 300);
                state.setForeground(QColor(0, 0, 0));
                state.setOption(ToolId::Brush, QStringLiteral("brush_size"),
                                40);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_hardness"), 100);
                state.setOption(ToolId::Brush, QStringLiteral("opacity"),
                                100);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_painting_mode"),
                                QStringLiteral("buildup"));
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_mask_stamp"),
                                QStringLiteral("testmask8"));
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_mask_ratio"), 100);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_mask_pressure"),
                                toggle);
                QWidget window;
                auto* canvas = new CanvasView(&state, &window);
                window.resize(800, 600);
                canvas->setGeometry(0, 0, 800, 600);
                window.show();
                canvas->zoomToFit();
                canvas->setZoom(4.0);
                app.processEvents();
                state.setActiveTool(ToolId::Brush);
                app.processEvents();
                const QPointF a = canvas->documentToView(QPointF(80, 75));
                const QPointF b = canvas->documentToView(QPointF(120, 75));
                const QPointF global =
                    canvas->viewport()->mapToGlobal(a.toPoint());
                static QPointingDevice stylus;
                auto tabletAt = [&](QEvent::Type type, const QPointF& p,
                                    Qt::MouseButtons buttons) {
                    QTabletEvent e(type, &stylus, p, global, 0.3, 0, 0,
                                   0.0, 0.0, 0, Qt::NoModifier,
                                   Qt::LeftButton, buttons);
                    QApplication::sendEvent(canvas->viewport(), &e);
                };
                tabletAt(QEvent::TabletPress, a, Qt::LeftButton);
                tabletAt(QEvent::TabletMove, b, Qt::LeftButton);
                tabletAt(QEvent::TabletRelease, b, Qt::NoButton);
                app.processEvents();
                DocumentItem* dd = state.activeDocument();
                int ink = 0;
                for (int y = 0; y < 150; ++y)
                    for (int x = 0; x < 200; ++x) {
                        const QColor c = dd->composite.pixelColor(x, y);
                        if ((c.red() + c.green() + c.blue()) / 3 < 128)
                            ++ink;
                    }
                return ink;
            };
            const int fullMask = maskInk(QStringLiteral("maskpressoff"),
                                         false);
            const int thinMask = maskInk(QStringLiteral("maskpresson"),
                                         true);
            CHECK(fullMask > 50);
            CHECK(thinMask * 2 < fullMask);
            state.setOption(ToolId::Brush, QStringLiteral("brush_mask_stamp"),
                            QString());
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_mask_pressure"), false);
        }

        // Smudge pressure: the toggle throttles the rate (relative
        // comparison; dry-brush pickup dilutes toward paper).
        {
            auto smudgeMean = [&](const QString& docName, bool toggle) {
                state.addDocument(docName, QSize(200, 150), 300);
                state.setForeground(QColor(0, 0, 0));
                state.setOption(ToolId::Brush, QStringLiteral("brush_size"),
                                60);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_hardness"), 100);
                state.setOption(ToolId::Brush, QStringLiteral("opacity"),
                                100);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_painting_mode"),
                                QStringLiteral("buildup"));
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_engine"),
                                QStringLiteral("smudge"));
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_smudge_rate"), 100);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_smudge_radius"), 100);
                state.setOption(ToolId::Brush,
                                QStringLiteral("brush_smudge_pressure"),
                                toggle);
                state.paintDab(QPointF(50, 75), 30, 1.0, 1.0,
                               QColor(0, 0, 0));
                state.flushPaint();
                app.processEvents();
                QWidget window;
                auto* canvas = new CanvasView(&state, &window);
                window.resize(800, 600);
                canvas->setGeometry(0, 0, 800, 600);
                window.show();
                canvas->zoomToFit();
                canvas->setZoom(4.0);
                app.processEvents();
                state.setActiveTool(ToolId::Brush);
                app.processEvents();
                const QPointF global =
                    canvas->viewport()->mapToGlobal(QPoint(10, 10));
                auto viewAt = [&](const QPointF& docPt) {
                    return canvas->documentToView(docPt);
                };
                static QPointingDevice stylus;
                auto tabletAt = [&](QEvent::Type type, const QPointF& p,
                                    Qt::MouseButtons buttons) {
                    QTabletEvent e(type, &stylus, p, global, 0.6, 0, 0,
                                   0.0, 0.0, 0, Qt::NoModifier,
                                   Qt::LeftButton, buttons);
                    QApplication::sendEvent(canvas->viewport(), &e);
                };
                tabletAt(QEvent::TabletPress, viewAt(QPointF(50, 75)),
                         Qt::LeftButton);
                tabletAt(QEvent::TabletMove, viewAt(QPointF(100, 75)),
                         Qt::LeftButton);
                tabletAt(QEvent::TabletMove, viewAt(QPointF(150, 75)),
                         Qt::LeftButton);
                tabletAt(QEvent::TabletRelease, viewAt(QPointF(150, 75)),
                         Qt::NoButton);
                app.processEvents();
                DocumentItem* dd = state.activeDocument();
                double sum = 0;
                int n = 0;
                for (int y = 60; y < 90; ++y)
                    for (int x = 95; x < 175; ++x) {
                        const QColor c = dd->composite.pixelColor(x, y);
                        sum += (c.red() + c.green() + c.blue()) / 3.0;
                        ++n;
                    }
                return sum / n;
            };
            const double fullMean =
                smudgeMean(QStringLiteral("smudgepressoff"), false);
            const double thinMean =
                smudgeMean(QStringLiteral("smudgepresson"), true);
            CHECK(fullMean < 245.0);
            CHECK(thinMean > fullMean + 5.0);
            state.setOption(ToolId::Brush, QStringLiteral("brush_engine"),
                            QString());
            state.setOption(ToolId::Brush, QStringLiteral("brush_smudge_pressure"), false);
        }
    }

    // 37. Tablet mode: the tool strip grows pen-sized tap targets and
    // shrinks back, persisted through settings.
    {
        QWidget window;
        auto* tools = new ToolsPanel(&state, &window);
        tools->show();
        app.processEvents();
        auto stripWidth = [&]() {
            // Rebuilds hide-then-deleteLater, and the headless parent is
            // never shown: read fixed sizes (what rebuild wrote), taking
            // the max over live (visible-or-fresh) tool buttons.
            int w = 0;
            const auto buttons = tools->findChildren<QToolButton*>();
            for (QToolButton* b : buttons) {
                if (!b->isHidden() && b->property("toolId").isValid())
                    w = std::max(w, b->minimumWidth());
            }
            return w;
        };
        CHECK(!state.tabletMode());
        CHECK(stripWidth() == 26);
        state.setTabletMode(true);
        app.processEvents();
        CHECK(state.tabletMode());
        CHECK(stripWidth() == 40);
        CHECK(state.settings().tabletMode);
        state.setTabletMode(false);
        app.processEvents();
        CHECK(stripWidth() == 26);
    }

    // 35. Sticky locks + pixel-snap stabilizer.
    {
        namespace bl = pittore::ui::brushlibrary;
        // Locks: a locked option survives preset application.
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 99);
        state.setOption(ToolId::Brush, QStringLiteral("opacity"), 50);
        state.setOption(ToolId::Brush, QStringLiteral("brush_texture"),
                        QStringLiteral("keep.png"));
        state.setOption(ToolId::Brush, QStringLiteral("lock_size"), true);
        state.setOption(ToolId::Brush, QStringLiteral("lock_opacity"),
                        true);
        state.setOption(ToolId::Brush, QStringLiteral("lock_texture"),
                        true);
        bl::BrushPreset p;
        p.name = QStringLiteral("LockProbe");
        p.size = 42.0;
        p.opacity = 100.0;
        p.textureFile = QStringLiteral("grain.png");
        p.hardness = 80.0;
        CHECK(bl::applyPreset(&state, p) == bl::ApplyResult::Applied);
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("brush_size"))
                       .toDouble(),
                   99.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("opacity"))
                       .toDouble(),
                   50.0, 1e-9);
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_texture"))
                  .toString() == QStringLiteral("keep.png"));
        // Unlocked fields still apply.
        CHECK_NEAR(state.option(ToolId::Brush,
                                QStringLiteral("brush_hardness"))
                       .toDouble(),
                   80.0, 1e-9);
        state.setOption(ToolId::Brush, QStringLiteral("lock_size"), false);
        state.setOption(ToolId::Brush, QStringLiteral("lock_opacity"),
                        false);
        state.setOption(ToolId::Brush, QStringLiteral("lock_texture"),
                        false);

        // Pixel snap: smoothing 100 toward a fractional point lands on
        // whole pixels in mode 2, off-grid in classic mode.
        auto snappedStroke = [&](const QString& docName, int mode) {
            state.addDocument(docName, QSize(200, 150), 300);
            state.setForeground(QColor(0, 0, 0));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 12);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"),
                            QStringLiteral("buildup"));
            state.setOption(ToolId::Brush, QStringLiteral("smoothing"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("smoothing_mode"),
                            mode);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(100.4, 75.2));
            const QPointF b = canvas->documentToView(QPointF(101.1, 75.5));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            return state.activeDocument()->composite.copy();
        };
        const QImage classic =
            snappedStroke(QStringLiteral("smoothclassic"), 0);
        const QImage pixel =
            snappedStroke(QStringLiteral("smoothpixel"), 2);
        auto inked = [&](const QImage& img) {
            int n = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if ((c.red() + c.green() + c.blue()) / 3 < 128) ++n;
                }
            return n;
        };
        CHECK(inked(classic) > 10);
        CHECK(inked(pixel) > 10);
        int diff = 0;
        for (int y = 0; y < classic.height(); ++y)
            for (int x = 0; x < classic.width(); ++x)
                if (classic.pixel(x, y) != pixel.pixel(x, y)) ++diff;
        CHECK(diff > 0);  // snap really moves the dab
        state.setOption(ToolId::Brush, QStringLiteral("smoothing"), 0);
        state.setOption(ToolId::Brush, QStringLiteral("smoothing_mode"),
                        0);
    }

    // 36. Drawing eraser: erase blend keeps the live Brush tip (round
    // trip + apply + stroke that removes instead of paints), and the
    // stylus eraser end flips the toggle without swapping tools.
    {
        namespace bl = pittore::ui::brushlibrary;
        bl::BrushPreset p;
        p.name = QStringLiteral("EraserBlendProbe");
        p.eraserBlend = true;
        bool ok = false;
        bl::BrushPreset q = bl::BrushPreset::fromJson(p.toJson(), &ok);
        CHECK(ok);
        CHECK(q.eraserBlend);
        state.setActiveTool(ToolId::Brush);
        CHECK(bl::applyPreset(&state, q) == bl::ApplyResult::Applied);
        CHECK(state.option(ToolId::Brush,
                           QStringLiteral("brush_erase_blend"))
                  .toBool());

        // Pre-painted block, then an erase-blend stroke across it.
        auto eraseStrokeInk = [&](const QString& docName, bool erase) {
            state.addDocument(docName, QSize(200, 150), 300);
            state.setForeground(QColor(0, 0, 0));
            state.setOption(ToolId::Brush, QStringLiteral("brush_size"),
                            40);
            state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"),
                            100);
            state.setOption(ToolId::Brush, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_painting_mode"),
                            QStringLiteral("buildup"));
            state.setOption(ToolId::Brush,
                            QStringLiteral("brush_erase_blend"), erase);
            state.paintDab(QPointF(100, 75), 30, 1.0, 1.0,
                           QColor(0, 0, 0));
            state.flushPaint();
            app.processEvents();
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(60, 75));
            const QPointF b = canvas->documentToView(QPointF(140, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            DocumentItem* dd = state.activeDocument();
            int ink = 0;
            for (int y = 0; y < 150; ++y)
                for (int x = 0; x < 200; ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    // Alpha-aware: erased-to-transparent reads black
                    // in RGB but carries no ink.
                    if (c.alpha() > 128 &&
                        (c.red() + c.green() + c.blue()) / 3 < 128)
                        ++ink;
                }
            return ink;
        };
        const int painted = eraseStrokeInk(QStringLiteral("eraseoff"),
                                           false);
        const int erased = eraseStrokeInk(QStringLiteral("eraseon"), true);
        std::printf("[viewq] eraser painted=%d erased=%d\n", painted,
                    erased);
        CHECK(painted > 1000);
        CHECK(erased * 3 < painted);  // block largely removed
        // Background rule: the carved area is white opaque, never a
        // transparency hole.
        {
            const QColor c = state.activeDocument()->composite.pixelColor(
                100, 75);
            CHECK(c.alpha() > 200);
            CHECK((c.red() + c.green() + c.blue()) / 3 > 200);
        }
        // Direct eraseDab control (bypasses canvas/tablet entirely).
        state.addDocument(QStringLiteral("erasedirect"), QSize(200, 150),
                          300);
        state.setForeground(QColor(0, 0, 0));
        state.paintDab(QPointF(100, 75), 30, 1.0, 1.0, QColor(0, 0, 0));
        state.flushPaint();
        app.processEvents();
        CHECK(state.eraseDab(QPointF(100, 75), 20, 1.0, 1.0, 1.0, 0.0,
                             false));
        state.flushPaint();
        app.processEvents();
        {
            DocumentItem* dd = state.activeDocument();
            int ink = 0;
            for (int y = 0; y < 150; ++y)
                for (int x = 0; x < 200; ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if (c.alpha() > 128 &&
                        (c.red() + c.green() + c.blue()) / 3 < 128)
                        ++ink;
                }
            std::printf("[viewq] eraser direct ink=%d\n", ink);
            CHECK(ink < 1700);  // r30 disc minus r20 hole, plus AA fringe
            // ...and the hole is background white, not transparency.
            const QColor c = dd->composite.pixelColor(100, 75);
            CHECK(c.alpha() > 200);
            CHECK((c.red() + c.green() + c.blue()) / 3 > 200);
        }
        // Stamp eraser on Background paints white through the same tip.
        {
            const auto* tip =
                state.brushStamp(QStringLiteral("testmask8"));
            CHECK(tip != nullptr);
            state.addDocument(QStringLiteral("erasestampbg"),
                              QSize(200, 150), 300);
            state.setForeground(QColor(0, 0, 0));
            state.paintDab(QPointF(100, 75), 30, 1.0, 1.0,
                           QColor(0, 0, 0));
            state.flushPaint();
            app.processEvents();
            CHECK(state.stampTipEraseDab(QPointF(100, 75), 20, 1.0, 0.0,
                                         *tip, 0, 1));
            state.flushPaint();
            app.processEvents();
            const QColor c = state.activeDocument()->composite.pixelColor(
                100, 75);
            CHECK(c.alpha() > 200);
            CHECK((c.red() + c.green() + c.blue()) / 3 > 200);
        }
        // Canvas bottoms: white paper restores exactly, black paper
        // restores black, transparent bottoms punch alpha holes.
        {
            auto paperErase = [&](const QString& docName, QColor paper,
                                  bool transparent) {
                state.addDocument(docName, QSize(200, 150), 300);
                state.activeDocument()->canvasPaper = paper;
                state.activeDocument()->canvasTransparent = transparent;
                state.setForeground(QColor(0, 0, 0));
                state.paintDab(QPointF(100, 75), 30, 1.0, 1.0,
                               QColor(0, 0, 0));
                state.flushPaint();
                app.processEvents();
                CHECK(state.eraseDab(QPointF(100, 75), 20, 1.0, 1.0, 1.0,
                                     0.0, false));
                state.flushPaint();
                app.processEvents();
                return state.activeDocument()->composite.pixelColor(100,
                                                                    75);
            };
            const QColor white =
                paperErase(QStringLiteral("paperwhite"),
                           QColor(255, 255, 255), false);
            CHECK(white.alpha() > 200);
            CHECK(white.red() == 255 && white.green() == 255 &&
                  white.blue() == 255);
            const QColor black =
                paperErase(QStringLiteral("paperblack"), QColor(0, 0, 0),
                           false);
            CHECK(black.alpha() > 200);
            CHECK((black.red() + black.green() + black.blue()) / 3 < 40);
            const QColor hole =
                paperErase(QStringLiteral("papertransparent"),
                           QColor(255, 255, 255), true);
            CHECK(hole.alpha() < 128);
        }
        // deriveCanvasPaper buckets fresh opens: paper fills map
        // to their bucket (white/black/transparent); plain documents
        // keep the default gray without deriving.
        {
            state.addDocument(QStringLiteral("derivepaper"),
                              QSize(200, 150), 300);
            DocumentItem* dd = state.activeDocument();
            CHECK(!dd->canvasTransparent);
            CHECK(dd->canvasPaper == QColor(0xf2, 0xf2, 0xf2));
            LayerItem* bg = &dd->layers.last();
            bg->pixels = std::make_shared<pittore::Image>(200, 150);
            bg->pixels->fill(pittore::RGBAf{1, 1, 1, 1});
            state.deriveCanvasPaper(*dd);
            CHECK(!dd->canvasTransparent);
            CHECK(dd->canvasPaper == QColor(255, 255, 255));
            bg->pixels->data()[0] = pittore::RGBAf{0xf2 / 255.0f,
                                                    0xf2 / 255.0f,
                                                    0xf2 / 255.0f, 1.0f};
            state.deriveCanvasPaper(*dd);
            CHECK(!dd->canvasTransparent);
            CHECK(dd->canvasPaper == QColor(0xf2, 0xf2, 0xf2));
            bg->pixels->data()[0] = pittore::RGBAf{0, 0, 0, 0};
            state.deriveCanvasPaper(*dd);
            CHECK(dd->canvasTransparent);
            bg->pixels->data()[0] = pittore::RGBAf{0, 0, 0, 1};
            state.deriveCanvasPaper(*dd);
            CHECK(!dd->canvasTransparent);
            CHECK(dd->canvasPaper == QColor(0, 0, 0));
        }
        // Eraser tool with a custom stamp tip: full stroke carves.
        {
            state.addDocument(QStringLiteral("eraserstampstroke"),
                              QSize(200, 150), 300);
            state.setForeground(QColor(0, 0, 0));
            state.paintDab(QPointF(100, 75), 30, 1.0, 1.0,
                           QColor(0, 0, 0));
            state.flushPaint();
            app.processEvents();
            state.setOption(ToolId::Eraser, QStringLiteral("brush_size"),
                            40);
            state.setOption(ToolId::Eraser, QStringLiteral("opacity"), 100);
            state.setOption(ToolId::Eraser, QStringLiteral("brush_stamp"),
                            QStringLiteral("testmask8"));
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            canvas->setZoom(4.0);
            app.processEvents();
            state.setActiveTool(ToolId::Eraser);
            app.processEvents();
            const QPointF a = canvas->documentToView(QPointF(60, 75));
            const QPointF b = canvas->documentToView(QPointF(140, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &move);
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            DocumentItem* dd = state.activeDocument();
            int ink = 0;
            for (int y = 0; y < 150; ++y)
                for (int x = 0; x < 200; ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if (c.alpha() > 128 &&
                        (c.red() + c.green() + c.blue()) / 3 < 128)
                        ++ink;
                }
            std::printf("[viewq] eraser stamp stroke ink=%d\n", ink);
            CHECK(ink < 1500);
            state.setOption(ToolId::Eraser, QStringLiteral("brush_stamp"),
                            QString());
        }
        {
            state.addDocument(QStringLiteral("erasetransparent"),
                              QSize(200, 150), 300);
            LayerItem top;
            top.name = QStringLiteral("Top");
            top.kind = LayerItem::Kind::Pixel;
            state.addLayer(top);
            state.setForeground(QColor(0, 0, 0));
            state.paintDab(QPointF(100, 75), 30, 1.0, 1.0,
                           QColor(0, 0, 0));
            state.flushPaint();
            app.processEvents();
            CHECK(state.eraseDab(QPointF(100, 75), 20, 1.0, 1.0, 1.0, 0.0,
                                 false));
            state.flushPaint();
            app.processEvents();
            LayerItem* layer = state.activeLayer();
            CHECK(layer != nullptr);
            const pittore::RGBAf& px =
                layer->pixels->data()[75 * 200 + 100];
            CHECK(px.a < 0.5f);  // carved to transparent, not white
        }
        CHECK(painted > 1000);
        CHECK(erased * 3 < painted);  // block largely removed
        state.setOption(ToolId::Brush, QStringLiteral("brush_erase_blend"),
                        false);

        // Stylus eraser end on the Brush: same tool, toggle on, then off.
        {
            state.addDocument(QStringLiteral("styluseraserblend"),
                              QSize(200, 150), 300);
            QWidget window;
            auto* canvas = new CanvasView(&state, &window);
            window.resize(800, 600);
            canvas->setGeometry(0, 0, 800, 600);
            window.show();
            canvas->zoomToFit();
            app.processEvents();
            state.setActiveTool(ToolId::Brush);
            app.processEvents();
            CHECK(!state.option(ToolId::Brush,
                                QStringLiteral("brush_erase_blend"))
                       .toBool());
            QPointingDevice eraserDev(
                QStringLiteral("eraser"), 2,
                QInputDevice::DeviceType::Stylus,
                QPointingDevice::PointerType::Eraser,
                QPointingDevice::Capabilities(), 1, 1);
            const QPointF a = canvas->documentToView(QPointF(100, 75));
            const QPointF global =
                canvas->viewport()->mapToGlobal(a.toPoint());
            QTabletEvent press(QEvent::TabletPress, &eraserDev, a, global,
                               1.0, 0, 0, 0.0, 0.0, 0, Qt::NoModifier,
                               Qt::LeftButton, Qt::LeftButton);
            QApplication::sendEvent(canvas->viewport(), &press);
            app.processEvents();
            CHECK(state.activeTool() == ToolId::Brush);  // tip kept
            CHECK(state.option(ToolId::Brush,
                               QStringLiteral("brush_erase_blend"))
                      .toBool());
            QTabletEvent release(QEvent::TabletRelease, &eraserDev, a,
                                 global, 1.0, 0, 0, 0.0, 0.0, 0,
                                 Qt::NoModifier, Qt::LeftButton,
                                 Qt::NoButton);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
            CHECK(!state.option(ToolId::Brush,
                                QStringLiteral("brush_erase_blend"))
                       .toBool());
        }
    }

    // Brush cursor preferences (cursor + outline split): defaults hide the
    // OS cursor and draw the nib preview; the painted ring always lands
    // inside its dirty rect (shared geometry), so no fragments pool.
    {
        // Pin defaults first (the sandbox home persists across runs).
        state.setCursorShape(0);
        state.setOutlineShape(2);
        state.setShowOutlineWhilePainting(true);
        state.setOutlineEffectiveSize(false);
        AppSettings s0 = state.settings();
        CHECK(s0.cursorShape == 0);
        CHECK(s0.outlineShape == 2);
        CHECK(s0.showOutlineWhilePainting);
        CHECK(!s0.outlineEffectiveSize);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        canvas->setGeometry(0, 0, 800, 600);
        state.addDocument(QStringLiteral("cursorring"), QSize(200, 150),
                          300);
        state.setActiveTool(ToolId::VectorBrushTool);
        state.setOption(ToolId::VectorBrushTool,
                        QStringLiteral("brush_width"), 8.0);
        // Hover the centre: ring visible, dirty rect covers the paint.
        QMouseEvent hover(QEvent::MouseMove, QPointF(400, 300),
                          QPointF(400, 300), Qt::NoButton, Qt::NoButton,
                          Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &hover);
        app.processEvents();
        CHECK(canvas->brushCursorVisible());
        const QRectF dirty = canvas->brushCursorViewRect();
        CHECK(!dirty.isEmpty());
        // Rebuild the paint geometry independently and require containment:
        // brush_width 8 at zoom 1 => ~4px radius + 3px slack + AA.
        const QPointF cView = canvas->documentToView(
            canvas->cursorDocumentPosition());
        CHECK(dirty.contains(cView));
        CHECK(dirty.width() > 8.0 && dirty.width() < 40.0);
        // Outline off => no ring, no rect.
        state.setOutlineShape(0);
        CHECK(!canvas->brushCursorVisible());
        CHECK(canvas->brushCursorViewRect().isEmpty());
        // Circle outline on a square nib still draws (diameter only).
        state.setOutlineShape(1);
        state.setOption(ToolId::VectorBrushTool,
                        QStringLiteral("brush_tip"), 1);
        CHECK(canvas->brushCursorVisible());
        CHECK(!canvas->brushCursorViewRect().isEmpty());
        // Back to defaults for later suites.
        state.setOption(ToolId::VectorBrushTool,
                        QStringLiteral("brush_tip"), 0);
        state.setOutlineShape(2);
        // Setters clamp and persist through settings.
        state.setCursorShape(99);
        CHECK(state.cursorShape() == 2);
        state.setCursorShape(0);
        state.setOutlineShape(-1);
        CHECK(state.outlineShape() == 0);
        state.setOutlineShape(2);
        delete canvas;
    }

    // Smudge tool: full dirty-brush strokes — smear, finger paint, pure
    // (no foreground deposit), sample-all pickup, blend modes, one-step
    // undo. Drives the real press/move/release path headless.
    {
        using Kind = LayerItem::Kind;
        state.addDocument(QStringLiteral("smudgetool"), QSize(200, 150), 300);
        DocumentItem* dd = state.activeDocument();
        // Left half black, right half white, on the background.
        CHECK(dd->layers.size() == 1);
        LayerItem& bg = dd->layers[0];
        dd->rebuildComposite();
        CHECK(bg.pixels);
        for (std::uint32_t y = 0; y < bg.pixels->height(); ++y)
            for (std::uint32_t x = 0; x < bg.pixels->width(); ++x)
                if (x < bg.pixels->width() / 2)
                    bg.pixels->data()[std::size_t(y) * bg.pixels->width() +
                                      x] = pittore::RGBAf{0, 0, 0, 1};
        ++bg.sourceStamp;
        dd->rebuildComposite();
        auto compAt = [&](int x, int y) {
            return dd->composite.pixelColor(x, y);
        };
        CHECK(compAt(99, 75).red() < 128);  // setup sanity: edge placed
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        canvas->setGeometry(0, 0, 800, 600);
        canvas->zoomToFit();
        canvas->setZoom(1.0);
        app.processEvents();
        state.setActiveTool(ToolId::Smudge);
        state.setOption(ToolId::Smudge, QStringLiteral("brush_size"), 24.0);
        state.setOption(ToolId::Smudge, QStringLiteral("strength"), 100);
        state.setOption(ToolId::Smudge, QStringLiteral("finger_paint"), false);
        state.setForeground(QColor(255, 0, 0));
        app.processEvents();
        auto stroke = [&](QPointF aDoc, QPointF bDoc, int moves) {
            const QPointF a = canvas->documentToView(aDoc);
            const QPointF b = canvas->documentToView(bDoc);
            const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            for (int i = 1; i <= moves; ++i) {
                const QPointF p = a + (b - a) * (double(i) / moves);
                QMouseEvent move(QEvent::MouseMove, p, global, Qt::NoButton,
                                Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &move);
            }
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
        };
        // 1. Smear across the edge: black drags into the white side, with no
        // red deposited (finger off). The travelling patch carries texture,
        // so the white side darkens instead of blurring to gray at the edge.
        state.setActiveLayerIndex(0);
        stroke(QPointF(60, 75), QPointF(140, 75), 8);
        const QColor dragged = compAt(110, 75);
        std::printf("[smudge] edge dragged=(%d,%d,%d)\n", dragged.red(),
                    dragged.green(), dragged.blue());
        CHECK(dragged.red() < 150);
        CHECK(std::abs(dragged.red() - dragged.green()) < 60);  // no red stamp
        // 2. One history step for the whole stroke.
        state.undo();
        app.processEvents();
        CHECK(compAt(110, 75).red() > 200);  // white side restored
        CHECK(dd->layers.size() == 1);
        // 3. Finger on: foreground red loads into the smear on gray.
        for (std::uint32_t y = 0; y < bg.pixels->height(); ++y)
            for (std::uint32_t x = 0; x < bg.pixels->width(); ++x)
                bg.pixels->data()[std::size_t(y) * bg.pixels->width() + x] =
                    pittore::RGBAf{0.5f, 0.5f, 0.5f, 1};
        ++bg.sourceStamp;
        dd->rebuildComposite();
        state.setOption(ToolId::Smudge, QStringLiteral("finger_paint"), true);
        state.setActiveLayerIndex(0);
        stroke(QPointF(60, 75), QPointF(140, 75), 8);
        // Sample near the stroke start, where the loaded paint is freshest.
        const QColor wet = compAt(70, 75);
        std::printf("[smudge] finger start=(%d,%d,%d)\n", wet.red(),
                    wet.green(), wet.blue());
        CHECK(wet.red() > wet.green() + 30);  // red carried in
        state.undo();
        // 4. Finger off on the same gray: no red deposited.
        state.setOption(ToolId::Smudge, QStringLiteral("finger_paint"),
                        false);
        state.setActiveLayerIndex(0);
        stroke(QPointF(60, 75), QPointF(140, 75), 8);
        const QColor pure = compAt(100, 75);
        std::printf("[smudge] pure mid=(%d,%d,%d)\n", pure.red(),
                    pure.green(), pure.blue());
        CHECK(std::abs(pure.red() - pure.green()) < 40);
        CHECK(std::abs(pure.red() - 127) < 60);  // still ~gray
        state.undo();
        // 5. Sample-all: red below, transparent active layer above.
        for (std::uint32_t y = 0; y < bg.pixels->height(); ++y)
            for (std::uint32_t x = 0; x < bg.pixels->width(); ++x)
                bg.pixels->data()[std::size_t(y) * bg.pixels->width() + x] =
                    pittore::RGBAf{1, 0, 0, 1};
        ++bg.sourceStamp;
        LayerItem top;
        top.name = QStringLiteral("smudge-top");
        top.kind = Kind::Pixel;
        top.pixels = std::make_shared<pittore::Image>(
            static_cast<std::uint32_t>(dd->size.width()),
            static_cast<std::uint32_t>(dd->size.height()));
        for (std::uint32_t i = 0;
             i < top.pixels->width() * top.pixels->height(); ++i)
            top.pixels->data()[i] = pittore::RGBAf{0, 0, 0, 0};
        state.addLayer(std::move(top));
        state.setActiveLayerIndex(0);
        app.processEvents();
        state.setOption(ToolId::Smudge, QStringLiteral("sample_all"), true);
        stroke(QPointF(60, 75), QPointF(140, 75), 8);
        // Read the transparent top layer itself: the composite always
        // shows the red background through it.
        auto topPx = [&](int x, int y) {
            const LayerItem& top = dd->layers[0];
            return top.pixels->data()[std::size_t(y) * top.pixels->width() +
                                      std::size_t(x)];
        };
        const pittore::RGBAf picked = topPx(100, 75);
        std::printf("[smudge] sample-all top=(%.2f,%.2f,%.2f,%.2f)\n",
                    picked.r, picked.g, picked.b, picked.a);
        CHECK(picked.a > 0.15f && picked.r > 0.4f);  // red picked up
        state.undo();
        app.processEvents();
        state.setOption(ToolId::Smudge, QStringLiteral("sample_all"), false);
        state.setActiveLayerIndex(0);  // undo may have moved the active row
        stroke(QPointF(60, 75), QPointF(140, 75), 8);
        const pittore::RGBAf bare = topPx(100, 75);
        CHECK(bare.a < 0.15f);  // transparent layer stays empty
        state.undo();
        // 6. Darken mode: white paint over black stays black.
        // (Re-fetch: addLayer above may have reallocated the vector.)
        LayerItem& bgDark = dd->layers[1];
        state.setActiveLayerIndex(1);
        state.setOption(ToolId::Smudge, QStringLiteral("finger_paint"), true);
        state.setForeground(QColor(255, 255, 255));
        state.setOption(ToolId::Smudge, QStringLiteral("mode"), 1);  // Darken
        for (std::uint32_t y = 0; y < bgDark.pixels->height(); ++y)
            for (std::uint32_t x = 0; x < bgDark.pixels->width(); ++x)
                bgDark.pixels->data()[std::size_t(y) * bgDark.pixels->width() +
                                      x] = pittore::RGBAf{0, 0, 0, 1};
        ++bgDark.sourceStamp;
        dd->rebuildComposite();
        stroke(QPointF(60, 75), QPointF(140, 75), 8);
        // Probe near the stroke start, where the loaded white is freshest
        // (the dirty brush dilutes toward black as it travels).
        const QColor dark = compAt(70, 75);
        std::printf("[smudge] darken start=(%d,%d,%d)\n", dark.red(),
                    dark.green(), dark.blue());
        CHECK(dark.red() < 100);  // min() keeps it black
        state.setOption(ToolId::Smudge, QStringLiteral("mode"), 0);
        state.undo();
        // 7. Normal mode with the same setup lightens (control case).
        // Probe 4px from the press point: deep in the first dab, where the
        // loaded white is still fresh before pickup dilutes it. (First-dab
        // pickup halves the load by design — dirty brush runs out.)
        state.setForeground(QColor(255, 255, 255));
        state.setActiveLayerIndex(1);  // undo may have moved the active row
        stroke(QPointF(60, 75), QPointF(140, 75), 8);
        const QColor lit = compAt(64, 75);
        std::printf("[smudge] normal start=(%d,%d,%d)\n", lit.red(),
                    lit.green(), lit.blue());
        CHECK(lit.red() > 50);
        state.undo();
        delete canvas;
    }

    // Patch: marquee-select a blemish, drag onto clean texture, release
    // heals the original area from the drop area in one undo step.
    {
        state.addDocument(QStringLiteral("patchtool"), QSize(200, 150), 300);
        DocumentItem* dd = state.activeDocument();
        LayerItem& bg = dd->layers[0];
        CHECK(bg.pixels);
        for (std::uint32_t y = 0; y < bg.pixels->height(); ++y)
            for (std::uint32_t x = 0; x < bg.pixels->width(); ++x)
                bg.pixels->data()[std::size_t(y) * bg.pixels->width() + x] =
                    (x >= 80 && x < 120 && y >= 55 && y < 95)
                        ? pittore::RGBAf{0, 0, 0, 1}
                        : pittore::RGBAf{1, 1, 1, 1};
        ++bg.sourceStamp;
        dd->rebuildComposite();
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        canvas->setGeometry(0, 0, 800, 600);
        canvas->zoomToFit();
        canvas->setZoom(1.0);
        app.processEvents();
        state.setActiveTool(ToolId::Patch);
        app.processEvents();
        auto gesture = [&](QPointF aDoc, QPointF bDoc, int moves) {
            const QPointF a = canvas->documentToView(aDoc);
            const QPointF b = canvas->documentToView(bDoc);
            const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            for (int i = 1; i <= moves; ++i) {
                const QPointF p = a + (b - a) * (double(i) / moves);
                QMouseEvent move(QEvent::MouseMove, p, global, Qt::NoButton,
                                Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &move);
            }
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
        };
        // 1. Marquee on empty canvas makes the selection (no pixels move).
        gesture(QPointF(85, 60), QPointF(115, 90), 4);
        CHECK(!dd->selection.isEmpty());
        CHECK(dd->composite.pixelColor(100, 75).red() < 128);  // still black
        // 2. Drag inside the selection onto white: the black heals away.
        gesture(QPointF(100, 75), QPointF(160, 75), 4);
        const QColor healed = dd->composite.pixelColor(100, 75);
        std::printf("[patch] healed=(%d,%d,%d)\n", healed.red(),
                    healed.green(), healed.blue());
        CHECK(healed.red() > 128);
        // 3. One undo restores the blemish and keeps the selection.
        state.undo();
        app.processEvents();
        CHECK(dd->composite.pixelColor(100, 75).red() < 128);
        delete canvas;
    }

    // ContentAwareMove: drag selected content to a new spot (it appears at
    // the drop) while the vacated hole heals; one undo step, one undo to
    // restore.
    {
        state.addDocument(QStringLiteral("camovetool"), QSize(200, 150), 300);
        DocumentItem* dd = state.activeDocument();
        LayerItem& bg = dd->layers[0];
        CHECK(bg.pixels);
        // Small black spot on a white field: the hole's surroundings are
        // white, so the vacated hole can genuinely heal white (a hole
        // fully inside black has nothing else to heal from).
        // Small black spot on a white field: the hole's ring is white, so
        // the vacated hole genuinely heals white (a big black field would
        // read as texture and correctly survive).
        for (std::uint32_t y = 0; y < bg.pixels->height(); ++y)
            for (std::uint32_t x = 0; x < bg.pixels->width(); ++x)
                bg.pixels->data()[std::size_t(y) * bg.pixels->width() + x] =
                    (x >= 97 && x < 103 && y >= 72 && y < 78)
                        ? pittore::RGBAf{0, 0, 0, 1}
                        : pittore::RGBAf{1, 1, 1, 1};
        ++bg.sourceStamp;
        dd->rebuildComposite();
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        canvas->setGeometry(0, 0, 800, 600);
        canvas->zoomToFit();
        canvas->setZoom(1.0);
        app.processEvents();
        state.setActiveTool(ToolId::ContentAwareMove);
        app.processEvents();
        auto gesture = [&](QPointF aDoc, QPointF bDoc, int moves) {
            const QPointF a = canvas->documentToView(aDoc);
            const QPointF b = canvas->documentToView(bDoc);
            const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            for (int i = 1; i <= moves; ++i) {
                const QPointF p = a + (b - a) * (double(i) / moves);
                QMouseEvent move(QEvent::MouseMove, p, global, Qt::NoButton,
                                Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &move);
            }
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
        };
        // 1. Marquee makes the selection (no pixels move).
        gesture(QPointF(85, 60), QPointF(115, 90), 4);
        CHECK(!dd->selection.isEmpty());
        CHECK(dd->composite.pixelColor(100, 75).red() < 128);  // still black
        // 2. Drag the selection onto white: black lands at the drop, the
        // hole heals behind it.
        gesture(QPointF(100, 75), QPointF(160, 75), 4);
        const QColor landed = dd->composite.pixelColor(160, 75);
        const QColor hole = dd->composite.pixelColor(100, 75);
        std::printf("[camove] landed=(%d,%d,%d) hole=(%d,%d,%d)\n",
                    landed.red(), landed.green(), landed.blue(), hole.red(),
                    hole.green(), hole.blue());
        CHECK(landed.red() < 128);
        CHECK(hole.red() > 200);
        // 3. One undo restores the spot and clears the drop.
        state.undo();
        app.processEvents();
        CHECK(dd->composite.pixelColor(100, 75).red() < 128);
        CHECK(dd->composite.pixelColor(160, 75).red() > 200);
        delete canvas;
    }

    // ContentAwareTracing: rect selection traces to a polygon Shape layer
    // (4 corners at full detail), to a mask, and Path honestly refuses.
    {
        state.addDocument(QStringLiteral("tracetool"), QSize(200, 150), 300);
        DocumentItem* dd = state.activeDocument();
        const int baseLayers = int(dd->layers.size());
        state.setSelection(QRectF(QPointF(20, 20), QPointF(60, 45)), false);
        // Path output: planned, refuses without state change.
        CHECK(!state.traceSelection(0, 100));
        CHECK(int(dd->layers.size()) == baseLayers);
        // Shape output: one polygon layer with 4 anchors + Close.
        CHECK(state.traceSelection(2, 100));
        CHECK(int(dd->layers.size()) == baseLayers + 1);
        const LayerItem& tl = dd->layers.front();
        CHECK(tl.art && !tl.art->isEmpty());
        int anchors = 0;
        for (const auto& s : tl.art->segments)
            if (s.kind == pittore::vector::Segment::Kind::MoveTo ||
                s.kind == pittore::vector::Segment::Kind::LineTo ||
                s.kind == pittore::vector::Segment::Kind::CubicTo)
                ++anchors;
        CHECK(anchors == 4);
        // Detail 0 collapses straight runs (fewer vertices, still closed).
        state.undo();
        app.processEvents();
        CHECK(state.traceSelection(2, 0));
        // Selection output: mask channel set, same bbox.
        CHECK(state.traceSelection(1, 100));
        CHECK(dd->selectionIsMask);
        CHECK(!dd->selectionMask.isNull());
        state.undo();
        app.processEvents();
    }

    // Area: press-drag measures the rectangle; a bare click clears it.
    // One undo step per measurement, like the ruler line.
    {
        state.addDocument(QStringLiteral("areatool"), QSize(200, 150), 300);
        DocumentItem* dd = state.activeDocument();
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        canvas->setGeometry(0, 0, 800, 600);
        canvas->zoomToFit();
        canvas->setZoom(1.0);
        app.processEvents();
        state.setActiveTool(ToolId::AreaTool);
        app.processEvents();
        auto gesture = [&](QPointF aDoc, QPointF bDoc, int moves) {
            const QPointF a = canvas->documentToView(aDoc);
            const QPointF b = canvas->documentToView(bDoc);
            const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, a, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &press);
            for (int i = 1; i <= moves; ++i) {
                const QPointF p = a + (b - a) * (double(i) / moves);
                QMouseEvent move(QEvent::MouseMove, p, global, Qt::NoButton,
                                Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(canvas->viewport(), &move);
            }
            QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &release);
            app.processEvents();
        };
        // 1. Drag measures the rectangle.
        gesture(QPointF(20, 20), QPointF(60, 45), 4);
        CHECK(dd->areaHasMeasurement);
        CHECK(std::abs(dd->areaRect.width() - 40.0) < 1.0);
        CHECK(std::abs(dd->areaRect.height() - 25.0) < 1.0);
        // 2. Undo clears the measurement.
        state.undo();
        app.processEvents();
        CHECK(!dd->areaHasMeasurement);
        // 3. A bare click leaves no measurement behind.
        gesture(QPointF(30, 30), QPointF(30, 30), 1);
        CHECK(!dd->areaHasMeasurement);
        delete canvas;
    }
    // reset restores schema defaults, tilt master scales the lean response.
    {
        namespace bl = pittore::ui::brushlibrary;
        state.addDocument(QStringLiteral("presetrt"), QSize(200, 150), 300);
        state.setActiveTool(ToolId::Brush);
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 33);
        state.setOption(ToolId::Brush, QStringLiteral("brush_hardness"), 77);
        state.setOption(ToolId::Brush, QStringLiteral("brush_angle"), 12);
        state.setOption(ToolId::Brush, QStringLiteral("brush_spacing"), 42);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilt_master"),
                        37);
        state.setOption(ToolId::Brush, QStringLiteral("brush_fade"), 120);
        state.setOption(ToolId::Brush, QStringLiteral("brush_darken"), 25);
        state.setOption(ToolId::Brush, QStringLiteral("brush_speed_size"),
                        60);
        state.setOption(ToolId::Brush, QStringLiteral("brush_painting_mode"),
                        QStringLiteral("buildup"));
        state.setOption(ToolId::Brush, QStringLiteral("brush_erase_blend"),
                        true);
        bl::BrushPreset p =
            bl::captureBrushState(&state, ToolId::Brush);
        CHECK_NEAR(p.size, 33.0, 1e-9);
        CHECK_NEAR(p.hardness, 77.0, 1e-9);
        CHECK_NEAR(p.angle, 12.0, 1e-9);
        CHECK_NEAR(p.spacing, 42.0, 1e-9);
        CHECK_NEAR(p.tiltMaster, 37.0, 1e-9);
        CHECK_NEAR(p.fadeLen, 120.0, 1e-9);
        CHECK_NEAR(p.darkenPct, 25.0, 1e-9);
        CHECK_NEAR(p.speedSize, 60.0, 1e-9);
        CHECK(p.paintingMode == QStringLiteral("buildup"));
        CHECK(p.eraserBlend);
        // JSON round-trips the new field; old files default it to neutral.
        p.name = QStringLiteral("rt");
        const QJsonObject o = p.toJson();
        CHECK(o.value(QStringLiteral("tilt_master")).toDouble(-1.0) == 37.0);
        bool ok = false;
        bl::BrushPreset q = bl::BrushPreset::fromJson(o, &ok);
        CHECK(ok);
        CHECK_NEAR(q.tiltMaster, 37.0, 1e-9);
        bl::BrushPreset legacy;
        CHECK_NEAR(legacy.tiltMaster, 100.0, 1e-9);
        // Mutate, re-apply, everything captured comes back.
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 9);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilt_master"),
                        100);
        state.setOption(ToolId::Brush, QStringLiteral("brush_erase_blend"),
                        false);
        CHECK(bl::applyPreset(&state, p) ==
              bl::ApplyResult::Applied);
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("brush_size"))
                       .toDouble(),
                   33.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("brush_tilt_master"))
                       .toDouble(),
                   37.0, 1e-9);
        CHECK(state.option(ToolId::Brush, QStringLiteral("brush_erase_blend"))
                  .toBool());
        // Reset restores schema defaults (size back to the 64 px preset).
        bl::resetBrushToolToDefaults(&state, ToolId::Brush);
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("brush_size"))
                       .toDouble(),
                   64.0, 1e-9);
        CHECK_NEAR(state.option(ToolId::Brush, QStringLiteral("brush_hardness"))
                       .toDouble(),
                   50.0, 1e-9);
        // Tilt master: a leaned stylus grows the dab at 100, not at 0.
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        canvas->setGeometry(0, 0, 800, 600);
        canvas->zoomToFit();
        canvas->setZoom(4.0);
        app.processEvents();
        state.setActiveTool(ToolId::Brush);
        app.processEvents();
        state.setForeground(QColor(0, 0, 0));
        state.setOption(ToolId::Brush, QStringLiteral("brush_size"), 20);
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilt_size"),
                        100);
        state.setOption(ToolId::Brush, QStringLiteral("brush_roundness"),
                        100);
        state.setOption(ToolId::Brush, QStringLiteral("smoothing"), 0);
        auto inkWidth = [&]() {
            DocumentItem* dd = state.activeDocument();
            int x0 = 1 << 30, x1 = -1;
            for (int y = 0; y < dd->composite.height(); ++y)
                for (int x = 0; x < dd->composite.width(); ++x) {
                    const QColor c = dd->composite.pixelColor(x, y);
                    if (c.alpha() > 128 &&
                        (c.red() + c.green() + c.blue()) / 3 < 128) {
                        x0 = std::min(x0, x);
                        x1 = std::max(x1, x);
                    }
                }
            return x1 - x0;
        };
        auto leanStroke = [&](double xt) {
            const QPointF a = canvas->documentToView(QPointF(60, 75));
            const QPointF b = canvas->documentToView(QPointF(140, 75));
            const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
            auto tabletAt = [&](QEvent::Type type, const QPointF& pp,
                                Qt::MouseButtons buttons) {
                static QPointingDevice stylus;
                QTabletEvent e(type, &stylus, pp, global, 1.0, xt, 0, 0.0,
                               0.0, 0, Qt::NoModifier, Qt::LeftButton,
                               buttons);
                QApplication::sendEvent(canvas->viewport(), &e);
            };
            tabletAt(QEvent::TabletPress, a, Qt::LeftButton);
            tabletAt(QEvent::TabletMove, b, Qt::LeftButton);
            tabletAt(QEvent::TabletRelease, b, Qt::NoButton);
            app.processEvents();
        };
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilt_master"),
                        100);
        leanStroke(55);
        const int wMaster = inkWidth();
        state.undo();
        state.setOption(ToolId::Brush, QStringLiteral("brush_tilt_master"),
                        0);
        leanStroke(55);
        const int wMuted = inkWidth();
        std::printf("[tiltmaster] full=%d muted=%d\n", wMaster, wMuted);
        CHECK(wMaster > wMuted + 10);
        state.undo();
        // Leave clean defaults for any later run reusing the sandbox.
        bl::resetBrushToolToDefaults(&state, ToolId::Brush);
        state.setOption(ToolId::Brush, QStringLiteral("brush_roundness"),
                        100);
        delete canvas;
    }

    // Grid + snapping share one spacing: the Preferences value drives both
    // the painted grid and the move-drag snap targets.
    {
        state.addDocument(QStringLiteral("gridsnap"), QSize(200, 150), 300);
        DocumentItem* dd = state.activeDocument();
        // Black paper: the constructor already baked the default sheet,
        // so drop it first or the assignment below changes nothing.
        dd->layers.back().pixels.reset();
        dd->canvasPaper = QColor(0, 0, 0);
        dd->rebuildComposite();
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(1.0);
        app.processEvents();
        // Default spacing paints/snaps on 64s.
        CHECK(canvas->gridStep() == 64.0);
        // Left edge at 67: nearest 64-line is 3 away, inside tolerance.
        CanvasView::MoveSnap s64 =
            canvas->snappedMoveSize(QPointF(67, 10), QSizeF(40, 40));
        CHECK(s64.offset.x() == 64.0);
        CHECK(s64.x && s64.xPos == 64.0);
        // Widen the setting: the same gesture now snaps to the 100 grid
        // (centre edge 107 pulls to 100) instead of 64.
        AppSettings s = state.settings();
        s.gridSpacing = 100.0;
        state.applySettings(s);
        app.processEvents();
        CHECK(canvas->gridStep() == 100.0);
        CanvasView::MoveSnap s100 =
            canvas->snappedMoveSize(QPointF(67, 10), QSizeF(40, 40));
        CHECK(s100.offset.x() == 60.0);
        CHECK(s100.x && s100.xPos == 100.0);
        // Pixel grid toggles and paints: black paper at 8x shows grid
        // lines as bright pixels in a viewport grab.
        CHECK(!canvas->pixelGridVisible());
        canvas->setPixelGridVisible(true);
        CHECK(canvas->pixelGridVisible());
        canvas->setZoom(8.0);
        app.processEvents();
        // The floating contextual task bar would pollute the grab with
        // bright text pixels (font-dependent), so hide it while counting
        // grid paint, then restore it. Grid inks are dim whites over
        // black, so count any painted (nonzero) pixel rather than only
        // near-white ones.
        QWidget* taskBar = canvas->taskBar();
        const bool taskBarWasVisible = taskBar && taskBar->isVisible();
        if (taskBar) taskBar->hide();
        app.processEvents();
        const QImage shot = canvas->viewport()->grab().toImage();
        int bright = 0;
        for (int y = 0; y < shot.height(); ++y)
            for (int x = 0; x < shot.width(); ++x) {
                const QColor c = shot.pixelColor(x, y);
                if (c.red() > 0 || c.green() > 0 || c.blue() > 0)
                    ++bright;
            }
        CHECK(bright > 100);
        canvas->setPixelGridVisible(false);
        canvas->setGridVisible(true);
        app.processEvents();
        const QImage shot2 = canvas->viewport()->grab().toImage();
        int bright2 = 0;
        for (int y = 0; y < shot2.height(); ++y)
            for (int x = 0; x < shot2.width(); ++x) {
                const QColor c = shot2.pixelColor(x, y);
                if (c.red() > 0 || c.green() > 0 || c.blue() > 0)
                    ++bright2;
            }
        CHECK(bright2 > 100);
        // The pixel grid strictly adds lines over the regular grid, so
        // the first grab must hold strictly more paint than the second.
        CHECK(bright > bright2);
        if (taskBar && taskBarWasVisible) taskBar->show();
        // Leave the sandbox as found.
        s = state.settings();
        s.gridSpacing = 64.0;
        state.applySettings(s);
        delete canvas;
    }

    // Ruler-drag guides, edge/guide toggles, snap to slices + layers.
    {
        state.addDocument(QStringLiteral("guidesnap"), QSize(200, 150), 300);
        DocumentItem* dd = state.activeDocument();
        // Black paper: the constructor already baked the default sheet,
        // so drop it first or the assignment below changes nothing.
        dd->layers.back().pixels.reset();
        dd->canvasPaper = QColor(0, 0, 0);
        dd->rebuildComposite();
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(1.0);
        app.processEvents();
        CanvasRuler* hruler = nullptr;
        for (QWidget* w : canvas->findChildren<QWidget*>()) {
            auto* r = dynamic_cast<CanvasRuler*>(w);
            if (r && r->width() > r->height()) hruler = r;
        }
        CHECK(hruler != nullptr);
        // Drag out of the horizontal ruler commits a guide at the dragged
        // document height.
        {
            const QPointF pp(100, 5);
            const QPointF global = hruler->mapToGlobal(pp.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, pp, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(hruler, &press);
            QMouseEvent move(QEvent::MouseMove, QPointF(100, 12), global,
                            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(hruler, &move);
            const double wantY =
                canvas
                    ->viewToDocument(QPointF(
                        100, canvas->viewport()->height() / 2.0))
                    .y();
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(100, 12),
                               global, Qt::LeftButton, Qt::NoButton,
                               Qt::NoModifier);
            QApplication::sendEvent(hruler, &release);
            app.processEvents();
            CHECK(dd->horizontalGuides.size() == 1);
            CHECK(std::abs(dd->horizontalGuides.front() - wantY) < 1.0);
        }
        // Escape abandons the next drag: no guide added.
        {
            const QPointF pp(50, 5);
            const QPointF global = hruler->mapToGlobal(pp.toPoint());
            QMouseEvent press(QEvent::MouseButtonPress, pp, global,
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(hruler, &press);
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(canvas->viewport(), &esc);
            QMouseEvent release(QEvent::MouseButtonRelease, pp, global,
                               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(hruler, &release);
            app.processEvents();
            CHECK(dd->horizontalGuides.size() == 1);
        }
        // Selection Edges hides the marching ants (keeps everything else).
        state.setSelection(QRectF(20, 20, 60, 40), false);
        app.processEvents();
        const QRectF selView(canvas->documentToView(QPointF(20, 20)),
                             canvas->documentToView(QPointF(80, 60)));
        auto countBright = [&]() {
            // Full-viewport grab (partial grabs don't repaint the document
            // offscreen), counted over the selection's view box only.
            const QRect box = selView.adjusted(-8, -8, 8, 8).toAlignedRect();
            const QImage shot = canvas->viewport()
                                    ->grab()
                                    .toImage()
                                    .convertToFormat(QImage::Format_ARGB32);
            int bright = 0;
            for (int y = std::max(0, box.top());
                 y < std::min(shot.height(), box.bottom()); ++y)
                for (int x = std::max(0, box.left());
                     x < std::min(shot.width(), box.right()); ++x) {
                    const QColor c = shot.pixelColor(x, y);
                    if (c.red() > 200 && c.green() > 200 && c.blue() > 200)
                        ++bright;
                }
            return bright;
        };
        CHECK(canvas->selectionEdgesVisible());
        const int brightOn = countBright();
        std::printf("[edges] on=%d\n", brightOn);
        CHECK(brightOn > 20);
        canvas->setSelectionEdgesVisible(false);
        CHECK(!canvas->selectionEdgesVisible());
        app.processEvents();
        const int brightOff = countBright();
        std::printf("[edges] off=%d\n", brightOff);
        CHECK(brightOff == 0);
        canvas->setSelectionEdgesVisible(true);
        // Smart guides toggle round-trips (the move-drag readout itself is
        // covered by the snap sections exercising real drags).
        CHECK(canvas->smartGuidesVisible());
        canvas->setSmartGuidesVisible(false);
        CHECK(!canvas->smartGuidesVisible());
        canvas->setSmartGuidesVisible(true);
        state.clearSelection();
        // Snap to slices: slice edges pull like guides (grid neutralised).
        AppSettings s = state.settings();
        s.gridSpacing = 1000.0;
        state.applySettings(s);
        dd->slices.append(QRect(140, 0, 20, 150));
        CanvasView::MoveSnap ss =
            canvas->snappedMoveSize(QPointF(127, 10), QSizeF(40, 40));
        // Centre edge (150) beats the left edge (140) at dist 3 vs 7.
        CHECK(ss.offset.x() == 130.0);
        CHECK(ss.x && ss.xPos == 150.0);
        // Left edge alone pulls exact.
        CanvasView::MoveSnap se =
            canvas->snappedMoveSize(QPointF(137, 10), QSizeF(40, 40));
        CHECK(se.offset.x() == 140.0);
        CHECK(se.x && se.xPos == 140.0);
        // Snap to layers: a placed layer's edges attract.
        QImage block(20, 20, QImage::Format_ARGB32);
        block.fill(QColor(255, 255, 255));
        state.placeImageLayer(block, QStringLiteral("blk"), QPointF(50, 50),
                              1.0);
        app.processEvents();
        CanvasView::MoveSnap sl =
            canvas->snappedMoveSize(QPointF(18, 100), QSizeF(40, 40));
        CHECK(sl.offset.x() == 20.0);
        s = state.settings();
        s.gridSpacing = 64.0;
        state.applySettings(s);
        delete canvas;
    }

    // Panel-selected occluded layer drags instead of losing to auto-select:
    // a small back layer fully covered by a big front one still moves when
    // the press lands inside the kept selection.
    {
        state.addDocument(QStringLiteral("occludedrag"), QSize(200, 150),
                          300);
        DocumentItem* dd = state.activeDocument();
        AppSettings s = state.settings();
        s.gridSpacing = 1000.0;  // neutralize grid/doc snap targets
        state.applySettings(s);
        QImage small(20, 20, QImage::Format_ARGB32);
        small.fill(QColor(255, 0, 0));
        state.placeImageLayer(small, QStringLiteral("small"),
                              QPointF(50, 50), 1.0);
        QImage big(120, 120, QImage::Format_ARGB32);
        big.fill(QColor(0, 0, 255));
        state.placeImageLayer(big, QStringLiteral("big"), QPointF(100, 75),
                              1.0);
        app.processEvents();
        // Big on top (index 0), small behind it (index 1), fully covered.
        CHECK(dd->layers.size() == 3);
        const int smallAt = 1;
        CHECK(dd->layers[smallAt].offset == QPointF(40, 40));
        // Panel-style selection of the back layer.
        state.setActiveLayerIndex(smallAt);
        dd->selectedLayers.clear();
        dd->selectedLayers.push_back(smallAt);
        QWidget window;
        auto* canvas = new CanvasView(&state, &window);
        window.resize(800, 600);
        canvas->setGeometry(0, 0, 800, 600);
        window.show();
        canvas->zoomToFit();
        canvas->setZoom(1.0);
        app.processEvents();
        state.setActiveTool(ToolId::Move);
        app.processEvents();
        std::printf("[occdrag] pre-press sel=%d act=%d\n",
                    dd->selectedLayers.isEmpty() ? -9
                                                : dd->selectedLayers.front(),
                    dd->activeLayer);
        const QPointF a = canvas->documentToView(QPointF(50, 50));
        const QPointF b = canvas->documentToView(QPointF(70, 50));
        const QPointF global = canvas->viewport()->mapToGlobal(a.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress, a, global,
                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &press);
        QMouseEvent move(QEvent::MouseMove, b, global, Qt::NoButton,
                        Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &move);
        QMouseEvent release(QEvent::MouseButtonRelease, b, global,
                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas->viewport(), &release);
        app.processEvents();
        // The back layer moved with the drag; selection never collapsed
        // onto the front image.
        const QPointF moved = dd->layers[smallAt].offset;
        std::printf("[occdrag] moved=(%.2f,%.2f) big=(%.2f,%.2f) sel=%d act=%d\n",
                    moved.x(), moved.y(), dd->layers[0].offset.x(),
                    dd->layers[0].offset.y(),
                    dd->selectedLayers.isEmpty() ? -9
                                                : dd->selectedLayers.front(),
                    dd->activeLayer);
        CHECK(std::abs(moved.x() - 60.0) < 1e-6);
        CHECK(std::abs(moved.y() - 40.0) < 1e-6);
        CHECK(dd->selectedLayers.size() == 1);
        CHECK(dd->selectedLayers.front() == smallAt);
        CHECK(dd->activeLayer == smallAt);
        s = state.settings();
        s.gridSpacing = 64.0;
        state.applySettings(s);
        delete canvas;
    }

    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
