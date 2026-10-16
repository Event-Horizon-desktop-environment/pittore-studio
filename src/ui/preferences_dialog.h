#pragma once
#include <QDialog>
#include <QHash>
#include <QMap>
#include <QStringList>
#include <QVector>

#include <functional>

#include "ui/ai_models.h"
#include "ui/app_state.h"

class QComboBox;
class QDoubleSpinBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTableWidget;

namespace pittore::ui {

// Optional extra rows for the Advanced page's on-disk data section
// (parallel display/path lists, e.g. the crash-recovery snapshot and the
// project library, which only the host window knows).
struct AdvancedHooks {
    QStringList extraNames;
    QStringList extraPaths;
};

// Optional live hooks for the Canvas page: writers that push the Show
// toggles onto the live canvas. Settings are always persisted; the hooks
// (null in tests) additionally update the running view on OK.
struct ViewHooks {
    std::function<void(bool)> setRulers;
    std::function<void(bool)> setGuides;
    std::function<void(bool)> setGrid;
    std::function<void(bool)> setSelectionEdges;
    std::function<void(bool)> setSmartGuides;
    std::function<void(bool)> setPixelGrid;
    std::function<void(bool)> setExtras;
};

// Optional live hooks for the Interface page (toolbar layout + workspace
// preset). MainWindow fills them from its ToolsPanel/WorkspaceManager; empty
// keeps the page to persisted settings only (tests). Plain data + callbacks
// so the dialog never links the dock/panel machinery.
struct InterfaceHooks {
    bool hasToolbar = false;
    bool twoColumn = false;
    QStringList workspaceNames;  // display labels, parallel to workspaceIds
    QStringList workspaceIds;    // ids to apply
    QString currentWorkspace;
    std::function<void(bool)> setTwoColumn;
    std::function<void()> showAllTools;
    std::function<void(const QString&)> applyWorkspace;
};

// iOS-style toggle switch backed by QAbstractButton state (isChecked,
// setChecked, toggled), so it drops into any checkbox slot.
class Switch;

// Edit ▸ Settings dialog. Reads the current AppSettings and commits
// everything to AppState::applySettings on OK (persisted to Settings.toml).
//
// conventional shell: a searchable category list on the left, the selected
// category's card rows on the right, OK/Cancel at the foot.
// Categories:
//   General            — theme, zoom/scroll, crash recovery.
//   Interface          — tablet mode (enlarged targets).
//   Canvas             — grid spacing, snapping master + Snap To targets.
//   Cursors            — OS cursor shape, canvas outline + painting behaviour.
//   Documents          — new-document size, resolution, color mode, background.
//   Export             — default format, quality, save location, ICC embed.
//   Performance        — GPU enable/select, CPU device, memory budget.
//   Machine Learning   — background-removal model catalogue.
//   Shortcuts          — editable keymap (keymap.json), reset to defaults.
//   Advanced           — developer reference mask + on-disk data paths.
class PreferencesDialog final : public QDialog {
    Q_OBJECT

  public:
    // `initialTab` deep-links to a category (English id, case-insensitive:
    // "General", "Interface", "Canvas", "Cursors", "Documents", "Export",
    // "Performance", "Machine Learning", "Shortcuts", "Advanced").
    // `hooks` supplies the live toolbar/workspace state for the Interface
    // page; empty keeps it to persisted settings only.
    explicit PreferencesDialog(AppState* state, QWidget* parent = nullptr,
                               const QString& initialTab = {},
                               const InterfaceHooks& hooks = {},
                               const ViewHooks& view = {},
                               const AdvancedHooks& advanced = {});

  signals:
    // Emitted on accept after a changed keymap is saved, so the host can
    // re-apply shortcuts to live menus without a restart.
    void keymapChanged();

  private:
    QWidget* buildGeneralPage();
    QWidget* buildInterfacePage();
    QWidget* buildCanvasPage();
    QWidget* buildCursorsPage();
    QWidget* buildDocumentsPage();
    QWidget* buildExportPage();
    QWidget* buildPerformancePage();
    QWidget* buildMachineLearningPage();
    QWidget* buildShortcutsPage();
    QWidget* buildAdvancedPage();
    void filterNav(const QString& text);

    void rebuildDeviceCombos();
    void updateGpuEnabled(bool on);
    // Refresh the memory-budget readout next to the slider.
    void updateRamLimitLabel();
    void updateExportQualityLabel();
    void loadShortcutEdits();
    bool saveShortcutEdits();

    // AI tab maintenance.
    void refreshAi();          // re-read the store: combo, rows, note
    void rebuildModelCombo();  // keep the pending selection
    void updateModelNote();
    void updateAiCacheLabel();
    QWidget* makeAiRow(const AiModel& model, QWidget* parent);

