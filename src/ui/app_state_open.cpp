// Opening documents: the native project container and layered PSD/PSB.
// Split out of app_state.cpp.

#include "ui/app_state.h"
#include "ui/app_state_detail.h"
#include "ui/color_mode.h"
#include "ui/embedded_icc.h"
#include "ui/project_manager.h"
#include "ui/svg_parts.h"

#include "engine/core/log.h"
#include "engine/core/parallel.h"
#include "engine/compute/layer_mask.h"
#include "engine/io/af.h"
#include "engine/io/af_layers.h"
#include "engine/io/icc.h"
#include "engine/io/psd.h"
#ifdef PITTORE_TIFF
#include "engine/io/tiff.h"
#endif

#include <QByteArray>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace pittore::ui {

bool AppState::openProject(const QString& path, QString* error) {
    ProjectFileData data;
    if (!loadProjectFile(path, &data, error)) {
        ::pittore::core::log::log_warning(
            "[import] project load failed %s: %s",
            QFileInfo(path).fileName().toUtf8().constData(),
            error && !error->isEmpty() ? error->toUtf8().constData() : "unknown");
        return false;
    }
    if (data.name.isEmpty()) data.name = QFileInfo(path).completeBaseName();
    if (data.size.isEmpty()) data.size = QSize(1920, 1080);

    DocumentItem* doc = addDocument(data.name, data.size, data.dpi);
    if (!doc) {
        if (error)
            *error = tr("Could not create the document (check the RAM limit in Preferences).");
        return false;
    }
    doc->colorMode = data.colorMode;
    doc->iccProfile = data.iccProfile;
    doc->filePath = QFileInfo(path).absoluteFilePath();
    doc->dirty = false;

    // Rebuild the layer stack from the persisted metadata (index 0 = top, in
    // file order). Non-pixel layers keep no pixels; pixel layers load
    // their 16-bit straight-alpha PNGs as native straight engine Images, and
    // adjustment layers restore their parameters (LUTs re-derived).
    QVector<LayerItem> layers;
    for (const ProjectLayerMeta& m : data.layers) {
        LayerItem li;
        li.name = m.name;
        li.kind = static_cast<LayerItem::Kind>(m.kind);
        li.visible = m.visible;
        li.locked = m.locked;
        li.opacity = m.opacity;
        li.fill = m.fill;
        li.blendMode = m.blendMode;
        li.offset = m.offset;
        li.scaleX = m.scaleX;
        li.scaleY = m.scaleY;
        li.lockTransparency = m.lockTransparency;
        li.clipped = m.clipped;
        li.indent = m.indent;
        if (m.hasMask && !m.mask.isNull()) {
            li.mask = straightRgba64ToImage(m.mask);
            if (li.mask) {
                li.hasMask = true;
                li.maskEnabled = m.maskEnabled;
                li.maskOffset = m.maskOffset;
                li.maskScaleX = m.maskScaleX;
                li.maskScaleY = m.maskScaleY;
                li.maskDensity = m.maskDensity;
                li.maskFeather = m.maskFeather;
                li.maskLinked = true;
                li.maskSelected = false;
                li.maskStamp = 1;
            }
        }
        li.isText = m.isText || m.liveText;
        li.liveText = m.liveText;
        li.art = m.art;
        if (m.hasAdjustment) {
            li.adjustmentKind = m.adjustmentKind;
            for (int k = 0; k < 16; ++k)
                li.adjustmentParams[k] = m.adjustmentParams[k];
            li.adjustmentCurve = m.adjustmentCurve;
            li.adjustmentCurveR = m.adjustmentCurveR;
            li.adjustmentCurveG = m.adjustmentCurveG;
            li.adjustmentCurveB = m.adjustmentCurveB;
            li.adjustmentType = adjustmentKindName(li.adjustmentKind);
            rebuildAdjustmentLUT(li);
        }
        if (m.hasToneBlend && li.kind == LayerItem::Kind::Group) {
            li.toneBlendGroup = true;
            li.toneBlend.strength =
                std::clamp(m.toneBlendStrength, 0.0f, 1.0f);
            li.toneBlend.color =
                std::clamp(m.toneBlendColor, 0.0f, 1.0f);
            li.toneBlend.contrast =
                std::clamp(m.toneBlendContrast, -1.0f, 1.0f);
            li.toneBlend.lowPass =
                std::clamp(m.toneBlendLowPass, 0.0f, 1.0f);
            li.toneBlend.contentType =
                qBound(0, m.toneBlendContentType, 2);
        }
        if (m.hasLiveFilter && li.kind == LayerItem::Kind::Pixel) {
            li.hasLiveFilter = true;
            li.liveFilterEnabled = m.liveFilterEnabled;
            li.liveFilterId = m.liveFilterId;
            li.liveFilterParams.clear();
            for (double v : m.liveFilterParams)
                li.liveFilterParams.push_back(v);
            // Cache re-derives lazily; the revision bump forces a device
            // re-upload even if a same-pointer image lingers in caches.
            ++li.styledRev;
        }
        if (m.liveText) {
            li.textSpec.text = m.text;
            li.textSpec.family = m.textFamily;
            li.textSpec.bold = m.textBold;
            li.textSpec.italic = m.textItalic;
            li.textSpec.align = qBound(0, m.textAlign, 2);
            li.textSpec.size = m.textSize;
            li.textSpec.lineHeight = m.textLineHeight;
            li.textSpec.tracking = m.textTracking;
            li.textSpec.wrapWidth = m.textWrapWidth;
            li.textSpec.frameHeight = m.textFrameHeight;
            li.textSpec.origin = m.textOrigin;
            li.textSpec.color = m.textColor;
            li.textSpec.underline = m.textUnderline;
            li.textSpec.strike = m.textStrike;
            li.textSpec.underlineColor = m.textUnderlineColor;
            li.textSpec.strikeColor = m.textStrikeColor;
            li.textSpec.backgroundColor = m.textBackgroundColor;
            li.textSpec.baselineShift = m.textBaselineShift;
            li.textSpec.hScale = m.textHScale;
            li.textSpec.vScale = m.textVScale;
            li.textSpec.superSub = m.textSuperSub;
            li.textSpec.allCaps = m.textAllCaps;
            li.textSpec.kerning = m.textKerning;
            li.textSpec.otFeatures = m.textOtFeatures;
            // Re-render from the spec so the pixels and the caret metrics match
            // exactly what the saved layer showed (and a font substitution is
            // picked up); fall back to the persisted pixels when no font is
            // available.
            renderLiveText(li);
            if (li.pixels) {
                // The saved offset is the on-screen truth: the session paints
                // from the live offset, while the spec origin can go stale
                // (then the layer looks fine until reload re-renders it
                // off-canvas). Re-anchor a stray origin from the saved
                // placement and render once more — same bounds, healed spec.
                const QPointF err = m.offset - li.offset;
                if (err.manhattanLength() > 2.0) {
                    li.textSpec.origin += err;
                    renderLiveText(li);
                }
            }
        }
        if (!m.pixels.isNull() && !li.pixels) {
            li.pixels = straightRgba64ToImage(m.pixels);
            ++li.sourceStamp;   // fresh native pixels → stale device cache
        }
        if (li.pixels && m.cmyk.size() == li.pixels->pixel_count() * 4) {
            li.cmykPixels =
                std::make_shared<pittore::color::CmykImage>();
            li.cmykPixels->width = li.pixels->width();
            li.cmykPixels->height = li.pixels->height();
            li.cmykPixels->pixels.resize(li.pixels->pixel_count());
            for (std::size_t i = 0; i < li.cmykPixels->pixels.size(); ++i)
                std::copy_n(m.cmyk.data() + i * 4, 4,
                            li.cmykPixels->pixels[i].data());
            li.cmykSourceStamp = li.sourceStamp;
            li.cmykPixels->appearanceFingerprint =
                pittore::color::CmykImage::fingerprint(*li.pixels);
            li.cmykPixels->profileFingerprint =
                pittore::color::CmykImage::fingerprint(
                    reinterpret_cast<const std::uint8_t*>(doc->iccProfile.constData()),
                    static_cast<std::size_t>(doc->iccProfile.size()));
        }
        // Re-arm verbatim PSD blocks: only fresh blocks were ever saved,
        // so the just-rebuilt counters are the new guard baseline.
        li.psdRawBlocks.reserve(static_cast<int>(m.psdRawBlocks.size()));
        for (const auto& rb : m.psdRawBlocks) {
            LayerItem::PsdRawBlock o;
            std::memcpy(o.sig, rb.sig, 4);
            std::memcpy(o.key, rb.key, 4);
            o.data = rb.data;
            o.padding = rb.padding;
            li.psdRawBlocks.push_back(std::move(o));
        }
        li.psdRawSrc = li.sourceStamp;
        li.psdRawAdj = li.adjustStamp;
        li.psdRawMask = li.maskStamp;
        li.psdRawHadMask = li.hasMask;
        li.psdRawKind = li.kind;
        li.psdRawOffset = li.offset;
        li.psdRawScaleX = li.scaleX;
        li.psdRawScaleY = li.scaleY;
        layers.append(li);
    }
    if (!layers.isEmpty()) {
        doc->layers = std::move(layers);
        doc->selectedLayers.clear();
        doc->activeLayer = 0;   // top layer active, as the Layers panel shows
    }
    // The eraser restores the canvas bottom, so record what was saved
    // (old files predate the field; the saver derives the same label).
    if (data.background == QStringLiteral("transparent")) {
        doc->canvasTransparent = true;
    } else if (data.background == QStringLiteral("black")) {
        doc->canvasPaper = QColor(0, 0, 0);
    } else {
        doc->canvasPaper = QColor(255, 255, 255);
    }
    doc->rebuildComposite();
    emit layersChanged();
    emit documentModified(doc);
    noteRecentProject(doc->filePath);
    ::pittore::core::log::log_info(
        "[import] project loaded %s: %dx%d dpi=%d layers=%d",
        QFileInfo(path).fileName().toUtf8().constData(), data.size.width(),
        data.size.height(), data.dpi, data.layers.size());
    return true;
}

