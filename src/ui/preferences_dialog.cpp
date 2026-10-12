#include "ui/preferences_dialog.h"
#include "ui/theme.h"

#include <QAbstractButton>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "engine/compute/factory.h"
#include "ui/brushes/brush_library.h"
#include "ui/export/shared/export_helpers.h"
#include "ui/keymap.h"
#include "ui/logging.h"
#include "ui/tools/defs/tool_defs.h"

namespace pittore::ui {
namespace {

QString dimStyle(AppState* state) {
    return QStringLiteral("color: %1;").arg(colorsFor(state->theme()).textDim.name());
}

}  // namespace

// iOS-style toggle switch backed by QAbstractButton state.
class Switch final : public QAbstractButton {
  public:
    explicit Switch(QWidget* parent = nullptr) : QAbstractButton(parent) {
        setCheckable(true);
        setFixedSize(40, 22);
        setCursor(Qt::PointingHandCursor);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QPalette pal = palette();
        const QColor track = isChecked() ? pal.color(QPalette::Highlight)
                                         : pal.color(QPalette::Midlight);
        const QColor edge = pal.color(QPalette::Mid);
        const QRectF groove(1, 3, width() - 2, height() - 6);
        p.setPen(QPen(edge, 1));
        p.setBrush(track);
        p.drawRoundedRect(groove, groove.height() / 2, groove.height() / 2);
        const qreal knobD = height() - 8;
        const qreal knobX = isChecked() ? width() - 4 - knobD : 4;
        p.setPen(Qt::NoPen);
        p.setBrush(isEnabled() ? Qt::white : pal.color(QPalette::ButtonText));
        p.drawEllipse(QRectF(knobX, 4, knobD, knobD));
    }
};

namespace {

// Card row: label on the left, control right-aligned, in a bordered card.
QWidget* cardRow(const QString& label, QWidget* control, QWidget* parent,
                 const QString& tooltip = {}) {
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("prefCard"));
    const QPalette pal = row->palette();
    row->setStyleSheet(
        QStringLiteral("QWidget#prefCard { background: %1; border: 1px solid %2; border-radius: 5px; }")
            .arg(pal.color(QPalette::AlternateBase).name(),
                 pal.color(QPalette::Mid).name()));
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(12, 8, 12, 8);
    h->setSpacing(12);
    auto* name = new QLabel(label, row);
    name->setWordWrap(true);
    h->addWidget(name, 1);
    h->addWidget(control, 0, Qt::AlignVCenter);
    if (!tooltip.isEmpty()) {
        row->setToolTip(tooltip);
        control->setToolTip(tooltip);
    }
    return row;
}

// Small section heading inside a page ("Document", "Recovery", ...).
QLabel* sectionHeader(const QString& text, QWidget* parent) {
    auto* h = new QLabel(text, parent);
    QFont f = h->font();
    f.setBold(true);
    h->setFont(f);
    return h;
}

// Paint a color-well button with its color (hex label, contrasting text).
void syncColorWell(QPushButton* btn, const QColor& c) {
    btn->setProperty("wellColor", c);
    btn->setText(c.name(QColor::HexArgb).toUpper());
    btn->setStyleSheet(
        QStringLiteral("background: %1; color: %2; border-radius: 4px;")
            .arg(c.name(), c.lightness() > 128 ? QStringLiteral("black")
                                               : QStringLiteral("white")));
}

// Color well button holding its color in the "wellColor" property; click
// opens a picker with alpha enabled (canvas inks carry alpha).
QPushButton* colorWell(const QColor& initial, QWidget* parent,
                       const QString& title) {
    auto* btn = new QPushButton(parent);
    btn->setFixedSize(96, 26);
    btn->setCursor(Qt::PointingHandCursor);
    syncColorWell(btn, initial);
    QObject::connect(btn, &QPushButton::clicked, btn, [btn, title] {
        const QColor picked = QColorDialog::getColor(
            btn->property("wellColor").value<QColor>(), btn, title,
            QColorDialog::ShowAlphaChannel);
        if (picked.isValid()) syncColorWell(btn, picked);
    });
    return btn;
}

}  // namespace

