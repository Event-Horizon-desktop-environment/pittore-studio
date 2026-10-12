#include "lab_mainwindow.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/ai/bg_remove.h"
#include "mcp_server.h"

namespace pittore::lab {
namespace {

// Wheel-zoom + drag-pan result pane. No Q_OBJECT (pure event override).
class ZoomView : public QGraphicsView {
  public:
    using QGraphicsView::QGraphicsView;

  protected:
    void wheelEvent(QWheelEvent* e) override {
        const double f = e->angleDelta().y() > 0 ? 1.25 : 0.8;
        scale(f, f);
        e->accept();
    }
};

QImage ensureARGB(const QImage& im) {
    return im.convertToFormat(QImage::Format_ARGB32);
}

// Transparency checkerboard, drawn once and tiled.
QImage checkerCanvas(int w, int h) {
    QImage bg(w, h, QImage::Format_ARGB32);
    const int t = 16;
    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(bg.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const bool a = ((x / t) + (y / t)) & 1;
            row[x] = a ? qRgb(255, 255, 255) : qRgb(204, 204, 204);
        }
    }
    return bg;
}

QImage solidCanvas(int w, int h, QRgb c) {
    QImage bg(w, h, QImage::Format_ARGB32);
    bg.fill(c);
    return bg;
}

// Cutout composited over a background canvas.
QImage overBackground(const QImage& cutout, QImage bg) {
    const QImage c = ensureARGB(cutout);
    if (bg.size() != c.size()) bg = bg.scaled(c.size());
    bg = ensureARGB(bg);
    QPainter p(&bg);
    p.drawImage(0, 0, c);
    p.end();
    return bg;
}

// Rubylith: red overlay whose alpha follows the matte (faint alpha reads loud).
QImage rubylith(const QImage& orig, const QImage& matte) {
    QImage base = ensureARGB(orig);
    const QImage g =
        matte.format() == QImage::Format_Grayscale8
            ? matte
            : matte.convertToFormat(QImage::Format_Grayscale8);
    const int w = base.width(), h = base.height();
    QImage red(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const uchar* grow = g.constScanLine(y);
        QRgb* rrow = reinterpret_cast<QRgb*>(red.scanLine(y));
        for (int x = 0; x < w; ++x) rrow[x] = qRgba(255, 0, 0, grow[x]);
    }
    QPainter p(&base);
    p.drawImage(0, 0, red);
    p.end();
    return base;
}

QImage bwView(const QImage& matte) {
    const QImage g =
        matte.format() == QImage::Format_Grayscale8
            ? matte
            : matte.convertToFormat(QImage::Format_Grayscale8);
    const int w = g.width(), h = g.height();
    QImage out(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; ++y) {
        const uchar* grow = g.constScanLine(y);
        QRgb* orow = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x)
            orow[x] = grow[x] >= 127 ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
    }
    return out;
}

// Fringe map: the transition band's raw color over black, so background
// contamination in the semi-transparent edge is impossible to miss.
QImage fringeView(const QImage& cutout, const QImage& matte) {
    const QImage c = ensureARGB(cutout);
    const QImage g =
        matte.format() == QImage::Format_Grayscale8
            ? matte
            : matte.convertToFormat(QImage::Format_Grayscale8);
    const int w = c.width(), h = c.height();
    QImage out(w, h, QImage::Format_ARGB32);
    out.fill(qRgb(0, 0, 0));
    for (int y = 0; y < h; ++y) {
        const uchar* crow = c.constScanLine(y);
        const uchar* grow = g.constScanLine(y);
        QRgb* orow = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int a = grow[x];
            if (a > 8 && a < 247) {
                const QRgb px = reinterpret_cast<const QRgb*>(crow)[x];
                orow[x] = qRgba(qRed(px), qGreen(px), qBlue(px), 255);
            }
        }
    }
    return out;
}

QImage matteGray(const QImage& matte) {
    const QImage g =
        matte.format() == QImage::Format_Grayscale8
            ? matte
            : matte.convertToFormat(QImage::Format_Grayscale8);
    return ensureARGB(g);
}

// Fraction of pixels with matte coverage >= 50% (0..1).
double selCoverage(const QImage& matte) {
    if (matte.isNull()) return 0.0;
    const QImage g =
        matte.format() == QImage::Format_Grayscale8
            ? matte
            : matte.convertToFormat(QImage::Format_Grayscale8);
    const qint64 w = g.width(), h = g.height();
    qint64 sel = 0;
    for (int y = 0; y < g.height(); ++y) {
        const uchar* row = g.constScanLine(y);
        for (int x = 0; x < g.width(); ++x)
            if (row[x] >= 128) ++sel;
    }
    return double(sel) / double(w * h);
}

