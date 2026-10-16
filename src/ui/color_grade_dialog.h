#pragma once
// Color Grading window (Filter menu, Liquify-style modal): conventional
// sections (Exposure / Enhance / White Balance) bound to real adjustment
// layers, with a live preview, presets, and per-section reset.
//
// Each row ensures its backing layer on first write. The whole session is
// one undo step: the first write opens it, OK commits ("Color Grade"),
// Cancel discards it. Slider storms are throttled (leading + trailing) so
// a drag never recomposites per tick.
#include <QDialog>
#include <QMap>
#include <QPair>
#include <QPixmap>
#include <QString>
#include <QVector>

#include <array>

class QDoubleSpinBox;
class QLabel;
class QSlider;
class QTimer;

namespace pittore::ui {

class AppState;

class ColorGradeDialog final : public QDialog {
    Q_OBJECT

  public:
    // Usable with any document open (pixel layers included): backing layers
    // are created on first write. Without a document it shows a note.
    explicit ColorGradeDialog(AppState* state, QWidget* parent = nullptr);

    void accept() override;
    void reject() override;

  private:
    struct Row {
        int kind = 0;
        int pi = 0;
        QString label;
        QString suffix;
        double factor = 1.0;
        double sliderMin = 0.0;
        double sliderMax = 100.0;
        double realMin = 0.0;
        double realMax = 1.0;
        double neutral = 0.0;
        QString trackCss;
        QSlider* slider = nullptr;
        QDoubleSpinBox* spin = nullptr;
    };

    int findLayer(int kind) const;
    int ensureLayer(int kind);
    // Open the session-wide undo step on first write, so layer creation and
    // every param edit coalesce into the single step accept() commits.
    void beginGradeGesture();
    void writeThrough(int layer, int pi, float value);
    void requestWrite(int kind, int pi, float value);
    void flushWrites();
    void syncRows();
    void applyPreset(int preset);
    void resetSection(const QVector<int>& kinds);
    void schedulePreview();
    void renderPreview();

    AppState* state_;
    int origActive_ = -1;
    bool gradeGesture_ = false;
    QTimer* writeTimer_ = nullptr;
    QMap<QPair<int, int>, float> pending_;
    QTimer* previewTimer_ = nullptr;
    bool previewPending_ = false;
    QLabel* preview_ = nullptr;
    QPixmap before_;
    QVector<Row> rows_;
};

}  // namespace pittore::ui