PreferencesDialog::PreferencesDialog(AppState* state, QWidget* parent,
                                       const QString& initialTab,
                                       const InterfaceHooks& hooks,
                                       const ViewHooks& view,
                                       const AdvancedHooks& advanced)
    : QDialog(parent), state_(state), hooks_(hooks), view_(view), advanced_(advanced) {
    setWindowTitle(tr("Settings"));
    resize(780, 580);

    // -- Left: search + category nav --------------------------------------
    auto* left = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);
    searchEdit_ = new QLineEdit(left);
    searchEdit_->setPlaceholderText(tr("Search"));
    searchEdit_->setClearButtonEnabled(true);
    leftLayout->addWidget(searchEdit_);
    navList_ = new QListWidget(left);
    navList_->setFixedWidth(190);
    navList_->setUniformItemSizes(true);
    navList_->setSpacing(4);
    const QList<QPair<QString, QString>> tabs{
        {tr("General"), QStringLiteral("General")},
        {tr("Interface"), QStringLiteral("Interface")},
        {tr("Canvas"), QStringLiteral("Canvas")},
        {tr("Cursors"), QStringLiteral("Cursors")},
        {tr("Documents"), QStringLiteral("Documents")},
        {tr("Export"), QStringLiteral("Export")},
        {tr("Performance"), QStringLiteral("Performance")},
        {tr("Machine Learning"), QStringLiteral("Machine Learning")},
        {tr("Shortcuts"), QStringLiteral("Shortcuts")},
        {tr("Advanced"), QStringLiteral("Advanced")},
    };
    for (const auto& tab : tabs) {
        // Display text stays translated; the English id rides in UserRole so
        // callers can deep-link without depending on the locale.
        auto* item = new QListWidgetItem(tab.first, navList_);
        item->setData(Qt::UserRole, tab.second);
    }
    leftLayout->addWidget(navList_, 1);

    // -- Right: section title + stacked pages ------------------------------
    auto* right = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(10);
    sectionTitle_ = new QLabel(tr("General"), right);
    QFont titleFont = sectionTitle_->font();
    titleFont.setPointSize(titleFont.pointSize() + 2);
    titleFont.setBold(true);
    sectionTitle_->setFont(titleFont);
    rightLayout->addWidget(sectionTitle_);
    pages_ = new QStackedWidget(right);
    pages_->addWidget(buildGeneralPage());
    pages_->addWidget(buildInterfacePage());
    pages_->addWidget(buildCanvasPage());
    pages_->addWidget(buildCursorsPage());
    pages_->addWidget(buildDocumentsPage());
    pages_->addWidget(buildExportPage());
    pages_->addWidget(buildPerformancePage());
    pages_->addWidget(buildMachineLearningPage());
    pages_->addWidget(buildShortcutsPage());
    pages_->addWidget(buildAdvancedPage());
    rightLayout->addWidget(pages_, 1);

    auto* body = new QHBoxLayout;
    body->setSpacing(12);
    body->addWidget(left, 0);
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::NoFrame);
    sep->setFixedWidth(1);
    sep->setStyleSheet(QStringLiteral("QFrame { background: %1; border: none; }").arg(cssColor(QColor(255, 255, 255, kDividerAlpha))));
    body->addWidget(sep);
    body->addWidget(right, 1);

    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* footer = new QHBoxLayout;
    footer->addStretch(1);
    footer->addWidget(buttons);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);
    root->addLayout(body, 1);
    root->addLayout(footer);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(navList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= pages_->count()) return;
        pages_->setCurrentIndex(row);
        if (QListWidgetItem* item = navList_->item(row))
            sectionTitle_->setText(item->text());
    });
    connect(searchEdit_, &QLineEdit::textChanged, this,
            &PreferencesDialog::filterNav);
    // Deep-link: "Performance", "Shortcuts", ... (case-insensitive English
    // id). Unknown/empty falls back to General.
    int initialRow = 0;
    if (!initialTab.isEmpty()) {
        for (int i = 0; i < navList_->count(); ++i) {
            if (navList_->item(i)->data(Qt::UserRole).toString().compare(
                    initialTab, Qt::CaseInsensitive) == 0) {
                initialRow = i;
                break;
            }
        }
    }
    navList_->setCurrentRow(initialRow);

    // -- Populate from current settings ---------------------------------------
    const AppSettings s = state_->settings();
    gpuEnabled_->setChecked(s.gpuEnabled);
    theme_->setCurrentIndex(theme_->findData(static_cast<int>(s.theme)));
    ramLimit_->setValue(
        qBound(ramLimit_->minimum(), s.ramLimitMb, ramLimit_->maximum()));
    updateRamLimitLabel();
    autosaveEnabled_->setChecked(s.autosaveEnabled);
    autosaveInterval_->setValue(qBound(1, s.autosaveIntervalMinutes, 60));
    autosaveInterval_->setEnabled(s.autosaveEnabled);
    reopenDocuments_->setChecked(s.reopenDocuments);
    zoomWithScroll_->setChecked(s.zoomWithScroll);
    tabletMode_->setChecked(s.tabletMode);
    fontPreview_->setCurrentIndex(
        qMax(0, fontPreview_->findData(qBound(0, s.fontPreviewSize, 3))));
    if (twoColumnTools_) twoColumnTools_->setChecked(hooks_.twoColumn);
    if (workspaceCombo_) {
        const int wi = workspaceCombo_->findData(hooks_.currentWorkspace);
        workspaceCombo_->setCurrentIndex(wi >= 0 ? wi : 0);
    }
    gridSpacing_->setValue(qBound(2.0, s.gridSpacing, 1024.0));
    snapDefault_->setChecked(s.snapEnabled);
    const int targets = state_->snapTargets();
    snapGuides_->setChecked(targets & AppState::SnapGuides);
    snapGrid_->setChecked(targets & AppState::SnapGrid);
    snapLayers_->setChecked(targets & AppState::SnapLayers);
    snapSlices_->setChecked(targets & AppState::SnapSlices);
    snapBounds_->setChecked(targets & AppState::SnapDocumentBounds);
    const bool snapOn = s.snapEnabled;
    snapGuides_->setEnabled(snapOn);
    snapGrid_->setEnabled(snapOn);
    snapLayers_->setEnabled(snapOn);
    snapSlices_->setEnabled(snapOn);
    snapBounds_->setEnabled(snapOn);
    showRulers_->setChecked(s.showRulers);
    showGuides_->setChecked(s.showGuides);
    showGrid_->setChecked(s.showGrid);
    showSelectionEdges_->setChecked(s.showSelectionEdges);
    showSmartGuides_->setChecked(s.showSmartGuides);
    showPixelGrid_->setChecked(s.showPixelGrid);
    showExtras_->setChecked(s.showExtras);
    showSliceNumbers_->setChecked(s.showSliceNumbers);
    gridSubdivisions_->setValue(qBound(1, s.gridSubdivisions, 16));
    nudgeStep_->setValue(qBound(0.1, s.nudgeStepPx, 1000.0));
    nudgeShiftStep_->setValue(qBound(0.1, s.nudgeShiftStepPx, 10000.0));
    syncColorWell(guideColorBtn_, s.guideColor);
    syncColorWell(gridColorBtn_, s.gridColor);
    transparencyCell_->setValue(qBound(4, s.transparencyCellPx, 64));
    syncColorWell(transparencyLightBtn_, s.transparencyLight);
    syncColorWell(transparencyDarkBtn_, s.transparencyDark);
    recentMax_->setValue(qBound(0, s.recentMax, 100));
    undoLimit_->setValue(qBound(1, s.undoLimit, 1000));
    {
        const int wi = workingProfile_->findData(s.workingProfile);
        workingProfile_->setCurrentIndex(wi >= 0 ? wi : 0);
    }
    mismatchPolicy_->setCurrentIndex(
        mismatchPolicy_->findData(qBound(0, s.colorMismatchPolicy, 3)));
    cursorShape_->setCurrentIndex(
        qBound(0, cursorShape_->findData(s.cursorShape), cursorShape_->count() - 1));
    outlineShape_->setCurrentIndex(
        qBound(0, outlineShape_->findData(s.outlineShape), outlineShape_->count() - 1));
    showOutlineWhilePainting_->setChecked(s.showOutlineWhilePainting);
    outlineEffectiveSize_->setChecked(s.outlineEffectiveSize);
    newDocWidth_->setValue(qBound(1, s.newDocWidth, 300000));
    newDocHeight_->setValue(qBound(1, s.newDocHeight, 300000));
    newDocDpi_->setValue(qBound(1, s.newDocDpi, 10000));
    {
        const int mi = newDocColorMode_->findData(s.newDocColorMode);
        newDocColorMode_->setCurrentIndex(mi >= 0 ? mi : 0);
        const int bi = newDocBackground_->findData(s.newDocBackground);
        newDocBackground_->setCurrentIndex(bi >= 0 ? bi : 0);
    }
    {
        // The available formats depend on runtime plugins; fall back to png
        // when the stored default is not offered on this machine.
        const int fi = exportFormat_->findData(s.exportFormat);
        exportFormat_->setCurrentIndex(fi >= 0 ? fi : qMax(0, exportFormat_->findData(QStringLiteral("png"))));
    }
    exportQuality_->setValue(qBound(1, s.exportQuality, 100));
    updateExportQualityLabel();
    exportLocation_->setCurrentIndex(qBound(0, s.exportLocation, 2));
    exportEmbedIcc_->setChecked(s.exportEmbedIcc);
    {
        const int pi = psdCompression_->findData(qBound(0, s.psdCompression, 1));
        psdCompression_->setCurrentIndex(pi >= 0 ? pi : 0);
    }
    loadShortcutEdits();
    referenceMask_->setText(s.referenceMask);

    rebuildDeviceCombos();
    if (!s.gpuDevice.isEmpty()) gpuDevice_->setCurrentText(s.gpuDevice);
    if (!s.cpuDevice.isEmpty()) cpuDevice_->setCurrentText(s.cpuDevice);
    updateGpuEnabled(s.gpuEnabled);

    // -- Apply on accept ------------------------------------------------------
    connect(this, &QDialog::accepted, this, [this] {
        AppSettings next = state_->settings();
        next.gpuEnabled = gpuEnabled_->isChecked();
        next.gpuDevice = gpuDevice_->currentData().toString();
        next.cpuDevice = cpuDevice_->currentData().toString();
        next.ramLimitMb = ramLimit_->value();
        next.theme = static_cast<UiTheme>(theme_->currentData().toInt());
        next.autosaveEnabled = autosaveEnabled_->isChecked();
        next.autosaveIntervalMinutes = autosaveInterval_->value();
        next.reopenDocuments = reopenDocuments_->isChecked();
        next.zoomWithScroll = zoomWithScroll_->isChecked();
        next.tabletMode = tabletMode_->isChecked();
        next.fontPreviewSize = fontPreview_->currentData().toInt();
        if (twoColumnTools_ && hooks_.setTwoColumn)
            hooks_.setTwoColumn(twoColumnTools_->isChecked());
        if (workspaceCombo_ && workspaceCombo_->currentIndex() >= 0 &&
            hooks_.applyWorkspace) {
            const QString want = workspaceCombo_->currentData().toString();
            if (want != hooks_.currentWorkspace) hooks_.applyWorkspace(want);
        }
        next.gridSpacing = gridSpacing_->value();
        next.snapEnabled = snapDefault_->isChecked();
        int newTargets = 0;
        if (snapGuides_->isChecked()) newTargets |= AppState::SnapGuides;
        if (snapGrid_->isChecked()) newTargets |= AppState::SnapGrid;
        if (snapLayers_->isChecked()) newTargets |= AppState::SnapLayers;
        if (snapSlices_->isChecked()) newTargets |= AppState::SnapSlices;
        if (snapBounds_->isChecked()) newTargets |= AppState::SnapDocumentBounds;
        next.snapTargets = newTargets;
        next.showRulers = showRulers_->isChecked();
        next.showGuides = showGuides_->isChecked();
        next.showGrid = showGrid_->isChecked();
        next.showSelectionEdges = showSelectionEdges_->isChecked();
        next.showSmartGuides = showSmartGuides_->isChecked();
        next.showPixelGrid = showPixelGrid_->isChecked();
        next.showExtras = showExtras_->isChecked();
        next.showSliceNumbers = showSliceNumbers_->isChecked();
        next.gridSubdivisions = gridSubdivisions_->value();
        next.nudgeStepPx = nudgeStep_->value();
        next.nudgeShiftStepPx = nudgeShiftStep_->value();
        next.guideColor = guideColorBtn_->property("wellColor").value<QColor>();
        next.gridColor = gridColorBtn_->property("wellColor").value<QColor>();
        next.transparencyCellPx = transparencyCell_->value();
        next.transparencyLight =
            transparencyLightBtn_->property("wellColor").value<QColor>();
        next.transparencyDark =
            transparencyDarkBtn_->property("wellColor").value<QColor>();
        next.recentMax = recentMax_->value();
        next.undoLimit = undoLimit_->value();
        next.workingProfile = workingProfile_->currentData().toString();
        if (next.workingProfile.isEmpty())
            next.workingProfile = QStringLiteral("sRGB IEC61966-2.1");
        next.colorMismatchPolicy = mismatchPolicy_->currentData().toInt();
        next.cursorShape = cursorShape_->currentData().toInt();
        next.outlineShape = outlineShape_->currentData().toInt();
        next.showOutlineWhilePainting = showOutlineWhilePainting_->isChecked();
        next.outlineEffectiveSize = outlineEffectiveSize_->isChecked();
        next.newDocWidth = newDocWidth_->value();
        next.newDocHeight = newDocHeight_->value();
        next.newDocDpi = newDocDpi_->value();
        next.newDocColorMode = newDocColorMode_->currentData().toString();
        next.newDocBackground = newDocBackground_->currentData().toString();
        next.exportFormat = exportFormat_->currentData().toString();
        next.exportQuality = exportQuality_->value();
        next.exportLocation = exportLocation_->currentIndex();
        next.exportEmbedIcc = exportEmbedIcc_->isChecked();
        next.psdCompression = qBound(0, psdCompression_->currentData().toInt(), 1);
        next.referenceMask = referenceMask_->text().trimmed();
        if (bgModel_ && bgModel_->currentIndex() >= 0)
            next.bgModel = bgModel_->currentData().toString();
        if (enhanceModel_ && enhanceModel_->currentIndex() >= 0)
            next.enhanceModel = enhanceModel_->currentData().toString();
        state_->applySettings(next);
        // Push the Show toggles onto the live canvas (persistence already
        // handled by applySettings above).
        if (view_.setRulers) view_.setRulers(next.showRulers);
        if (view_.setGuides) view_.setGuides(next.showGuides);
        if (view_.setGrid) view_.setGrid(next.showGrid);
        if (view_.setSelectionEdges) view_.setSelectionEdges(next.showSelectionEdges);
        if (view_.setSmartGuides) view_.setSmartGuides(next.showSmartGuides);
        if (view_.setPixelGrid) view_.setPixelGrid(next.showPixelGrid);
        if (view_.setExtras) view_.setExtras(next.showExtras);
        if (saveShortcutEdits()) emit keymapChanged();
    });
}