// Convert a PSD user-mask channel into the UI's opaque-grey layer-native
// mask image of `width`×`height`, where output pixel (x, y) sits at document
// (orgX + x, orgY + y). Relative mask rects are measured from the record
// origin (`recX`, `recY`); for pixel layers both origins are the layer
// origin, for adjustment layers the output grid is the whole document.
// Samples outside the stored mask bounds reveal, matching the codec's
// flattening behaviour.
std::shared_ptr<pittore::Image> psdMaskToImage(
    const pittore::io::PsdLayerFile& l, std::uint32_t width,
    std::uint32_t height, std::uint32_t orgX, std::uint32_t orgY,
    std::uint32_t recX, std::uint32_t recY) {
    if (!l.hasMask || l.mask.empty() || l.maskWidth == 0 ||
        l.maskHeight == 0 || width == 0 || height == 0)
        return nullptr;
    if (l.mask.size() !=
        static_cast<std::size_t>(l.maskWidth) * l.maskHeight)
        return nullptr;
    auto mask = std::make_shared<pittore::Image>(width, height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint64_t docY = static_cast<std::uint64_t>(orgY) + y;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint64_t docX = static_cast<std::uint64_t>(orgX) + x;
            std::int64_t mx, my;
            if (l.maskRelative) {
                mx = static_cast<std::int64_t>(docX) -
                     static_cast<std::int64_t>(recX) + l.maskLeft;
                my = static_cast<std::int64_t>(docY) -
                     static_cast<std::int64_t>(recY) + l.maskTop;
            } else {
                mx = static_cast<std::int64_t>(docX) - l.maskLeft;
                my = static_cast<std::int64_t>(docY) - l.maskTop;
            }
            float coverage = 1.0f;
            if (mx >= 0 && my >= 0 &&
                static_cast<std::uint64_t>(mx) < l.maskWidth &&
                static_cast<std::uint64_t>(my) < l.maskHeight) {
                coverage =
                    static_cast<float>(
                        l.mask[static_cast<std::size_t>(my) * l.maskWidth +
                               static_cast<std::size_t>(mx)]) /
                    65535.0f;
            }
            mask->at(x, y) = pittore::compute::make_mask_pixel(coverage);
        }
    }
    return mask;
}

