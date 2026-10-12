#pragma once
// Refine Lab main window: a live control surface over the real segmentation +
// refine pipeline. Left = zoomable result pane (wheel to zoom, drag to pan);
// right = every parameter as a slider/checkbox, grouped the way the pipeline
// runs. Any change re-runs the (proxy-scaled) pipeline on a short debounce so
// you see the effect immediately; Segment runs the AI once and caches the seed
// matte. Save/load a preset JSON to carry tuned values out.
//
// The whole surface is ALSO driven remotely through an embedded MCP server
// (see mcp_server.h): every slider, button and view mode is a tool, so an
// agent can segment, sweep parameters, render the result and read it back —
// the "endless testing" loop without touching the mouse. Undo/redo keep a
// history of tuning states; Reset returns to defaults so a sweep can start
// clean. The image auto-fits the pane on load.

#include <QMainWindow>

#include "lab_engine.h"
#include "lab_params.h"

class QComboBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QScrollArea;
class QTimer;
class QWidget;

namespace pittore::lab {

class LabMcpServer;

class LabWindow : public QMainWindow {
  public:
    explicit LabWindow(QWidget* parent = nullptr);
    ~LabWindow() override;

    // --- Remote (MCP) control surface -------------------------------------
    // Everything below runs on the GUI thread (the MCP server marshals calls
    // onto it), so it is safe to touch widgets and the engine directly.
    QJsonObject paramsState() const;             // params + live status
    void applyParams(const QJsonObject& o);      // merge + recompute
    void resetParams();                          // back to defaults
    void resetImage();                           // drop the seed matte
    void undo();
    void redo();
    int undoCount() const { return undoStack_.size(); }
    int redoCount() const { return redoStack_.size(); }
    void pushHistory();                          // snapshot for undo
    bool segment(QString* err);                  // run the AI seed
    QJsonObject refine();                        // run pipeline, return stats
    QImage render();                             // current view as an image
    bool loadImageFile(const QString& path, QString* err);
    void setViewMode(int m);
    void setSolidBg(int i);
    void setDarkMode(bool on);
    bool hasImage() const { return !fullImg_.isNull(); }
    bool hasMatte() const { return !fullMatte_.isNull(); }
    QString statusText() const;
    bool startMcp(quint16 port, QString* err);   // start the MCP server

  private:
    void buildToolbar();
    QWidget* buildControls();
    void connectParams();
    void scheduleRecompute();
    void doRefine();
    void updateView();
    void loadImage(const QString& path);
    void runSegment();
    void rebuildMatteFromRaw();
    void syncUiFromParams();
    void queueFit();
    void savePreset();
    void loadPreset();
    void fitView();
    void applyDarkPalette(bool on);

    LabParams params_;
    LabEngine engine_;
    QImage fullImg_;    // opened source, full res
    QImage fullMatte_;  // AI seed matte, full res (empty until Segment)
    std::vector<float> fullRawAlpha_;  // raw model alpha (for live level band)
    RunResult last_;
    QVector<QJsonObject> undoStack_;
    QVector<QJsonObject> redoStack_;

    LabMcpServer* mcp_ = nullptr;
    QGraphicsView* view_ = nullptr;
    QGraphicsScene* scene_ = nullptr;
    QTimer* debounce_ = nullptr;
    QLabel* status_ = nullptr;
    QComboBox* viewMode_ = nullptr;
    QComboBox* solidBg_ = nullptr;
    QComboBox* modelBox_ = nullptr;
    QScrollArea* panelScroll_ = nullptr;
    QWidget* panel_ = nullptr;
    bool fitPending_ = false;
    bool dark_ = false;
};

}  // namespace pittore::lab