QWidget* PreferencesDialog::buildPerformancePage() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(8);

    gpuEnabled_ = new Switch(page);
    column->addWidget(cardRow(tr("Use GPU acceleration (when available)"),
                              gpuEnabled_, page));
    gpuDevice_ = new QComboBox(page);
    column->addWidget(cardRow(tr("GPU device"), gpuDevice_, page));
    cpuDevice_ = new QComboBox(page);
    column->addWidget(cardRow(tr("CPU device"), cpuDevice_, page));

    ramTotalMb_ = totalSystemRamMb();
    const int ramMax = ramTotalMb_ > 0 ? ramTotalMb_ : 65535;
    ramLimit_ = new QSlider(Qt::Horizontal, page);
    ramLimit_->setRange(0, ramMax);
    ramLimit_->setSingleStep(256);
    ramLimit_->setPageStep(4096);
    ramLimit_->setToolTip(
        tr("Working memory budget per document (fresh installs default to "
           "90% of system memory).\n"
           "Large imports are automatically downscaled to a proxy that fits\n"
           "instead of being refused; only an explicit new document larger\n"
           "than this budget is refused. Far left (0) = no budget."));
    ramLimitLabel_ = new QLabel(page);
    ramLimitLabel_->setMinimumWidth(140);
    connect(ramLimit_, &QSlider::valueChanged, this,
            &PreferencesDialog::updateRamLimitLabel);
    auto* ramRow = new QWidget(page);
    auto* ramLayout = new QHBoxLayout(ramRow);
    ramLayout->setContentsMargins(0, 0, 0, 0);
    ramLayout->addWidget(ramLimit_, 1);
    ramLayout->addWidget(ramLimitLabel_);
    column->addWidget(cardRow(tr("Memory budget"), ramRow, page,
                              ramLimit_->toolTip()));

    column->addWidget(sectionHeader(tr("History"), page));
    undoLimit_ = new QSpinBox(page);
    undoLimit_->setRange(1, 1000);
    undoLimit_->setToolTip(
        tr("Undo steps kept per document. More steps hold more pixel "
           "snapshots in memory."));
    column->addWidget(cardRow(tr("Undo limit"), undoLimit_, page,
                              undoLimit_->toolTip()));
    column->addStretch(1);

    connect(gpuEnabled_, &QAbstractButton::toggled, this,
            &PreferencesDialog::updateGpuEnabled);
    return page;
}

QWidget* PreferencesDialog::buildGeneralPage() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(8);

    column->addWidget(sectionHeader(tr("Interface"), page));
    theme_ = new QComboBox(page);
    for (const auto& entry :
         QVector<QPair<QString, UiTheme>>{
             {tr("Black"), UiTheme::Black},
             {tr("Dark Gray"), UiTheme::DarkGray},
             {tr("Medium Gray"), UiTheme::MediumGray},
             {tr("Light Gray"), UiTheme::LightGray}})
        theme_->addItem(entry.first, static_cast<int>(entry.second));
    column->addWidget(cardRow(tr("Theme"), theme_, page));
    zoomWithScroll_ = new Switch(page);
    zoomWithScroll_->setToolTip(tr("Swap wheel and Ctrl+wheel: wheel zooms, Ctrl+wheel scrolls."));
    column->addWidget(cardRow(tr("Zoom with scroll wheel"), zoomWithScroll_,
                              page, zoomWithScroll_->toolTip()));

    // R98 crash recovery: periodic write-ahead snapshots of unsaved work.
    column->addWidget(sectionHeader(tr("Crash recovery"), page));
    autosaveEnabled_ = new Switch(page);
    column->addWidget(cardRow(tr("Save a recovery snapshot automatically"),
                              autosaveEnabled_, page));
    autosaveInterval_ = new QSpinBox(page);
    autosaveInterval_->setRange(1, 60);
    autosaveInterval_->setSuffix(tr(" min"));
    autosaveInterval_->setToolTip(
        tr("How often an unsaved document is written to a recovery file.\n"
           "After a crash the file is offered for recovery on the next launch."));
    column->addWidget(cardRow(tr("Interval"), autosaveInterval_, page,
                              autosaveInterval_->toolTip()));

    column->addWidget(sectionHeader(tr("Session"), page));
    reopenDocuments_ = new Switch(page);
    reopenDocuments_->setToolTip(
        tr("Reopen file-backed documents on startup (unsaved work is not "
           "restored)."));
    column->addWidget(cardRow(tr("Reopen documents on startup"), reopenDocuments_,
                              page, reopenDocuments_->toolTip()));
    column->addStretch(1);
    connect(autosaveEnabled_, &QAbstractButton::toggled, autosaveInterval_,
            &QWidget::setEnabled);
    return page;
}