QImage renderView(const RunResult& r, const LabParams& p) {
    switch (p.view) {
        case ViewMode::Original:
            return ensureARGB(r.original);
        case ViewMode::Matte:
            return matteGray(r.matte);
        case ViewMode::Rubylith:
            return rubylith(r.original, r.matte);
        case ViewMode::CompositeAlpha:
            return overBackground(r.cutout, checkerCanvas(r.cutout.width(),
                                                          r.cutout.height()));
        case ViewMode::CompositeSolid:
            return overBackground(r.cutout, solidCanvas(r.cutout.width(),
                                                        r.cutout.height(),
                                                        solidBgColor(p.solidBg)));
        case ViewMode::BlackWhite:
            return bwView(r.matte);
        case ViewMode::FringeMap:
            return fringeView(r.cutout, r.matte);
        default:
            return ensureARGB(r.original);
    }
}

// Every .onnx under the candidate model dirs (PITTORE_MODELS_DIR first).
struct ModelEntry {
    QString path;
    QString label;
};
QVector<ModelEntry> findModels() {
    QVector<ModelEntry> out;
    QStringList dirs;
    const QString env = qEnvironmentVariable("PITTORE_MODELS_DIR");
    if (!env.isEmpty()) dirs << env;
    dirs << QDir::homePath() + "/.local/share/InfinityPhoto/models";
    dirs << QDir::homePath() + "/.local/share/PittoreStudio/models";
    for (const QString& d : dirs) {
        const QDir dir(d);
        if (!dir.exists()) continue;
        const QStringList files =
            dir.entryList({"*.onnx"}, QDir::Files, QDir::Name);
        for (const QString& f : files) {
            const QString full = dir.filePath(f);
            bool dup = false;
            for (const auto& e : out)
                if (e.path == full) dup = true;
            if (!dup)
                out.append(ModelEntry{
                    full, f + QStringLiteral("  —  ") + QDir(d).dirName()});
        }
    }
    return out;
}

}  // namespace

LabWindow::LabWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Refine Lab — Pittore pipeline tuner"));
    resize(1400, 900);

    scene_ = new QGraphicsScene(this);
    view_ = new ZoomView(this);
    view_->setScene(scene_);
    view_->setDragMode(QGraphicsView::ScrollHandDrag);
    view_->setRenderHints(QPainter::SmoothPixmapTransform);
    setCentralWidget(view_);

    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(90);
    connect(debounce_, &QTimer::timeout, this, [this] { doRefine(); });

    buildToolbar();
    QDockWidget* dock = new QDockWidget(QStringLiteral("Controls"), this);
    dock->setObjectName(QStringLiteral("controls"));
    dock->setWidget(buildControls());
    dock->setFeatures(QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    status_ = new QLabel(this);
    statusBar()->addWidget(status_, 1);
    statusBar()->showMessage(QStringLiteral(
        "Open an image, then Segment to build the AI seed matte."));

    // Auto-open the bundled test portrait when present.
    const QStringList candidates = {
        QStringLiteral("Tests/d.png"),
        QCoreApplication::applicationDirPath() + "/../Tests/d.png",
        QStringLiteral("/home/matt/Documents/GitHub/pittore-studio/tests/Refine-app/Tests/d.png"),
    };
    for (const QString& c : candidates)
        if (QFileInfo::exists(c)) {
            loadImage(c);
            break;
        }
}

void LabWindow::buildToolbar() {
    QToolBar* tb = addToolBar(QStringLiteral("main"));
    tb->setObjectName(QStringLiteral("mainbar"));
    tb->setMovable(false);

    tb->addAction(QStringLiteral("Open…"), this, [this] {
        const QString f = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open image"), QString(),
            QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.tif *.tiff)"));
        if (!f.isEmpty()) loadImage(f);
    });
    tb->addAction(QStringLiteral("Segment (AI)"), this, [this] { runSegment(); });
    tb->addAction(QStringLiteral("Fit"), this, [this] { fitView(); });
    tb->addSeparator();

    viewMode_ = new QComboBox(this);
    for (int i = 0; i < int(ViewMode::Count); ++i)
        viewMode_->addItem(QString::fromLatin1(viewModeName(ViewMode(i))));
    viewMode_->setCurrentIndex(int(params_.view));
    connect(viewMode_, &QComboBox::currentIndexChanged, this, [this](int i) {
        params_.view = ViewMode(i);
        updateView();
    });
    tb->addWidget(new QLabel(QStringLiteral(" View: "), this));
    tb->addWidget(viewMode_);

    solidBg_ = new QComboBox(this);
    for (int i = 0; i < 4; ++i) solidBg_->addItem(solidBgName(i));
    connect(solidBg_, &QComboBox::currentIndexChanged, this,
            [this](int i) {
                params_.solidBg = i;
                updateView();
            });
    tb->addWidget(new QLabel(QStringLiteral("  BG: "), this));
    tb->addWidget(solidBg_);

    tb->addSeparator();
    tb->addAction(QStringLiteral("Save preset…"), this, [this] { savePreset(); });
    tb->addAction(QStringLiteral("Load preset…"), this, [this] { loadPreset(); });

    tb->addSeparator();
    QAction* darkAct = tb->addAction(QStringLiteral("Dark"));
    darkAct->setCheckable(true);
    darkAct->setChecked(dark_);
    connect(darkAct, &QAction::toggled, this, [this](bool on) { setDarkMode(on); });
}

