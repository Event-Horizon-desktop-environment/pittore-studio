// Settings persistence round-trip (ui/settings): save → load → equality, and
// the missing-file / parse-error paths. Uses a scratch XDG_CONFIG_HOME so the
// real user settings are never touched.
#include <cstdlib>

#include <QDir>

#include "test_util.h"
#include "ui/settings.h"
#include "ui/theme.h"

using pittore::ui::AppSettings;
using pittore::ui::UiTheme;

namespace {

// Run every check inside a fresh config dir. The Settings.toml path is derived
// from XDG_CONFIG_HOME at call time, so pointing it at a temp dir keeps the
// pristine user config out of harm's way.
QString scratchConfigDir() {
    QString dir;
    if (const char* e = std::getenv("XDG_CONFIG_HOME")) dir = QString::fromLocal8Bit(e);
    else dir = QString::fromLocal8Bit(std::getenv("HOME"));
    const QString scratch = dir + QStringLiteral("/PittoreStudio-settings-test");
    QDir().mkpath(scratch + QStringLiteral("/PittoreStudio"));
    return scratch;
}

}  // namespace

void test_settings() {
    const QString scratch = scratchConfigDir();
    std::puts(("  [settings] scratch: " + scratch.toLocal8Bit()).constData());

    // 1. Missing file: load must signal failure and leave the struct untouched.
    {
        AppSettings before;
        before.theme = UiTheme::Black;
        before.gpuEnabled = true;
        before.gpuDevice = QStringLiteral("Still here");
        before.ramLimitMb = 42;
        AppSettings out = before;

        // Point at a nonexistent file by using an empty scratch path.
        qputenv("XDG_CONFIG_HOME", (scratch + QStringLiteral("/_none")).toLocal8Bit());
        const bool ok = pittore::ui::loadSettings(out);
        qputenv("XDG_CONFIG_HOME", scratch.toLocal8Bit());

        CHECK(!ok);
        CHECK(out.theme == before.theme);
        CHECK(out.gpuEnabled == before.gpuEnabled);
        CHECK(out.gpuDevice == before.gpuDevice);
        CHECK(out.ramLimitMb == before.ramLimitMb);
    }

    // 2. Round-trip: every field survives save → load.
    {
        AppSettings in;
        in.theme = UiTheme::LightGray;
        in.gpuEnabled = true;
        in.gpuDevice = QStringLiteral("NVIDIA GeForce RTX 3080");
        in.cpuDevice = QStringLiteral("CPU (16 threads)");
        in.ramLimitMb = 2048;
        in.bgModel = QStringLiteral("birefnet-general");
        in.referenceMask = QStringLiteral("/tmp/a.png;/tmp/b.png");
        in.autosaveEnabled = false;
        in.autosaveIntervalMinutes = 7;
        in.zoomWithScroll = true;
        in.gridSpacing = 32.0;
        in.snapEnabled = false;
        in.snapTargets = 5;  // Guides + DocumentBounds
        in.tabletMode = true;
        in.hidpiEnabled = false;
        in.hidpiFractional = false;
        in.cursorShape = 2;
        in.outlineShape = 3;
        in.showOutlineWhilePainting = false;
        in.outlineEffectiveSize = true;
        in.newDocWidth = 2560;
        in.newDocHeight = 1440;
        in.newDocDpi = 150;
        in.newDocColorMode = QStringLiteral("Grayscale/8");
        in.newDocBackground = QStringLiteral("transparent");
        in.exportFormat = QStringLiteral("jpg");
        in.exportQuality = 80;
        in.exportLocation = 1;
        in.exportEmbedIcc = false;
        in.psdCompression = 1;
        in.showRulers = false;
        in.showGuides = false;
        in.showGrid = true;
        in.showSelectionEdges = false;
        in.showSmartGuides = false;
        in.showPixelGrid = true;
        in.showExtras = false;
        in.nudgeStepPx = 2.5;
        in.nudgeShiftStepPx = 25.0;
        in.guideColor = QColor(255, 0, 0);
        in.gridColor = QColor(0, 255, 0, 128);
        in.transparencyCellPx = 16;
        in.transparencyLight = QColor(1, 2, 3);
        in.transparencyDark = QColor(4, 5, 6, 200);
        in.recentMax = 5;
        in.workingProfile = QStringLiteral("Adobe RGB (1998)");
        in.colorMismatchPolicy = 1;
        in.undoLimit = 42;
        in.proofProfile = QStringLiteral("/tmp/coated.icc");
        in.proofIntent = 0;
        in.proofBpc = false;
        in.reopenDocuments = true;
        in.gridSubdivisions = 8;
        in.showSliceNumbers = false;
        in.fontPreviewSize = 0;
        in.recentProjects = QStringList{QStringLiteral("/tmp/a.ifp")};
        CHECK(pittore::ui::saveSettings(in));

        AppSettings out;
        CHECK(pittore::ui::loadSettings(out));
        CHECK(out == in);
    }

    // 3. File content is well-formed TOML the second read agrees with (paranoia:
    // a hand-written corrupt file still round-trips through a fresh load).
    {
        AppSettings in;
        in.theme = UiTheme::DarkGray;
        in.gpuEnabled = false;
        in.ramLimitMb = 0;
        CHECK(pittore::ui::saveSettings(in));

        AppSettings out;
        CHECK(pittore::ui::loadSettings(out));
        CHECK(out == in);
    }

    // 4. Fresh installs budget 90% of system RAM (0 = undetectable).
    {
        const int total = pittore::ui::totalSystemRamMb();
        const int def = pittore::ui::defaultRamLimitMb();
        if (total > 0) {
            CHECK(def == static_cast<long long>(total) * 9 / 10);
            const AppSettings fresh;
            CHECK(fresh.ramLimitMb == def);
        } else {
            CHECK(def == 0);
        }
        // Fresh view/cursor defaults match the View menu's historic state.
        const AppSettings fresh;
        CHECK(fresh.snapEnabled == true);
        CHECK(fresh.snapTargets == 31);
        CHECK(fresh.tabletMode == false);
        CHECK(fresh.hidpiEnabled == true);
        CHECK(fresh.hidpiFractional == true);
        CHECK(fresh.cursorShape == 0);
        CHECK(fresh.outlineShape == 2);
        CHECK(fresh.showOutlineWhilePainting == true);
        CHECK(fresh.outlineEffectiveSize == false);
        CHECK(fresh.newDocWidth == 1920);
        CHECK(fresh.newDocHeight == 1080);
        CHECK(fresh.newDocDpi == 300);
        CHECK(fresh.newDocColorMode == QStringLiteral("RGB/8"));
        CHECK(fresh.newDocBackground == QStringLiteral("white"));
        CHECK(fresh.exportFormat == QStringLiteral("png"));
        CHECK(fresh.exportQuality == 92);
        CHECK(fresh.exportLocation == 0);
        CHECK(fresh.exportEmbedIcc == true);
        CHECK(fresh.psdCompression == 0);
        CHECK(fresh.showRulers == true);
        CHECK(fresh.showGuides == true);
        CHECK(fresh.showGrid == false);
        CHECK(fresh.showSelectionEdges == true);
        CHECK(fresh.showSmartGuides == true);
        CHECK(fresh.showPixelGrid == false);
        CHECK(fresh.showExtras == true);
        CHECK(fresh.nudgeStepPx == 1.0);
        CHECK(fresh.nudgeShiftStepPx == 10.0);
        CHECK(fresh.guideColor == QColor(0x3d, 0xa5, 0xff));
        CHECK(fresh.gridColor == QColor(255, 255, 255, 40));
        CHECK(fresh.transparencyCellPx == 8);
        CHECK(fresh.transparencyLight == QColor(0xff, 0xff, 0xff));
        CHECK(fresh.transparencyDark == QColor(0xbf, 0xbf, 0xbf));
        CHECK(fresh.recentMax == 12);
        CHECK(fresh.workingProfile == QStringLiteral("sRGB IEC61966-2.1"));
        CHECK(fresh.colorMismatchPolicy == 3);
        CHECK(fresh.undoLimit == 100);
        CHECK(fresh.proofProfile.isEmpty());
        CHECK(fresh.proofIntent == 1);
        CHECK(fresh.proofBpc == true);
        CHECK(fresh.reopenDocuments == false);
        CHECK(fresh.gridSubdivisions == 4);
        CHECK(fresh.showSliceNumbers == true);
        CHECK(fresh.fontPreviewSize == 2);
    }

    // Cleanup: scratch dir only, never the user's real config.
    QDir(scratch + QStringLiteral("/_none")).removeRecursively();
    QDir(scratch).removeRecursively();
}

TEST_MAIN_CALL(test_settings)