QWidget* PreferencesDialog::buildInterfacePage() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(8);

    column->addWidget(sectionHeader(tr("Touch & pen"), page));
    tabletMode_ = new Switch(page);
    tabletMode_->setToolTip(
        tr("Enlarge tool strip and persona bar targets for pen taps."));
    column->addWidget(cardRow(tr("Tablet mode"), tabletMode_, page,
                              tabletMode_->toolTip()));

    column->addWidget(sectionHeader(tr("Text"), page));
    fontPreview_ = new QComboBox(page);
    fontPreview_->addItem(tr("None"), 0);
    fontPreview_->addItem(tr("Small"), 1);
    fontPreview_->addItem(tr("Medium"), 2);
    fontPreview_->addItem(tr("Large"), 3);
    fontPreview_->setToolTip(
        tr("Render each family in its own typeface in font lists."));
    column->addWidget(cardRow(tr("Font preview size"), fontPreview_, page,
                              fontPreview_->toolTip()));

    if (hooks_.hasToolbar) {
        column->addWidget(sectionHeader(tr("Toolbar"), page));
        twoColumnTools_ = new Switch(page);
        twoColumnTools_->setToolTip(tr("Two-column tool strip."));
        column->addWidget(cardRow(tr("Two-column tools"), twoColumnTools_,
                                  page, twoColumnTools_->toolTip()));
        auto* showAll = new QPushButton(tr("Show all tools"), page);
        showAll->setToolTip(
            tr("Unhide every tool hidden via Customize Toolbar."));
        column->addWidget(cardRow(tr("Hidden tools"), showAll, page,
                                  showAll->toolTip()));
        connect(showAll, &QPushButton::clicked, this, [this] {
            if (hooks_.showAllTools) hooks_.showAllTools();
        });
    }
    if (!hooks_.workspaceIds.isEmpty()) {
        column->addWidget(sectionHeader(tr("Workspace"), page));
        workspaceCombo_ = new QComboBox(page);
        for (int i = 0; i < hooks_.workspaceIds.size(); ++i)
            workspaceCombo_->addItem(hooks_.workspaceNames.value(i), hooks_.workspaceIds.at(i));
        workspaceCombo_->setToolTip(
            tr("Panel layout to switch to when Settings closes with OK."));
        column->addWidget(cardRow(tr("Preset"), workspaceCombo_, page,
                                  workspaceCombo_->toolTip()));
    }
    column->addStretch(1);
    return page;
}

QWidget* PreferencesDialog::buildCanvasPage() {
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(8);

    column->addWidget(sectionHeader(tr("Grid & guides"), page));
    gridSpacing_ = new QDoubleSpinBox(page);
    gridSpacing_->setRange(2.0, 1024.0);
    gridSpacing_->setSingleStep(8.0);
    gridSpacing_->setSuffix(tr(" px"));
    column->addWidget(cardRow(tr("Grid spacing"), gridSpacing_, page));
    gridColorBtn_ = colorWell(QColor(255, 255, 255, 40), page, tr("Grid color"));
    column->addWidget(cardRow(tr("Grid color"), gridColorBtn_, page));
    guideColorBtn_ =
        colorWell(QColor(0x3d, 0xa5, 0xff), page, tr("Guide color"));
    column->addWidget(cardRow(tr("Guide color"), guideColorBtn_, page));
    gridSubdivisions_ = new QSpinBox(page);
    gridSubdivisions_->setRange(1, 16);
    gridSubdivisions_->setToolTip(
        tr("Fainter lines between major gridlines (1 = off)."));
    column->addWidget(cardRow(tr("Grid subdivisions"), gridSubdivisions_, page,
                              gridSubdivisions_->toolTip()));

    column->addWidget(sectionHeader(tr("Snapping"), page));
    snapDefault_ = new Switch(page);
    snapDefault_->setToolTip(tr("Master toggle for move-drag snapping."));
    column->addWidget(cardRow(tr("Enable snapping"), snapDefault_, page,
                              snapDefault_->toolTip()));
    snapGuides_ = new Switch(page);
    column->addWidget(cardRow(tr("Snap to guides"), snapGuides_, page));
    snapGrid_ = new Switch(page);
    column->addWidget(cardRow(tr("Snap to grid"), snapGrid_, page));
    snapLayers_ = new Switch(page);
    column->addWidget(cardRow(tr("Snap to layers"), snapLayers_, page));
    snapSlices_ = new Switch(page);
    column->addWidget(cardRow(tr("Snap to slices"), snapSlices_, page));
    snapBounds_ = new Switch(page);
    column->addWidget(cardRow(tr("Snap to document bounds"), snapBounds_, page));
    {
        // Bulk helpers mirroring View > Snap To > All/None.
        auto* all = new QPushButton(tr("All"), page);
        auto* none = new QPushButton(tr("None"), page);
        auto* bulk = new QWidget(page);
        auto* bulkLayout = new QHBoxLayout(bulk);
        bulkLayout->setContentsMargins(0, 0, 0, 0);
        bulkLayout->addWidget(all);
        bulkLayout->addWidget(none);
        column->addWidget(cardRow(tr("Targets"), bulk, page));
        connect(all, &QPushButton::clicked, this, [this] {
            snapGuides_->setChecked(true);
            snapGrid_->setChecked(true);
            snapLayers_->setChecked(true);
            snapSlices_->setChecked(true);
            snapBounds_->setChecked(true);
        });
        connect(none, &QPushButton::clicked, this, [this] {
            snapGuides_->setChecked(false);
            snapGrid_->setChecked(false);
            snapLayers_->setChecked(false);
            snapSlices_->setChecked(false);
            snapBounds_->setChecked(false);
        });
    }

    column->addWidget(sectionHeader(tr("Show"), page));
    showRulers_ = new Switch(page);
    column->addWidget(cardRow(tr("Rulers"), showRulers_, page));
    showGuides_ = new Switch(page);
    column->addWidget(cardRow(tr("Guides"), showGuides_, page));
    showGrid_ = new Switch(page);
    column->addWidget(cardRow(tr("Grid"), showGrid_, page));
    showSelectionEdges_ = new Switch(page);
    column->addWidget(cardRow(tr("Selection edges"), showSelectionEdges_, page));
    showSmartGuides_ = new Switch(page);
    column->addWidget(cardRow(tr("Smart guides"), showSmartGuides_, page));
    showPixelGrid_ = new Switch(page);
    column->addWidget(cardRow(tr("Pixel grid"), showPixelGrid_, page));
    showExtras_ = new Switch(page);
    showExtras_->setToolTip(tr("Master toggle for overlay extras."));
    column->addWidget(cardRow(tr("Extras"), showExtras_, page,
                              showExtras_->toolTip()));
    showSliceNumbers_ = new Switch(page);
    showSliceNumbers_->setToolTip(
        tr("Number tags on slice rectangles (the rectangles always draw)."));
    column->addWidget(cardRow(tr("Slice numbers"), showSliceNumbers_, page,
                              showSliceNumbers_->toolTip()));

    column->addWidget(sectionHeader(tr("Nudge"), page));
    nudgeStep_ = new QDoubleSpinBox(page);
    nudgeStep_->setRange(0.1, 1000.0);
    nudgeStep_->setSingleStep(1.0);
    nudgeStep_->setDecimals(1);
    nudgeStep_->setSuffix(tr(" px"));
    nudgeStep_->setToolTip(tr("Arrow-key layer nudge in document pixels."));
    column->addWidget(cardRow(tr("Nudge distance"), nudgeStep_, page,
                              nudgeStep_->toolTip()));
    nudgeShiftStep_ = new QDoubleSpinBox(page);
    nudgeShiftStep_->setRange(0.1, 10000.0);
    nudgeShiftStep_->setSingleStep(5.0);
    nudgeShiftStep_->setDecimals(1);
    nudgeShiftStep_->setSuffix(tr(" px"));
    nudgeShiftStep_->setToolTip(tr("Arrow-key nudge with Shift held."));
    column->addWidget(cardRow(tr("Shift nudge distance"), nudgeShiftStep_, page,
                              nudgeShiftStep_->toolTip()));

    column->addWidget(sectionHeader(tr("Transparency"), page));
    transparencyCell_ = new QSpinBox(page);
    transparencyCell_->setRange(4, 64);
    transparencyCell_->setSuffix(tr(" px"));
    transparencyCell_->setToolTip(tr("Checkerboard cell size on screen."));
    column->addWidget(cardRow(tr("Cell size"), transparencyCell_, page,
                              transparencyCell_->toolTip()));
    transparencyLightBtn_ =
        colorWell(QColor(0xff, 0xff, 0xff), page, tr("Light squares"));
    column->addWidget(cardRow(tr("Light squares"), transparencyLightBtn_, page));
    transparencyDarkBtn_ =
        colorWell(QColor(0xbf, 0xbf, 0xbf), page, tr("Dark squares"));
    column->addWidget(cardRow(tr("Dark squares"), transparencyDarkBtn_, page));
    // Targets only matter while the master toggle is on; keep the values
    // so re-enabling restores the previous set.
    connect(snapDefault_, &QAbstractButton::toggled, this, [this](bool on) {
        snapGuides_->setEnabled(on);
        snapGrid_->setEnabled(on);
        snapLayers_->setEnabled(on);
        snapSlices_->setEnabled(on);
        snapBounds_->setEnabled(on);
    });
    column->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll);
    return page;
}

