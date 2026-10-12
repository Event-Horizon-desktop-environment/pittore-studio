#pragma once
// Refine Selection dialog (Select Subject / Object Select follow-up):
// Matte edges / Border / Smooth / Feather / Ramp + adjustment
// brush (Matte/Foreground/Background/Feather) + preview modes + Selection /
// Mask / decontaminated-layer outputs. Collects a RefineResult on accept;
// MainWindow::applyRefineResult commits it (one undo step per output).
#include <QDialog>
#include <QImage>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QMouseEvent;
class QPushButton;
class QSlider;
class QSpinBox;

namespace pittore::ui {

class AppState;
class DocumentItem;

struct RefineResult {
    bool accepted = false;
    QImage mask;   // doc-sized Grayscale8 refined coverage
    enum class Output {
        Selection,
        LayerMask,
        NewDecontaminatedLayer,
        NewDecontaminatedLayerWithMask,
    };
    Output output = Output::Selection;
};

class RefineDialog final : public QDialog {
    Q_OBJECT

  public:
    RefineDialog(AppState* state, DocumentItem* doc, QWidget* parent = nullptr);

    RefineResult result() const { return result_; }

  protected:
    // Preview interaction arrives via event filter (label-local coords):
    // paint strokes, hover ring, leave-to-clear.
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    // One adjustment-brush dab in work-rect coordinates.
    struct Stroke {
        QPointF pos;
        double radius = 25.0;
        int mode = 0;   // 0 matte, 1 foreground, 2 background, 3 feather
    };

    void recompute();       // full pipeline at proxy scale (live)
    QImage recomputeFull() const;   // full-resolution result for Apply
    // Shared pipeline: base/comp crops in the same space (comp is the ARGB
    // edge guide the snap solves against), stroke coords scaled by coordScale
    // into that space (proxyScale_ live, 1.0 at Apply).
    // rampPct is -100..100: positive hardens, negative grows the edge softly.
    // shiftPx displaces the edge (- = contract, + = expand) after feather.
    // smartRadius runs the snap at a narrow and a wide band, accepting the
    // wide result only where the mask edge is soft (Photoshop-style).
    QImage runPipeline(const QImage& base, const QImage& comp, int bandPx,
                       int smoothPasses, int featherPx, int shiftPx, int rampPct,
                       const QVector<Stroke>& strokes, bool smartRadius,
                       double coordScale) const;
    void refreshPreview();  // render preview_ from composite + refined crops
    QImage renderPreview() const;  // preview pixmap for the current view mode
    void rebuildOutputList();
    void enhanceEdgesWithAi();  // Enhance Edges button: local portrait-matting
                                // AI re-estimates hair/soft edges inside the
                                // edge band (falls back to the colour snap
                                // when no AI model is installed).
    void paintAt(const QPointF& labelPos);

    // Work-rect mapping between the preview label and mask pixels.
    QPointF labelToWork(const QPointF& labelPos) const;
    // Scroll-to-zoom around the cursor (0.25x-8x) + middle-drag pan.
    void zoomAt(const QPointF& labelPos, double factor);
    static QPoint clampViewOff(QPoint off, const QSize& box, const QSize& pm);

    AppState* state_ = nullptr;
    DocumentItem* doc_ = nullptr;

    QImage inputMask_;       // doc-sized Grayscale8 entering the dialog
    QImage compositeFull_;   // doc composite for the preview + colour guide
    QRect workRect_;         // bbox padded, intersected with the canvas
    QImage baseCrop_;        // input crop over workRect_ (full resolution)
    // Interactive proxy (longest side capped): every live recompute, brush
    // stamp and preview render runs here, so painting stays instant no
    // matter the document size. Strokes are stored in full crop coords and
    // replayed 1:1 for the Apply-time full-resolution result.
    double proxyScale_ = 1.0;
    QImage baseProxy_;
    QImage compProxy_;
    QImage refinedProxy_;
    QVector<Stroke> strokes_;
    // In-progress press footprint in proxy-crop coords + proxy radius, drawn
    // as translucent discs so painting previews the real brush coverage
    // (committed paint already shows in the matte itself). Cleared on release
    // when the pipeline re-solves.
    struct PressDot {
        QPointF pos;
        double radius = 1.0;
    };
    QVector<PressDot> pressDots_;

    RefineResult result_;

    // Widgets.
    QLabel* preview_ = nullptr;
    QComboBox* previewCombo_ = nullptr;   // Overlay/Black/White/B&W/Transparent
    QCheckBox* matteEdgesCheck_ = nullptr;
    QCheckBox* smartRadiusCheck_ = nullptr;
    QPushButton* enhanceButton_ = nullptr;
    QLabel* enhanceStatus_ = nullptr;
    QSlider* borderSlider_ = nullptr;     // 0-100 %
    QLabel* borderValue_ = nullptr;
    QSlider* smoothSlider_ = nullptr;     // 0-100 px
    QLabel* smoothValue_ = nullptr;
    QSlider* featherSlider_ = nullptr;    // 0-100 px
    QLabel* featherValue_ = nullptr;
    QSlider* shiftSlider_ = nullptr;       // -100..100 %
    QLabel* shiftValue_ = nullptr;
    QSlider* rampSlider_ = nullptr;       // 0-100 %
    QLabel* rampValue_ = nullptr;
    QVector<QPushButton*> brushModes_;
    QComboBox* brushSizeCombo_ = nullptr;
    QComboBox* outputCombo_ = nullptr;
    QCheckBox* decontamCheck_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;

    bool painting_ = false;
    QPointF hoverLabel_;   // brush ring position, label coords
    bool hoverValid_ = false;
    QSize previewPx_;      // last pixmap size (letterbox-aware mapping)
    QPoint previewOff_;    // pixmap top-left in label coords (zoom/pan)
    double viewZoom_ = 1.0;
    bool panDragging_ = false;
    QPointF panStart_;
    QPoint panOffStart_;
    // Gap-fill cursor: reset on every press so separate strokes never get
    // connected by a streak across the image.
    bool haveLastDab_ = false;
    QPointF lastDab_;
    // Debounced recompute: slider drags update the label instantly and
    // recompute on release (conventional); keyboard nudges, which never
    // emit a release, fall through to this timer instead.
    QTimer* recomputeTimer_ = nullptr;
};

}  // namespace pittore::ui