QWidget* LabWindow::buildControls() {
    panel_ = new QWidget;
    QVBoxLayout* root = new QVBoxLayout(panel_);

    // ---- Source / segment -------------------------------------------------
    auto* srcBox = new QGroupBox(QStringLiteral("Segmentation (AI seed)"), panel_);
    QVBoxLayout* srcL = new QVBoxLayout(srcBox);
    QHBoxLayout* modelRow = new QHBoxLayout;
    modelBox_ = new QComboBox(this);
    const QVector<ModelEntry> models = findModels();
    int selected = 0;
    for (int i = 0; i < models.size(); ++i) {
        modelBox_->addItem(models[i].label, models[i].path);
        if (models[i].path == params_.modelPath) selected = i;
    }
    if (!models.isEmpty()) {
        modelBox_->setCurrentIndex(selected);
        params_.modelPath = modelBox_->currentData().toString();
    }
    connect(modelBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        params_.modelPath = modelBox_->currentData().toString();
    });
    modelRow->addWidget(new QLabel(QStringLiteral("Model:"), this));
    modelRow->addWidget(modelBox_, 1);
    srcL->addLayout(modelRow);
    auto* segBtn = new QPushButton(QStringLiteral("Segment"), this);
    connect(segBtn, &QPushButton::clicked, this, [this] { runSegment(); });
    srcL->addWidget(segBtn);

    // Helper builders bound to params_ fields.
    auto addCheck = [&](QVBoxLayout* l, const QString& title,
                        bool LabParams::* field, bool alsoView = false) {
        auto* cb = new QCheckBox(title, panel_);
        cb->setChecked(params_.*field);
        connect(cb, &QCheckBox::toggled, this, [this, field, alsoView](bool v) {
            params_.*field = v;
            if (alsoView)
                updateView();
            else
                scheduleRecompute();
        });
        l->addWidget(cb);
        return cb;
    };
    auto addInt = [&](QVBoxLayout* l, const QString& title,
                      int LabParams::* field, int mn, int mx,
                      const QString& suffix = QString()) {
        auto* row = new QHBoxLayout;
        auto* lab = new QLabel(title, panel_);
        auto* sl = new QSlider(Qt::Horizontal, panel_);
        sl->setRange(mn, mx);
        sl->setValue(params_.*field);
        auto* sp = new QSpinBox(panel_);
        sp->setRange(mn, mx);
        sp->setValue(params_.*field);
        sp->setSuffix(suffix);
        connect(sl, &QSlider::valueChanged, sp, &QSpinBox::setValue);
        connect(sp, &QSpinBox::valueChanged, sl, &QSlider::setValue);
        connect(sp, &QSpinBox::valueChanged, this, [this, field](int v) {
            params_.*field = v;
            scheduleRecompute();
        });
        row->addWidget(lab);
        row->addWidget(sl, 1);
        row->addWidget(sp);
        l->addLayout(row);
    };
    auto addDouble = [&](QVBoxLayout* l, const QString& title,
                         double LabParams::* field, double mn, double mx,
                         double step, int decimals) {
        auto* row = new QHBoxLayout;
        auto* lab = new QLabel(title, panel_);
        auto* sl = new QSlider(Qt::Horizontal, panel_);
        // Map double -> int slider (scaled by 10^decimals).
        const int scale = int(std::pow(10.0, decimals));
        sl->setRange(int(mn * scale), int(mx * scale));
        sl->setValue(int(params_.*field * scale));
        auto* sp = new QDoubleSpinBox(panel_);
        sp->setRange(mn, mx);
        sp->setSingleStep(step);
        sp->setDecimals(decimals);
        sp->setValue(params_.*field);
        connect(sl, &QSlider::valueChanged, this,
                [sp, scale](int v) { sp->setValue(v / double(scale)); });
        connect(sp, &QDoubleSpinBox::valueChanged, this,
                [sl, scale](double v) { sl->setValue(int(v * scale)); });
        connect(sp, &QDoubleSpinBox::valueChanged, this,
                [this, field](double v) {
                    params_.*field = v;
                    scheduleRecompute();
                });
        row->addWidget(lab);
        row->addWidget(sl, 1);
        row->addWidget(sp);
        l->addLayout(row);
    };

    // ---- Segment level band (live over cached raw alpha) -----------------
    {
        auto* row = new QHBoxLayout;
        auto mk = [&](const QString& t, double LabParams::* f) {
            auto* lab = new QLabel(t, panel_);
            auto* sp = new QDoubleSpinBox(panel_);
            sp->setRange(0.0, 1.0);
            sp->setSingleStep(0.01);
            sp->setDecimals(3);
            sp->setValue(params_.*f);
            connect(sp, &QDoubleSpinBox::valueChanged, this,
                    [this, f](double v) {
                        params_.*f = v;
                        rebuildMatteFromRaw();
                    });
            row->addWidget(lab);
            row->addWidget(sp);
        };
        srcL->addLayout(row);
        mk(QStringLiteral("Level lo"), &LabParams::segmentLevelLo);
        mk(QStringLiteral("Level hi"), &LabParams::segmentLevelHi);
    }
    root->addWidget(srcBox);

    // ---- Refine stages ----------------------------------------------------
    auto* refBox = new QGroupBox(QStringLiteral("Refine pipeline"), panel_);
    QVBoxLayout* refL = new QVBoxLayout(refBox);
    addCheck(refL, QStringLiteral("Remove speckles"), &LabParams::useSpeckles);
    addInt(refL, QStringLiteral("  min island px"), &LabParams::speckleMinPx, 0,
           512);
    addCheck(refL, QStringLiteral("Decay wash"), &LabParams::useDecay);
    addInt(refL, QStringLiteral("  support px"), &LabParams::decaySupportPx, 4,
           128);
    addDouble(refL, QStringLiteral("  texture tol"), &LabParams::decayTexTol,
              0.0, 40.0, 0.5, 1);
    addCheck(refL, QStringLiteral("Edge snap"), &LabParams::useSnap);
    addCheck(refL, QStringLiteral("  smart radius"), &LabParams::smartRadius);
    addInt(refL, QStringLiteral("  band px (0=from border %)"),
           &LabParams::snapBandPx, 0, 128);
    addInt(refL, QStringLiteral("  border %"), &LabParams::borderPct, 0, 100,
           QStringLiteral("%"));
    addInt(refL, QStringLiteral("  narrow divisor"), &LabParams::snapNarrowDiv,
           1, 8);
    addInt(refL, QStringLiteral("  passes"), &LabParams::snapPasses, 1, 10);
    addCheck(refL, QStringLiteral("Smooth"), &LabParams::useSmooth);
    addInt(refL, QStringLiteral("  smooth slider"), &LabParams::smoothSlider, 0,
           30);
    addCheck(refL, QStringLiteral("Feather"), &LabParams::useFeather);
    addInt(refL, QStringLiteral("  feather px"), &LabParams::featherPx, 0, 64);
    addCheck(refL, QStringLiteral("Shift"), &LabParams::useShift);
    addInt(refL, QStringLiteral("  shift %"), &LabParams::shiftPct, -100, 100,
           QStringLiteral("%"));
    addCheck(refL, QStringLiteral("Ramp"), &LabParams::useRamp);
    addInt(refL, QStringLiteral("  ramp %"), &LabParams::rampPct, -100, 100,
           QStringLiteral("%"));
    root->addWidget(refBox);

    // ---- AI refine --------------------------------------------------------
    auto* aiBox = new QGroupBox(QStringLiteral("AI refine (guided)"), panel_);
    QVBoxLayout* aiL = new QVBoxLayout(aiBox);
    addCheck(aiL, QStringLiteral("Enable AI refine"), &LabParams::useRefineAi);
    addInt(aiL, QStringLiteral("  radius"), &LabParams::refineRadius, 1, 40);
    addDouble(aiL, QStringLiteral("  eps"), &LabParams::refineEps, 1e-5, 0.1,
              1e-3, 5);
    addDouble(aiL, QStringLiteral("  level lo"), &LabParams::refineLevelLo, 0.0,
              1.0, 0.01, 3);
    addDouble(aiL, QStringLiteral("  level hi"), &LabParams::refineLevelHi, 0.0,
              1.0, 0.01, 3);
    root->addWidget(aiBox);

    // ---- Decontamination --------------------------------------------------
    auto* dcBox = new QGroupBox(QStringLiteral("Color decontamination"), panel_);
    QVBoxLayout* dcL = new QVBoxLayout(dcBox);
    addCheck(dcL, QStringLiteral("Decontaminate colors"), &LabParams::decontam);
    addInt(dcL, QStringLiteral("  fill radius"), &LabParams::decontamRadius, 2,
           64);
    root->addWidget(dcBox);

    // ---- Preview ----------------------------------------------------------
    auto* pvBox = new QGroupBox(QStringLiteral("Preview"), panel_);
    QVBoxLayout* pvL = new QVBoxLayout(pvBox);
    {
        auto* row = new QHBoxLayout;
        auto* lab = new QLabel(QStringLiteral("Proxy cap (px)"), panel_);
        auto* sl = new QSlider(Qt::Horizontal, panel_);
        sl->setRange(0, 2400);
        sl->setValue(int(params_.proxyCap));
        auto* sp = new QSpinBox(panel_);
        sp->setRange(0, 2400);
        sp->setValue(int(params_.proxyCap));
        sp->setSpecialValueText(QStringLiteral("full res"));
        connect(sl, &QSlider::valueChanged, sp, &QSpinBox::setValue);
        connect(sp, &QSpinBox::valueChanged, sl, &QSlider::setValue);
        connect(sp, &QSpinBox::valueChanged, this, [this](int v) {
            params_.proxyCap = v;
            scheduleRecompute();
        });
        row->addWidget(lab);
        row->addWidget(sl, 1);
        row->addWidget(sp);
        pvL->addLayout(row);
    }
    root->addWidget(pvBox);

    // ---- Stages -----------------------------------------------------------
    auto* stBox = new QGroupBox(QStringLiteral("Pipeline stages"), panel_);
    QVBoxLayout* stL = new QVBoxLayout(stBox);
    QLabel* stageList = new QLabel(panel_);
    stageList->setObjectName(QStringLiteral("stageList"));
    stageList->setTextInteractionFlags(Qt::TextSelectableByMouse);
    stL->addWidget(stageList);
    auto* stageBtn = new QPushButton(QStringLiteral("Dump stage mattes to /tmp"),
                                     this);
    connect(stageBtn, &QPushButton::clicked, this, [this] {
        int i = 0;
        for (const Stage& s : last_.stages) {
            const QString name = QStringLiteral("%1_%2.png")
                                     .arg(i++)
                                     .arg(s.first.simplified()
                                              .replace(' ', '_')
                                              .replace('/', '_'));
            s.second.save("/tmp/refinelab_" + name);
        }
        status_->setText(
            QStringLiteral("Wrote %1 stage mattes to /tmp/refinelab_*")
                .arg(last_.stages.size()));
    });
    stL->addWidget(stageBtn);
    root->addWidget(stBox);

    root->addStretch(1);

    panelScroll_ = new QScrollArea(this);
    panelScroll_->setWidget(panel_);
    panelScroll_->setWidgetResizable(true);
    panelScroll_->setMinimumWidth(360);
    return panelScroll_;
}