// Builds a DocumentItem from a decoded layered PSD/PSB. `layers` is file order
// (bottom → top); the panel wants top-first with group folder rows above their
// children, so a small nesting tree (derived from `indent` and the live groups)
// is emitted in reverse. Groups don't composite themselves — their opacity and
// visibility are folded into the child layers so the render is correct.
bool AppState::openPsdLayers(const QString& path, const pittore::io::PsdLayersDoc& pd,
                             QString* error, const QString& embeddedProfile) {
    const int dw = static_cast<int>(pd.width);
    const int dh = static_cast<int>(pd.height);
    if (dw <= 0 || dh <= 0) {
        if (error) *error = tr("PSD layer section has an empty canvas.");
        return false;
    }
    const QString title = QFileInfo(path).completeBaseName();
    DocumentItem* doc = addDocument(title, QSize(dw, dh), 72);
    if (!doc) {
        if (error)
            *error = tr("Could not create the document (check the RAM limit in Preferences).");
        return false;
    }
    // Tag from the file's real mode: CMYK keeps its separation bytes for
    // re-export, grayscale is luma-tagged, and Lab/RGB import as the
    // appearance RGB they were decoded into. Depth suffix rides along.
    const bool wide = pd.depth >= 16;
    if (pd.colorMode == 4) {
        doc->colorMode = wide ? QStringLiteral("CMYK/16") : QStringLiteral("CMYK/8");
        if (!pd.icc.empty())
            doc->iccProfile =
                QByteArray(reinterpret_cast<const char*>(pd.icc.data()),
                           static_cast<qsizetype>(pd.icc.size()));
    } else if (pd.colorMode == 1) {
        doc->colorMode =
            wide ? QStringLiteral("Grayscale/16") : QStringLiteral("Grayscale/8");
    } else {
        doc->colorMode = wide ? QStringLiteral("RGB/16") : QStringLiteral("RGB/8");
    }
    doc->importSourcePath = QFileInfo(path).absoluteFilePath();

    // Nesting tree from the flat layer list. A layer's `indent` is the group
    // depth it lives at; a group opens a container for children at indent+1,
    // and the container closes once the next layer's indent is no deeper.
    struct Node {
        LayerItem item;
        QVector<int> children;  // indices into `nodes`, file order
    };
    QVector<Node> nodes;
    QVector<int> roots;   // top-level layer indices, file order
    QVector<int> open;    // open group node indices, stack of depth
    for (const pittore::io::PsdLayerFile& l : pd.layers) {
        while (!open.isEmpty() && nodes[open.back()].item.indent >= l.indent)
            open.pop_back();
        LayerItem li;
        li.name = QString::fromStdString(l.name);
        if (li.name.isEmpty()) li.name = l.isGroup ? tr("Group") : tr("Layer");
        li.kind = l.isGroup ? LayerItem::Kind::Group
                  : (l.isAdjustment ? LayerItem::Kind::Adjustment
                                    : LayerItem::Kind::Pixel);
        li.visible = l.visible;
        li.opacity = qBound(1, qRound(l.opacity * 100.0 / 255.0), 100);
        li.blendMode = QString::fromStdString(l.blend);
        if (li.blendMode.isEmpty()) li.blendMode = QStringLiteral("Normal");
        li.indent = l.indent;
        li.groupExpanded = l.groupExpanded;
        li.clipped = !l.isGroup && l.clipped;
        if (l.isAdjustment) {
            // Live adjustment: params + control points straight across, LUT
            // re-derived. A doc-sized mask keeps import placement trivial.
            li.adjustmentKind = l.adjustmentKind;
            for (int k = 0; k < 16; ++k)
                li.adjustmentParams[k] = l.adjustmentParams[k];
            li.adjustmentCurve.reserve(
                static_cast<int>(l.adjustmentCurve.size()));
            for (const auto& pt : l.adjustmentCurve)
                li.adjustmentCurve.append(QPointF(pt.first, pt.second));
            for (const auto& pt : l.adjustmentCurveR)
                li.adjustmentCurveR.append(QPointF(pt.first, pt.second));
            for (const auto& pt : l.adjustmentCurveG)
                li.adjustmentCurveG.append(QPointF(pt.first, pt.second));
            for (const auto& pt : l.adjustmentCurveB)
                li.adjustmentCurveB.append(QPointF(pt.first, pt.second));
            li.adjustmentType = adjustmentKindName(li.adjustmentKind);
            rebuildAdjustmentLUT(li);
            auto mask = psdMaskToImage(l, static_cast<std::uint32_t>(dw),
                                       static_cast<std::uint32_t>(dh), 0, 0,
                                       l.left, l.top);
            if (mask) {
                li.mask = std::move(mask);
                li.hasMask = true;
                li.maskEnabled = l.maskEnabled;
                li.maskLinked = true;
                li.maskSelected = false;
                li.maskOffset = QPointF(0, 0);
                li.maskScaleX = li.maskScaleY = 1.0;
                li.maskStamp = 1;
            }
            // Verbatim foreign blocks ride along with their guard stamps;
            // the PSD exporter re-emits them only while untouched.
            li.psdRawBlocks.reserve(l.rawBlocks.size());
            for (const auto& rb : l.rawBlocks) {
                LayerItem::PsdRawBlock o;
                std::memcpy(o.sig, rb.sig, 4);
                std::memcpy(o.key, rb.key, 4);
                o.data = rb.data;
                o.padding = rb.padding;
                li.psdRawBlocks.push_back(std::move(o));
            }
            li.psdRawSrc = li.sourceStamp;
            li.psdRawAdj = li.adjustStamp;
            li.psdRawMask = li.maskStamp;
            li.psdRawHadMask = li.hasMask;
            li.psdRawKind = li.kind;
            li.psdRawOffset = li.offset;
            li.psdRawScaleX = li.scaleX;
            li.psdRawScaleY = li.scaleY;
            li.psdChannelMethod = l.channelMethod;
        } else if (!l.isGroup) {
            li.offset = QPointF(l.left, l.top);
            li.scaleX = 1.0;
            li.scaleY = 1.0;
            if (!l.rgba.empty() && l.width > 0 && l.height > 0) {
                const QImage q = qimageFromRgba16(l.rgba, static_cast<int>(l.width),
                                                   static_cast<int>(l.height));
                li.pixels = straightRgba64ToImage(q);
                if (li.pixels) ++li.sourceStamp;  // fresh native pixels
                if (li.pixels && l.cmyk.size() ==
                                     static_cast<std::size_t>(l.width) *
                                         l.height * 4) {
                    li.cmykPixels =
                        std::make_shared<pittore::color::CmykImage>();
                    li.cmykPixels->width = l.width;
                    li.cmykPixels->height = l.height;
                    li.cmykPixels->pixels.resize(
                        static_cast<std::size_t>(l.width) * l.height);
                    for (std::size_t i = 0;
                         i < li.cmykPixels->pixels.size(); ++i)
                        std::copy_n(l.cmyk.data() + i * 4, 4,
                                    li.cmykPixels->pixels[i].data());
                    li.cmykSourceStamp = li.sourceStamp;
                    li.cmykPixels->appearanceFingerprint =
                        pittore::color::CmykImage::fingerprint(*li.pixels);
                    li.cmykPixels->profileFingerprint =
                        pittore::color::CmykImage::fingerprint(
                            pd.icc.data(), pd.icc.size());
                }
                auto mask = psdMaskToImage(l, l.width, l.height, l.left,
                                           l.top, l.left, l.top);
                if (mask && li.pixels) {
                    li.mask = std::move(mask);
                    li.hasMask = true;
                    li.maskEnabled = l.maskEnabled;
                    li.maskLinked = true;
                    li.maskSelected = false;
                    li.maskOffset = li.offset;
                    li.maskScaleX = li.maskScaleY = 1.0;
                    li.maskStamp = 1;
                }
            }
            li.psdRawBlocks.reserve(l.rawBlocks.size());
            for (const auto& rb : l.rawBlocks) {
                LayerItem::PsdRawBlock o;
                std::memcpy(o.sig, rb.sig, 4);
                std::memcpy(o.key, rb.key, 4);
                o.data = rb.data;
                o.padding = rb.padding;
                li.psdRawBlocks.push_back(std::move(o));
            }
            li.psdRawSrc = li.sourceStamp;
            li.psdRawAdj = li.adjustStamp;
            li.psdRawMask = li.maskStamp;
            li.psdRawHadMask = li.hasMask;
            li.psdRawKind = li.kind;
            li.psdRawOffset = li.offset;
            li.psdRawScaleX = li.scaleX;
            li.psdRawScaleY = li.scaleY;
            li.psdChannelMethod = l.channelMethod;
        }
        const int idx = nodes.size();
        nodes.append(Node{std::move(li), {}});
        if (!open.isEmpty()) nodes[open.back()].children.append(idx);
        else roots.append(idx);
        if (l.isGroup) open.append(idx);
    }

    // Panel emission: reverse of paint order, group header row above children.
    // Group opacity/visibility propagate downward.
    QVector<LayerItem> panel;
    std::function<void(int, double, bool)> emitNode =
        [&](int idx, double inheritedOpacity, bool inheritedHidden) {
            const Node& n = nodes[idx];
            const LayerItem& src = n.item;
            if (src.kind == LayerItem::Kind::Group) {
                panel.append(src);
                const double childMul = inheritedOpacity * (src.opacity / 100.0);
                const bool childHidden = inheritedHidden || !src.visible;
                for (int i = n.children.size() - 1; i >= 0; --i)
                    emitNode(n.children[i], childMul, childHidden);
            } else {
                LayerItem pl = src;
                pl.visible = pl.visible && !inheritedHidden;
                pl.opacity = qBound(1, qRound(pl.opacity * inheritedOpacity), 100);
                panel.append(pl);
            }
        };
    for (int i = roots.size() - 1; i >= 0; --i) emitNode(roots[i], 1.0, false);

    doc->layers = std::move(panel);
    doc->selectedLayers.clear();
    doc->activeLayer = 0;
    deriveCanvasPaper(*doc);
    doc->dirty = false;
    doc->rebuildComposite();
    // Profile policy before the recent note: a Cancelled open leaves no trace.
    if (!applyImportedProfile(*doc, embeddedProfile, error)) {
        const int idx = documents_.indexOf(doc);
        if (idx >= 0) closeDocument(idx);
        return false;
    }
    emit layersChanged();
    emit documentModified(doc);
    noteRecentProject(path);
    return true;
}

// Combine document-space .af masks into one layer-native opaque-grey
// mask. Masks multiply where they overlap and reveal everywhere else.
std::shared_ptr<pittore::Image> afMasksToImage(
    int left, int top, std::uint32_t width, std::uint32_t height,
    const std::vector<const pittore::io::AfMask*>& masks) {
    if (masks.empty() || width == 0 || height == 0) return nullptr;
    auto mask = std::make_shared<pittore::Image>(width, height);
    mask->fill(pittore::compute::make_mask_pixel(1.0f));
    bool any = false;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::int64_t docY = static_cast<std::int64_t>(top) + y;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::int64_t docX = static_cast<std::int64_t>(left) + x;
            float coverage = 1.0f;
            for (const pittore::io::AfMask* m : masks) {
                if (!m || m->width == 0 || m->height == 0) continue;
                if (m->px.size() !=
                    static_cast<std::size_t>(m->width) * m->height)
                    continue;
                const std::int64_t mx = docX - m->left;
                const std::int64_t my = docY - m->top;
                if (mx < 0 || my < 0 ||
                    static_cast<std::uint64_t>(mx) >= m->width ||
                    static_cast<std::uint64_t>(my) >= m->height)
                    continue;
                coverage *=
                    m->px[static_cast<std::size_t>(my) * m->width +
                          static_cast<std::size_t>(mx)] /
                    255.0f;
                any = true;
            }
            if (coverage < 1.0f)
                mask->at(x, y) =
                    pittore::compute::make_mask_pixel(coverage);
        }
    }
    return any ? mask : nullptr;
}

