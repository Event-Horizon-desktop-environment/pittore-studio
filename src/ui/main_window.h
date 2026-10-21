#pragma once
#include <QHash>
#include <QMap>
#include <QSet>
#include <QMainWindow>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QVector>

#include "ui/app_state.h"
#include "ui/tool_registry.h"

#include <functional>
#include <vector>

class QAction;
class QDockWidget;
class QDragEnterEvent;
class QDropEvent;
class QMenu;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QTabBar;
class QTimer;
class QToolButton;

namespace pittore::ui {

class CanvasView;
class OptionsBar;
class ToolsPanel;
class WorkspaceManager;
struct RefineResult;

// R24 workspace anatomy, assembled:
//   A  application bar   — menus, workspace switcher (menuBar + corner widget)
//   B  options bar       — OptionsBar, tool context
//   C  tools panel       — ToolsPanel, left dock area
//   D  document window   — tab bar + CanvasView, the central widget
//   E  panel docks       — QDockWidget per PanelInfo, right dock areas
//   F  status bar        — zoom field, document stats, compute device
//
// The window owns no editing state of its own: every widget below binds to
// AppState, which is why panels can be torn off, tabbed or hidden without any
// of them holding a pointer to another.
class MainWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit MainWindow(AppState* state, QWidget* parent = nullptr);
    ~MainWindow() override;

    AppState* state() const { return state_; }

    // Panels are created lazily; this is also what the Window menu, the
    // collapsed icon rail and the workspace presets call.
    void showPanel(const QString& id, bool show = true);
    // Raise a panel on the left rail next to Tools, regardless of where it
    // normally docks. Used when a dialog wants the user's eye beside the
    // canvas (Colour Grading surfaces the Adjustments panel this way). A
    // floating panel stays where the user put it.
    void raisePanelOnLeft(const QString& id);
    bool panelVisible(const QString& id) const;

    // Headless-safe command entry for the MCP bridge and tests. Only
    // commands that never open dialogs, change task context, or need a
    // cursor position run; everything else is refused with a status hint.
    // Returns true when the command executed.
    bool runHeadlessCommand(const QString& id);