void LabWindow::scheduleRecompute() { debounce_->start(); }

void LabWindow::loadImage(const QString& path) {
    QImage img(path);
    if (img.isNull()) {
        QMessageBox::warning(this, QStringLiteral("Refine Lab"),
                             QStringLiteral("Could not load %1").arg(path));
        return;
    }
    fullImg_ = img.convertToFormat(QImage::Format_ARGB32);
    fullMatte_ = QImage();
    fullRawAlpha_.clear();
    fitPending_ = true;
    status_->setText(
        QStringLiteral("Loaded %1 (%2×%3). Run Segment to build the seed matte.")
            .arg(path)
            .arg(fullImg_.width())
            .arg(fullImg_.height()));
    updateView();
}

void LabWindow::runSegment() {
    QString e;
    segment(&e);
}

bool LabWindow::segment(QString* errOut) {
    if (fullImg_.isNull()) {
        if (errOut) *errOut = QStringLiteral("Load an image first.");
        status_->setText(QStringLiteral("Load an image first."));
        return false;
    }
    if (params_.modelPath.isEmpty()) {
        if (errOut) *errOut = QStringLiteral("Pick a model first.");
        status_->setText(QStringLiteral("Pick a model first."));
        return false;
    }
    status_->setText(QStringLiteral("Segmenting with %1 …")
                             .arg(QFileInfo(params_.modelPath).fileName()));
    QCoreApplication::processEvents();
    QString err;
    std::vector<float> raw;
    const QImage matte =
        engine_.segment(fullImg_, params_, &err, &raw);
    if (matte.isNull()) {
        if (errOut) *errOut = err;
        status_->setText(QStringLiteral("Segment failed: %1").arg(err));
        return false;
    }
    fullRawAlpha_ = std::move(raw);
    fullMatte_ = matte;
    last_ = RunResult();
    fitPending_ = true;
    status_->setText(
        QStringLiteral("Segmented %1×%2 — refine live below.")
            .arg(matte.width())
            .arg(matte.height()));
    scheduleRecompute();
    return true;
}

