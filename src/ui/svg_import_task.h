#pragma once
// Threaded SVG import: bytes in on any thread, result out on the GUI
// thread. SvgImportTask is the synchronous core (fully headless-testable);
// importSvgAsync runs it on the global pool and delivers the finished task
// to a receiver object via queued connection. The worker touches only its
// own bytes/result/progress - never live document state - and the result
// is dropped unread if the receiver dies first.
#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

#include "ui/svg_parts.h"

namespace pittore::ui {

struct SvgImportTask {
    QByteArray bytes;
    // Null means legacy synchronous behavior: no trace, no cancel.
    std::shared_ptr<SvgImportProgress> progress;
    SvgImportResult result;
    int dpi = 96;
    QString error;
    bool cancelled = false;
    bool ok = false;
    // Runs the import on the calling thread.
    void run();
};

// Fires on the receiver's thread with the finished task (ok, cancelled, or
// failed with error set). Cancelled completions carry an empty error: quiet
// abort, never a warning box.
using SvgImportDone = std::function<void(SvgImportTask)>;

void importSvgAsync(QObject* receiver, QByteArray bytes,
                    std::shared_ptr<SvgImportProgress> progress,
                    SvgImportDone done);

}  // namespace pittore::ui