bool AppState::openAfLayers(const QString& path, pittore::io::AfLayersDoc& pd,
                            const QSize& canvasHint, const QImage& flattenedBase,
                            QString* error) {
    // af_layers reports the stacked page canvas (each spread's "PagR" trim,
    // else "SprB", else "DfSz", stacked vertically), so the canvas comes
    // straight from the decode.
    // The embedded preview is only a downscaled render of the same page and is
    // kept as the flattened base layer, not as a size source. Fall back to the
    // preview, then to the layer extents, only when the decoder named no page.
    int dw = static_cast<int>(pd.width), dh = static_cast<int>(pd.height);
    if (dw <= 0 || dh <= 0) {
        dw = canvasHint.width();
        dh = canvasHint.height();
    }
    if (dw <= 0 || dh <= 0) {
        for (const pittore::io::AfLayer& l : pd.layers) {
            if (l.width == 0 || l.height == 0) continue;
            dw = std::max(dw, l.left + static_cast<int>(l.width));
            dh = std::max(dh, l.top + static_cast<int>(l.height));
        }
    }
    // 1<<28 is the decoder's own per-bitmap pixel cap; a larger page is a
    // misread, not a document.
    if (dw <= 0 || dh <= 0 || std::uint64_t(dw) * dh > (1ull << 28)) {
        if (error) *error = tr("Affinity layer canvas (%1×%2) is beyond the document size cap.")
                                .arg(dw)
                                .arg(dh);
        return false;
    }
    const QString title = QFileInfo(path).completeBaseName();
    DocumentItem* doc = addDocument(title, QSize(dw, dh), 72);
    if (!doc) {
        if (error)
            *error = tr("Could not create the document (check the RAM limit in Preferences).");
        return false;
    }
    doc->colorMode = QStringLiteral("RGB/8");
    doc->importSourcePath = QFileInfo(path).absoluteFilePath();

    // The embedded flattened preview is ground truth for whatever the decoder
    // could not rebuild (a deleted linked file, an unsupported layer kind), so
    // it is never discarded. It becomes the VISIBLE base only when no pixel
    // layer was recovered at all — a pure vector/group tree, or a document
    // with no raster content — because there is then nothing else to render.
    //
    // Otherwise the recovered layers ARE the canvas: they keep the file's own
    // visibility so an enabled layer opens enabled, and the preview sits in
    // the panel as a hidden bottom row for the skipped content. Hiding the
    // recovered stack behind the preview (what this did when any layer was
    // skipped) cost the whole layer list its state and left only a
    // full-resolution-less preview on screen.
    bool hasPixelLayer = false;
    for (const pittore::io::AfLayer& l : pd.layers)
        if (!l.isGroup && !l.rgba.empty()) {
            hasPixelLayer = true;
            break;
        }
    const bool hasFlat = !flattenedBase.isNull() && flattenedBase.width() > 0 &&
                         flattenedBase.height() > 0;
    const bool useFlatBase = hasFlat && !hasPixelLayer;

    // Same nesting emission as openPsdLayers: layers are file order (bottom →
    // top); the panel wants top-first with group folders above their children.
    // Groups don't composite themselves — their opacity/visibility fold into
    // the child layers so the render stays correct.
    struct Node {
        LayerItem item;
        QVector<int> children;
        std::vector<pittore::io::AfMask> masks;
    };
    QVector<Node> nodes;
    QVector<int> roots;
    QVector<int> open;
    std::vector<std::vector<pittore::io::AfMask>> activeGroupMasks;
    for (pittore::io::AfLayer& l : pd.layers) {
        while (!open.isEmpty() && nodes[open.back()].item.indent >= l.indent) {
            open.pop_back();
            activeGroupMasks.pop_back();
        }
        LayerItem li;
        li.name = QString::fromStdString(l.name);
        if (li.name.isEmpty()) li.name = l.isGroup ? tr("Group") : tr("Layer");
        li.kind = l.isGroup ? LayerItem::Kind::Group : LayerItem::Kind::Pixel;
        li.isText = !l.isGroup && l.isText;
        // Behind the flattened base there is nothing to show; otherwise the
        // layer opens exactly as the file recorded it, enabled or not.
        li.visible = useFlatBase ? false : l.visible;
        li.opacity = qBound(1, qRound(l.opacity * 100.0 / 255.0), 100);
        li.blendMode = QString::fromStdString(l.blend);
        if (li.blendMode.isEmpty()) li.blendMode = QStringLiteral("Normal");
        li.indent = l.indent;
        li.groupExpanded = l.groupExpanded;
        li.clipped = !l.isGroup && l.clipped;
        if (!l.isGroup) {
            li.offset = QPointF(l.left, l.top);
            li.scaleX = 1.0;
            li.scaleY = 1.0;
            // A native .af shape or free path keeps its true geometry, so
            // it exports as vector instead of a bitmap.
            li.art = l.art;
            if (!l.rgba.empty() && l.width > 0 && l.height > 0) {
                // Every recovered layer comes in packed: straight 8-bit in
                // deferredRgba8 at 4 B/px instead of RGBAf at 16 B/px. The
                // compositor expands the ones that are actually visible the
                // first time it gathers them (ensureLayerPixels ->
                // materializeLayerPixels), so a document only pays 16 B/px for
                // what it is showing; layers the file keeps off stay at a
                // quarter of that and expand on the toggle, an export or a
                // paint stroke. The stash is the 8-bit source the decoder
                // widened as u8*257, so expanding is lossless.
                // l.rgba holds one u16 per channel, so its element count equals
                // the packed byte count (w*h*4 either way).
                const std::size_t pxCount =
                    static_cast<std::size_t>(l.width) * l.height;
                const std::size_t channels = pxCount * 4;
                const bool stashable =
                    l.rgba.size() >= channels &&
                    channels <=
                        static_cast<std::size_t>(std::numeric_limits<int>::max());
                if (stashable) {
                    QByteArray packed(static_cast<int>(channels), Qt::Uninitialized);
                    uchar* dst = reinterpret_cast<uchar*>(packed.data());
                    // u16 = u8*257, and (u8*257)>>8 == u8 for every 8-bit
                    // value, so packing the decoder's 16-bit samples is exact.
                    // Each i writes only its own byte, so the narrowing can
                    // split across cores without changing a single sample.
                    pittore::core::parallel_for(
                        static_cast<std::uint32_t>(channels), 4096,
                        [&](std::uint32_t i0, std::uint32_t i1) {
                            for (std::size_t i = i0; i < i1; ++i)
                                dst[i] = static_cast<uchar>(l.rgba[i] >> 8);
                        });
                    li.deferredRgba8 = std::move(packed);
                    li.deferredWidth = l.width;
                    li.deferredHeight = l.height;
                } else {
                    const QImage q =
                        qimageFromRgba16(l.rgba, static_cast<int>(l.width),
                                         static_cast<int>(l.height));
                    li.pixels = straightRgba64ToImage(q);
                    if (li.pixels) ++li.sourceStamp;  // fresh native pixels
                }
                // Inherited group masks plus the layer's own masks become one
                // editable layer-native mask. Group masks are not baked into
                // alpha anymore.
                std::vector<const pittore::io::AfMask*> combined;
                for (const auto& groupMasks : activeGroupMasks)
                    for (const auto& m : groupMasks) combined.push_back(&m);
                for (const auto& m : l.masks) combined.push_back(&m);
                auto mask = afMasksToImage(l.left, l.top, l.width, l.height,
                                           combined);
                if (mask && (li.pixels || hasDeferredPixels(li))) {
                    li.mask = std::move(mask);
                    li.hasMask = true;
                    li.maskEnabled = true;
                    li.maskLinked = true;
                    li.maskSelected = false;
                    li.maskOffset = li.offset;
                    li.maskScaleX = li.maskScaleY = 1.0;
                    li.maskStamp = 1;
                }
            }
            // A text layer the decoder could re-set keeps its spec: the layer
            // stays live, so a double-click resumes editing it and the first
            // edit re-renders through the font stack instead of resampling the
            // placed glyphs. The decoded pixels are kept until then.
            if (li.isText && l.hasText) {
                li.liveText = true;
                li.textSpec.text = QString::fromStdString(l.textSpec.text);
                li.textSpec.family = QString::fromStdString(l.textSpec.family);
                li.textSpec.bold = l.textSpec.bold;
                li.textSpec.italic = l.textSpec.italic;
                li.textSpec.size = l.textSpec.size;
                li.textSpec.align =
                    l.textSpec.align == pittore::text::Align::Center ? 1
                    : l.textSpec.align == pittore::text::Align::Right ? 2
                                                                       : 0;
                li.textSpec.lineHeight = l.textSpec.lineHeight;
                li.textSpec.tracking = l.textSpec.tracking;
                li.textSpec.wrapWidth = l.textSpec.wrapWidth.value_or(0.0f);
                li.textSpec.frameHeight = l.textFrameHeight;
                li.textSpec.origin = QPointF(l.textOriginX, l.textOriginY);
                li.textSpec.color = QColor::fromRgbF(
                    l.textColor[0], l.textColor[1], l.textColor[2], l.textColor[3]);
                const pittore::text::TextLayout laid = textLayoutFor(li.textSpec);
                li.textLayoutWidth = laid.layoutWidth;
                li.textFirstBaseline = laid.firstBaseline;
                li.textLineAdvance = laid.lineAdvance;
            }
        }
        const int idx = nodes.size();
        nodes.append(Node{std::move(li), {}, l.masks});
        if (!open.isEmpty()) nodes[open.back()].children.append(idx);
        else roots.append(idx);
        if (l.isGroup) {
            open.append(idx);
            activeGroupMasks.push_back(l.masks);
        }
        // The decode holds every layer's RGBA16 buffer at once — gigabytes of
        // tiles for a large document, none of which anyone has looked at yet.
        // Drop this layer's now that it has been consumed: the swap releases
        // the allocation instead of leaving it reserved.
        std::vector<std::uint16_t>().swap(l.rgba);
    }

    QVector<LayerItem> panel;
    std::function<void(int, double, bool)> emitNode =
        [&](int idx, double inheritedOpacity, bool inheritedHidden) {
            const Node& n = nodes[idx];
            const LayerItem& src = n.item;
            if (src.kind == LayerItem::Kind::Group) {
                panel.append(src);
                const double childMul = inheritedOpacity * (src.opacity / 100.0);
                const bool childHidden = inheritedHidden || !src.visible;
                for (int i = n.children.size() - 1; i >= 0; --i)
                    emitNode(n.children[i], childMul, childHidden);
            } else {
                LayerItem pl = src;
                pl.visible = pl.visible && !inheritedHidden;
                pl.opacity = qBound(1, qRound(pl.opacity * inheritedOpacity), 100);
                panel.append(pl);
            }
        };
    for (int i = roots.size() - 1; i >= 0; --i) emitNode(roots[i], 1.0, false);

    // Ground-truth base: the flattened render, scaled to fill the canvas.
    // Kept only when it carries information the recovered stack lacks: the
    // render itself (nothing else recovered) or the reference copy of
    // skipped content. A clean import drops it instead of leaving a
    // confusing extra row in the panel. Both incomplete signals count:
    // skippedLayers is the authoritative count, while complete==false covers
    // docs that predate the counter (the codec defines complete as
    // skippedLayers==0, so they agree on real decodes).
    const bool keepFlat =
        hasFlat && (useFlatBase || pd.skippedLayers > 0 || !pd.complete);
    if (keepFlat) {
        // Ground-truth base: the flattened render, scaled to fill the canvas.
        LayerItem base;
        base.name = tr("Background (flattened)");
        base.kind = LayerItem::Kind::Pixel;
        // It IS the render when nothing else was recovered; otherwise it is
        // the reference copy of whatever the decoder had to skip, so it stays
        // in the panel instead of being discarded — just not in the way.
        base.visible = useFlatBase;
        base.opacity = 100;
        base.blendMode = QStringLiteral("Normal");
        base.indent = 0;
        base.groupExpanded = false;
        base.offset = QPointF(0, 0);
        base.scaleX = double(dw) / double(flattenedBase.width());
        base.scaleY = double(dh) / double(flattenedBase.height());
        auto px = straightRgba64ToImage(flattenedBase);
        if (px) {
            base.pixels = std::move(px);
            ++base.sourceStamp;
        }
        panel.append(base);
        if (useFlatBase) {
            ::pittore::core::log::log_info(
                "[import] AF %s: no recovered pixel layers; flattened base "
                "(%dx%d -> canvas %dx%d) is the render",
                QFileInfo(path).fileName().toUtf8().constData(),
                flattenedBase.width(), flattenedBase.height(), dw, dh);
        } else {
            ::pittore::core::log::log_info(
                "[import] AF %s: %zu recovered layers shown with the file's own "
                "visibility; flattened preview (%dx%d -> canvas %dx%d) kept as a "
                "hidden layer for the %d skipped",
                QFileInfo(path).fileName().toUtf8().constData(), pd.layers.size(),
                flattenedBase.width(), flattenedBase.height(), dw, dh,
                pd.skippedLayers);
        }
    } else if (hasFlat) {
        ::pittore::core::log::log_info(
            "[import] AF %s: all %zu layers recovered, nothing skipped; "
            "flattened preview dropped",
            QFileInfo(path).fileName().toUtf8().constData(), pd.layers.size());
    } else {
        ::pittore::core::log::log_info(
            "[import] AF %s: no flattened base, showing recovered layers",
            QFileInfo(path).fileName().toUtf8().constData());
    }

    doc->layers = std::move(panel);
    doc->selectedLayers.clear();
    doc->activeLayer = 0;
    deriveCanvasPaper(*doc);
    doc->dirty = false;
    doc->rebuildComposite();
    emit layersChanged();
    emit documentModified(doc);
    noteRecentProject(path);
    return true;
}