void LabWindow::rebuildMatteFromRaw() {
    if (fullRawAlpha_.empty() || fullImg_.isNull()) return;
    const int w = fullImg_.width(), h = fullImg_.height();
    if (fullRawAlpha_.size() != std::size_t(w) * std::size_t(h)) return;
    const double lo = params_.segmentLevelLo, hi = params_.segmentLevelHi;
    const double span = (hi > lo) ? (hi - lo) : 1.0;
    QImage m(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* row = m.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const double v =
                (fullRawAlpha_[std::size_t(y) * w + x] - lo) / span;
            row[x] = uchar(std::clamp(int(v * 255.0 + 0.5), 0, 255));
        }
    }
    fullMatte_ = m;
    scheduleRecompute();
}

void LabWindow::doRefine() {
    if (fullImg_.isNull() || fullMatte_.isNull()) return;
    QImage img = fullImg_, seed = fullMatte_;
    if (params_.proxyCap > 0) {
        const double maxd = std::max(img.width(), img.height());
        if (maxd > params_.proxyCap) {
            const double s = params_.proxyCap / maxd;
            const QSize sz(std::max(1, int(img.width() * s + 0.5)),
                           std::max(1, int(img.height() * s + 0.5)));
            img = img.scaled(sz, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            seed = seed.scaled(sz, Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation);
        }
    }
    last_ = engine_.refine(img, seed, params_);
    if (!last_.ok) {
        status_->setText(QStringLiteral("Refine failed: %1").arg(last_.error));
        return;
    }
    updateView();
    QStringList names;
    for (const Stage& s : last_.stages) names << s.first;
    if (auto* lbl = panel_->findChild<QLabel*>(QStringLiteral("stageList")))
        lbl->setText(names.isEmpty() ? QStringLiteral("(no stages ran)")
                                     : names.join(QStringLiteral(" → ")));
    status_->setText(
        QStringLiteral("%1×%2  %3 s  %4 stages  [%5]")
            .arg(last_.matte.width())
            .arg(last_.matte.height())
            .arg(last_.ms, 0, 'f', 2)
            .arg(last_.stages.size())
            .arg(QString::fromLatin1(viewModeName(params_.view))));
}

void LabWindow::updateView() {
    if (last_.matte.isNull()) {
        // Show the original (or seed) until a refine has run.
        QImage disp = fullImg_.isNull()
                          ? QImage()
                          : (params_.view == ViewMode::Matte &&
                                     !fullMatte_.isNull()
                                 ? matteGray(fullMatte_)
                                 : ensureARGB(fullImg_));
        scene_->clear();
        if (!disp.isNull()) {
            const QPixmap pm = QPixmap::fromImage(disp);
            scene_->addPixmap(pm);
            scene_->setSceneRect(pm.rect());
        }
        if (fitPending_ && !disp.isNull()) {
            queueFit();
            fitPending_ = false;
        }
        return;
    }
    const QImage disp = renderView(last_, params_);
    scene_->clear();
    const QPixmap pm = QPixmap::fromImage(disp);
    scene_->addPixmap(pm);
    scene_->setSceneRect(pm.rect());
    if (fitPending_) {
        queueFit();
        fitPending_ = false;
    }
}

// Fit to the CURRENT viewport, but on the next event-loop turn so it runs
// after the window has been shown and laid out (fitting during the ctor, or
// the instant an image loads, would fit to a zero/tiny viewport).
void LabWindow::queueFit() {
    QTimer::singleShot(0, this, [this] { fitView(); });
}

void LabWindow::fitView() {
    if (!scene_->sceneRect().isEmpty())
        view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
}

void LabWindow::savePreset() {
    const QString f = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save preset"), QStringLiteral("refine-preset.json"),
        QStringLiteral("JSON (*.json)"));
    if (f.isEmpty()) return;
    QFile file(f);
    if (!file.open(QIODevice::WriteOnly)) {
        status_->setText(QStringLiteral("Could not write %1").arg(f));
        return;
    }
    file.write(QJsonDocument(params_.toJson()).toJson(QJsonDocument::Indented));
    status_->setText(QStringLiteral("Saved preset to %1").arg(f));
}

