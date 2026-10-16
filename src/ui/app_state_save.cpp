// Persisting a document: the native container, the layered exporters, crash
// recovery, session restore and the imported colour-profile policy.
// Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/project_manager.h"
#include "ui/settings.h"
#include "ui/tools/log/tool_log.h"

#include "engine/core/log.h"
#include "engine/io/af_layers/emit/af_write.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace pittore::ui {

bool AppState::saveProject(const QString& path, QString* error) {
    DocumentItem* doc = activeDocument();
    if (!doc) {
        if (error) *error = tr("No document open to save.");
        return false;
    }
    if (path.trimmed().isEmpty()) {
        if (error) *error = tr("No project file given.");
        return false;
    }
    // Layered Affinity files save through the .af exporter (same bytes as
    // File > Export Layered Affinity, so Save and Export agree). Anything
    // without an Affinity record bakes or drops exactly as reported there;
    // the live document is untouched, so re-saving is idempotent.
    const QString low = path.toLower();
    if (low.endsWith(QStringLiteral(".af")) || low.endsWith(QStringLiteral(".afphoto")) ||
        low.endsWith(QStringLiteral(".afdesign")) || low.endsWith(QStringLiteral(".afpub"))) {
        auto enc = exportAfLayers(*doc, error);
        if (!enc) return false;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = tr("Could not write %1.").arg(path);
            return false;
        }
        f.write(reinterpret_cast<const char*>(enc->bytes.data()),
                static_cast<qint64>(enc->bytes.size()));
        doc->filePath = QFileInfo(path).absoluteFilePath();
        doc->dirty = false;
        noteRecentProject(doc->filePath);
        emit documentModified(doc);
        return true;
    }
    // Layered PSD the same way (same bytes as Export Layered PSD). The
    // encoder writes v1 only, so .psb is left out: falling through would
    // mislabel project bytes with a .psd suffix.
    if (low.endsWith(QStringLiteral(".psd"))) {
        auto bytes = exportLayeredPsd(*doc, error);
        if (!bytes) return false;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = tr("Could not write %1.").arg(path);
            return false;
        }
        f.write(reinterpret_cast<const char*>(bytes->data()),
                static_cast<qint64>(bytes->size()));
        doc->filePath = QFileInfo(path).absoluteFilePath();
        doc->dirty = false;
        noteRecentProject(doc->filePath);
        emit documentModified(doc);
        return true;
    }
    if (!saveProjectFile(path, projectDataFromDocument(*doc), error)) return false;
    doc->filePath = QFileInfo(path).absoluteFilePath();
    doc->dirty = false;
    noteRecentProject(doc->filePath);
    emit documentModified(doc);
    return true;
}

QString AppState::activeProjectPath() const {
    const DocumentItem* doc = activeDocument();
    return doc ? doc->filePath : QString();
}

bool AppState::writeRecoverySnapshot(const QString& path, QString* error) {
    DocumentItem* doc = activeDocument();
    if (!doc) {
        if (error) *error = tr("No document open to autosave.");
        return false;
    }
    if (path.trimmed().isEmpty()) {
        if (error) *error = tr("No recovery file given.");
        return false;
    }
    // Deliberately does NOT set doc->filePath / clear dirty / note recent: a
    // recovery snapshot is write-ahead insurance, not a user Save.
    return saveProjectFile(path, projectDataFromDocument(*doc), error);
}

void AppState::noteRecentProject(const QString& path) {
    if (path.isEmpty()) return;
    const QString abs = QFileInfo(path).absoluteFilePath();
    settings_.recentProjects.removeAll(abs);
    settings_.recentProjects.prepend(abs);
    while (settings_.recentProjects.size() > qMax(0, settings_.recentMax))
        settings_.recentProjects.removeLast();
    saveSettings(settings_);
}

bool AppState::saveSession(const QString& file) const {
    QStringList paths;
    for (const DocumentItem* d : documents_) {
        if (!d) continue;
        const QString p = !d->filePath.isEmpty() ? d->filePath : d->importSourcePath;
        if (!p.isEmpty() && QFileInfo::exists(p)) paths << p;
    }
    if (paths.isEmpty()) return false;
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("documents"), QJsonArray::fromStringList(paths));
    root.insert(QStringLiteral("active"), qBound(0, activeDocument_, paths.size() - 1));
    QFile f(file);
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return true;
}

int AppState::restoreSession(const QString& file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != 1) return 0;
    const QJsonArray docs = root.value(QStringLiteral("documents")).toArray();
    int restored = 0;
    for (const QJsonValue& v : docs) {
        const QString path = v.toString();
        if (path.isEmpty() || !QFileInfo::exists(path)) continue;
        QString error;
        const bool ok =
            isNativeProjectSuffix(QFileInfo(path).suffix())
                ? openProject(path, &error)
                : openImageFile(path, &error);
        if (ok)
            ++restored;
        else
            ::pittore::core::log::log_warning(
                "[session] skipped %s: %s", path.toUtf8().constData(),
                error.toUtf8().constData());
    }
    if (restored > 0) {
        const int want = root.value(QStringLiteral("active")).toInt(-1);
        setActiveDocumentIndex(qBound(0, want < 0 ? restored - 1 : want,
                                     static_cast<int>(documents_.size()) - 1));
    }
    return restored;
}