    AppState* state_;
    InterfaceHooks hooks_;
    ViewHooks view_;
    AdvancedHooks advanced_;
    QLineEdit* searchEdit_ = nullptr;
    QListWidget* navList_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QLabel* sectionTitle_ = nullptr;
    Switch* gpuEnabled_ = nullptr;
    QComboBox* gpuDevice_ = nullptr;
    QComboBox* cpuDevice_ = nullptr;
    // False when the binary enumerated no GPU backend (release builds
    // compile CUDA/HIP out): the toggle and combo stay disabled with an
    // explanation instead of an eternally empty dropdown.
    bool gpuAvailable_ = true;
    // Working memory budget as a slider: 0 = unlimited, otherwise MB capped
    // at detected system RAM. ramTotalMb_ <= 0 means RAM is undetectable and
    // the slider falls back to a fixed range.
    QSlider* ramLimit_ = nullptr;
    QLabel* ramLimitLabel_ = nullptr;
    int ramTotalMb_ = 0;
    QSpinBox* undoLimit_ = nullptr;
    QComboBox* theme_ = nullptr;
    Switch* autosaveEnabled_ = nullptr;
    QSpinBox* autosaveInterval_ = nullptr;
    Switch* reopenDocuments_ = nullptr;
    Switch* zoomWithScroll_ = nullptr;
    Switch* tabletMode_ = nullptr;
    Switch* hidpiEnabled_ = nullptr;
    Switch* hidpiFractional_ = nullptr;
    QComboBox* fontPreview_ = nullptr;
    Switch* twoColumnTools_ = nullptr;
    QComboBox* workspaceCombo_ = nullptr;
    QDoubleSpinBox* gridSpacing_ = nullptr;
    Switch* snapDefault_ = nullptr;
    Switch* snapGuides_ = nullptr;
    Switch* snapGrid_ = nullptr;
    Switch* snapLayers_ = nullptr;
    Switch* snapSlices_ = nullptr;
    Switch* snapBounds_ = nullptr;
    Switch* showRulers_ = nullptr;
    Switch* showGuides_ = nullptr;
    Switch* showGrid_ = nullptr;
    Switch* showSelectionEdges_ = nullptr;
    Switch* showSmartGuides_ = nullptr;
    Switch* showPixelGrid_ = nullptr;
    Switch* showExtras_ = nullptr;
    Switch* showSliceNumbers_ = nullptr;
    QSpinBox* gridSubdivisions_ = nullptr;
    QDoubleSpinBox* nudgeStep_ = nullptr;
    QDoubleSpinBox* nudgeShiftStep_ = nullptr;
    QPushButton* guideColorBtn_ = nullptr;
    QPushButton* gridColorBtn_ = nullptr;
    QSpinBox* transparencyCell_ = nullptr;
    QPushButton* transparencyLightBtn_ = nullptr;
    QPushButton* transparencyDarkBtn_ = nullptr;
    QComboBox* cursorShape_ = nullptr;
    QComboBox* outlineShape_ = nullptr;
    Switch* showOutlineWhilePainting_ = nullptr;
    Switch* outlineEffectiveSize_ = nullptr;

    QSpinBox* newDocWidth_ = nullptr;
    QSpinBox* newDocHeight_ = nullptr;
    QSpinBox* newDocDpi_ = nullptr;
    QComboBox* newDocColorMode_ = nullptr;
    QComboBox* newDocBackground_ = nullptr;
    QComboBox* workingProfile_ = nullptr;
    QComboBox* mismatchPolicy_ = nullptr;
    QSpinBox* recentMax_ = nullptr;

    QComboBox* exportFormat_ = nullptr;
    QSlider* exportQuality_ = nullptr;
    QLabel* exportQualityLabel_ = nullptr;
    QComboBox* exportLocation_ = nullptr;
    Switch* exportEmbedIcc_ = nullptr;
    QComboBox* psdCompression_ = nullptr;

    QTableWidget* shortcutTable_ = nullptr;
    QLineEdit* shortcutFilter_ = nullptr;
    QMap<QString, QKeySequenceEdit*> shortcutEdits_;
    QMap<QString, QString> shortcutDefaults_;
    void filterShortcuts(const QString& text);
    void updateShortcutConflicts();

    QComboBox* bgModel_ = nullptr;
    QLabel* bgModelNote_ = nullptr;
    QComboBox* enhanceModel_ = nullptr;
    QLabel* enhanceModelNote_ = nullptr;
    QLabel* aiCacheLabel_ = nullptr;
    QVector<std::function<void()>> aiRefreshers_;
    QHash<QString, int> aiProgress_;
    QLineEdit* referenceMask_ = nullptr;
};

}  // namespace pittore::ui