void LabWindow::loadPreset() {
    const QString f = QFileDialog::getOpenFileName(
        this, QStringLiteral("Load preset"), QString(),
        QStringLiteral("JSON (*.json)"));
    if (f.isEmpty()) return;
    QFile file(f);
    if (!file.open(QIODevice::ReadOnly)) {
        status_->setText(QStringLiteral("Could not read %1").arg(f));
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    params_.fromJson(o);
    // Reflect the loaded values in the view-mode combo and re-run.
    if (viewMode_) viewMode_->setCurrentIndex(int(params_.view));
    if (solidBg_) solidBg_->setCurrentIndex(params_.solidBg);
    if (!fullMatte_.isNull()) rebuildMatteFromRaw();
    scheduleRecompute();
    status_->setText(QStringLiteral("Loaded preset from %1").arg(f));
}

// ---------------------------------------------------------------------------
// Remote (MCP) control surface
// ---------------------------------------------------------------------------

LabWindow::~LabWindow() = default;

QString LabWindow::statusText() const {
    return status_ ? status_->text() : QString();
}

QJsonObject LabWindow::paramsState() const {
    QJsonObject o;
    o[QStringLiteral("params")] = params_.toJson();
    o[QStringLiteral("hasImage")] = hasImage();
    o[QStringLiteral("hasMatte")] = hasMatte();
    if (hasImage()) {
        o[QStringLiteral("imageW")] = fullImg_.width();
        o[QStringLiteral("imageH")] = fullImg_.height();
    }
    if (hasMatte()) {
        o[QStringLiteral("matteW")] = fullMatte_.width();
        o[QStringLiteral("matteH")] = fullMatte_.height();
    }
    o[QStringLiteral("undo")] = undoStack_.size();
    o[QStringLiteral("redo")] = redoStack_.size();
    o[QStringLiteral("dark")] = dark_;
    o[QStringLiteral("status")] = statusText();
    if (last_.ok) {
        o[QStringLiteral("refineMs")] = last_.ms;
        o[QStringLiteral("coverage")] =
            double(last_.matte.width() ? selCoverage(last_.matte) : 0.0);
        QJsonArray st;
        for (const Stage& s : last_.stages) st.append(s.first);
        o[QStringLiteral("stages")] = st;
    }
    return o;
}

void LabWindow::applyParams(const QJsonObject& p) {
    params_.fromJson(p);
    syncUiFromParams();
}

void LabWindow::resetParams() {
    params_ = LabParams();
    syncUiFromParams();
}

void LabWindow::resetImage() {
    fullMatte_ = QImage();
    fullRawAlpha_.clear();
    last_ = RunResult();
    updateView();
    status_->setText(QStringLiteral("Seed matte cleared — Segment to rebuild."));
}

void LabWindow::pushHistory() {
    undoStack_.append(params_.toJson());
    if (undoStack_.size() > 64) undoStack_.removeFirst();
    redoStack_.clear();
}

void LabWindow::undo() {
    if (undoStack_.isEmpty()) return;
    redoStack_.append(params_.toJson());
    params_.fromJson(undoStack_.takeLast());
    syncUiFromParams();
}

void LabWindow::redo() {
    if (redoStack_.isEmpty()) return;
    undoStack_.append(params_.toJson());
    params_.fromJson(redoStack_.takeLast());
    syncUiFromParams();
}

QJsonObject LabWindow::refine() {
    doRefine();
    return paramsState();
}

QImage LabWindow::render() {
    if (last_.matte.isNull()) {
        if (fullImg_.isNull()) return QImage();
        if (params_.view == ViewMode::Matte && !fullMatte_.isNull())
            return matteGray(fullMatte_);
        return ensureARGB(fullImg_);
    }
    return renderView(last_, params_);
}

bool LabWindow::loadImageFile(const QString& path, QString* err) {
    QImage img(path);
    if (img.isNull()) {
        if (err) *err = QStringLiteral("Could not load %1").arg(path);
        return false;
    }
    loadImage(path);
    return true;
}

void LabWindow::setViewMode(int m) {
    params_.view = ViewMode(std::clamp(m, 0, int(ViewMode::Count) - 1));
    if (viewMode_) viewMode_->setCurrentIndex(int(params_.view));
    updateView();
}

void LabWindow::setSolidBg(int i) {
    params_.solidBg = std::clamp(i, 0, 3);
    if (solidBg_) solidBg_->setCurrentIndex(params_.solidBg);
    updateView();
}

bool LabWindow::startMcp(quint16 port, QString* err) {
    if (mcp_) return true;
    mcp_ = new LabMcpServer(this);
    if (!mcp_->listen(port, err)) {
        delete mcp_;
        mcp_ = nullptr;
        return false;
    }
    statusBar()->showMessage(
        QStringLiteral("MCP listening on http://127.0.0.1:%1/mcp")
            .arg(mcp_->port()));
    return true;
}

// Mirror params_ into the widgets so a remote edit is visible in the UI.
void LabWindow::syncUiFromParams() {
    if (viewMode_) viewMode_->setCurrentIndex(int(params_.view));
    if (solidBg_) solidBg_->setCurrentIndex(params_.solidBg);
    if (!fullImg_.isNull() && !fullRawAlpha_.empty())
        rebuildMatteFromRaw();
    else
        scheduleRecompute();
}

void LabWindow::applyDarkPalette(bool on) {
    if (!on) {
        qApp->setStyleSheet(QString());
        qApp->setPalette(QPalette());
        if (view_) view_->setBackgroundBrush(QColor(255, 255, 255));
        return;
    }
    // A palette alone is not enough: the widget style paints group boxes,
    // sliders, combo boxes, toolbars and scroll bars from its own metrics and
    // ignores most QPalette roles. Ship a full stylesheet so the WHOLE surface
    // goes dark (photophobia-friendly), not just the text.
    const QString qss = QStringLiteral(R"(
        QWidget { background-color: #202226; color: #d7dae0; }
        QMainWindow { background-color: #191b1e; }
        QToolTip { color: #d7dae0; background-color: #2a2d32; border: 1px solid #3b3f46; }

        QMenuBar { background-color: #202226; border-bottom: 1px solid #32363c; }
        QMenuBar::item:selected { background-color: #32363c; }
        QMenu { background-color: #2a2d32; border: 1px solid #3b3f46; }
        QMenu::item:selected { background-color: #3a6ea8; }
        QMenu::separator { height: 1px; background: #3b3f46; margin: 4px 6px; }

        QToolBar { background: transparent; border: none; spacing: 4px; padding: 2px; }
        QToolBar::separator { background: #3b3f46; width: 1px; margin: 5px 3px; }
        QStatusBar { background-color: #191b1e; color: #aeb3bb; border-top: 1px solid #32363c; }

        QDockWidget { color: #d7dae0; }
        QDockWidget::title { background: #2a2d32; padding: 5px; border-bottom: 1px solid #3b3f46; }

        QGroupBox { border: 1px solid #3b3f46; border-radius: 4px; margin-top: 11px; padding: 6px 6px 4px 6px; font-weight: 600; }
        QGroupBox::title { subcontrol-origin: margin; left: 9px; padding: 0 4px; color: #8fb0d8; }
        QLabel { background: transparent; }

        QPushButton { background-color: #32363c; border: 1px solid #454a52; border-radius: 3px; padding: 4px 12px; }
        QPushButton:hover { background-color: #3c414a; }
        QPushButton:pressed { background-color: #2a2d32; }
        QPushButton:checked { background-color: #3a6ea8; border-color: #4d80bb; }

        QComboBox { background-color: #2a2d32; border: 1px solid #454a52; border-radius: 3px; padding: 2px 8px; min-height: 20px; }
        QComboBox:hover { border-color: #565c66; }
        QComboBox:on { background-color: #32363c; }
        QComboBox QAbstractItemView { background-color: #2a2d32; border: 1px solid #454a52; selection-background-color: #3a6ea8; color: #d7dae0; }

        QSpinBox, QDoubleSpinBox { background-color: #2a2d32; border: 1px solid #454a52; border-radius: 3px; padding: 1px 5px; selection-background-color: #3a6ea8; }
        QSpinBox::up-button, QDoubleSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::down-button { background: #32363c; border-left: 1px solid #454a52; width: 16px; }
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow, QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { width: 8px; height: 8px; }

        QSlider::groove:horizontal { height: 4px; background: #3b3f46; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: #3a6ea8; border-radius: 2px; }
        QSlider::handle:horizontal { width: 15px; margin: -6px 0; background: #7aabd8; border-radius: 8px; }
        QSlider::handle:horizontal:hover { background: #9bc0e6; }

        QCheckBox { spacing: 7px; background: transparent; }
        QCheckBox::indicator { width: 15px; height: 15px; border: 1px solid #454a52; border-radius: 3px; background: #2a2d32; }
        QCheckBox::indicator:checked { background: #3a6ea8; border-color: #4d80bb; image: none; }

        QScrollArea { border: none; background: transparent; }
        QScrollBar:vertical { background: #202226; width: 12px; margin: 0; }
        QScrollBar::handle:vertical { background: #454a52; border-radius: 6px; min-height: 26px; }
        QScrollBar::handle:vertical:hover { background: #565c66; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical, QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { height: 0; background: none; }
        QScrollBar:horizontal { background: #202226; height: 12px; margin: 0; }
        QScrollBar::handle:horizontal { background: #454a52; border-radius: 6px; min-width: 26px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal, QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { width: 0; background: none; }

        QGraphicsView { background-color: #16181b; border: 1px solid #32363c; }
    )");
    qApp->setStyleSheet(qss);
    if (view_) view_->setBackgroundBrush(QColor(22, 24, 27));
}

void LabWindow::setDarkMode(bool on) {
    dark_ = on;
    applyDarkPalette(on);
}

}  // namespace pittore::lab