void AppState::setProfileMismatchResolver(
    std::function<ImportProfileChoice(const QString&, const QString&)> fn) {
    mismatchResolver_ = std::move(fn);
}

void AppState::setDeferMismatchDialogs(bool on) { deferMismatch_ = on; }

bool AppState::hasPendingProfileMismatch() const {
    return pendingMismatch_.has_value();
}

bool AppState::resolvePendingProfileMismatch(QString* error) {
    if (!pendingMismatch_) return true;
    PendingMismatch pending = *pendingMismatch_;
    pendingMismatch_.reset();
    if (!documents_.contains(pending.doc)) return true;  // gone meanwhile
    Q_UNUSED(error);  // Cancel leaves *error empty by contract: quiet abort.
    deferMismatch_ = false;  // the question is due: always ask inline now
    auto tag = resolveImportedProfile(pending.embedded);
    if (!tag) {
        const int idx = documents_.indexOf(pending.doc);
        if (idx >= 0) closeDocument(idx);
        return false;
    }
    pending.doc->profile = *tag;
    return true;
}

std::optional<QString> AppState::resolveImportedProfile(
    const QString& embedded, DocumentItem* doc) {
    const QString working = settings_.workingProfile;
    // Diagnostic trail for the mismatch dialog (see also the caller's
    // "[import] embedded profile" line): each decision point logs so a
    // missing dialog can be localized to detection, policy, or UI.
    ::pittore::core::log::log_info(
        "[import] resolve profile embedded='%s' working='%s'",
        embedded.toUtf8().constData(), working.toUtf8().constData());
    if (embedded.trimmed().isEmpty()) {
        ::pittore::core::log::log_info(
            "[import] resolve profile: none embedded, assuming working");
        return working;  // missing: assume working
    }
    const int policy = qBound(0, settings_.colorMismatchPolicy, 3);
    const bool match =
        canonicalProfileName(embedded) == canonicalProfileName(working);
    ::pittore::core::log::log_info(
        "[import] resolve profile: policy=%d match=%d resolver=%s", policy,
        match ? 1 : 0, mismatchResolver_ ? "set" : "unset");
    if (match && policy != 3) return embedded;
    // Deferred: store the question for after first paint
    // instead of prompting mid-open; the doc carries the working tag until
    // the drain re-decides. Silent outcomes still resolve inline.
    if (deferMismatch_ && doc && (policy == 0 || policy == 3) &&
        mismatchResolver_) {
        pendingMismatch_ = PendingMismatch{doc, embedded};
        return working;
    }
    ImportProfileChoice choice = ImportProfileChoice::ConvertToWorking;
    if (policy == 2)
        choice = ImportProfileChoice::UseEmbedded;
    else if ((policy == 0 || policy == 3) && mismatchResolver_)
        choice = mismatchResolver_(embedded, working);
    ::pittore::core::log::log_info("[import] resolve profile: choice=%d",
                                    static_cast<int>(choice));
    switch (choice) {
        case ImportProfileChoice::UseEmbedded:
            setStatusHint(tr("Imported keeping the embedded profile (%1).")
                              .arg(embedded));
            return embedded;
        case ImportProfileChoice::Discard:
            setStatusHint(tr("Imported with the embedded profile discarded."));
            return QString("");
        case ImportProfileChoice::Cancel: return std::nullopt;
        case ImportProfileChoice::ConvertToWorking:
        default:
            setStatusHint(tr("Imported converting to the working space (%1).")
                              .arg(working));
            return working;
    }
}

bool AppState::applyImportedProfile(DocumentItem& doc, const QString& embedded,
                                    QString* error) {
    Q_UNUSED(error);  // Cancel leaves *error empty by contract: quiet abort.
    auto tag = resolveImportedProfile(embedded, &doc);
    if (!tag) return false;
    doc.profile = *tag;
    return true;
}


// Serialize a live document into the on-disk project description (no file IO).
// The single source for createNewProject and saveProject; round-trips exactly
// through loadProjectFile + openProject.
bool psdRawBlocksFresh(const LayerItem& l) {
    if (l.psdRawBlocks.empty()) return false;
    if (l.kind != l.psdRawKind) return false;
    if (l.sourceStamp != l.psdRawSrc) return false;
    if (l.kind == LayerItem::Kind::Adjustment && l.adjustStamp != l.psdRawAdj)
        return false;
    if (l.hasMask != l.psdRawHadMask) return false;
    if (l.hasMask && l.mask && l.maskStamp != l.psdRawMask) return false;
    if (l.offset != l.psdRawOffset || l.scaleX != l.psdRawScaleX ||
        l.scaleY != l.psdRawScaleY)
        return false;
    return true;
}

}  // namespace pittore::ui
