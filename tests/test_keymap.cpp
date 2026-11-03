// test_keymap.cpp — keymap matching: keyEventMatchesShortcut() (Shift reserved
// as the brush hardness flag) and keyEventMatchesShortcutExact() (every
// modifier counts), plus the shipped clip/blend defaults. No config file, no
// widgets.
#include <cstdio>

#include <QKeyEvent>

#include "test_util.h"
#include "ui/keymap.h"

int main() {
    using pittore::ui::defaultKeymapEntries;
    using pittore::ui::keyEventMatchesShortcut;
    using pittore::ui::keyEventMatchesShortcutExact;
    const QKeyEvent bareLeft(QEvent::KeyPress, Qt::Key_BracketLeft,
                             Qt::NoModifier, QStringLiteral("["));
    const QKeyEvent shiftLeft(QEvent::KeyPress, Qt::Key_BracketLeft,
                              Qt::ShiftModifier, QStringLiteral("{"));
    const QKeyEvent ctrlLeft(QEvent::KeyPress, Qt::Key_BracketLeft,
                             Qt::ControlModifier, QStringLiteral("["));
    const QKeyEvent bareRight(QEvent::KeyPress, Qt::Key_BracketRight,
                              Qt::NoModifier, QStringLiteral("]"));
    const QKeyEvent ctrlB(QEvent::KeyPress, Qt::Key_B, Qt::ControlModifier,
                          QStringLiteral("b"));
    const QKeyEvent bareB(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier,
                          QStringLiteral("b"));
    const QKeyEvent bareX(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier,
                          QStringLiteral("x"));
    // Defaults: [ / ] shrink/grow, Shift ignored (hardness flag).
    CHECK(keyEventMatchesShortcut(&bareLeft, QStringLiteral("[")));
    CHECK(keyEventMatchesShortcut(&shiftLeft, QStringLiteral("[")));
    CHECK(!keyEventMatchesShortcut(&ctrlLeft, QStringLiteral("[")));
    CHECK(!keyEventMatchesShortcut(&bareRight, QStringLiteral("[")));
    CHECK(keyEventMatchesShortcut(&bareRight, QStringLiteral("]")));
    // Modifier bindings compare exactly (minus Shift).
    CHECK(keyEventMatchesShortcut(&ctrlB, QStringLiteral("Ctrl+B")));
    CHECK(!keyEventMatchesShortcut(&bareB, QStringLiteral("Ctrl+B")));
    CHECK(keyEventMatchesShortcut(&bareX, QStringLiteral("X")));
    // Empty, garbage and multi-key sequences never match.
    CHECK(!keyEventMatchesShortcut(&bareLeft, QString()));
    CHECK(!keyEventMatchesShortcut(&bareLeft, QStringLiteral("Ctrl+K,Ctrl+U")));
    CHECK(!keyEventMatchesShortcut(nullptr, QStringLiteral("[")));

    // Exact variant (Alt+Shift+<letter> family): Shift is part of the binding.
    const QKeyEvent altShiftM(QEvent::KeyPress, Qt::Key_M,
                              Qt::AltModifier | Qt::ShiftModifier,
                              QStringLiteral("M"));
    const QKeyEvent altM(QEvent::KeyPress, Qt::Key_M, Qt::AltModifier,
                         QStringLiteral("m"));
    const QKeyEvent shiftM(QEvent::KeyPress, Qt::Key_M, Qt::ShiftModifier,
                           QStringLiteral("M"));
    const QKeyEvent ctrlAltG(QEvent::KeyPress, Qt::Key_G,
                             Qt::ControlModifier | Qt::AltModifier,
                             QStringLiteral("g"));
    CHECK(keyEventMatchesShortcutExact(&altShiftM, QStringLiteral("Alt+Shift+M")));
    CHECK(!keyEventMatchesShortcutExact(&altM, QStringLiteral("Alt+Shift+M")));
    CHECK(!keyEventMatchesShortcutExact(&shiftM, QStringLiteral("Alt+Shift+M")));
    CHECK(!keyEventMatchesShortcutExact(&altShiftM, QStringLiteral("Alt+M")));
    CHECK(keyEventMatchesShortcutExact(&ctrlAltG, QStringLiteral("Ctrl+Alt+G")));
    CHECK(!keyEventMatchesShortcutExact(&ctrlAltG,
                                        QStringLiteral("Ctrl+Alt+Shift+G")));
    // Shift alone never matches a bare binding in the exact variant.
    CHECK(!keyEventMatchesShortcutExact(&shiftLeft, QStringLiteral("[")));
    CHECK(!keyEventMatchesShortcutExact(&shiftM, QStringLiteral("M")));
    CHECK(!keyEventMatchesShortcutExact(&altShiftM, QString()));
    CHECK(!keyEventMatchesShortcutExact(&altShiftM,
                                        QStringLiteral("Ctrl+K,Ctrl+U")));
    CHECK(!keyEventMatchesShortcutExact(nullptr, QStringLiteral("Alt+Shift+M")));

    // The Photoshop-style defaults shipped for the clip/blend family.
    const QMap<QString, QString> defaults = defaultKeymapEntries();
    CHECK(defaults.value(QStringLiteral("toggle-clip")) == QStringLiteral("Ctrl+Alt+G"));
    CHECK(defaults.value(QStringLiteral("toggle-clip-alt")) == QStringLiteral("Alt+Shift+G"));
    CHECK(defaults.value(QStringLiteral("blend-multiply")) == QStringLiteral("Alt+Shift+M"));
    CHECK(defaults.value(QStringLiteral("blend-screen")) == QStringLiteral("Alt+Shift+S"));

    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return pittore_test::failures() == 0 ? 0 : 1;
}
