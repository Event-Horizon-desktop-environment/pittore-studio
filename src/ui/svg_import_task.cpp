// Threaded SVG import runner: pool execution plus GUI-thread delivery.
// See the header for the threading contract.
#include "ui/svg_import_task.h"

#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>

namespace pittore::ui {
namespace {

class ImportRunner : public QRunnable {
  public:
    ImportRunner(QObject* receiver, QByteArray bytes,
                 std::shared_ptr<SvgImportProgress> progress,
                 SvgImportDone done)
        : receiver_(receiver),
          guard_(receiver),
          bytes_(std::move(bytes)),
          progress_(std::move(progress)),
          done_(std::move(done)) {}
    void run() override {
        SvgImportTask task;
        task.bytes = std::move(bytes_);
        task.progress = std::move(progress_);
        task.run();
        // Queued events for a destroyed object are dropped by Qt, so a
        // receiver that died mid-import simply never hears back.
        if (!guard_.isNull())
            QMetaObject::invokeMethod(
                receiver_,
                [done = std::move(done_), task = std::move(task)]() mutable {
                    done(std::move(task));
                },
                Qt::QueuedConnection);
    }

  private:
    QObject* receiver_ = nullptr;
    QPointer<QObject> guard_;
    QByteArray bytes_;
    std::shared_ptr<SvgImportProgress> progress_;
    SvgImportDone done_;
};

}  // namespace

void SvgImportTask::run() {
    ok = svgPartsImport(bytes, &result, &dpi, &error,
                        progress ? progress.get() : nullptr);
    cancelled =
        !ok && progress && progress->cancelled.load(std::memory_order_relaxed);
    if (cancelled) error.clear();  // cancel intent dominates any race
}

void importSvgAsync(QObject* receiver, QByteArray bytes,
                    std::shared_ptr<SvgImportProgress> progress,
                    SvgImportDone doneCb) {
    auto* runner =
        new ImportRunner(receiver, std::move(bytes), std::move(progress),
                         std::move(doneCb));
    QThreadPool::globalInstance()->start(runner);
}

}  // namespace pittore::ui
