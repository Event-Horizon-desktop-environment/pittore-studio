#include "ui/settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <fstream>

#ifdef __linux__
#include <sys/sysinfo.h>
#endif

#include <toml++/toml.hpp>

namespace pittore::ui {
QString canonicalProfileName(const QString& name) {
    QString norm;
    norm.reserve(name.size());
    for (const QChar ch : name.toLower()) {
        if (ch.isLetterOrNumber()) norm.append(ch);
    }
    if (norm.startsWith(QStringLiteral("srgb"))) return QStringLiteral("srgb");
    if (norm.startsWith(QStringLiteral("adobergb"))) return QStringLiteral("adobergb");
    if (norm.startsWith(QStringLiteral("displayp3")) || norm.startsWith(QStringLiteral("p3d65")))
        return QStringLiteral("displayp3");
    if (norm.startsWith(QStringLiteral("prophotorgb")) || norm.startsWith(QStringLiteral("rommrgb")))
        return QStringLiteral("prophoto");
    return norm;
}

namespace {

QString themeName(UiTheme t) {
    switch (t) {
        case UiTheme::Black: return QStringLiteral("Black");
        case UiTheme::DarkGray: return QStringLiteral("Dark Gray");
        case UiTheme::MediumGray: return QStringLiteral("Medium Gray");
        case UiTheme::LightGray: return QStringLiteral("Light Gray");
    }
    return QStringLiteral("Dark Gray");
}

UiTheme themeFromName(const std::string& name) {
    if (name == "Medium Gray") return UiTheme::MediumGray;
    if (name == "Light Gray") return UiTheme::LightGray;
    if (name == "Black") return UiTheme::Black;
    return UiTheme::DarkGray;
}

std::string toStd(const QString& s) { return s.toStdString(); }

}  // namespace

QString settingsPath() {
    QDir base(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation));
    base.mkpath(QStringLiteral("PittoreStudio"));
    const QString fresh = base.filePath(QStringLiteral("PittoreStudio/Settings.toml"));
    if (!QFileInfo::exists(fresh)) {
        const QString legacy = base.filePath(QStringLiteral("InfinityPhoto/Settings.toml"));
        if (QFileInfo::exists(legacy)) {
            QDir().mkpath(QFileInfo(fresh).absolutePath());
            QFile::copy(legacy, fresh);
        }
    }
    return fresh;
}

int totalSystemRamMb() {
#ifdef __linux__
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        const unsigned long long total =
            static_cast<unsigned long long>(info.totalram) * info.mem_unit;
        return static_cast<int>(total / (1024 * 1024));
    }
#endif
    return 0;
}

int defaultRamLimitMb() {
    const int total = totalSystemRamMb();
    if (total <= 0) return 0;
    const long long ninety = static_cast<long long>(total) * 9 / 10;
    return ninety < 1 ? 0 : static_cast<int>(ninety);
}

