#include <QApplication>
#include <QGuiApplication>
#include <QStyleFactory>

#include <cstring>

#include "engine/core/log.h"
#include "ui/ai_models.h"
#include "ui/app_state.h"
#include "ui/logging.h"
#include "ui/main_window.h"
#include "ui/mcp/mcp_bridge.h"
#include "ui/settings.h"
#include "ui/theme.h"

namespace {

bool hasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    // CLI helpers for `just models`. Kept here so this file stays free of
    // Qt's protected symbols (the app links non-PIE via nvcc).
    if (hasFlag(argc, argv, "--list-models"))
        return pittore::ui::ai_model_cli(argc, argv, /*list=*/true);
    if (hasFlag(argc, argv, "--fetch-models"))
        return pittore::ui::ai_model_cli(argc, argv, /*list=*/false);
    if (hasFlag(argc, argv, "--fetch-model"))
        return pittore::ui::ai_model_cli(argc, argv, /*list=*/false);

    // Display scaling from Settings.toml, read before the toolkit starts
    // (it only honours these at construction). HiDPI off runs the whole
    // interface unscaled for small screens; otherwise fractional scales
    // like 125/150% either stay exact or round to whole pixels. An explicit
    // environment override always wins over the stored preference.
    pittore::ui::AppSettings early;
    pittore::ui::loadSettings(early);
    if (!early.hidpiEnabled &&
        !qEnvironmentVariableIsSet("QT_ENABLE_HIGHDPI_SCALING"))
        qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        early.hidpiFractional
            ? Qt::HighDpiScaleFactorRoundingPolicy::PassThrough
            : Qt::HighDpiScaleFactorRoundingPolicy::Round);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("painter"));
    QCoreApplication::setOrganizationName(QStringLiteral("PittoreStudio"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.1"));

    // Fusion lets our theme own every colour; native styles hard-code theirs.
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // Set up logging first so early startup still gets logged. Always on
    // (alpha); PITTORE_DEBUG=0 silences verbose lines.
    pittore::ui::install_logging();
    ::pittore::core::log::log_info(
        "[app] Pittore Studio 0.1.1 starting | log dir: %s",
        ::pittore::core::log::log_dir().c_str());

    pittore::ui::AppState state;
    QApplication::setPalette(pittore::ui::paletteFor(state.theme()));
    app.setStyleSheet(pittore::ui::styleSheetFor(state.theme()));

    pittore::ui::MainWindow window(&state);
    window.resize(1680, 1000);
    window.show();

    // Live-session AI bridge: local socket for the pittore-mcp shim.
    // Bridgeless (second instance) is fine — the app runs on regardless.
    pittore::ui::McpBridge mcpBridge(&state);
    // Headless command entry (Auto Refine et al. by id) for MCP clients.
    mcpBridge.setCommandRunner([&window](const QString& id) {
        return window.runHeadlessCommand(id);
    });
    mcpBridge.start();

    return QApplication::exec();
}
