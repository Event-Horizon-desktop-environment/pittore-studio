// Threaded SVG import: the task runs the import off-thread with progress
// and cancel, and the runner delivers the finished task on the GUI thread.
// Headless (offscreen platform); small synthetics only, no fixtures.
#include <QApplication>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <utility>
#include <vector>

#include "test_util.h"
#include "ui/svg_import_task.h"

using namespace pittore::ui;

namespace {

QByteArray tinySvg() {
    return QByteArray(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\">"
        "<rect width=\"64\" height=\"64\" fill=\"#ffffff\"/>"
        "<rect x=\"8\" y=\"8\" width=\"48\" height=\"48\" fill=\"#000000\"/>"
        "</svg>");
}

// Enough raster work to cancel mid-flight deterministically: the abort
// lands while hundreds of jobs remain, never before the first one.
QByteArray manyRectSvg() {
    QByteArray xml(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" "
        "height=\"1000\">");
    for (int i = 0; i < 2000; ++i) {
        xml += "<rect x=\"";
        xml += QByteArray::number((i * 37) % 990);
        xml += "\" y=\"";
        xml += QByteArray::number((i * 53) % 990);
        xml += "\" width=\"10\" height=\"10\" fill=\"#123456\"/>";
    }
    xml += "</svg>";
    return xml;
}

void test_task_completes() {
    SvgImportTask task;
    task.bytes = tinySvg();
    auto progress = std::make_shared<SvgImportProgress>();
    std::vector<std::pair<int, int>> seen;
    progress->onUpdate = [&](int percent, int phase) {
        seen.emplace_back(percent, phase);
    };
    task.progress = progress;
    task.run();
    CHECK(task.ok);
    CHECK(!task.cancelled);
    CHECK(task.error.isEmpty());
    CHECK(!task.result.layers.isEmpty());
    CHECK(progress->percent.load() == 100);
    CHECK(progress->phase.load() ==
          static_cast<int>(SvgImportPhase::Done));
    // Monotonic by construction, even across raster worker threads.
    for (std::size_t i = 1; i < seen.size(); ++i) {
        CHECK(seen[i].first >= seen[i - 1].first);
        CHECK(seen[i].second >= seen[i - 1].second);
    }
    CHECK(!seen.empty());
}

void test_preset_cancel() {
    SvgImportTask task;
    task.bytes = tinySvg();
    auto progress = std::make_shared<SvgImportProgress>();
    progress->cancelled = true;
    task.progress = progress;
    task.run();
    CHECK(!task.ok);
    CHECK(task.cancelled);
    // Quiet abort: no error text, no partial commit.
    CHECK(task.error.isEmpty());
    CHECK(task.result.layers.isEmpty());
}

void test_cancel_mid_raster() {
    SvgImportTask task;
    task.bytes = manyRectSvg();
    auto progress = std::make_shared<SvgImportProgress>();
    progress->onUpdate = [&](int percent, int) {
        // First raster callback aborts with ~1999 shapes still queued:
        // deterministic mid-flight cancel however fast the box is.
        if (percent >= 40) progress->cancelled = true;
    };
    task.progress = progress;
    task.run();
    CHECK(!task.ok);
    CHECK(task.cancelled);
    CHECK(task.error.isEmpty());
    CHECK(task.result.layers.isEmpty());
}

void test_async_delivers_on_gui_thread() {
    QEventLoop loop;
    bool done = false, onGui = false, ok = false;
    int layers = 0;
    QObject receiver;
    auto progress = std::make_shared<SvgImportProgress>();
    importSvgAsync(&receiver, tinySvg(), progress,
                   [&](SvgImportTask task) {
                       done = true;
                       onGui = QThread::currentThread() ==
                               QCoreApplication::instance()->thread();
                       ok = task.ok;
                       layers = task.result.layers.size();
                       loop.quit();
                   });
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);
    loop.exec();
    CHECK(done);
    CHECK(onGui);
    CHECK(ok);
    CHECK(layers > 0);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    test_task_completes();
    test_preset_cancel();
    test_cancel_mid_raster();
    test_async_delivers_on_gui_thread();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