QWidget* PreferencesDialog::buildCursorsPage() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(8);

    column->addWidget(sectionHeader(tr("Brush cursor"), page));
    cursorShape_ = new QComboBox(page);
    cursorShape_->addItem(tr("Outline Only"), 0);
    cursorShape_->addItem(tr("Arrow"), 1);
    cursorShape_->addItem(tr("Crosshair"), 2);
    cursorShape_->setToolTip(
        tr("System pointer paired with the canvas-drawn tip outline."));
    column->addWidget(cardRow(tr("OS cursor shape"), cursorShape_, page,
                              cursorShape_->toolTip()));
    outlineShape_ = new QComboBox(page);
    outlineShape_->addItem(tr("No Outline"), 0);
    outlineShape_->addItem(tr("Circle"), 1);
    outlineShape_->addItem(tr("Tip Preview"), 2);
    outlineShape_->addItem(tr("Tilt"), 3);
    outlineShape_->setToolTip(tr("Canvas-drawn tip outline around the pointer."));
    column->addWidget(cardRow(tr("Outline shape"), outlineShape_, page,
                              outlineShape_->toolTip()));
    showOutlineWhilePainting_ = new Switch(page);
    showOutlineWhilePainting_->setToolTip(
        tr("Keep the outline visible mid-stroke; otherwise it returns on release."));
    column->addWidget(cardRow(tr("Show while painting"), showOutlineWhilePainting_,
                              page, showOutlineWhilePainting_->toolTip()));
    outlineEffectiveSize_ = new Switch(page);
    outlineEffectiveSize_->setToolTip(
        tr("Size the ring by the maximum diameter instead of the live "
           "pressure/velocity width."));
    column->addWidget(cardRow(tr("Effective ring size"), outlineEffectiveSize_,
                              page, outlineEffectiveSize_->toolTip()));
    column->addStretch(1);
    return page;
}

QWidget* PreferencesDialog::buildDocumentsPage() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(8);

    column->addWidget(sectionHeader(tr("Default size"), page));
    newDocWidth_ = new QSpinBox(page);
    newDocWidth_->setRange(1, 300000);
    newDocWidth_->setSuffix(tr(" px"));
    column->addWidget(cardRow(tr("Width"), newDocWidth_, page));
    newDocHeight_ = new QSpinBox(page);
    newDocHeight_->setRange(1, 300000);
    newDocHeight_->setSuffix(tr(" px"));
    column->addWidget(cardRow(tr("Height"), newDocHeight_, page));
    newDocDpi_ = new QSpinBox(page);
    newDocDpi_->setRange(1, 10000);
    newDocDpi_->setSuffix(tr(" ppi"));
    column->addWidget(cardRow(tr("Resolution"), newDocDpi_, page));

    column->addWidget(sectionHeader(tr("Color & background"), page));
    newDocColorMode_ = new QComboBox(page);
    for (const QString& m : {QStringLiteral("RGB/8"), QStringLiteral("RGB/16"),
                             QStringLiteral("RGB/32"), QStringLiteral("Grayscale/8"),
                             QStringLiteral("CMYK/8"), QStringLiteral("CMYK/16")})
        newDocColorMode_->addItem(m, m);
    column->addWidget(cardRow(tr("Color mode"), newDocColorMode_, page));
    newDocBackground_ = new QComboBox(page);
    newDocBackground_->addItem(tr("White"), QStringLiteral("white"));
    newDocBackground_->addItem(tr("Transparent"), QStringLiteral("transparent"));
    newDocBackground_->addItem(tr("Black"), QStringLiteral("black"));
    column->addWidget(cardRow(tr("Background"), newDocBackground_, page));

    column->addWidget(sectionHeader(tr("Working space"), page));
    workingProfile_ = new QComboBox(page);
    for (const QString& p : {QStringLiteral("sRGB IEC61966-2.1"),
                             QStringLiteral("Adobe RGB (1998)"),
                             QStringLiteral("Display P3")})
        workingProfile_->addItem(p, p);
    workingProfile_->setToolTip(
        tr("Profile new documents and untagged imports assume."));
    column->addWidget(cardRow(tr("Working profile"), workingProfile_, page,
                              workingProfile_->toolTip()));
    mismatchPolicy_ = new QComboBox(page);
    mismatchPolicy_->addItem(tr("Ask on mismatch"), 0);
    mismatchPolicy_->addItem(tr("Convert to working space"), 1);
    mismatchPolicy_->addItem(tr("Keep embedded profile"), 2);
    mismatchPolicy_->addItem(tr("Always ask about embedded profiles"), 3);
    mismatchPolicy_->setToolTip(
        tr("What to do when an imported file's embedded profile does not "
           "match the working profile."));
    column->addWidget(cardRow(tr("On profile mismatch"), mismatchPolicy_, page,
                              mismatchPolicy_->toolTip()));

    column->addWidget(sectionHeader(tr("Recent projects"), page));
    recentMax_ = new QSpinBox(page);
    recentMax_->setRange(0, 100);
    recentMax_->setToolTip(tr("Entries kept in Open Recent (0 keeps none)."));
    column->addWidget(cardRow(tr("Keep entries"), recentMax_, page,
                              recentMax_->toolTip()));
    column->addStretch(1);
    return page;
}

QWidget* PreferencesDialog::buildExportPage() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(8);

    column->addWidget(sectionHeader(tr("Defaults"), page));
    exportFormat_ = new QComboBox(page);
    for (const QString& f : export_detail::availableExportFormats())
        exportFormat_->addItem(f.toUpper(), f);
    exportFormat_->setToolTip(tr("Preselected format in the Export dialog."));
    column->addWidget(cardRow(tr("File format"), exportFormat_, page,
                              exportFormat_->toolTip()));
    exportQuality_ = new QSlider(Qt::Horizontal, page);
    exportQuality_->setRange(1, 100);
    exportQuality_->setToolTip(tr("Quality for lossy formats (JPEG/WebP/AVIF)."));
    exportQualityLabel_ = new QLabel(page);
    exportQualityLabel_->setMinimumWidth(36);
    connect(exportQuality_, &QSlider::valueChanged, this,
            &PreferencesDialog::updateExportQualityLabel);
    auto* qualityRow = new QWidget(page);
    auto* qualityLayout = new QHBoxLayout(qualityRow);
    qualityLayout->setContentsMargins(0, 0, 0, 0);
    qualityLayout->addWidget(exportQuality_, 1);
    qualityLayout->addWidget(exportQualityLabel_);
    column->addWidget(cardRow(tr("Quality"), qualityRow, page,
                              exportQuality_->toolTip()));
    exportLocation_ = new QComboBox(page);
    exportLocation_->addItems(
        {tr("Ask each time"), tr("Same folder as document"), tr("Choose folder…")});
    column->addWidget(cardRow(tr("Save to"), exportLocation_, page));
    exportEmbedIcc_ = new Switch(page);
    exportEmbedIcc_->setToolTip(tr("Embed the color profile in exported files."));
    column->addWidget(cardRow(tr("Embed ICC profile"), exportEmbedIcc_, page,
                              exportEmbedIcc_->toolTip()));
    psdCompression_ = new QComboBox(page);
    psdCompression_->addItem(tr("RLE (compatible)"), 0);
    psdCompression_->addItem(tr("ZIP (smaller)"), 1);
    psdCompression_->setToolTip(
        tr("Channel compression for layered PSD export. Untouched ZIP-origin "
           "layers keep ZIP either way."));
    column->addWidget(cardRow(tr("Layered PSD compression"), psdCompression_,
                              page, psdCompression_->toolTip()));
    column->addStretch(1);
    return page;
}