bool loadSettings(AppSettings& out) {
    const QString path = settingsPath();
    try {
        const toml::table tbl = toml::parse_file(toStd(path));

        if (const toml::node* theme = tbl.get("theme"))
            out.theme = themeFromName(theme->value_or<std::string>(""));

        if (const toml::node* computeNode = tbl.get("compute")) {
            const toml::table* compute = computeNode->as_table();
            if (compute) {
                if (const toml::node* gpu = compute->get("gpu_enabled"))
                    out.gpuEnabled = gpu->value_or(false);
                if (const toml::node* dev = compute->get("gpu_device"))
                    out.gpuDevice = QString::fromStdString(dev->value_or<std::string>(""));
                if (const toml::node* dev = compute->get("cpu_device"))
                    out.cpuDevice = QString::fromStdString(dev->value_or<std::string>(""));
                if (const toml::node* mb = compute->get("ram_limit_mb"))
                    out.ramLimitMb = mb->value_or<int>(0);
            }
        }

        if (const toml::node* aiNode = tbl.get("ai")) {
            const toml::table* ai = aiNode->as_table();
            if (ai) {
                if (const toml::node* model = ai->get("background_model")) {
                    const std::string id = model->value_or<std::string>("");
                    if (!id.empty()) out.bgModel = QString::fromStdString(id);
                }
                if (const toml::node* enh = ai->get("enhance_model")) {
                    const std::string id = enh->value_or<std::string>("");
                    if (!id.empty()) out.enhanceModel = QString::fromStdString(id);
                }
                if (const toml::node* ref = ai->get("reference_mask"))
                    out.referenceMask =
                        QString::fromStdString(ref->value_or<std::string>(""));
            }
        }

        if (const toml::node* autoNode = tbl.get("autosave")) {
            const toml::table* autosave = autoNode->as_table();
            if (autosave) {
                if (const toml::node* on = autosave->get("enabled"))
                    out.autosaveEnabled = on->value_or(true);
                if (const toml::node* mins = autosave->get("interval_minutes"))
                    out.autosaveIntervalMinutes = mins->value_or<int>(5);
            }
        }

        if (const toml::node* sessionNode = tbl.get("session")) {
            const toml::table* session = sessionNode->as_table();
            if (session) {
                if (const toml::node* r = session->get("reopen_documents"))
                    out.reopenDocuments = r->value_or(false);
            }
        }

        if (const toml::node* viewNode = tbl.get("view")) {
            const toml::table* view = viewNode->as_table();
            if (view) {
                if (const toml::node* z = view->get("zoom_with_scroll"))
                    out.zoomWithScroll = z->value_or(false);
                if (const toml::node* g = view->get("grid_spacing"))
                    out.gridSpacing = g->value_or<double>(64.0);
                if (const toml::node* s = view->get("snap_enabled"))
                    out.snapEnabled = s->value_or(true);
                if (const toml::node* st = view->get("snap_targets"))
                    out.snapTargets = std::clamp(st->value_or(31), 0, 31);
                if (const toml::node* t = view->get("tablet_mode"))
                    out.tabletMode = t->value_or(false);
                if (const toml::node* he = view->get("hidpi_enabled"))
                    out.hidpiEnabled = he->value_or(true);
                if (const toml::node* hf = view->get("hidpi_fractional"))
                    out.hidpiFractional = hf->value_or(true);
                if (const toml::node* f = view->get("font_preview_size"))
                    out.fontPreviewSize = std::clamp(f->value_or(2), 0, 3);
                if (const toml::node* c = view->get("cursor_shape"))
                    out.cursorShape = std::clamp(c->value_or(0), 0, 2);
                if (const toml::node* o = view->get("outline_shape"))
                    out.outlineShape = std::clamp(o->value_or(2), 0, 3);
                if (const toml::node* w = view->get("show_outline_while_painting"))
                    out.showOutlineWhilePainting = w->value_or(true);
                if (const toml::node* e = view->get("outline_effective_size"))
                    out.outlineEffectiveSize = e->value_or(false);
                if (const toml::node* r = view->get("show_rulers"))
                    out.showRulers = r->value_or(true);
                if (const toml::node* g = view->get("show_guides"))
                    out.showGuides = g->value_or(true);
                if (const toml::node* gv = view->get("show_grid"))
                    out.showGrid = gv->value_or(false);
                if (const toml::node* se = view->get("show_selection_edges"))
                    out.showSelectionEdges = se->value_or(true);
                if (const toml::node* sg = view->get("show_smart_guides"))
                    out.showSmartGuides = sg->value_or(true);
                if (const toml::node* pg = view->get("show_pixel_grid"))
                    out.showPixelGrid = pg->value_or(false);
                if (const toml::node* ex = view->get("show_extras"))
                    out.showExtras = ex->value_or(true);
                if (const toml::node* n = view->get("nudge_step_px"))
                    out.nudgeStepPx = std::clamp(n->value_or(1.0), 0.1, 1000.0);
                if (const toml::node* ns = view->get("nudge_shift_step_px"))
                    out.nudgeShiftStepPx = std::clamp(ns->value_or(10.0), 0.1, 10000.0);
                if (const toml::node* sd = view->get("grid_subdivisions"))
                    out.gridSubdivisions = std::clamp(sd->value_or(4), 1, 16);
                if (const toml::node* sn = view->get("show_slice_numbers"))
                    out.showSliceNumbers = sn->value_or(true);
                auto readColor = [](const toml::table* view, const char* key,
                                    const QColor& fallback) {
                    if (const toml::node* c = view->get(key)) {
                        const QColor parsed(QString::fromStdString(
                            c->value_or<std::string>("")));
                        if (parsed.isValid()) return parsed;
                    }
                    return fallback;
                };
                out.guideColor = readColor(view, "guide_color", out.guideColor);
                out.gridColor = readColor(view, "grid_color", out.gridColor);
                out.transparencyLight =
                    readColor(view, "transparency_light", out.transparencyLight);
                out.transparencyDark =
                    readColor(view, "transparency_dark", out.transparencyDark);
                if (const toml::node* tc = view->get("transparency_cell_px"))
                    out.transparencyCellPx = std::clamp(tc->value_or(8), 4, 64);
            }
        }

        if (const toml::node* recents = tbl.get("recent_projects")) {            if (const toml::array* arr = recents->as_array()) {
                out.recentProjects.clear();
                for (const auto& item : *arr) {
                    if (const auto s = item.value<std::string>())
                        out.recentProjects.append(QString::fromStdString(*s));
                }
            }
        }

        if (const toml::node* docNode = tbl.get("documents")) {
            const toml::table* doc = docNode->as_table();
            if (doc) {
                if (const toml::node* w = doc->get("default_width"))
                    out.newDocWidth = std::clamp(w->value_or(1920), 1, 300000);
                if (const toml::node* h = doc->get("default_height"))
                    out.newDocHeight = std::clamp(h->value_or(1080), 1, 300000);
                if (const toml::node* d = doc->get("default_dpi"))
                    out.newDocDpi = std::clamp(d->value_or(300), 1, 10000);
                if (const toml::node* m = doc->get("default_color_mode")) {
                    const std::string id = m->value_or<std::string>("");
                    if (!id.empty())
                        out.newDocColorMode = QString::fromStdString(id);
                }
                if (const toml::node* b = doc->get("default_background")) {
                    const std::string id = b->value_or<std::string>("");
                    if (!id.empty())
                        out.newDocBackground = QString::fromStdString(id);
                }
                if (const toml::node* r = doc->get("recent_max"))
                    out.recentMax = std::clamp(r->value_or(12), 0, 100);
                if (const toml::node* w = doc->get("working_profile")) {
                    const std::string id = w->value_or<std::string>("");
                    if (!id.empty())
                        out.workingProfile = QString::fromStdString(id);
                }
                if (const toml::node* m = doc->get("mismatch_policy"))
                    out.colorMismatchPolicy = std::clamp(m->value_or(0), 0, 3);
            }
        }

        if (const toml::node* expNode = tbl.get("export")) {
            const toml::table* exp = expNode->as_table();
            if (exp) {
                if (const toml::node* f = exp->get("default_format")) {
                    const std::string id = f->value_or<std::string>("");
                    if (!id.empty())
                        out.exportFormat = QString::fromStdString(id);
                }
                if (const toml::node* q = exp->get("default_quality"))
                    out.exportQuality = std::clamp(q->value_or(92), 1, 100);
                if (const toml::node* l = exp->get("default_location"))
                    out.exportLocation = std::clamp(l->value_or(0), 0, 2);
                if (const toml::node* e = exp->get("embed_icc"))
                    out.exportEmbedIcc = e->value_or(true);
                if (const toml::node* p = exp->get("psd_compression"))
                    out.psdCompression = std::clamp(p->value_or(0), 0, 1);
            }
        }

        if (const toml::node* histNode = tbl.get("history")) {
            const toml::table* hist = histNode->as_table();
            if (hist) {
                if (const toml::node* u = hist->get("undo_limit"))
                    out.undoLimit = std::clamp(u->value_or(100), 1, 1000);
            }
        }

        if (const toml::node* proofNode = tbl.get("proof")) {
            const toml::table* proof = proofNode->as_table();
            if (proof) {
                if (const toml::node* p = proof->get("profile"))
                    out.proofProfile =
                        QString::fromStdString(p->value_or<std::string>(""));
                if (const toml::node* i = proof->get("intent"))
                    out.proofIntent = std::clamp(i->value_or(1), 0, 3);
                if (const toml::node* b = proof->get("bpc"))
                    out.proofBpc = b->value_or(true);
            }
        }

        if (const toml::node* cmykNode = tbl.get("cmyk")) {
            const toml::table* cmyk = cmykNode->as_table();
            if (cmyk) {
                if (const toml::node* p = cmyk->get("profile"))
                    out.cmykProfile =
                        QString::fromStdString(p->value_or<std::string>(""));
            }
        }
        return true;
    } catch (const toml::parse_error&) {
        return false;
    }
}