bool AppState::openSvgParts(const QString& path, const SvgImportResult& svg, int dpi,
                            QString* error) {
    if (svg.docSize.isEmpty()) {
        if (error) *error = tr("SVG produced no usable size.");
        return false;
    }
    const QString title = QFileInfo(path).completeBaseName();
    DocumentItem* doc = addDocument(title, svg.docSize, dpi > 0 ? dpi : 96);
    if (!doc) {
        if (error)
            *error = tr("Could not create the document (check the RAM limit in Preferences).");
        return false;
    }
    doc->colorMode = QStringLiteral("RGB/8");
    doc->importSourcePath = QFileInfo(path).absoluteFilePath();
    doc->layers.clear();
    for (const SvgPartLayer& p : svg.layers) {
        LayerItem li;
        li.name = p.name;
        if (li.name.isEmpty()) li.name = p.group ? tr("Group") : tr("Shape");
        li.kind = p.group ? LayerItem::Kind::Group : LayerItem::Kind::Pixel;
        li.indent = p.depth;
        li.groupExpanded = true;
        li.opacity = qBound(1, qRound(p.opacity * 100.0), 100);
        if (!p.group) {
            li.offset = p.offset;
            li.scaleX = 1.0;
            li.scaleY = 1.0;
            li.art = p.art;   // retained geometry for real vector SVG export
            li.flatArt.assign(p.flatArt.begin(), p.flatArt.end());
            li.sharedNode = p.shared;
            li.sharedRes = p.sharedRes;
            if (!p.pixels.isNull()) {
                li.pixels = straightRgba64ToImage(p.pixels);
                if (li.pixels) ++li.sourceStamp;
            }
            li.flatStamp = li.sourceStamp;
        }
        doc->layers.append(std::move(li));
    }
    if (doc->layers.isEmpty()) {
        if (error) *error = tr("SVG produced no importable parts.");
        return false;
    }
    doc->selectedLayers.clear();
    doc->activeLayer = 0;
    deriveCanvasPaper(*doc);
    doc->dirty = false;
    doc->rebuildComposite();
    emit layersChanged();
    emit documentModified(doc);
    noteRecentProject(path);
    return true;
}