QWidget* PreferencesDialog::buildShortcutsPage() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(8);

    auto* intro = new QLabel(
        tr("Click a shortcut field and press the new keys. Empty a field to "
           "unbind it. Applied when Settings closes with OK."),
        page);
    intro->setWordWrap(true);
    intro->setStyleSheet(dimStyle(state_));
    column->addWidget(intro);

    shortcutFilter_ = new QLineEdit(page);
    shortcutFilter_->setObjectName(QStringLiteral("shortcuts.filter"));
    shortcutFilter_->setPlaceholderText(tr("Filter commands…"));
    shortcutFilter_->setClearButtonEnabled(true);
    column->addWidget(shortcutFilter_);
    connect(shortcutFilter_, &QLineEdit::textChanged, this,
            &PreferencesDialog::filterShortcuts);

    shortcutDefaults_ = defaultKeymapEntries();
    QStringList ids = shortcutDefaults_.keys();
    ids.sort();
    shortcutTable_ = new QTableWidget(static_cast<int>(ids.size()), 2, page);
    shortcutTable_->setObjectName(QStringLiteral("shortcuts.table"));
    shortcutTable_->setHorizontalHeaderLabels({tr("Command"), tr("Shortcut")});
    shortcutTable_->verticalHeader()->setVisible(false);
    shortcutTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    shortcutTable_->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    shortcutTable_->setSelectionMode(QAbstractItemView::NoSelection);
    shortcutEdits_.clear();
    for (int row = 0; row < ids.size(); ++row) {
        const QString id = ids.at(row);
        auto* name = new QTableWidgetItem(QString(id).replace(u'-', u' '));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        name->setData(Qt::UserRole, id);
        shortcutTable_->setItem(row, 0, name);
        auto* edit = new QKeySequenceEdit(page);
        edit->setClearButtonEnabled(true);
        edit->setToolTip(tr("Press the replacement shortcut, or clear to unbind."));
        shortcutTable_->setCellWidget(row, 1, edit);
        shortcutEdits_.insert(id, edit);
        connect(edit, &QKeySequenceEdit::keySequenceChanged, this,
                [this](const QKeySequence&) { updateShortcutConflicts(); });
    }
    column->addWidget(shortcutTable_, 1);

    auto* bottom = new QHBoxLayout;
    auto* reset = new QPushButton(tr("Reset to Defaults"), page);
    bottom->addWidget(reset);
    bottom->addStretch(1);
    column->addLayout(bottom);
    connect(reset, &QPushButton::clicked, this, [this] {
        for (auto it = shortcutEdits_.begin(); it != shortcutEdits_.end(); ++it)
            it.value()->setKeySequence(
                QKeySequence(shortcutDefaults_.value(it.key()),
                             QKeySequence::PortableText));
        updateShortcutConflicts();
        if (shortcutFilter_) filterShortcuts(shortcutFilter_->text());
    });
    return page;
}

QWidget* PreferencesDialog::buildAdvancedPage() {
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(8);

    column->addWidget(sectionHeader(tr("Developer"), content));
    referenceMask_ = new QLineEdit(content);
    referenceMask_->setPlaceholderText(tr("Empty = normal AI output"));
    referenceMask_->setClearButtonEnabled(true);
    referenceMask_->setToolTip(
        tr("';'-separated ground-truth masks used to conform AI selections "
           "for testing. Empty = normal AI output."));
    column->addWidget(cardRow(tr("Reference mask"), referenceMask_, content,
                              referenceMask_->toolTip()));

    column->addWidget(sectionHeader(tr("On-disk data"), content));
    const QList<QPair<QString, QString>> paths = {
        {tr("Settings file"), settingsPath()},
        {tr("Shortcuts file"), keymapFilePath()},
        {tr("AI models folder"), aiModelsDir()},
        {tr("Brush library"), brushlibrary::brushLibraryPath()},
        {tr("Brush tips folder"), brushlibrary::brushTipsDir()},
        {tr("Export presets"), QFileInfo(settingsPath()).absolutePath() +
                                   QStringLiteral("/export_presets.json")},
        {tr("Log folder"), log_dir()},
    };
    QList<QPair<QString, QString>> allPaths = paths;
    for (int i = 0; i < advanced_.extraPaths.size(); ++i)
        allPaths.append({advanced_.extraNames.value(i), advanced_.extraPaths.at(i)});
    for (const auto& entry : allPaths) {
        auto* open = new QPushButton(tr("Open"), content);
        open->setFixedWidth(80);
        const QString target = entry.second;
        connect(open, &QPushButton::clicked, this, [target] {
            if (target.isEmpty()) return;
            const QFileInfo info(target);
            const QString dir = info.isDir() ? info.absoluteFilePath()
                                             : info.absolutePath();
            QDir().mkpath(dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                info.isDir() ? info.absoluteFilePath() : dir));
        });
        auto* row = new QWidget(content);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(12, 8, 12, 8);
        h->setSpacing(12);
        auto* left = new QVBoxLayout;
        left->setSpacing(2);
        auto* name = new QLabel(entry.first, row);
        auto* addr = new QLabel(QDir::toNativeSeparators(entry.second), row);
        addr->setWordWrap(true);
        addr->setTextInteractionFlags(Qt::TextSelectableByMouse);
        addr->setStyleSheet(dimStyle(state_));
        left->addWidget(name);
        left->addWidget(addr);
        h->addLayout(left, 1);
        h->addWidget(open, 0, Qt::AlignVCenter);
        row->setObjectName(QStringLiteral("prefCard"));
        row->setStyleSheet(
            QStringLiteral("QWidget#prefCard { background: %1; border: 1px solid %2; border-radius: 5px; }")
                .arg(row->palette().color(QPalette::AlternateBase).name(),
                     row->palette().color(QPalette::Mid).name()));
        column->addWidget(row);
    }
    column->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll);
    return page;
}

QWidget* PreferencesDialog::buildMachineLearningPage() {
    auto* page = new QWidget;
    auto* column = new QVBoxLayout(page);
    column->setContentsMargins(4, 4, 4, 4);
    column->setSpacing(8);

    auto* intro = new QLabel(
        tr("Background removal runs entirely on this machine. Download a model "
           "below, then choose it as the model to use. Weights are stored in:\n%1")
            .arg(QDir::toNativeSeparators(aiModelsDir())),
        page);
    intro->setWordWrap(true);
    intro->setStyleSheet(dimStyle(state_));
    column->addWidget(intro);

    aiCacheLabel_ = new QLabel(page);
    aiCacheLabel_->setStyleSheet(dimStyle(state_));
    column->addWidget(aiCacheLabel_);

    auto* form = new QFormLayout;
    bgModel_ = new QComboBox(page);
    bgModel_->setObjectName(QStringLiteral("ai.bgModel"));
    bgModel_->setToolTip(tr("Model used by Remove Background / Select Subject."));
    form->addRow(tr("Model to use"), bgModel_);
    enhanceModel_ = new QComboBox(page);
    enhanceModel_->setObjectName(QStringLiteral("ai.enhanceModel"));
    enhanceModel_->setToolTip(
        tr("Local model used by Enhance Edges for hair and soft edges. "
           "Uninstalled picks fall back to the best installed hair model."));
    form->addRow(tr("Enhance Edges model"), enhanceModel_);
    column->addLayout(form);

    bgModelNote_ = new QLabel(page);
    bgModelNote_->setWordWrap(true);
    bgModelNote_->setStyleSheet(dimStyle(state_));
    column->addWidget(bgModelNote_);
    enhanceModelNote_ = new QLabel(page);
    enhanceModelNote_->setWordWrap(true);
    enhanceModelNote_->setStyleSheet(dimStyle(state_));
    column->addWidget(enhanceModelNote_);
    connect(bgModel_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateModelNote(); });
    connect(enhanceModel_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateModelNote(); });

    auto* scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* list = new QWidget;
    auto* listLayout = new QVBoxLayout(list);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(10);
    QString category;
    for (const AiModel& m : allAiModels()) {
        if (m.category != category) {
            category = m.category;
            auto* heading = new QLabel(category, list);
            QFont f = heading->font();
            f.setBold(true);
            heading->setFont(f);
            heading->setStyleSheet(dimStyle(state_));
            listLayout->addWidget(heading);
        }
        listLayout->addWidget(makeAiRow(m, list));
    }
    listLayout->addStretch(1);
    scroll->setWidget(list);
    column->addWidget(scroll, 1);

    auto* bottom = new QHBoxLayout;
    auto* downloadAll = new QPushButton(tr("Download all"), page);
    auto* openFolder = new QPushButton(tr("Open models folder"), page);
    bottom->addWidget(downloadAll);
    bottom->addWidget(openFolder);
    bottom->addStretch(1);
    column->addLayout(bottom);

    connect(downloadAll, &QPushButton::clicked, this,
            [] { aiModelStore().downloadAll(); });
    connect(openFolder, &QPushButton::clicked, this, [] {
        QDir().mkpath(aiModelsDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(aiModelsDir()));
    });

    AiModelStore& store = aiModelStore();
    connect(&store, &AiModelStore::changed, this, &PreferencesDialog::refreshAi);
    connect(&store, &AiModelStore::progress, this,
            [this](const QString& id, qint64 received, qint64 total) {
                aiProgress_.insert(id, total > 0 ? int(received * 100 / total) : 0);
                refreshAi();
            });

    refreshAi();
    return page;
}

