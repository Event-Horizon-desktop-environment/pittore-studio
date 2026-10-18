#include "ui/keymap.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QStandardPaths>

namespace pittore::ui {

QString keymapFilePath()
{
    QDir base(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation));
    return base.filePath(QStringLiteral("PittoreStudio/keymap.json"));
}

QMap<QString, QString> defaultKeymapEntries()
{
    QMap<QString, QString> m;
    m.insert(QStringLiteral("undo"), QStringLiteral("Ctrl+Z"));
    m.insert(QStringLiteral("redo"), QStringLiteral("Ctrl+Shift+Z"));
    m.insert(QStringLiteral("copy"), QStringLiteral("Ctrl+C"));
    m.insert(QStringLiteral("copy-merged"), QStringLiteral("Ctrl+Shift+C"));
    m.insert(QStringLiteral("cut"), QStringLiteral("Ctrl+X"));
    m.insert(QStringLiteral("paste"), QStringLiteral("Ctrl+V"));
    m.insert(QStringLiteral("paste-in-place"), QStringLiteral("Ctrl+Shift+V"));
    m.insert(QStringLiteral("clear"), QStringLiteral("Delete"));
    m.insert(QStringLiteral("fill-fg"), QStringLiteral("Alt+Backspace"));
    m.insert(QStringLiteral("fill-bg"), QStringLiteral("Ctrl+Backspace"));
    m.insert(QStringLiteral("select-all"), QStringLiteral("Ctrl+A"));
    m.insert(QStringLiteral("deselect"), QStringLiteral("Ctrl+D"));
    m.insert(QStringLiteral("reselect"), QStringLiteral("Ctrl+Shift+D"));
    m.insert(QStringLiteral("invert-selection"), QStringLiteral("Ctrl+Shift+I"));
    m.insert(QStringLiteral("new-layer"), QStringLiteral("Ctrl+Shift+N"));
    m.insert(QStringLiteral("layer-via-copy"), QStringLiteral("Ctrl+J"));
    m.insert(QStringLiteral("layer-via-cut"), QStringLiteral("Ctrl+Shift+J"));
    m.insert(QStringLiteral("group-layers"), QStringLiteral("Ctrl+G"));
    m.insert(QStringLiteral("ungroup-layers"), QStringLiteral("Ctrl+Shift+G"));
    m.insert(QStringLiteral("bring-to-front"), QStringLiteral("Ctrl+Shift+]"));
    m.insert(QStringLiteral("bring-forward"), QStringLiteral("Ctrl+]"));
    m.insert(QStringLiteral("send-backward"), QStringLiteral("Ctrl+["));
    m.insert(QStringLiteral("send-to-back"), QStringLiteral("Ctrl+Shift+["));
    m.insert(QStringLiteral("toggle-clip"), QStringLiteral("Ctrl+Alt+G"));
    // Second clipping binding: the Photoshop-alt layout for a brush-first
    // workflow (Alt+Shift+G), routed through the same command.
    m.insert(QStringLiteral("toggle-clip-alt"), QStringLiteral("Alt+Shift+G"));
    // Painting blend-mode letters (Alt+Shift+<key>); with the Move tool they
    // set the active layer's blend mode instead.
    m.insert(QStringLiteral("blend-multiply"), QStringLiteral("Alt+Shift+M"));
    m.insert(QStringLiteral("blend-screen"), QStringLiteral("Alt+Shift+S"));
    m.insert(QStringLiteral("merge-down"), QStringLiteral("Ctrl+E"));
    m.insert(QStringLiteral("merge-visible"), QStringLiteral("Ctrl+Shift+E"));
    m.insert(QStringLiteral("free-transform"), QStringLiteral("Ctrl+T"));
    m.insert(QStringLiteral("zoom-in"), QStringLiteral("Ctrl++"));
    m.insert(QStringLiteral("zoom-out"), QStringLiteral("Ctrl+-"));
    m.insert(QStringLiteral("fit-screen"), QStringLiteral("Ctrl+0"));
    m.insert(QStringLiteral("actual-pixels"), QStringLiteral("Ctrl+1"));
    m.insert(QStringLiteral("toggle-rulers"), QStringLiteral("Ctrl+R"));
    m.insert(QStringLiteral("toggle-grid"), QStringLiteral("Ctrl+'"));
    m.insert(QStringLiteral("toggle-guides"), QStringLiteral("Ctrl+;"));
    m.insert(QStringLiteral("toggle-extras"), QStringLiteral("Ctrl+H"));
    m.insert(QStringLiteral("toggle-snap"), QStringLiteral("Ctrl+Shift+;"));
    m.insert(QStringLiteral("spotlight"), QStringLiteral("Ctrl+Shift+P"));
    m.insert(QStringLiteral("preferences"), QStringLiteral("Ctrl+K"));
    m.insert(QStringLiteral("levels"), QStringLiteral("Ctrl+L"));
    m.insert(QStringLiteral("curves"), QStringLiteral("Ctrl+M"));
    m.insert(QStringLiteral("hue-sat"), QStringLiteral("Ctrl+U"));
    m.insert(QStringLiteral("invert"), QStringLiteral("Ctrl+I"));
    m.insert(QStringLiteral("brush-smaller"), QStringLiteral("["));
    m.insert(QStringLiteral("brush-larger"), QStringLiteral("]"));
    m.insert(QStringLiteral("swap-colors"), QStringLiteral("X"));
    m.insert(QStringLiteral("reset-colors"), QStringLiteral("D"));
    m.insert(QStringLiteral("toggle-panels"), QStringLiteral("Tab"));
    m.insert(QStringLiteral("screen-mode"), QStringLiteral("F"));
    m.insert(QStringLiteral("next-doc"), QStringLiteral("Ctrl+Tab"));
    m.insert(QStringLiteral("prev-doc"), QStringLiteral("Ctrl+Shift+Tab"));
    return m;
}