bool AppState::placeSvgParts(const QString& path, const SvgImportResult& svg,
                             const QPointF& centerDoc, QString* error) {
    // No document open: the drop becomes a new document, exactly like
    // openImageFile (a drag into an empty workspace opens the SVG).
    DocumentItem* d = activeDocument();
    if (!d) return openSvgParts(path, svg, 96, error);

    if (svg.docSize.isEmpty() || svg.layers.isEmpty()) {
        if (error) *error = tr("SVG produced no usable parts.");
        return false;
    }

    // Fit a larger SVG down and centre its artwork on the drop point —
    // mirroring acceptDropImage, never upscaling a smaller one.
    const int dw = qMax(1, d->size.width()), dh = qMax(1, d->size.height());
    const double fit =
        qMin(1.0, qMin(double(dw) / qMax(1, svg.docSize.width()),
                       double(dh) / qMax(1, svg.docSize.height())));
    const QPointF delta =
        centerDoc - QPointF(svg.docSize.width() * 0.5 * fit,
                            svg.docSize.height() * 0.5 * fit);

    const int at = qBound(0, d->activeLayer, d->layers.size());
    // Rebase the SVG's absolute depths into the insertion context: the
    // incoming top-level rows become siblings of the row at `at` (appending
    // past the end lands at top level). Absolute depths would split an
    // enclosing group in two — its header's subtree scan would stop at the
    // first incoming depth-0 row, orphaning the rows below into the SVG and
    // blanking the outer group's combined thumbnail.
    const int ctx = (at < d->layers.size()) ? d->layers[at].indent : 0;
    // THROWAWAY thumb logging (removed after diagnosis).
    ::pittore::core::log::log_info(
        "[thumb] placeSvg at=%d ctx=%d incoming=%d", at, ctx,
        svg.layers.size());
    QVector<LayerItem> incoming;
    incoming.reserve(svg.layers.size());
    for (const SvgPartLayer& p : svg.layers) {
        LayerItem li;
        li.name = p.name;
        if (li.name.isEmpty()) li.name = p.group ? tr("Group") : tr("Shape");
        li.kind = p.group ? LayerItem::Kind::Group : LayerItem::Kind::Pixel;
        li.indent = p.depth + ctx;
        li.groupExpanded = true;
        li.opacity = qBound(1, qRound(p.opacity * 100.0), 100);
        li.art = p.art;   // retained geometry → real vector on SVG export
        if (!p.group) {
            li.offset = p.offset * fit + delta;
            li.scaleX = fit;
            li.scaleY = fit;
            if (!p.pixels.isNull()) {
                li.pixels = straightRgba64ToImage(p.pixels);
                if (li.pixels) ++li.sourceStamp;
            }
        }
        incoming.append(std::move(li));
    }
    if (incoming.isEmpty()) {
        if (error) *error = tr("SVG produced no importable parts.");
        return false;
    }

    // `incoming` is panel order (index 0 = top); inserting it at `at` bottom-
    // first keeps the SVG's own paint order with the top part on top. One
    // splice, not N inserts: repeated insert() shifts the tail every time
    // (O(n*m) moves - minutes for an 80k-part drop), while three linear
    // passes land the identical order.
    d->beginUndoAction();
    {
        QVector<LayerItem> merged;
        merged.reserve(d->layers.size() + incoming.size());
        for (int i = 0; i < at; ++i)
            merged.append(std::move(d->layers[i]));
        for (LayerItem& li : incoming) merged.append(std::move(li));
        for (int i = at; i < d->layers.size(); ++i)
            merged.append(std::move(d->layers[i]));
        d->layers = std::move(merged);
    }
    d->activeLayer = at;
    d->selectedLayers.clear();
    d->selectedLayers.push_back(at);
    d->rebuildComposite();
    d->commitUndoAction(tr("Place SVG"), QStringLiteral("vector"));
    emit layersChanged();
    emit activeLayerChanged();
    emit documentModified(d);
    emit historyChanged();
    return true;
}