  protected:
    void closeEvent(QCloseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    // Construction stages.
    void buildMenuBar();
    void buildApplicationBarCorner();
    void buildDocumentArea();
    void buildDocks();
    void buildStatusBar();
    // Default tab selection: Layers leads the top rail group, History the
    // bottom one — re-asserted after construction and workspace switches so
    // batch panel shows never strand an odd tab on top. Skips hidden or
    // torn-out docks (explicit user arrangement always wins otherwise).
    void raiseDefaultTabs();
    // Dock tab bars keep full names with scroll arrows instead of clipping
    // to "Chan..." (the document tab bar is excluded).
    void fixDockTabBars();
    void wireState();
    void installShortcuts();

    // Menu builders — one per top-level menu so the tree stays legible.
    void buildFileMenu();
    void buildEditMenu();
    void buildImageMenu();
    void buildLayerMenu();
    void buildTypeMenu();
    void buildSelectMenu();
    void buildFilterMenu();
    void buildViewMenu();
    void buildObjectMenu();
    void buildPathMenu();
    void buildPluginsMenu();
    void buildWindowMenu();
    void buildHelpMenu();

    // --- menu op implementations (src/ui/window/menus/ops/menu_ops_*.cpp) ---
    // Handlers too substantial for a builder lambda: one file per menu
    // family, declared here in the batch that owns it. Each declaration
    // names the menu items it serves.

    // Behaviour.
    void applyTheme();
    void syncDocumentTabs();
    void syncWindowMenu();
    void updateStatus();
    void applyScreenMode(ScreenMode mode);
    void applyChromeVisibility(ChromeVisibility visibility);
    void runCommand(const QString& id);
    void runAiBackgroundRemoval(const QString& id);  // select-subject / remove-background / select-and-mask
    void runEnhanceEdges();  // task-bar + Select menu: AI hair-edge enhance of
                             // the live selection (falls back to colour snap)
    void runAutoRefine();    // task-bar: full tuned pipeline (segment + refine
                             // + decontam) applied directly, no dialog
    // Copy the active pixel layer (optionally clipped to the live selection)
    // into a cropped QImage whose top-left lands at `docRectOut`; used by
    // Copy / Paste and Layer via Copy. Returns a null image when refused.
    QImage maskedSelectionCopy(QRectF* docRectOut = nullptr);
    // Internal document clipboard (Copy/Paste across documents in the app;
    // system clipboard integration is out of scope for now).
    QImage clipboardImage_;
    QRectF clipboardRect_;
    QString clipboardLabel_;
    // Reflect undo/redo availability (+ the step names) on the Edit menu actions.
    void syncUndoRedo();
    void newDocumentDialog();
    void showStartPage();
    void openProjectDialog();
    void openProjectFile(const QString& path);
    // Place tool click: file dialog, then place raster/SVG centred on docPos.
    void placeAt(const QPointF& docPos);
    void saveActiveProject();
    void saveActiveProjectAs();
    void exportActiveDocument();   // Export As… / Save a Copy… (any format)
    void exportLayeredPsd();       // File ▸ Export ▸ Export Layered PSD…
    void exportLayeredAf();        // File ▸ Export ▸ Export Layered Affinity…
    void quickExportPng();
    // Refine Selection dialog (Select Subject / Object Select follow-up):
    // opens the dialog when a selection exists (running Select Subject first
    // when autoSubject and there is none), then commits the result.
    void refineSelection(bool autoSubject = false);
    void applyRefineResult(const RefineResult& result);
    void editToolbarDialog();
    void newWorkspaceDialog();
    void keyboardShortcutsDialog();
    void showSpotlight();
    void applyKeymapToMenus();
    // Re-read keymap.json onto the ApplicationShortcut carriers installed by
    // installShortcuts (spotlight, zoom, ...). Called at startup and whenever
    // Settings > Shortcuts is applied.
    void refreshCarrierShortcuts();
    // Brush size/hardness keys (keymap ids brush-smaller/larger). Shared by
    // the first press and key auto-repeat, so holding [ / ] walks the size
    // for every brush tool (issue #8). Returns true when it handled the key.
    bool handleBrushSizeKey(QKeyEvent* key);
    // Alt+Shift+<letter> family (keymap ids toggle-clip-alt, blend-multiply,
    // blend-screen): strict modifier match (Shift counts), then route to the
    // clip toggle or the tool/layer blend mode. Returns true when handled.
    bool handleBlendModeKey(QKeyEvent* key);
    // Points the import profile-mismatch resolver at the modal dialog
    // (embedded-profile mismatch workflow). Re-armed after any
    // stretch that must stay silent (session restore).
    void armProfileMismatchResolver();
    void filterDialog(const QString &filterId);
    void liquifyDialog();
    void filterGalleryDialog(const QString &category = QString());
    void applyFilterOneShot(const QString &filterId, const std::vector<double> &params = {});
    void preferencesDialog();
    // Same dialog deep-linked to a category ("Performance", "Shortcuts",
    // ... — see PreferencesDialog). Unknown names fall back to General.
    void openPreferencesAt(const QString& tab);
    // View > Proof Setup > Custom…: proof profile file, rendering intent,
    // black-point compensation. Writes through applySettings (persisted,
    // no backend rebuild).
    void proofSetupDialog();
    // Image > Mode > CMYK (shown only when no working profile is configured):
    // destination profile file, then one profiled conversion. Saved through
    // applySettings so the next flip is silent.
    void cmykConvertDialog();
    // Tonal / filter dialogs (R49, R54) and the R17 New Guide dialog.
    void levelsDialog();
    void curvesDialog();
    void colorGradeDialog();
    void addNoiseDialog();
    void medianDialog();
    void unsharpMaskDialog();
    void newGuideDialog();
    // One-shot sharpen of the active pixel layer (Sharpen / Sharpen More).
    void sharpenActiveLayer(double amount);
    // R19 Slice tool: build slices from the current guides, and export every
    // slice as a PNG into a chosen folder.
    void slicesFromGuides();
    void exportSlices();
    // R98 crash safety: write-ahead recovery marker + periodic autosave.
    void initCrashSafety();
    void scheduleAutosave();
    void autosaveNow();
    void clearRecovery();
    void recoverSession();
    QString recoveryFilePath() const;
    // Snapshot left by a pre-.psc version (recovery/session.ifp): preferred
    // when the current .psc snapshot is absent, then consumed the same way.
    QString legacyRecoveryFilePath() const;
    // The snapshot to recover, or empty: current .psc first, legacy .ifp next.
    QString existingRecoveryFilePath() const;
    QString recoveryMarkerPath() const;
    // True when the previous run crashed with an unrecovered snapshot: the
    // crash-recovery flow owns that launch, not session restore.
    bool crashedLastRun() const;
    // Clean-quit session for "reopen documents on startup".
    QString sessionFilePath() const;
    // The non-destructive Layer Style dialog for the active layer, brought up
    // with `effectIndex` selected (StyleEffect; -1 for the first enabled one).
    void layerStyleDialog(int effectIndex);
    // The Live Tone Blend Group settings dialog for a group header: live
    // preview while open, one history step on OK, revert on Cancel.
    void toneBlendDialog(int groupIndex);
    void aboutDialog();

    // --- Edit menu (src/ui/window/menus/ops/menu_ops_edit.cpp) -------------
    // History / clipboard.
    void toggleLastState();      // Toggle Last State (undo, then redo back)
    void cutSelection();         // Cut
    void copyMerged();           // Copy Merged
    void pasteMaskedToSelection(bool inside);  // Paste Into / Paste Outside
    void purgeClipboard();       // Purge ▸ Clipboard
    // Text.
    void findReplaceTextDialog();  // Find and Replace Text…
    // Fill / Stroke / Content-Aware Fill.
    void fillDialog();                    // Fill…
    void strokeSelectionDialog();         // Stroke…
    void strokeSelectionOutline(const QColor& color, int widthPx);
    void contentAwareFill();              // Content-Aware Fill…
    // Transform submenu (label-keyed dispatch; `name` from the builder).
    void transformMenuCommand(const QString& label);
    void transformAgain();
    void applyRotateTransform(double degrees);
    void applyFlipTransform(bool horizontal);
    void applyScaleTransform(double factorX, double factorY);
    void scaleLayerDialog();
    void rotateLayerDialog();
    // Stack operations.
    void autoAlignLayers();   // Auto-Align Layers…
    void autoBlendLayers();   // Auto-Blend Layers…
    // Define …
    void defineBrushPreset();  // Define Brush Preset…
    void definePattern();      // Define Pattern…
    // Honest status hints for entries this build cannot back.
    void spellCheckUnavailable();        // Check Spelling…
    void generativeExpandUnavailable();  // Generative Expand
    void skyReplacementUnavailable();    // Sky Replacement…
    void defineCustomShapeUnavailable(); // Define Custom Shape…
    // The last rotation / flip / scale, so Transform ▸ Again can repeat it.
    std::function<void()> lastTransform_;

    QDockWidget* dockFor(const QString& id, bool create);
    // Floating panels (Character, Paragraph) are top-level windows rather than
    // docked tabs; this returns the window, creating it on demand.
    QWidget* floatingPanel(const QString& id, bool create);
    QAction* makeAction(QMenu* menu, const QString& text, const QString& shortcut = {},
                        std::function<void()> handler = {});
    QAction* makeCheckableAction(QMenu* menu, const QString& text, const QString& shortcut,
                                 bool checked, std::function<void(bool)> handler);

    AppState* state_;
    OptionsBar* options_ = nullptr;
    ToolsPanel* tools_ = nullptr;
    CanvasView* canvas_ = nullptr;
    QTabBar* documentTabs_ = nullptr;
    QDockWidget* toolsDock_ = nullptr;
    // Right-rail tab groups: top (led by Layers) split above bottom.
    QDockWidget* topAnchor_ = nullptr;
    QDockWidget* bottomAnchor_ = nullptr;
    // First show() only: pre-show raise() calls do not stick on hidden tab
    // groups, so the default tab selection is asserted once visible.
    bool shownOnce_ = false;
    // Stable group anchor: the existing anchor while still docked, else the
    // first still-docked group member. Never reassign on plain show/hide.
    QDockWidget* groupAnchor(bool top);
    void setGroupAnchor(bool top, QDockWidget* anchor);
    // Re-tabify a group's members onto `first` so it sits leftmost.
    void leadGroupWith(bool top, QDockWidget* first);
    // Ids already tabified into a rail group. Hiding keeps tab membership,
    // so a re-shown member must NOT re-tabify (that would shove its tab to
    // the end of the bar); only never-grouped docks join.
    QSet<QString> railGrouped_;
    WorkspaceManager* workspaces_ = nullptr;

    QMenu* windowMenu_ = nullptr;
    QMenu* workspaceMenu_ = nullptr;
    QToolButton* workspaceButton_ = nullptr;

    QAction* undoAction_ = nullptr;   // Edit ▸ Undo (Ctrl+Z), label synced
    QAction* redoAction_ = nullptr;   // Edit ▸ Redo (Ctrl+Shift+Z), label synced

    QLineEdit* zoomField_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* deviceLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;

    QTimer* autosaveTimer_ = nullptr;

    QString lastFilterId_;
    std::vector<double> lastFilterParams_;

    QHash<QString, QPointer<QDockWidget>> docks_;
    QHash<QString, QPointer<QWidget>> floatingPanels_;
    QHash<QString, QAction*> panelActions_;

    // Spring-loaded shortcut bookkeeping (R21): a tool letter held for longer
    // than the dwell time is a temporary switch that pops on key release.
    ToolId springTool_ = ToolId::Move;
    bool springActive_ = false;
    ToolId toolBeforeLiquify_ = ToolId::Move;
    bool spaceHeld_ = false;
    bool ctrlHeld_ = false;
    qint64 springPressTime_ = 0;
    // Two-digit opacity entry (tap 4 then 5 for 45%). Holds the
    // first digit and its timestamp; -1 when idle.
    int numFirst_ = -1;
    qint64 numFirstMs_ = 0;
    // Keymap-driven ApplicationShortcut carriers (keymap id + action),
    // installed once and re-pointed by refreshCarrierShortcuts().
    QVector<QPair<QString, QAction*>> carrierShortcuts_;
    // Last-loaded keymap.json (defaults included): the event filter matches
    // remappable canvas keys (brush size) against this instead of hitting
    // disk per keypress. Refreshed with the carriers.
    QMap<QString, QString> keymapCache_;
    // View-menu checkmark sync: each checkable View action registers a
    // refresher re-reading live state; every View submenu re-runs them on
    // aboutToShow so the menu never shows a stale check after a Settings or
    // canvas change made elsewhere.
    QVector<std::function<void()>> viewMenuRefreshers_;

    QByteArray standardState_;
    bool suppressTabSync_ = false;
    // Re-entry guard for the (modal) project manager start page.
    bool startPageActive_ = false;
};

}  // namespace pittore::ui