QWidget* PreferencesDialog::makeAiRow(const AiModel& model, QWidget* parent) {
    const ThemeColors c = colorsFor(state_->theme());
    const QString id = model.id;

    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("aiCard"));
    // Object-name selector so the card frame does not leak onto its children.
    row->setStyleSheet(
        QStringLiteral("QWidget#aiCard { border: 1px solid %1; border-radius: 4px; "
                       "background: transparent; }")
            .arg(c.border.name()));
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(8, 6, 8, 6);
    h->setSpacing(12);

    auto* left = new QVBoxLayout;
    left->setSpacing(4);
    auto* name = new QLabel(model.name, row);
    QFont nameFont = name->font();
    nameFont.setBold(true);
    name->setFont(nameFont);
    auto* desc = new QLabel(model.description, row);
    desc->setWordWrap(true);
    desc->setStyleSheet(dimStyle(state_));
    auto* meta = new QLabel(
        tr("%1 · %2 · %3").arg(model.category,
                               formatModelSize(model.decoderBytes > 0
                                                   ? model.bytes + model.decoderBytes
                                                   : model.bytes),
                               model.license),
        row);
    meta->setStyleSheet(dimStyle(state_));
    left->addWidget(name);
    left->addWidget(desc);
    left->addWidget(meta);
    h->addLayout(left, 1);

    auto* right = new QVBoxLayout;
    right->setSpacing(4);
    auto* status = new QLabel(row);
    status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    status->setStyleSheet(dimStyle(state_));
    auto* button = new QPushButton(row);
    button->setFixedWidth(110);
    right->addWidget(status);
    right->addWidget(button);
    h->addLayout(right);

    auto refresh = [this, id, status, button] {
        AiModelStore& s = aiModelStore();
        const AiModel* model = aiModel(id);
        const bool localOnly = model && model->url.isEmpty();
        if (localOnly) {
            // Local-only pairs (local SAM): no download; import from disk.
            if (s.state(id) == AiModelState::Present) {
                status->setText(tr("Ready (local)"));
                button->setText(tr("Remove"));
                button->setEnabled(true);
                button->setToolTip(tr("Delete the local model files"));
            } else {
                status->setText(tr("Not installed — pick the .onnx files"));
                button->setText(tr("Import…"));
                button->setEnabled(true);
                button->setToolTip(tr("Copy the encoder + decoder into the "
                                      "model cache"));
            }
            return;
        }
        switch (s.state(id)) {
            case AiModelState::Present:
                aiProgress_.remove(id);
                status->setText(tr("Ready"));
                button->setText(tr("Remove"));
                button->setEnabled(true);
                button->setToolTip(tr("Delete the downloaded file"));
                break;
            case AiModelState::Downloading:
                status->setText(tr("Downloading %1%").arg(aiProgress_.value(id)));
                button->setText(tr("Downloading…"));
                button->setEnabled(false);
                button->setToolTip(QString());
                break;
            case AiModelState::Error:
                aiProgress_.remove(id);
                status->setText(tr("Failed"));
                button->setText(tr("Retry"));
                button->setEnabled(true);
                button->setToolTip(s.errorText(id));
                break;
            case AiModelState::Absent:
                aiProgress_.remove(id);
                status->setText(tr("Not downloaded"));
                button->setText(tr("Download"));
                button->setEnabled(true);
                button->setToolTip(tr("Download %1").arg(formatModelSize(
                    aiModel(id) ? aiModel(id)->bytes : 0)));
                break;
        }
    };
    connect(button, &QPushButton::clicked, this, [this, id] {
        AiModelStore& s = aiModelStore();
        const AiModel* m = aiModel(id);
        if (!m) return;
        if (!m->url.isEmpty()) {
            if (s.state(id) == AiModelState::Present)
                s.remove(id);
            else
                s.download(id);
            return;
        }
        // Local-only pair: import from a folder, or remove when installed.
        if (s.state(id) == AiModelState::Present) {
            s.remove(id);
            return;
        }
        const QString enc = QFileDialog::getOpenFileName(
            this, tr("Select the SAM encoder model (.onnx)"),
            QDir::homePath(), tr("ONNX model (*.onnx)"));
        if (enc.isEmpty()) return;
        const QString err = s.importPairFiles(id, enc);
        if (!err.isEmpty())
            QMessageBox::warning(this, tr("Object Select model"), err);
    });
    aiRefreshers_.append(refresh);
    refresh();
    return row;
}

void PreferencesDialog::rebuildModelCombo() {
    auto fill = [this](QComboBox* combo, const QString& setting) {
        if (!combo) return;
        const QString keep = combo->count() > 0
                                 ? combo->currentData().toString()
                                 : setting;
        const QSignalBlocker block(combo);
        combo->clear();
        for (const AiModel& m : allAiModels()) {
            const bool ready =
                aiModelStore().state(m.id) == AiModelState::Present;
            combo->addItem(ready ? tr("%1 (ready)").arg(m.name) : m.name,
                           m.id);
        }
        const QString want = keep.isEmpty() ? setting : keep;
        const int idx = combo->findData(want);
        combo->setCurrentIndex(idx >= 0 ? idx : 0);
    };
    fill(bgModel_, state_->settings().bgModel);
    fill(enhanceModel_, state_->settings().enhanceModel);
}

void PreferencesDialog::updateModelNote() {
    auto noteFor = [](QComboBox* combo, QLabel* label) {
        if (!label || !combo || combo->currentIndex() < 0) {
            if (label) label->clear();
            return;
        }
        const QString id = combo->currentData().toString();
        const AiModel* m = aiModel(id);
        if (!m) {
            label->clear();
            return;
        }
        QString note = m->description;
        switch (aiModelStore().state(id)) {
            case AiModelState::Present: note += tr("  —  ready."); break;
            case AiModelState::Downloading: note += tr("  —  downloading…"); break;
            case AiModelState::Error: note += tr("  —  download failed."); break;
            case AiModelState::Absent:
                note += tr("  —  not downloaded yet; use Download below.");
                break;
        }
        label->setText(note);
    };
    noteFor(bgModel_, bgModelNote_);
    noteFor(enhanceModel_, enhanceModelNote_);
}

void PreferencesDialog::refreshAi() {
    if (!bgModel_ && !enhanceModel_) return;
    rebuildModelCombo();
    for (auto& fn : aiRefreshers_) fn();
    updateModelNote();
    updateAiCacheLabel();
}

void PreferencesDialog::updateAiCacheLabel() {
    if (!aiCacheLabel_) return;
    qint64 total = 0;
    int present = 0;
    for (const AiModel& m : allAiModels()) {
        if (aiModelStore().state(m.id) != AiModelState::Present) continue;
        ++present;
        const QFileInfo main(aiModelPath(m.id));
        if (main.isFile()) total += main.size();
        if (!m.decoderFile.isEmpty()) {
            const QFileInfo dec(aiModelDecoderPath(m.id));
            if (dec.isFile()) total += dec.size();
        }
    }
    aiCacheLabel_->setText(
        present > 0 ? tr("Cache: %1 across %2 of %3 models.")
                          .arg(formatModelSize(total), QString::number(present),
                               QString::number(allAiModels().size()))
                    : tr("Cache: empty — nothing downloaded yet."));
}