QMap<QString, QString> loadKeymapEntries()
{
    QMap<QString, QString> entries = defaultKeymapEntries();
    QFile f(keymapFilePath());
    if (!f.open(QIODevice::ReadOnly))
        return entries;
    const QByteArray raw = f.readAll();
    f.close();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return entries;
    const QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!it.value().isString())
            continue;
        const QString k = it.key();
        const QString v = it.value().toString().trimmed();
        if (v.isEmpty()) {
            entries.remove(k);
            continue;
        }
        if (QKeySequence(v, QKeySequence::PortableText).isEmpty() && v.size() != 1)
            continue;
        entries.insert(k, v);
    }
    return entries;
}

bool saveKeymapEntries(const QMap<QString, QString> &entries)
{
    QJsonObject obj;
    for (auto it = entries.begin(); it != entries.end(); ++it)
        obj.insert(it.key(), it.value());
    QJsonDocument doc(obj);
    const QString path = keymapFilePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

bool keyEventMatchesShortcut(const QKeyEvent *key, const QString &seq)
{
    if (!key)
        return false;
    const QKeySequence want(seq, QKeySequence::PortableText);
    if (want.isEmpty() || want.count() != 1)
        return false;
    const QKeyCombination combo = want[0];
    // Shift is the brush hardness flag, not part of the binding.
    const Qt::KeyboardModifiers wantMods = combo.keyboardModifiers() &
                                           (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    const Qt::KeyboardModifiers gotMods =
        key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    return wantMods == gotMods && combo.key() == Qt::Key(key->key());
}

bool keyEventMatchesShortcutExact(const QKeyEvent *key, const QString &seq)
{
    if (!key)
        return false;
    const QKeySequence want(seq, QKeySequence::PortableText);
    if (want.isEmpty() || want.count() != 1)
        return false;
    const QKeyCombination combo = want[0];
    const Qt::KeyboardModifiers all = Qt::ShiftModifier | Qt::ControlModifier |
                                       Qt::AltModifier | Qt::MetaModifier;
    return (combo.keyboardModifiers() & all) == (key->modifiers() & all) &&
           combo.key() == Qt::Key(key->key());
}

}