bool saveSettings(const AppSettings& in) {
    const QString path = settingsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    toml::table compute;
    compute.insert("gpu_enabled", in.gpuEnabled);
    compute.insert("gpu_device", toStd(in.gpuDevice));
    compute.insert("cpu_device", toStd(in.cpuDevice));
    compute.insert("ram_limit_mb", in.ramLimitMb);

    toml::table ai;
    ai.insert("background_model", toStd(in.bgModel));
    ai.insert("enhance_model", toStd(in.enhanceModel));
    ai.insert("reference_mask", toStd(in.referenceMask));

    toml::table autosave;
    autosave.insert("enabled", in.autosaveEnabled);
    autosave.insert("interval_minutes", in.autosaveIntervalMinutes);

    toml::table session;
    session.insert("reopen_documents", in.reopenDocuments);

    toml::table view;
    view.insert("zoom_with_scroll", in.zoomWithScroll);
    view.insert("grid_spacing", in.gridSpacing);
    view.insert("snap_enabled", in.snapEnabled);
    view.insert("snap_targets", in.snapTargets);
    view.insert("tablet_mode", in.tabletMode);
    view.insert("hidpi_enabled", in.hidpiEnabled);
    view.insert("hidpi_fractional", in.hidpiFractional);
    view.insert("font_preview_size", in.fontPreviewSize);
    view.insert("cursor_shape", in.cursorShape);
    view.insert("outline_shape", in.outlineShape);
    view.insert("show_outline_while_painting", in.showOutlineWhilePainting);
    view.insert("outline_effective_size", in.outlineEffectiveSize);
    view.insert("show_rulers", in.showRulers);
    view.insert("show_guides", in.showGuides);
    view.insert("show_grid", in.showGrid);
    view.insert("show_selection_edges", in.showSelectionEdges);
    view.insert("show_smart_guides", in.showSmartGuides);
    view.insert("show_pixel_grid", in.showPixelGrid);
    view.insert("show_extras", in.showExtras);
    view.insert("nudge_step_px", in.nudgeStepPx);
    view.insert("nudge_shift_step_px", in.nudgeShiftStepPx);
    view.insert("grid_subdivisions", in.gridSubdivisions);
    view.insert("show_slice_numbers", in.showSliceNumbers);
    view.insert("guide_color", toStd(in.guideColor.name(QColor::HexArgb)));
    view.insert("grid_color", toStd(in.gridColor.name(QColor::HexArgb)));
    view.insert("transparency_cell_px", in.transparencyCellPx);
    view.insert("transparency_light",
                toStd(in.transparencyLight.name(QColor::HexArgb)));
    view.insert("transparency_dark",
                toStd(in.transparencyDark.name(QColor::HexArgb)));

    toml::table root;
    root.insert("theme", toStd(themeName(in.theme)));
    root.insert("compute", std::move(compute));
    root.insert("ai", std::move(ai));
    root.insert("autosave", std::move(autosave));
    root.insert("session", std::move(session));
    root.insert("view", std::move(view));

    toml::table documents;
    documents.insert("default_width", in.newDocWidth);
    documents.insert("default_height", in.newDocHeight);
    documents.insert("default_dpi", in.newDocDpi);
    documents.insert("default_color_mode", toStd(in.newDocColorMode));
    documents.insert("default_background", toStd(in.newDocBackground));
    documents.insert("recent_max", in.recentMax);
    documents.insert("working_profile", toStd(in.workingProfile));
    documents.insert("mismatch_policy", in.colorMismatchPolicy);
    root.insert("documents", std::move(documents));

    toml::table exp;
    exp.insert("default_format", toStd(in.exportFormat));
    exp.insert("default_quality", in.exportQuality);
    exp.insert("default_location", in.exportLocation);
    exp.insert("embed_icc", in.exportEmbedIcc);
    exp.insert("psd_compression", in.psdCompression);
    root.insert("export", std::move(exp));

    toml::table history;
    history.insert("undo_limit", in.undoLimit);
    root.insert("history", std::move(history));

    toml::table proof;
    proof.insert("profile", toStd(in.proofProfile));
    proof.insert("intent", in.proofIntent);
    proof.insert("bpc", in.proofBpc);
    root.insert("proof", std::move(proof));

    toml::table cmyk;
    cmyk.insert("profile", toStd(in.cmykProfile));
    root.insert("cmyk", std::move(cmyk));

    // Recents as a plain TOML array.
    toml::array recents;
    for (const QString& dir : in.recentProjects) recents.push_back(toStd(dir));
    root.insert("recent_projects", std::move(recents));

    std::ofstream out(path.toStdString(), std::ios::trunc);
    if (!out) return false;
    out << root;
    return out.good();
}

}  // namespace pittore::ui