void PreferencesDialog::rebuildDeviceCombos() {
    gpuDevice_->clear();
    cpuDevice_->clear();
    for (const auto& d : pittore::compute::enumerate_devices()) {
        const QString name =
            QString::fromLatin1(pittore::compute::to_string(d.type)) +
            QStringLiteral(" · ") + QString::fromStdString(d.name);
        if (d.type == pittore::compute::BackendType::CPU)
            cpuDevice_->addItem(name, QString::fromStdString(d.name));
        else
            gpuDevice_->addItem(name, QString::fromStdString(d.name));
    }
    // Default selections: first available GPU / first available CPU.
    if (gpuDevice_->count() > 0) gpuDevice_->setCurrentIndex(0);
    if (cpuDevice_->count() > 0) cpuDevice_->setCurrentIndex(0);
}

void PreferencesDialog::updateGpuEnabled(bool on) {
    gpuDevice_->setEnabled(on);
}

void PreferencesDialog::filterNav(const QString& text) {
    if (!navList_) return;
    const QString needle = text.trimmed().toLower();
    for (int i = 0; i < navList_->count(); ++i) {
        QListWidgetItem* item = navList_->item(i);
        item->setHidden(!needle.isEmpty() &&
                        !item->text().toLower().contains(needle));
    }
    // Keep a visible selection: the current row may have been filtered out.
    const int current = navList_->currentRow();
    if (current < 0 || (current < navList_->count() &&
                        navList_->item(current)->isHidden())) {
        for (int i = 0; i < navList_->count(); ++i) {
            if (!navList_->item(i)->isHidden()) {
                navList_->setCurrentRow(i);
                return;
            }
        }
    }
}

void PreferencesDialog::updateExportQualityLabel() {
    if (!exportQuality_ || !exportQualityLabel_) return;
    exportQualityLabel_->setText(QString::number(exportQuality_->value()));
}

void PreferencesDialog::loadShortcutEdits() {
    if (shortcutEdits_.isEmpty()) return;
    const QMap<QString, QString> entries = loadKeymapEntries();
    for (auto it = shortcutEdits_.begin(); it != shortcutEdits_.end(); ++it) {
        const QSignalBlocker block(it.value());
        it.value()->setKeySequence(QKeySequence(
            entries.value(it.key(), shortcutDefaults_.value(it.key())),
            QKeySequence::PortableText));
    }
    updateShortcutConflicts();
    if (shortcutFilter_) filterShortcuts(shortcutFilter_->text());
}

void PreferencesDialog::filterShortcuts(const QString& text) {
    if (!shortcutTable_) return;
    const QString needle = text.trimmed().toLower();
    for (int row = 0; row < shortcutTable_->rowCount(); ++row) {
        const QTableWidgetItem* name = shortcutTable_->item(row, 0);
        const auto* edit = qobject_cast<QKeySequenceEdit*>(
            shortcutTable_->cellWidget(row, 1));
        const QString cmd = name ? name->text() : QString();
        const QString seq =
            edit ? edit->keySequence().toString(QKeySequence::NativeText) : QString();
        shortcutTable_->setRowHidden(
            row, !needle.isEmpty() &&
                     !cmd.contains(needle, Qt::CaseInsensitive) &&
                     !seq.contains(needle, Qt::CaseInsensitive));
    }
}

void PreferencesDialog::updateShortcutConflicts() {
    if (!shortcutTable_) return;
    QHash<QString, QStringList> bySeq;
    for (auto it = shortcutEdits_.begin(); it != shortcutEdits_.end(); ++it) {
        const QString seq = it.value()
                                ->keySequence()
                                .toString(QKeySequence::PortableText)
                                .trimmed();
        if (!seq.isEmpty()) bySeq[seq] << it.key();
    }
    // Canvas/tool input that never consults the keymap: bare keys the event
    // filter consumes first (tool letters, opacity digits, nudge arrows,
    // spring-hand Space, ...). A remap onto one of these loses to the canvas.
    QSet<QString> reserved;
    for (const ToolGroup& group : allGroups()) {
        if (group.key != 0)
            reserved.insert(QString(QChar::fromLatin1(group.key)).toUpper());
    }
    for (const QString& fixed : {QStringLiteral("SPACE"), QStringLiteral("DELETE"),
                                 QStringLiteral("BACKSPACE"), QStringLiteral("ESCAPE"),
                                 QStringLiteral("LEFT"), QStringLiteral("UP"),
                                 QStringLiteral("RIGHT"), QStringLiteral("DOWN"),
                                 QStringLiteral("0"), QStringLiteral("1"),
                                 QStringLiteral("2"), QStringLiteral("3"),
                                 QStringLiteral("4"), QStringLiteral("5"),
                                 QStringLiteral("6"), QStringLiteral("7"),
                                 QStringLiteral("8"), QStringLiteral("9")})
        reserved.insert(fixed);
    for (int row = 0; row < shortcutTable_->rowCount(); ++row) {
        QTableWidgetItem* name = shortcutTable_->item(row, 0);
        if (!name) continue;
        const QString id = name->data(Qt::UserRole).toString();
        const auto* edit = shortcutEdits_.value(id, nullptr);
        const QString seq = edit ? edit->keySequence()
                                       .toString(QKeySequence::PortableText)
                                       .trimmed()
                                 : QString();
        const QStringList clash =
            seq.isEmpty() ? QStringList() : bySeq.value(seq);
        QStringList notes;
        if (clash.size() > 1) {
            QStringList others = clash;
            others.removeAll(id);
            std::sort(others.begin(), others.end());
            notes << tr("Conflicts with %1").arg(others.join(QStringLiteral(", ")));
        }
        // Only bare single keys can hit canvas input (modifiers route to
        // menus/shortcuts instead); Shift is ignored like the filter's
        // hardness flag.
        const QKeySequence parsed(seq, QKeySequence::PortableText);
        if (!seq.isEmpty() && parsed.count() == 1 &&
            !(parsed[0].keyboardModifiers() &
              (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
            reserved.contains(seq.toUpper()))
            notes << tr("Reserved by canvas input — remaps here lose");
        if (!notes.isEmpty()) {
            name->setData(Qt::BackgroundRole, QColor(160, 40, 40, 90));
            name->setToolTip(notes.join(QStringLiteral("\n")));
        } else {
            name->setData(Qt::BackgroundRole, QVariant());
            name->setToolTip(QString());
        }
    }
}

bool PreferencesDialog::saveShortcutEdits() {
    if (shortcutEdits_.isEmpty()) return false;
    const QMap<QString, QString> before = loadKeymapEntries();
    QMap<QString, QString> next = before;
    for (auto it = shortcutEdits_.begin(); it != shortcutEdits_.end(); ++it) {
        const QString seq =
            it.value()->keySequence().toString(QKeySequence::PortableText).trimmed();
        if (seq.isEmpty()) {
            next.remove(it.key());
            continue;
        }
        if (QKeySequence(seq, QKeySequence::PortableText).isEmpty() && seq.size() != 1)
            continue;  // ignore garbage the edit somehow holds
        next.insert(it.key(), seq);
    }
    if (next == before) return false;
    saveKeymapEntries(next);
    return true;
}

void PreferencesDialog::updateRamLimitLabel() {
    if (!ramLimit_ || !ramLimitLabel_) return;
    const int v = ramLimit_->value();
    if (v <= 0) {
        ramLimitLabel_->setText(tr("Unlimited"));
        return;
    }
    QString size = v >= 1024 ? tr("%1 GB").arg(v / 1024.0, 0, 'f', 1)
                             : tr("%1 MB").arg(v);
    if (ramTotalMb_ > 0)
        size += tr(" (%1%)").arg(qRound(v * 100.0 / ramTotalMb_));
    ramLimitLabel_->setText(size);
}

}  // namespace pittore::ui