bool AppState::openImageFile(const QString& path, QString* error) {
    if (path.isEmpty()) {
        if (error) *error = tr("No file given.");
        return false;
    }
    // Native project (.psc, legacy .ifp): load with full layer structure.
    if (isNativeProjectSuffix(QFileInfo(path).suffix()))
        return openProject(path, error);

    const QString suffix = QFileInfo(path).suffix().toLower();
    const QFileInfo finfo(path);
    ::pittore::core::log::log_info(
        "[import] open start path=%s size=%lld suffix=%s",
        path.toUtf8().constData(), finfo.size(), suffix.toUtf8().constData());

    // PSD/PSB: import the TRUE layer structure (groups, blends, opacity,
    // visibility, names) when the layered codec understands the file; anything
    // exotic falls back to the flattened composite path below.
    // psdBytes survives the branch so the flattened fallback can still read
    // the embedded ICC profile without re-reading a large file.
    QByteArray psdBytes;
    QString psdEmbeddedProfile;
    if (suffix == QLatin1String("psd") || suffix == QLatin1String("psb")) {
        QFile f(path);
        std::string codecError;
        if (f.open(QIODevice::ReadOnly)) {
            psdBytes = f.readAll();
            const std::vector<std::uint8_t> raw(psdBytes.constBegin(),
                                                psdBytes.constEnd());
            const std::vector<std::uint8_t> psdIccBytes =
                pittore::io::psdEmbeddedIcc(raw);
            psdEmbeddedProfile = QString::fromStdString(
                pittore::io::iccProfileDescription(
                    psdIccBytes.data(), psdIccBytes.size()));
            ::pittore::core::log::log_info(
                "[import] PSD embedded ICC: %zu bytes -> '%s' for %s",
                psdIccBytes.size(), psdEmbeddedProfile.toUtf8().constData(),
                finfo.fileName().toUtf8().constData());
            auto layers = pittore::io::psdDecodeLayers(raw, &codecError,
                                                        &psdIccBytes);
            if (layers && !layers->layers.empty()) {
                ::pittore::core::log::log_info(
                    "[import] PSD layered decode OK %s -> %ux%u depth=%d layers=%zu",
                    finfo.fileName().toUtf8().constData(), layers->width,
                    layers->height, layers->depth, layers->layers.size());
                const bool ok = openPsdLayers(path, *layers, error,
                                                psdEmbeddedProfile);
                if (!ok)
                    ::pittore::core::log::log_warning(
                        "[import] PSD layered decode OK but document import failed %s",
                        finfo.fileName().toUtf8().constData());
                return ok;
            }
            ::pittore::core::log::log_warning(
                "[import] PSD layered decode rejected %s: %s",
                finfo.fileName().toUtf8().constData(),
                codecError.empty() ? "no layers" : codecError.c_str());
        } else {
            ::pittore::core::log::log_warning("[import] cannot open %s for reading",
                                               path.toUtf8().constData());
        }
        ::pittore::core::log::log_info(
            "[import] PSD %s: falling back to flattened composite decode",
            finfo.fileName().toUtf8().constData());
        // Fall through to the flattened decode below (also tries the plugin).
    }

    // .af/.afphoto/.afdesign/.afpub: the layered codec (af_layers.h)
    // recovers the real bitmap layers from the metadata object tree + the
    // delta-coded tile stream when it understands the container; anything
    // exotic falls back to the flattened embedded-preview composite below.
    if (suffix == QLatin1String("af") || suffix == QLatin1String("afphoto") ||
        suffix == QLatin1String("afdesign") || suffix == QLatin1String("afpub")) {
        QFile f(path);
        std::string codecError;
        if (f.open(QIODevice::ReadOnly)) {
            // Read straight into the decode buffer: the previous QByteArray
            // copy held a second copy of the whole archive in memory for as
            // long as both were alive (75 MiB on a real document).
            const qint64 n = f.size();
            std::vector<std::uint8_t> raw(n > 0 ? static_cast<std::size_t>(n) : 0);
            const qint64 got =
                n > 0 ? f.read(reinterpret_cast<char*>(raw.data()), n) : 0;
            if (got < 0)
                raw.clear();
            else if (got < n)
                raw.resize(static_cast<std::size_t>(got));
            auto layers = pittore::io::afDecodeLayers(raw, &codecError,
                                                       finfo.absolutePath().toStdString());
            if (layers) {
                // Diagnostics the user asked for: which tree was chosen, each
                // bitmap's counter/range/grid and the tile consumption result.
                for (const std::string& line : layers->log)
                    ::pittore::core::log::log_info("[af] %s", line.c_str());
                if (!layers->layers.empty()) {
                    // Canvas hint from the flattened embedded preview — the
                    // preview is a downscaled render, so openAfLayers takes
                    // the larger of the hint and the layered canvases. The
                    // preview raster also becomes the ground-truth base layer
                    // (recovered layers are hidden until their placements are
                    // mapped, so the document shows the correct picture).
                    QSize canvasHint;
                    QImage flatBase;
                    {
                        auto pv = pittore::io::afDecodeDocument(raw, nullptr);
                        if (pv && !pv->previews.empty()) {
                            const auto& pr = pv->previews[pv->primary];
                            canvasHint = QSize(static_cast<int>(pr.width),
                                               static_cast<int>(pr.height));
                            if (!pr.rgba.empty() && pr.width > 0 && pr.height > 0)
                                flatBase = qimageFromRgba16(
                                    pr.rgba, static_cast<int>(pr.width),
                                    static_cast<int>(pr.height));
                        }
                    }
                    ::pittore::core::log::log_info(
                        "[import] AF layered decode OK %s -> canvas=%dx%d layers=%zu "
                        "skipped=%zu complete=%d",
                        finfo.fileName().toUtf8().constData(), layers->width,
                        layers->height, layers->layers.size(), layers->skippedLayers,
                        layers->complete ? 1 : 0);
                    const bool ok = openAfLayers(path, *layers, canvasHint,
                                                 flatBase, error);
                    if (!ok)
                        ::pittore::core::log::log_warning(
                            "[import] AF layered decode OK but document import failed %s",
                            finfo.fileName().toUtf8().constData());
                    return ok;
                }
            }
            ::pittore::core::log::log_warning(
                "[import] AF layered decode rejected %s: %s",
                finfo.fileName().toUtf8().constData(),
                codecError.empty() ? "no layers" : codecError.c_str());
        } else {
            ::pittore::core::log::log_warning("[import] cannot open %s for reading",
                                               path.toUtf8().constData());
        }
        ::pittore::core::log::log_info(
            "[import] AF %s: falling back to flattened composite decode",
            finfo.fileName().toUtf8().constData());
        // Fall through to the flattened decode below.
    }

    // SVG: import its pieces as separate layers/groups so a logo or diagram
    // opens editable part by part, not as one flattened bitmap.
    if (suffix == QLatin1String("svg")) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            SvgImportResult svg;
            int dpi = 96;
            QString svgError;
            if (svgPartsImport(f.readAll(), &svg, &dpi, &svgError)) {
                const bool ok = openSvgParts(path, svg, dpi, error);
                ::pittore::core::log::log_info(
                    "[import] SVG parts %s %s",
                    finfo.fileName().toUtf8().constData(),
                    ok ? "-> imported" : "parts parsed but document import failed");
                return ok;
            }
            ::pittore::core::log::log_warning(
                "[import] SVG parts parse failed %s: %s",
                finfo.fileName().toUtf8().constData(),
                svgError.toUtf8().constData());
        } else {
            ::pittore::core::log::log_warning("[import] cannot open %s for reading",
                                               path.toUtf8().constData());
        }
    }

    // Everything else: decode flattened and import as a single pixel layer.
    // Huge files open as a box-averaged proxy that fits the pixel budget
    // instead of refusing outright: the RAM-limit working set when one is
    // set, else the 100MP dense cap (a 416MP dense document is a 21GB working
    // set with multi-second composites — it never runs well).
    int dpi = 72;
    int proxyFactor = 1;
    QSize fullSize;
    QImage img;
    std::optional<pittore::io::PsdImage> flatPsd;
    std::vector<std::uint16_t> flatTiffCmyk;
    std::vector<std::uint8_t> flatTiffIcc;
    int flatTiffDepth = 0;
    {
        QSize probed;
        int probeDpi = 0;
        constexpr std::uint64_t kHardDenseCapPixels = 100'000'000;
        constexpr std::uint64_t kProxyTargetPixels = 16'000'000;
        std::uint64_t cap = 0;   // 0 = dense legacy path
        if (probeImageSize(path, probed, probeDpi) && !probed.isEmpty() &&
            probed.width() > 0 && probed.height() > 0) {
            const std::uint64_t npix =
                static_cast<std::uint64_t>(probed.width()) * probed.height();
            // Honour the RAM budget exactly: even a 1MB limit proxies
            // correctly (no floor — the factor math already bounds it).
            // The RAM budget can only lower the hard dense cap, never raise
            // it: past 100MP a dense document never runs well.
            std::uint64_t denseBudget = kHardDenseCapPixels;
            if (settings_.ramLimitMb > 0) {
                // A full-resolution CMYK TIFF temporarily holds the decoded
                // 16-bit ink vector and the retained sidecar during import.
                // Budget that extra 16 bytes per pixel before allowing it to
                // bypass the ordinary proxy path.
                const std::uint64_t bytesPerPixel =
                    (suffix == QLatin1String("tif") ||
                     suffix == QLatin1String("tiff")) ? 68ull : 52ull;
                const std::uint64_t ramBudget =
                    static_cast<std::uint64_t>(settings_.ramLimitMb) *
                    1048576ull / bytesPerPixel;
                denseBudget = std::min(denseBudget, ramBudget);
            }
            if (npix > denseBudget)
                cap = std::min(denseBudget, kProxyTargetPixels);
        }
        img = qimageFromFileCapped(path, cap, error, &dpi, &proxyFactor,
                                   &fullSize);
        if (!img.isNull()) {
            if (proxyFactor < 1) proxyFactor = 1;
            if (fullSize.isEmpty()) fullSize = img.size();
            // The Qt image plugin returns appearance RGB and discards device
            // inks. For reasonably sized flattened PSD/PSB files, also decode
            // the native channels so an imported CMYK layer can round-trip.
            const std::uint64_t np = static_cast<std::uint64_t>(img.width()) *
                                     img.height();
            if ((suffix == QLatin1String("psd") ||
                 suffix == QLatin1String("psb")) &&
                proxyFactor == 1 && np <= 16'000'000 && !psdBytes.isEmpty()) {
                const std::vector<std::uint8_t> raw(psdBytes.constBegin(),
                                                    psdBytes.constEnd());
                const auto icc = pittore::io::psdEmbeddedIcc(raw);
                flatPsd = pittore::io::psdDecode(raw, nullptr, &icc);
                if (flatPsd && flatPsd->width ==
                                   static_cast<std::uint32_t>(img.width()) &&
                    flatPsd->height ==
                        static_cast<std::uint32_t>(img.height())) {
                    img = qimageFromRgba16(flatPsd->rgba, img.width(),
                                           img.height());
                } else {
                    flatPsd.reset();
                }
            }
#ifdef PITTORE_TIFF
            if ((suffix == QLatin1String("tif") ||
                 suffix == QLatin1String("tiff")) &&
                proxyFactor == 1) {
                std::uint32_t cw = 0, ch = 0;
                if (pittore::io::tiffDecodeCmykFile(
                        QFile::encodeName(path).constData(), cw, ch,
                        flatTiffCmyk, &flatTiffIcc, &flatTiffDepth) &&
                    (cw != static_cast<std::uint32_t>(img.width()) ||
                     ch != static_cast<std::uint32_t>(img.height()))) {
                    flatTiffCmyk.clear();
                    flatTiffIcc.clear();
                }
            }
#endif
        }
    }
    if (img.isNull()) {
        ::pittore::core::log::log_warning(
            "[import] flattened decode failed %s: %s",
            finfo.fileName().toUtf8().constData(),
            error && !error->isEmpty() ? error->toUtf8().constData() : "unknown");
        return false;
    }
    if (dpi < 1) dpi = 72;

    const QString title = QFileInfo(path).completeBaseName();
    // Large-file guard: every allocation below this point can throw
    // std::bad_alloc (6.6GB engine Image, 6.6GB acc buffer) or return null
    // (3.3GB QImage, 1.6GB composite). Convert both into a clean open
    // failure instead of terminating the app.
    DocumentItem* doc = nullptr;
    try {
        doc = addDocument(title, img.size(), dpi);
    } catch (const std::bad_alloc&) {
        doc = nullptr;
    } catch (...) {
        doc = nullptr;
    }
    if (!doc) {
        // Say what actually happened: dimensions, estimated working set and
        // the active budget — the old generic text sent users hunting.
        const double needMb =
            static_cast<double>(documentWorkingSetBytes(img.size())) /
            (1024.0 * 1024.0);
        if (error) {
            if (settings_.ramLimitMb > 0) {
                *error = tr("Could not create %1 × %2 px document "
                            "(needs ~%3 MB, budget %4 MB in Preferences).")
                             .arg(img.width())
                             .arg(img.height())
                             .arg(QString::number(needMb, 'f', 0))
                             .arg(settings_.ramLimitMb);
            } else {
                *error = tr("Could not create %1 × %2 px document "
                            "(needs ~%3 MB).")
                             .arg(img.width())
                             .arg(img.height())
                             .arg(QString::number(needMb, 'f', 0));
            }
        }
        return false;
    }
    if ((flatPsd && flatPsd->colorMode == 4) || !flatTiffCmyk.empty()) {
        const bool is16 = !flatTiffCmyk.empty()
                              ? flatTiffDepth >= 16
                              : (flatPsd && flatPsd->depth >= 16);
        doc->colorMode = is16
                             ? QStringLiteral("CMYK/16")
                             : QStringLiteral("CMYK/8");
        const std::vector<std::uint8_t> raw(psdBytes.constBegin(),
                                            psdBytes.constEnd());
        const auto icc = !flatTiffIcc.empty()
                             ? flatTiffIcc
                             : pittore::io::psdEmbeddedIcc(raw);
        doc->iccProfile = QByteArray(
            reinterpret_cast<const char*>(icc.data()),
            static_cast<qsizetype>(icc.size()));
    } else if (flatPsd && flatPsd->colorMode == 1) {
        doc->colorMode = flatPsd->depth >= 16
                             ? QStringLiteral("Grayscale/16")
                             : QStringLiteral("Grayscale/8");
    } else {
        doc->colorMode = QStringLiteral("RGB/8");
    }
    doc->importSourcePath = QFileInfo(path).absoluteFilePath();

    std::shared_ptr<pittore::Image> pixels;    try {
        pixels = straightRgba64ToImage(img);
    } catch (const std::bad_alloc&) {
        pixels = nullptr;
    } catch (...) {
        pixels = nullptr;
    }
    if (!pixels) {
        if (error) *error = tr("Not enough memory to open this image.");
        // Roll back the empty document addDocument just created.
        const int idx = documents_.indexOf(doc);
        if (idx >= 0) closeDocument(idx);
        return false;
    }
    LayerItem layer;
    layer.name = title;
    layer.kind = LayerItem::Kind::Pixel;
    layer.pixels = std::move(pixels);
    ++layer.sourceStamp;   // fresh native pixels → stale device cache
    if (flatPsd && flatPsd->colorMode == 4 &&
        flatPsd->cmyk.size() == layer.pixels->pixel_count() * 4) {
        layer.cmykPixels = std::make_shared<pittore::color::CmykImage>();
        layer.cmykPixels->width = layer.pixels->width();
        layer.cmykPixels->height = layer.pixels->height();
        layer.cmykPixels->pixels.resize(layer.pixels->pixel_count());
        for (std::size_t i = 0; i < layer.cmykPixels->pixels.size(); ++i)
            std::copy_n(flatPsd->cmyk.data() + i * 4, 4,
                        layer.cmykPixels->pixels[i].data());
        layer.cmykSourceStamp = layer.sourceStamp;
        layer.cmykPixels->appearanceFingerprint =
            pittore::color::CmykImage::fingerprint(*layer.pixels);
        layer.cmykPixels->profileFingerprint =
            pittore::color::CmykImage::fingerprint(
                reinterpret_cast<const std::uint8_t*>(doc->iccProfile.constData()),
                static_cast<std::size_t>(doc->iccProfile.size()));
    } else if (!flatTiffCmyk.empty() &&
               flatTiffCmyk.size() == layer.pixels->pixel_count() * 4) {
        layer.cmykPixels = std::make_shared<pittore::color::CmykImage>();
        layer.cmykPixels->width = layer.pixels->width();
        layer.cmykPixels->height = layer.pixels->height();
        layer.cmykPixels->pixels.resize(layer.pixels->pixel_count());
        for (std::size_t i = 0; i < layer.cmykPixels->pixels.size(); ++i)
            std::copy_n(flatTiffCmyk.data() + i * 4, 4,
                        layer.cmykPixels->pixels[i].data());
        layer.cmykSourceStamp = layer.sourceStamp;
        layer.cmykPixels->appearanceFingerprint =
            pittore::color::CmykImage::fingerprint(*layer.pixels);
        layer.cmykPixels->profileFingerprint =
            pittore::color::CmykImage::fingerprint(
                reinterpret_cast<const std::uint8_t*>(doc->iccProfile.constData()),
                static_cast<std::size_t>(doc->iccProfile.size()));
        std::vector<std::uint16_t>().swap(flatTiffCmyk);
    }
    doc->layers.clear();
    doc->layers.append(std::move(layer));
    doc->selectedLayers.clear();
    doc->activeLayer = 0;
    deriveCanvasPaper(*doc);
    doc->dirty = false;
    try {
        doc->rebuildComposite();
    } catch (const std::bad_alloc&) {
        if (error) *error = tr("Not enough memory to open this image.");
        const int idx = documents_.indexOf(doc);
        if (idx >= 0) closeDocument(idx);
        return false;
    } catch (...) {
        if (error) *error = tr("Could not open this image.");
        const int idx = documents_.indexOf(doc);
        if (idx >= 0) closeDocument(idx);
        return false;
    }
    emit layersChanged();
    emit documentModified(doc);
    // Embedded profile policy (standard mismatch workflow): PSD/PSB from
    // the hoisted file bytes (no re-read), TIFF from its ICCPROFILE tag,
    // everything else from the file's own ICC block — the decoded image's
    // color space only fills in when the container scan finds nothing,
    // because the toolkit both drops profiles it cannot model (CMYK and
    // friends) and relabels ones it can. An embedded profile must never read
    // as "nothing embedded". Cancel rolls the document back with no trace.
    {
        QString embedded = psdEmbeddedProfile;
        QString source = QStringLiteral("PSD 0x0422 resource");
#ifdef PITTORE_TIFF
        if (embedded.isEmpty() && (suffix == QLatin1String("tif") ||
                                   suffix == QLatin1String("tiff"))) {
            embedded = QString::fromStdString(pittore::io::tiffIccProfileName(
                path.toLocal8Bit().constData()));
            if (!embedded.isEmpty())
                source = QStringLiteral("TIFF ICCPROFILE tag");
        }
#endif
        if (embedded.isEmpty() && suffix != QLatin1String("psd") &&
            suffix != QLatin1String("psb"))
            embedded = embeddedProfileName(path, img, &source);
        if (embedded.isEmpty())
            ::pittore::core::log::log_info(
                "[import] no embedded profile in %s (source: %s)",
                QFileInfo(path).fileName().toUtf8().constData(),
                source.toUtf8().constData());
        else
            ::pittore::core::log::log_info(
                "[import] embedded profile '%s' detected for %s (source: %s)",
                embedded.toUtf8().constData(),
                QFileInfo(path).fileName().toUtf8().constData(),
                source.toUtf8().constData());
        if (!applyImportedProfile(*doc, embedded, error)) {
            const int idx = documents_.indexOf(doc);
            if (idx >= 0) closeDocument(idx);
            return false;
        }
    }
    noteRecentProject(path);
    if (proxyFactor > 1) {
        doc->isProxy = true;
        doc->proxyFactor = proxyFactor;
        doc->fullSize = fullSize;
        doc->sourcePath = QFileInfo(path).absoluteFilePath();
        setStatusHint(tr("Opened at 1/%1 scale (%2 × %3) — full image is %4 × %5.")
                          .arg(proxyFactor)
                          .arg(img.width())
                          .arg(img.height())
                          .arg(fullSize.width())
                          .arg(fullSize.height()));
        ::pittore::core::log::log_info(
            "[import] document opened %s: proxy 1/%d %dx%d of %dx%d dpi=%d (flattened 1 layer)",
            QFileInfo(path).fileName().toUtf8().constData(), proxyFactor,
            img.width(), img.height(), fullSize.width(), fullSize.height(),
            dpi);
    } else {
        ::pittore::core::log::log_info(
            "[import] document opened %s: %dx%d dpi=%d (flattened 1 layer)",
            QFileInfo(path).fileName().toUtf8().constData(), img.width(),
            img.height(), dpi);
    }
    return true;
}

}  // namespace pittore::ui
