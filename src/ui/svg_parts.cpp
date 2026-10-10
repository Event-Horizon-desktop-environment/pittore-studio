#include "ui/svg_parts.h"
#include "ui/svg_bridge.h"

#include "engine/vector/text_flow.h"

#include <QFontMetricsF>
#include <QFont>
#include <QFile>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QStringList>
#include <QXmlStreamReader>
#include <QtMath>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <thread>
#include <vector>

#include "engine/core/log.h"
#include "engine/vector/marker.h"

namespace pittore::ui {

namespace {

constexpr int kMaxPartsLeaves = 8000;

// Import budget, measured in retained geometry bytes instead of leaves:
// 200k tiny icons cost less than 20k dense polygons, so every file is judged
// by its own weight. The cap scales with installed RAM (1/8th, clamped), so
// a workstation opens big maps while a small machine still refuses OOMs.
qint64 svgGeometryBudget() {
    constexpr qint64 kMin = 256ll * 1024 * 1024;
    constexpr qint64 kMax = 2ll * 1024 * 1024 * 1024;
    qint64 totalKb = 0;
    QFile mf(QStringLiteral("/proc/meminfo"));
    if (mf.open(QIODevice::ReadOnly)) {
        // Procfs files report size 0, so atEnd() is true before the first
        // read: drive the loop off readLine() hitting EOF instead.
        for (;;) {
            const QByteArray line = mf.readLine(128);
            if (line.isEmpty()) break;
            long long kb = 0;
            if (std::sscanf(line.constData(), "MemTotal: %lld", &kb) == 1 &&
                kb > 0) {
                totalKb = kb;
                break;
            }
        }
    }
    if (totalKb > 0)
        return qBound(kMin, totalKb * 1024 / 8, kMax);
    return 512ll * 1024 * 1024;
}

// Post-merge guards: rows are emitted one per shape and group (no run
// merging - see svgPartsImport), so a real map legitimately arrives with
// tens of thousands of rows; the cap sits an order of magnitude above that
// and only catches pathological files. Rows bound composite/panel work;
// raster area bounds memory, so its cap is derived from the machine-scaled
// geometry budget (pixels = budget bytes / 4 for RGBA8) with a 64MP floor.
constexpr int kMaxImportRows = 262144;

qint64 svgRasterAreaBudget() {
    return std::max<qint64>(64ll * 1024 * 1024, svgGeometryBudget() / 4);
}

// Per-parse leaf budget: prevents a multi-million-element SVG from turning
// into a multi-million-layer document (OOM + frozen UI). The walker drops
// excess leaves and sets overflow so the importer can fall back to a single
// flattened raster instead.
struct WalkBudget {
    int leaves = 0;
    int limit = kMaxPartsLeaves;
    bool overflow = false;
    // False when the document holds no <textPath>: skips retaining every
    // id'd shape's full attributes for targets that don't exist (the Brazil
    // map copies 33MB of d-strings otherwise).
    bool needShapeRefs = true;
    // Byte budget on retained geometry (0 = off): estimated per pushed leaf
    // as node overhead plus path elements. Trips on weight, not count.
    qint64 bytes = 0;
    qint64 byteBudget = 0;
    // Import diagnostics sink (null skips all counting): set by the budgeted
    // parse entry point, read by the walker below.
    SvgImportDiag* diag = nullptr;
};

// Record an issue key with a cap on distinct keys: the total still counts,
// only the map stays small.
void noteCapped(QHash<QString, int>& map, const QString& key) {
    auto it = map.constFind(key);
    if (it != map.constEnd()) {
        map[key] = it.value() + 1;
        return;
    }
    if (map.size() >= 16) return;
    map.insert(key, 1);
}

// Byte-level run collapse for huge flat SVGs: consecutive identical
// self-closing shape tags with only whitespace between are source-over
// idempotent when fully opaque, so all but the first can be dropped before
// any XML parsing. Turns the 100MB/1.8M-identical-rect stress file from ~7s
// of QXmlStreamReader work into a sub-second import without changing pixels.
// Conservative: only self-closing rect/circle/ellipse/line/polyline/polygon/
// path runs, only when the raw tag carries no opacity/url/mask/filter/clip/
// marker hints and the ancestor opacity stack is exactly 1.
QByteArray collapseIdenticalRuns(const QByteArray& in) {
    constexpr qsizetype kCollapseMinBytes = 5 * 1024 * 1024;
    if (in.size() < kCollapseMinBytes) return in;

    const char* d = in.constData();
    const qsizetype n = in.size();
    QByteArray out;
    out.reserve(in.size() / 8);
    auto isWs = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    auto startsWithAt = [&](qsizetype pos, const char* needle) {
        for (int k = 0; needle[k]; ++k) {
            if (pos + k >= n || d[pos + k] != needle[k]) return false;
        }
        return true;
    };
    auto isShapeTagAt = [&](qsizetype pos, qsizetype& nameLen) -> bool {
        // pos at '<', returns true for collapsible shape starts.
        static const char* kShapes[] = {"<rect",   "<circle", "<ellipse", "<line",
                                        "<polyline", "<polygon", "<path", nullptr};
        for (int s = 0; kShapes[s]; ++s) {
            const char* sh = kShapes[s];
            int l = 0;
            while (sh[l]) ++l;
            bool ok = true;
            for (int k = 0; k < l; ++k) {
                if (pos + k >= n || d[pos + k] != sh[k]) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            // Next char must end the name (space, '/', '>', newline).
            if (pos + l < n) {
                const char c = d[pos + l];
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '/' && c != '>')
                    continue;
            }
            nameLen = l;
            return true;
        }
        return false;
    };
    // Find '>' respecting quotes; returns -1 when unterminated.
    auto tagEndAt = [&](qsizetype pos) -> qsizetype {
        char q = 0;
        for (qsizetype i = pos + 1; i < n; ++i) {
            const char c = d[i];
            if (q) {
                if (c == q) q = 0;
                continue;
            }
            if (c == '"' || c == '\'') {
                q = c;
                continue;
            }
            if (c == '>') return i;
        }
        return -1;
    };
    auto selfClosingEnd = [&](qsizetype tagEnd) -> bool {
        qsizetype i = tagEnd - 1;
        while (i > 0 && isWs(d[i])) --i;
        return i > 0 && d[i] == '/';
    };
    auto rawHasRisk = [&](qsizetype s, qsizetype e) -> bool {
        // e inclusive '>'.
        const qsizetype len = e - s + 1;
        if (len <= 0 || len > 65536) return true;
        // Lowercase scan is overkill; SVG attrs are lowercase in practice.
        // Search for the risky substrings directly.
        for (qsizetype i = s; i <= e; ++i) {
            if (startsWithAt(i, "opacity") || startsWithAt(i, "url(") ||
                startsWithAt(i, "mask") || startsWithAt(i, "filter") ||
                startsWithAt(i, "clip") || startsWithAt(i, "marker"))
                return true;
        }
        return false;
    };
    auto parseOpacityInTag = [&](qsizetype s, qsizetype e, double* v) -> bool {
        for (qsizetype i = s; i <= e; ++i) {
            if (!startsWithAt(i, "opacity")) continue;
            qsizetype j = i + 7;
            while (j <= e && (isWs(d[j]) || d[j] == '=')) {
                if (d[j] == '=') {
                    ++j;
                    break;
                }
                ++j;
            }
            while (j <= e && isWs(d[j])) ++j;
            if (j > e) return false;
            char q2 = 0;
            if (d[j] == '"' || d[j] == '\'') {
                q2 = d[j];
                ++j;
            }
            qsizetype k = j;
            while (k <= e && ((d[k] >= '0' && d[k] <= '9') || d[k] == '.' || d[k] == '+' ||
                              d[k] == '-' || d[k] == 'e' || d[k] == 'E'))
                ++k;
            if (k == j) return false;
            QByteArray num(d + j, (int)(k - j));
            bool ok = false;
            const double vv = num.toDouble(&ok);
            if (!ok) return false;
            if (q2 && k <= e && d[k] == q2) {
                // quoted, fine
            }
            *v = vv;
            return true;
        }
        return false;
    };

    std::vector<double> opStack;
    opStack.push_back(1.0);
    qsizetype prevShapeStart = -1, prevShapeEnd = -1;
    qsizetype copyFrom = 0;
    qsizetype i = 0;
    bool droppedAny = false;
    auto flushTo = [&](qsizetype pos) {
        if (pos > copyFrom) out.append(d + copyFrom, (int)(pos - copyFrom));
        copyFrom = pos;
    };
    while (i < n) {
        if (d[i] != '<') {
            ++i;
            continue;
        }
        if (startsWithAt(i, "<!--")) {
            const char* end = (const char*)memmem(d + i, (size_t)(n - i), "-->", 3);
            qsizetype next = end ? (qsizetype)(end - d) + 3 : n;
            // Comments break runs but ride along.
            prevShapeStart = -1;
            i = next;
            continue;
        }
        if (startsWithAt(i, "<?") || startsWithAt(i, "<!")) {
            qsizetype e = tagEndAt(i);
            if (e < 0) break;
            prevShapeStart = -1;
            i = e + 1;
            continue;
        }
        const bool closing = (i + 1 < n && d[i + 1] == '/');
        qsizetype e = tagEndAt(i);
        if (e < 0) break;
        const bool selfClose = !closing && selfClosingEnd(e);
        qsizetype nameLen = 0;
        const bool isShape = !closing && isShapeTagAt(i, nameLen);
        if (closing) {
            // Pop one ancestor level for any close tag (g/svg/defs/...).
            if (opStack.size() > 1) opStack.pop_back();
            prevShapeStart = -1;
            i = e + 1;
            continue;
        }
        if (!selfClose) {
            // Open container: push inherited opacity.
            double v = 0;
            double cur = opStack.back();
            if (parseOpacityInTag(i, e, &v)) cur = cur * v;
            opStack.push_back(cur);
            prevShapeStart = -1;
            i = e + 1;
            continue;
        }
        // Self-closing leaf.
        if (!isShape) {
            prevShapeStart = -1;
            i = e + 1;
            continue;
        }
        // Whitespace-only gap since the immediately previous shape?
        // (Dropped duplicates still count as previous: a run A A A compares
        // each element with its immediate predecessor, so a 1.8M run keeps
        // the first and drops the rest instead of keeping every other.)
        bool gapWs = false;
        if (prevShapeStart >= 0) {
            gapWs = true;
            for (qsizetype k = prevShapeEnd + 1; k < i; ++k) {
                if (!isWs(d[k])) {
                    gapWs = false;
                    break;
                }
            }
        }
        const qsizetype curLen = e - i + 1;
        bool identical = false;
        if (gapWs && prevShapeStart >= 0 && opStack.back() == 1.0) {
            const qsizetype prevLen = prevShapeEnd - prevShapeStart + 1;
            // Identical bytes to a safe predecessor imply safety (the
            // predecessor was kept only after rawHasRisk passed, and drops
            // only happen on identical-safe runs), so no re-scan is needed
            // here — this keeps the 1.8M-run collapse memcmp-only.
            if (prevLen == curLen && memcmp(d + prevShapeStart, d + i, (size_t)curLen) == 0) {
                identical = true;
            }
        }
        if (identical) {
            // Drop the duplicate; the immediate predecessor (even though
            // dropped) stays the comparison base for the run.
            //
            // Bulk fast path: a 2-long identical run almost always extends to
            // thousands (1.8M here). Skip the whole back-to-back run with
            // memcmp only instead of paying QXml tag parsing per element.
            droppedAny = true;
            const qsizetype patLen = curLen;
            const char* pat = d + i;
            flushTo(i);
            qsizetype j = e + 1;
            qsizetype lastDropStart = i, lastDropEnd = e;
            while (j < n) {
                qsizetype k = j;
                while (k < n && isWs(d[k])) ++k;
                if (k + patLen > n) break;
                if (memcmp(d + k, pat, (size_t)patLen) != 0) break;
                // Another identical back-to-back copy (whitespace gap dropped
                // too — ignorable between elements).
                lastDropStart = k;
                lastDropEnd = k + patLen - 1;
                j = k + patLen;
            }
            copyFrom = j;
            prevShapeStart = lastDropStart;
            prevShapeEnd = lastDropEnd;
            i = j;
            continue;
        }
        // Keep: update the immediate-predecessor base when safe, else break.
        if (opStack.back() == 1.0 && !rawHasRisk(i, e)) {
            prevShapeStart = i;
            prevShapeEnd = e;
        } else {
            prevShapeStart = -1;
        }
        i = e + 1;
    }
    if (!droppedAny) return in;
    flushTo(n);
    // Collapsed nothing: return the original to avoid a 100MB copy.
    if (out.size() == n) return in;
    return out;
}

// ---------------------------------------------------------------------------
// Small parsing helpers
// ---------------------------------------------------------------------------

// Returns an attribute value, falling back to a `style="k:v; ..."` entry.
QString readAttr(const QXmlStreamAttributes& a, const QString& name) {
    if (a.hasAttribute(name)) return a.value(name).toString();
    if (a.hasAttribute(QLatin1String("style"))) {
        const QString style = a.value(QLatin1String("style")).toString();
        for (const QString& piece : style.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
            const int colon = piece.indexOf(QLatin1Char(':'));
            if (colon > 0 &&
                piece.left(colon).trimmed().compare(name, Qt::CaseInsensitive) == 0)
                return piece.mid(colon + 1).trimmed();
        }
    }
    return QString();
}

double parseLength(const QString& s, double dflt) {
    QString t = s.trimmed();
    if (t.isEmpty()) return dflt;
    bool ok = false;
    double v = t.toDouble(&ok);
    if (ok) return v;
    if (t.endsWith(QLatin1String("px"))) {
        v = t.chopped(2).toDouble(&ok);
        if (ok) return v;
    }
    if (t.endsWith(QLatin1String("pt"))) {
        v = t.chopped(2).toDouble(&ok);
        if (ok) return v * 96.0 / 72.0;
    }
    if (t.endsWith(QLatin1String("in"))) {
        v = t.chopped(2).toDouble(&ok);
        if (ok) return v * 96.0;
    }
    if (t.endsWith(QLatin1String("cm"))) {
        v = t.chopped(2).toDouble(&ok);
        if (ok) return v * 96.0 / 2.54;
    }
    if (t.endsWith(QLatin1String("mm"))) {
        v = t.chopped(2).toDouble(&ok);
        if (ok) return v * 96.0 / 25.4;
    }
    return dflt;
}

// Fast ASCII double parse over QChar data: the SVG path/points grammar only
// produces plain decimals, so this skips QStringView-temp + toDouble()
// overhead per number (millions of numbers in map files). Falls back to
// toDouble on anything unusual to stay exact.
bool fastStrToDouble(const QChar* p, int len, double& v) {
    if (len <= 0 || len > 64) return false;
    int k = 0;
    bool neg = false;
    const uint first = p[0].unicode();
    if (first == '+' || first == '-') {
        neg = (first == '-');
        k = 1;
        if (k >= len) return false;
    }
    double intPart = 0.0;
    bool any = false;
    while (k < len) {
        const uint u = p[k].unicode();
        if (u < '0' || u > '9') break;
        any = true;
        intPart = intPart * 10.0 + (u - '0');
        ++k;
    }
    double fracPart = 0.0, fracDiv = 1.0;
    if (k < len && p[k].unicode() == '.') {
        ++k;
        while (k < len) {
            const uint u = p[k].unicode();
            if (u < '0' || u > '9') break;
            any = true;
            fracPart = fracPart * 10.0 + (u - '0');
            fracDiv *= 10.0;
            ++k;
        }
    }
    double expv = 0.0;
    bool expNeg = false;
    if (any && k < len) {
        const uint u = p[k].unicode();
        if (u == 'e' || u == 'E') {
            ++k;
            if (k < len) {
                const uint s = p[k].unicode();
                if (s == '+' || s == '-') {
                    expNeg = (s == '-');
                    ++k;
                }
            }
            bool eany = false;
            while (k < len) {
                const uint u2 = p[k].unicode();
                if (u2 < '0' || u2 > '9') break;
                eany = true;
                expv = expv * 10.0 + (u2 - '0');
                ++k;
            }
            if (!eany) return false;
        }
    }
    if (!any || k != len) return false;
    double val = intPart + (fracDiv == 1.0 ? 0.0 : fracPart / fracDiv);
    if (expv != 0.0) val *= std::pow(10.0, expNeg ? -expv : expv);
    v = neg ? -val : val;
    return true;
}

bool strToDoubleFast(const QString& s, int pos, int len, double& v) {
    if (fastStrToDouble(s.constData() + pos, len, v)) return true;
    bool ok = false;
    v = QStringView(s).mid(pos, len).toDouble(&ok);
    return ok;
}

// Whitespace/comma separated, possibly sign-adjacent numbers.
std::vector<double> parseNumbers(const QString& s) {
    std::vector<double> nums;
    int i = 0;
    const int n = s.size();
    while (i < n) {
        while (i < n && (s[i].isSpace() || s[i] == QLatin1Char(','))) ++i;
        if (i >= n) break;
        const int start = i;
        if (i < n && (s[i] == QLatin1Char('+') || s[i] == QLatin1Char('-'))) ++i;
        bool any = false;
        while (i < n && s[i].isDigit()) { ++i; any = true; }
        if (i < n && s[i] == QLatin1Char('.')) {
            ++i;
            while (i < n && s[i].isDigit()) { ++i; any = true; }
        }
        if (any && i < n && (s[i] == QLatin1Char('e') || s[i] == QLatin1Char('E'))) {
            const int save = i;
            ++i;
            if (i < n && (s[i] == QLatin1Char('+') || s[i] == QLatin1Char('-'))) ++i;
            const int digitStart = i;
            while (i < n && s[i].isDigit()) ++i;
            if (digitStart == i) i = save;
        }
        if (!any || i == start) break;
        double v = 0.0;
        if (strToDoubleFast(s, start, i - start, v)) nums.push_back(v);
        else break;
    }
    return nums;
}

bool parseTransformList(const QString& attr, QTransform* out) {
    if (attr.isEmpty()) {
        *out = QTransform();
        return true;
    }
    int i = 0;
    const int n = attr.size();
    QTransform t;
    bool ok = true;
    while (i < n && ok) {
        while (i < n && (attr[i].isSpace() || attr[i] == QLatin1Char(','))) ++i;
        const int nameStart = i;
        while (i < n && attr[i].isLetter()) ++i;
        const QString fn = attr.mid(nameStart, i - nameStart);
        const int open = attr.indexOf(QLatin1Char('('), i);
        if (open < 0) { ok = false; break; }
        const int close = attr.indexOf(QLatin1Char(')'), open);
        if (close < 0) { ok = false; break; }
        const std::vector<double> nums = parseNumbers(attr.mid(open + 1, close - open - 1));
        i = close + 1;
        if (fn.compare(QLatin1String("translate"), Qt::CaseInsensitive) == 0) {
            const double tx = nums.size() > 0 ? nums[0] : 0.0;
            const double ty = nums.size() > 1 ? nums[1] : 0.0;
            t.translate(tx, ty);
        } else if (fn.compare(QLatin1String("scale"), Qt::CaseInsensitive) == 0) {
            const double sx = nums.size() > 0 ? nums[0] : 1.0;
            const double sy = nums.size() > 1 ? nums[1] : sx;
            t.scale(sx, sy);
        } else if (fn.compare(QLatin1String("rotate"), Qt::CaseInsensitive) == 0) {
            const double a = nums.size() > 0 ? nums[0] : 0.0;
            if (nums.size() > 2) {
                t.translate(nums[1], nums[2]);
                t.rotate(a);
                t.translate(-nums[1], -nums[2]);
            } else {
                t.rotate(a);
            }
        } else if (fn.compare(QLatin1String("skewX"), Qt::CaseInsensitive) == 0) {
            t.shear(std::tan(qDegreesToRadians(nums.size() > 0 ? nums[0] : 0.0)), 0.0);
        } else if (fn.compare(QLatin1String("skewY"), Qt::CaseInsensitive) == 0) {
            t.shear(0.0, std::tan(qDegreesToRadians(nums.size() > 0 ? nums[0] : 0.0)));
        } else if (fn.compare(QLatin1String("matrix"), Qt::CaseInsensitive) == 0) {
            if (nums.size() >= 6)
                t = QTransform(nums[0], nums[1], nums[2], nums[3], nums[4], nums[5]);
            else ok = false;
        } else {
            ok = false;
        }
    }
    if (!ok) return false;
    *out = t;
    return true;
}

bool parseColor(const QString& v, QColor* out) {
    const QString s = v.trimmed();
    if (s.isEmpty()) return false;
    if (s.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0) return false;
    if (s.compare(QLatin1String("transparent"), Qt::CaseInsensitive) == 0) {
        *out = QColor(0, 0, 0, 0);
        return true;
    }
    if (s.startsWith(QLatin1Char('#'))) {
        const QString hex = s.mid(1);
        bool ok = false;
        if (hex.size() == 3) {
            const int r = hex.mid(0, 1).toInt(&ok, 16);
            const int g = hex.mid(1, 1).toInt(&ok, 16);
            const int b = hex.mid(2, 1).toInt(&ok, 16);
            if (ok) {
                *out = QColor(r * 17, g * 17, b * 17);
                return true;
            }
        } else if (hex.size() == 6) {
            const int vv = hex.toInt(&ok, 16);
            if (ok) {
                *out = QColor((vv >> 16) & 0xff, (vv >> 8) & 0xff, vv & 0xff);
                return true;
            }
        } else if (hex.size() == 8) {
            const int vv = hex.toInt(&ok, 16);
            if (ok) {
                *out = QColor((vv >> 24) & 0xff, (vv >> 16) & 0xff, (vv >> 8) & 0xff,
                              vv & 0xff);
                return true;
            }
        }
        return false;
    }
    if (s.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive)) {
        const int open = s.indexOf(QLatin1Char('('));
        const int close = s.lastIndexOf(QLatin1Char(')'));
        if (open >= 0 && close > open) {
            const QStringList parts = s.mid(open + 1, close - open - 1).split(QLatin1Char(','));
            auto val = [](const QString& t) {
                QString u = t.trimmed();
                if (u.endsWith(QLatin1Char('%')))
                    return qBound(0, qRound(u.chopped(1).toDouble() * 2.55), 255);
                return qBound(0, qRound(u.toDouble()), 255);
            };
            if (parts.size() >= 3) {
                QColor c(val(parts[0]), val(parts[1]), val(parts[2]));
                if (parts.size() >= 4) c.setAlphaF(qBound(0.0, parts[3].trimmed().toDouble(), 1.0));
                *out = c;
                return true;
            }
        }
        return false;
    }
    // Named colors: try Qt's SVG palette first, then a common subset.
    const QColor c = QColor::fromString(s);
    if (c.isValid()) {
        *out = c;
        return true;
    }
    static const QHash<QString, QColor> kNames = {
        {QStringLiteral("black"), QColor(0, 0, 0)},
        {QStringLiteral("white"), QColor(255, 255, 255)},
        {QStringLiteral("red"), QColor(255, 0, 0)},
        {QStringLiteral("green"), QColor(0, 128, 0)},
        {QStringLiteral("lime"), QColor(0, 255, 0)},
        {QStringLiteral("blue"), QColor(0, 0, 255)},
        {QStringLiteral("yellow"), QColor(255, 255, 0)},
        {QStringLiteral("cyan"), QColor(0, 255, 255)},
        {QStringLiteral("aqua"), QColor(0, 255, 255)},
        {QStringLiteral("magenta"), QColor(255, 0, 255)},
        {QStringLiteral("fuchsia"), QColor(255, 0, 255)},
        {QStringLiteral("gray"), QColor(128, 128, 128)},
        {QStringLiteral("grey"), QColor(128, 128, 128)},
        {QStringLiteral("silver"), QColor(192, 192, 192)},
        {QStringLiteral("maroon"), QColor(128, 0, 0)},
        {QStringLiteral("olive"), QColor(128, 128, 0)},
        {QStringLiteral("purple"), QColor(128, 0, 128)},
        {QStringLiteral("teal"), QColor(0, 128, 128)},
        {QStringLiteral("navy"), QColor(0, 0, 128)},
        {QStringLiteral("orange"), QColor(255, 165, 0)},
        {QStringLiteral("brown"), QColor(165, 42, 42)},
        {QStringLiteral("pink"), QColor(255, 192, 203)},
    };
    const auto it = kNames.constFind(s.toLower());
    if (it != kNames.constEnd()) {
        *out = *it;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// SVG path grammar → QPainterPath (all commands, arcs → cubics)
// ---------------------------------------------------------------------------

void appendArc(QPainterPath& p, const QPointF& p0, double rx, double ry, double phi,
               bool largeArc, bool sweep, const QPointF& p1) {
    if (p0 == p1 || rx == 0.0 || ry == 0.0) {
        p.lineTo(p1);
        return;
    }
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    const double phiRad = qDegreesToRadians(phi);
    const double cosPhi = std::cos(phiRad);
    const double sinPhi = std::sin(phiRad);
    const double dx2 = (p0.x() - p1.x()) / 2.0;
    const double dy2 = (p0.y() - p1.y()) / 2.0;
    const double x1p = cosPhi * dx2 + sinPhi * dy2;
    const double y1p = -sinPhi * dx2 + cosPhi * dy2;
    const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        const double s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
    }
    const double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    const double num = rx * rx * ry * ry - den;
    double coef = den == 0.0 ? 0.0 : std::sqrt(std::max(0.0, num / den));
    if (largeArc == sweep) coef = -coef;
    const double cxp = coef * (rx * y1p / ry);
    const double cyp = -coef * (ry * x1p / rx);
    const double cx = cosPhi * cxp - sinPhi * cyp + (p0.x() + p1.x()) / 2.0;
    const double cy = sinPhi * cxp + cosPhi * cyp + (p0.y() + p1.y()) / 2.0;
    const double ux = (x1p - cxp) / rx;
    const double uy = (y1p - cyp) / ry;
    const double vx = (-x1p - cxp) / rx;
    const double vy = (-y1p - cyp) / ry;
    double startAngle = std::atan2(uy, ux);
    double sweepAngle = std::atan2(vy, vx) - startAngle;
    if (!sweep && sweepAngle > 0)
        sweepAngle -= 2.0 * M_PI;
    else if (sweep && sweepAngle < 0)
        sweepAngle += 2.0 * M_PI;
    const int segs = std::max(1, static_cast<int>(
                                     std::ceil(std::fabs(sweepAngle) / (M_PI / 2.0))));
    const double delta = sweepAngle / segs;
    double t = startAngle;
    for (int s = 0; s < segs; ++s) {
        const double t2 = t + delta;
        const double etch = (4.0 / 3.0) * std::tan(delta / 4.0);
        const double e1x = std::cos(t) - etch * std::sin(t);
        const double e1y = std::sin(t) + etch * std::cos(t);
        const double e2x = std::cos(t2) + etch * std::sin(t2);
        const double e2y = std::sin(t2) - etch * std::cos(t2);
        const double c1x = cx + rx * cosPhi * e1x - ry * sinPhi * e1y;
        const double c1y = cy + rx * sinPhi * e1x + ry * cosPhi * e1y;
        const double c2x = cx + rx * cosPhi * e2x - ry * sinPhi * e2y;
        const double c2y = cy + rx * sinPhi * e2x + ry * cosPhi * e2y;
        const double ex = cx + rx * cosPhi * std::cos(t2) - ry * sinPhi * std::sin(t2);
        const double ey = cy + rx * sinPhi * std::cos(t2) + ry * cosPhi * std::sin(t2);
        p.cubicTo(QPointF(c1x, c1y), QPointF(c2x, c2y), QPointF(ex, ey));
        t = t2;
    }
}

QPainterPath parseSvgPath(const QString& d) {
    QPainterPath p;
    QPointF cur, ctrl, start;
    bool ctrlValid = false;
    int i = 0;
    const int n = d.size();
    auto isSep = [](QChar ch) { return ch.isSpace() || ch == QLatin1Char(','); };
    auto nextNum = [&](double& v) -> bool {
        while (i < n && isSep(d[i])) ++i;
        const int s = i;
        if (i < n && (d[i] == QLatin1Char('+') || d[i] == QLatin1Char('-'))) ++i;
        bool any = false;
        while (i < n && d[i].isDigit()) { ++i; any = true; }
        if (i < n && d[i] == QLatin1Char('.')) {
            ++i;
            while (i < n && d[i].isDigit()) { ++i; any = true; }
        }
        if (any && i < n && (d[i] == QLatin1Char('e') || d[i] == QLatin1Char('E'))) {
            const int save = i;
            ++i;
            if (i < n && (d[i] == QLatin1Char('+') || d[i] == QLatin1Char('-'))) ++i;
            const int digitStart = i;
            while (i < n && d[i].isDigit()) ++i;
            if (digitStart == i) i = save;
        }
        if (i == s) return false;
        double fv = 0.0;
        if (!strToDoubleFast(d, s, i - s, fv)) return false;
        v = fv;
        return true;
    };

    QChar cmd;  // null = no command yet (QChar(int) is explicit in Qt6)
    while (true) {
        while (i < n && isSep(d[i])) ++i;
        if (i >= n) break;
        if (d[i].isLetter()) {
            cmd = d[i];
            ++i;
        }
        if (cmd.isNull()) break;
        const bool rel = cmd.isLower();
        const QChar c = rel ? cmd.toUpper() : cmd;
        double a = 0, b = 0, c1 = 0, c2 = 0, e1 = 0, e2 = 0;
        switch (c.unicode()) {
            case 'M': {
                // Only the first pair of this command starts a subpath; any
                // further pairs are implicit lineto segments (SVG spec).
                bool firstPair = true;
                while (nextNum(a) && nextNum(b)) {
                    QPointF v(a, b);
                    if (rel) v += cur;
                    if (firstPair) {
                        p.moveTo(v);
                        firstPair = false;
                    } else {
                        p.lineTo(v);
                    }
                    cur = v;
                    start = v;
                }
                ctrlValid = false;
                if (firstPair) goto done;
                break;
            }
            case 'L': {
                while (nextNum(a) && nextNum(b)) {
                    QPointF v(a, b);
                    if (rel) v += cur;
                    p.lineTo(v);
                    cur = v;
                }
                ctrlValid = false;
                break;
            }
            case 'H': {
                while (nextNum(a)) {
                    const double x = rel ? cur.x() + a : a;
                    cur.setX(x);
                    p.lineTo(cur);
                }
                ctrlValid = false;
                break;
            }
            case 'V': {
                while (nextNum(a)) {
                    const double y = rel ? cur.y() + a : a;
                    cur.setY(y);
                    p.lineTo(cur);
                }
                ctrlValid = false;
                break;
            }
            case 'C': {
                while (nextNum(a) && nextNum(b) && nextNum(c1) && nextNum(c2) &&
                       nextNum(e1) && nextNum(e2)) {
                    const QPointF ctrl1(a, b), ctrl2(c1, c2), end(e1, e2);
                    p.cubicTo(rel ? cur + ctrl1 : ctrl1, rel ? cur + ctrl2 : ctrl2,
                              rel ? cur + end : end);
                    ctrl = rel ? cur + ctrl2 : ctrl2;
                    ctrlValid = true;
                    cur = rel ? cur + end : end;
                }
                break;
            }
            case 'S': {
                while (nextNum(a) && nextNum(b) && nextNum(c1) && nextNum(c2)) {
                    const QPointF end(c1, c2);
                    QPointF c1p =
                        ctrlValid ? QPointF(2 * cur.x() - ctrl.x(), 2 * cur.y() - ctrl.y())
                                  : cur;
                    QPointF c2p = rel ? cur + QPointF(a, b) : QPointF(a, b);
                    p.cubicTo(c1p, c2p, rel ? cur + end : end);
                    ctrl = c2p;
                    ctrlValid = true;
                    cur = rel ? cur + end : end;
                }
                break;
            }
            case 'Q': {
                while (nextNum(a) && nextNum(b) && nextNum(c1) && nextNum(c2)) {
                    const QPointF ctl(a, b), end(c1, c2);
                    p.quadTo(rel ? cur + ctl : ctl, rel ? cur + end : end);
                    ctrl = rel ? cur + ctl : ctl;
                    ctrlValid = true;
                    cur = rel ? cur + end : end;
                }
                break;
            }
            case 'T': {
                while (nextNum(a) && nextNum(b)) {
                    const QPointF end(a, b);
                    const QPointF ctl =
                        ctrlValid ? QPointF(2 * cur.x() - ctrl.x(), 2 * cur.y() - ctrl.y())
                                  : cur;
                    p.quadTo(ctl, rel ? cur + end : end);
                    ctrl = ctl;
                    ctrlValid = true;
                    cur = rel ? cur + end : end;
                }
                break;
            }
            case 'A': {
                double rx = 0, ry = 0, phi = 0, largeArc = 0, sweep = 0,
                       ex = 0, ey = 0;
                while (nextNum(rx) && nextNum(ry) && nextNum(phi) &&
                       nextNum(largeArc) && nextNum(sweep) && nextNum(ex) &&
                       nextNum(ey)) {
                    // rx ry x-axis-rotation large-arc-flag,sweep-flag x y
                    const QPointF end =
                        rel ? cur + QPointF(ex, ey) : QPointF(ex, ey);
                    appendArc(p, cur, rx, ry, phi, largeArc != 0.0,
                              sweep != 0.0, end);
                    cur = end;
                }
                ctrlValid = false;
                break;
            }
            case 'Z': {
                p.closeSubpath();
                cur = start;
                ctrlValid = false;
                break;
            }
            default: goto done;
        }
    }
done:
    return p;
}

// ---------------------------------------------------------------------------
// Text (best effort; fonts need a GUI app, otherwise an approximate box)
// ---------------------------------------------------------------------------

QFont fontForAttrs(const QXmlStreamAttributes& a);

// Forward declarations for the text layouts below (defined with the walker).
struct PaintState;
struct WalkBudget;
bool buildShapeGeometry(const QString& tag,
                        const std::function<QString(QLatin1String)>& get,
                        QPainterPath& path);
QString refTarget(const QString& v);
void walkChildren(QXmlStreamReader& xml, const QTransform& tf, const PaintState& ps,
                  SvgResources& res, QVector<SvgNode>& out, WalkBudget* budget);
void walkElement(QXmlStreamReader& xml, const QTransform& parentTf,
                 const PaintState& parentPaint, SvgResources& res,
                 QVector<SvgNode>& out, WalkBudget* budget);

QPainterPath textGlyphPath(const QXmlStreamAttributes& a, const QString& text,
                           double* outWidth) {
    const double x = parseLength(readAttr(a, QLatin1String("x")), 0.0);
    const double y = parseLength(readAttr(a, QLatin1String("y")), 0.0);
    const double size = parseLength(readAttr(a, QLatin1String("font-size")), 16.0);
    QPainterPath p;
    if (!text.isEmpty()) {
        const QFont f = fontForAttrs(a);
        if (QGuiApplication::instance()) {
            p.addText(x, y, f, text);
            if (outWidth) *outWidth = QFontMetricsF(f).horizontalAdvance(text);
        } else {
            // No font engine: render a filled box sized to the text (the same
            // approximation the engine's svg_parse uses).
            const double w = text.size() * size * 0.5;
            if (outWidth) *outWidth = w;
            p.addRect(x, y - size * 0.8, w, size * 1.2);
        }
    }
    return p;
}

QFont fontForAttrs(const QXmlStreamAttributes& a) {
    const double size = parseLength(readAttr(a, QLatin1String("font-size")), 16.0);
    const QString family =
        readAttr(a, QLatin1String("font-family")).isEmpty()
            ? QStringLiteral("sans-serif")
            : readAttr(a, QLatin1String("font-family"));
    QFont f(family);
    f.setPixelSize(qMax(1, qRound(size)));
    const QString weight = readAttr(a, QLatin1String("font-weight")).toLower();
    if (weight == QLatin1String("bold") || weight == QStringLiteral("700") ||
        weight == QStringLiteral("800"))
        f.setBold(true);
    if (readAttr(a, QLatin1String("font-style")).toLower() == QLatin1String("italic"))
        f.setItalic(true);
    return f;
}

// Lay `text` along the shape-ref target (text-on-path) using the engine's
// arclength walker; advances come from the shaped font so spacing matches.
QPainterPath textOnPathGlyphs(const QXmlStreamAttributes& a, const QString& text,
                              const QString& targetId, const SvgResources& res) {
    QPainterPath out;
    const auto it = res.shapeRefs.constFind(targetId);
    if (it == res.shapeRefs.constEnd() || text.isEmpty()) return out;
    const SvgShapeRef& ref = it.value();
    QPainterPath spine;
    auto get = [&](QLatin1String k) -> QString { return ref.attrs.value(k).toString(); };
    if (!buildShapeGeometry(ref.tag, get, spine) || spine.isEmpty()) return out;
    // Longest flattened subpath is the baseline.
    std::vector<std::pair<double, double>> flat;
    double best = -1.0;
    for (const QPolygonF& poly : spine.toSubpathPolygons()) {
        double len = 0.0;
        for (int i = 1; i < poly.size(); i++)
            len += QLineF(poly[i - 1], poly[i]).length();
        if (len > best) {
            best = len;
            flat.clear();
            for (const QPointF& pt : poly) flat.emplace_back(pt.x(), pt.y());
        }
    }
    if (flat.size() < 2) return out;
    const QFont f = fontForAttrs(a);
    const QFontMetricsF fm(f);
    std::vector<double> advances;
    for (const QChar& ch : text) advances.push_back(fm.horizontalAdvance(ch));
    const double spacing =
        parseLength(readAttr(a, QLatin1String("letter-spacing")), 0.0);
    const double startOffset =
        parseLength(readAttr(a, QLatin1String("startOffset")), 0.0);
    const auto glyphs = pittore::vector::textOnPathLayout(
        text.toStdString(), flat, advances, spacing, startOffset);
    size_t gi = 0;
    for (int ci = 0; ci < text.size() && gi < glyphs.size();) {
        // Advance one QString char (glyph list parallels UTF-8 codepoints;
        // BMP text aligns 1:1, astral pairs consume a placeholder each).
        const auto& g = glyphs[gi++];
        const QChar ch = text[ci++];
        if (!g.visible) continue;
        QPainterPath one;
        one.addText(0, 0, f, QString(ch));
        QTransform t;
        t.translate(g.x, g.y);
        t.rotate(g.rotationDeg);
        out.addPath(t.map(one));
    }
    return out;
}

// Flow `text` into `cols` (text-in-shape / flowRoot) via the engine layout.
QPainterPath flowTextGlyphs(const QXmlStreamAttributes& a, const QString& text,
                            const QList<QRectF>& cols) {
    QPainterPath out;
    if (text.trimmed().isEmpty() || cols.isEmpty()) return out;
    const QFont f = fontForAttrs(a);
    const QFontMetricsF fm(f);
    const double size = parseLength(readAttr(a, QLatin1String("font-size")), 16.0);
    const double lineHeight = size * 1.25;
    std::vector<double> advances;
    for (const QChar& ch : text) advances.push_back(fm.horizontalAdvance(ch));
    std::vector<std::array<double, 4>> boxes;
    for (const QRectF& c : cols)
        boxes.push_back({c.x(), c.y(), c.x() + c.width(), c.y() + c.height()});
    const auto glyphs = pittore::vector::textInShapeLayout(
        text.toStdString(), boxes, advances, lineHeight, true);
    size_t gi = 0;
    for (int ci = 0; ci < text.size() && gi < glyphs.size();) {
        const auto& g = glyphs[gi++];
        const QChar ch = text[ci++];
        if (ch == QLatin1Char('\n') || ch == QLatin1Char(' ')) {
            if (ch == QLatin1Char('\n')) continue;
        }
        if (!g.visible) continue;
        QPainterPath one;
        one.addText(g.x, g.y, f, QString(ch));
        out.addPath(one);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Gradient definitions (<defs>)
// ---------------------------------------------------------------------------

std::shared_ptr<SvgGradient> parseGradient(QXmlStreamReader& xml,
                                           const QXmlStreamAttributes& a) {
    auto g = std::make_shared<SvgGradient>();
    if (xml.name() == QLatin1String("linearGradient")) {
        g->radial = false;
        g->x1 = parseLength(readAttr(a, QLatin1String("x1")), 0.0);
        g->y1 = parseLength(readAttr(a, QLatin1String("y1")), 0.0);
        g->x2 = parseLength(readAttr(a, QLatin1String("x2")), 1.0);
        g->y2 = parseLength(readAttr(a, QLatin1String("y2")), 0.0);
    } else if (xml.name() == QLatin1String("radialGradient")) {
        g->radial = true;
        g->cx = parseLength(readAttr(a, QLatin1String("cx")), 0.5);
        g->cy = parseLength(readAttr(a, QLatin1String("cy")), 0.5);
        g->r = parseLength(readAttr(a, QLatin1String("r")), 0.5);
    }
    g->userSpace =
        readAttr(a, QLatin1String("gradientUnits")) == QLatin1String("userSpaceOnUse");
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("stop")) {
            if (g->stops.size() < 64) {
                const QXmlStreamAttributes sa = xml.attributes();
                SvgGradient::Stop s;
                s.offset = parseLength(readAttr(sa, QLatin1String("offset")), 0.0);
                QString c = readAttr(sa, QLatin1String("stop-color"));
                if (!parseColor(c, &s.color)) s.color = QColor(0, 0, 0);
                const double so = parseLength(readAttr(sa, QLatin1String("stop-opacity")), 1.0);
                s.color.setAlphaF(qBound(0.0, so * s.color.alphaF(), 1.0));
                const QString style = readAttr(sa, QLatin1String("style"));
                if (!style.isEmpty()) {
                    // style="stop-color:...; stop-opacity:..."
                    for (const QString& piece :
                         style.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                        const int colon = piece.indexOf(QLatin1Char(':'));
                        if (colon <= 0) continue;
                        const QString k = piece.left(colon).trimmed();
                        const QString v = piece.mid(colon + 1).trimmed();
                        if (k == QLatin1String("stop-color")) {
                            QColor cc;
                            if (parseColor(v, &cc)) s.color = cc;
                        } else if (k == QLatin1String("stop-opacity")) {
                            s.color.setAlphaF(qBound(0.0, v.toDouble(), 1.0));
                        }
                    }
                }
                g->stops.push_back(s);
            }
            xml.skipCurrentElement();
        } else {
            xml.skipCurrentElement();
        }
    }
    if (g->stops.empty()) return nullptr;
    return g;
}

void collectDefs(QXmlStreamReader& xml, QHash<QString, std::shared_ptr<SvgGradient>>& out) {
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("linearGradient") ||
            xml.name() == QLatin1String("radialGradient")) {
            const QXmlStreamAttributes a = xml.attributes();
            const QString id = a.value(QLatin1String("id")).toString();
            if (!id.isEmpty()) {
                auto g = parseGradient(xml, a);
                if (g) out.insert(id, g);
                continue;
            }
            xml.skipCurrentElement();
        } else {
            xml.skipCurrentElement();
        }
    }
}

// Collect every gradient in the document before the shape walk. Gradients can
// live anywhere (usually <defs>, but the spec just requires an id), and shapes
// routinely reference a gradient that appears earlier or later, so a single
// ordered pass over <defs> would miss most references.
void scanGradients(QXmlStreamReader& xml,
                   QHash<QString, std::shared_ptr<SvgGradient>>& out) {
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) continue;
        if (xml.name() != QLatin1String("linearGradient") &&
            xml.name() != QLatin1String("radialGradient"))
            continue;
        const QXmlStreamAttributes a = xml.attributes();
        const QString id = a.value(QLatin1String("id")).toString();
        if (id.isEmpty() || out.contains(id)) {
            xml.skipCurrentElement();
            continue;
        }
        if (auto g = parseGradient(xml, a)) out.insert(id, g);
    }
}

// ---------------------------------------------------------------------------
// Retained object resources: patterns, markers, clips, masks, filters,
// meshes and textPath shape refs. Same two-pass rationale as gradients.
// ---------------------------------------------------------------------------


// url(#id) or #id → id, else empty.
QString refTarget(const QString& v) {
    QString s = v.trimmed();
    if (s.startsWith(QLatin1String("url("))) {
        const int h = s.indexOf(QLatin1Char('#'));
        const int e = s.indexOf(QLatin1Char(')'), h);
        if (h < 0) return {};
        return s.mid(h + 1, (e < 0 ? s.size() : e) - h - 1).trimmed();
    }
    if (s.startsWith(QLatin1Char('#'))) return s.mid(1).trimmed();
    return {};
}

// One shape-geometry implementation shared by the walker and the textPath
// target rebuild: `get` reads raw attribute strings from either a live
// QXmlStreamAttributes or a stored SvgShapeRef.
bool buildShapeGeometry(const QString& tag,
                        const std::function<QString(QLatin1String)>& get,
                        QPainterPath& path) {
    if (tag == QLatin1String("rect")) {
        const double x = parseLength(get(QLatin1String("x")), 0.0);
        const double y = parseLength(get(QLatin1String("y")), 0.0);
        const double w = parseLength(get(QLatin1String("width")), 0.0);
        const double h = parseLength(get(QLatin1String("height")), 0.0);
        const double rr = qMax(parseLength(get(QLatin1String("rx")), -1.0),
                               parseLength(get(QLatin1String("ry")), -1.0));
        if (w > 0.0 && h > 0.0) {
            if (rr > 0.0)
                path.addRoundedRect(QRectF(x, y, w, h), qMin(rr, w / 2.0),
                                    qMin(rr, h / 2.0));
            else
                path.addRect(x, y, w, h);
        }
    } else if (tag == QLatin1String("circle")) {
        const double cx = parseLength(get(QLatin1String("cx")), 0.0);
        const double cy = parseLength(get(QLatin1String("cy")), 0.0);
        const double r = parseLength(get(QLatin1String("r")), 0.0);
        if (r > 0.0) path.addEllipse(QPointF(cx, cy), r, r);
    } else if (tag == QLatin1String("ellipse")) {
        const double cx = parseLength(get(QLatin1String("cx")), 0.0);
        const double cy = parseLength(get(QLatin1String("cy")), 0.0);
        const double rx = parseLength(get(QLatin1String("rx")), 0.0);
        const double ry = parseLength(get(QLatin1String("ry")), 0.0);
        if (rx > 0.0 && ry > 0.0) path.addEllipse(QPointF(cx, cy), rx, ry);
    } else if (tag == QLatin1String("line")) {
        path.moveTo(parseLength(get(QLatin1String("x1")), 0.0),
                    parseLength(get(QLatin1String("y1")), 0.0));
        path.lineTo(parseLength(get(QLatin1String("x2")), 0.0),
                    parseLength(get(QLatin1String("y2")), 0.0));
    } else if (tag == QLatin1String("polyline") || tag == QLatin1String("polygon")) {
        const std::vector<double> pts = parseNumbers(get(QLatin1String("points")));
        for (std::size_t k = 0; k + 1 < pts.size(); k += 2) {
            if (k == 0) path.moveTo(pts[0], pts[1]);
            else path.lineTo(pts[k], pts[k + 1]);
        }
        if (tag == QLatin1String("polygon")) path.closeSubpath();
    } else if (tag == QLatin1String("path")) {
        path = parseSvgPath(get(QLatin1String("d")));
    } else {
        return false;
    }
    return !path.isEmpty();
}

// ---------------------------------------------------------------------------
// Embedded rasters (<image>)
// ---------------------------------------------------------------------------

// data:[<mime>][;base64],<payload> → decoded bitmap, shared by <use> clones,
// pattern tiles and shared rows (counted once, at decode). Null for external
// refs, unknown encodings, decode failures and over-budget pixels: the caps
// keep an embedded-raster bomb from inflating decoded memory.
constexpr qint64 kMaxImageBytes = 256ll * 1024 * 1024;
constexpr int kMaxImageEdge = 8192;

std::shared_ptr<QImage> decodeImageDataUri(const QString& href,
                                           SvgResources& res) {
    if (!href.startsWith(QLatin1String("data:"))) return nullptr;
    const int comma = href.indexOf(QLatin1Char(','));
    if (comma < 0) return nullptr;
    if (res.imageBytes >= kMaxImageBytes) return nullptr;
    const QString head = href.left(comma);
    const QString payload = href.mid(comma + 1);
    const QByteArray raw =
        head.endsWith(QLatin1String(";base64"), Qt::CaseInsensitive)
            ? QByteArray::fromBase64(payload.toLatin1())
            : QByteArray::fromPercentEncoding(payload.toUtf8());
    if (raw.isEmpty()) return nullptr;
    auto img = std::make_shared<QImage>();
    if (!img->loadFromData(raw) || img->isNull()) return nullptr;
    if (img->width() > kMaxImageEdge || img->height() > kMaxImageEdge)
        return nullptr;
    ++res.images;
    res.imageBytes += (qint64)img->width() * img->height() * 4;
    return img;
}

// preserveAspectRatio → fit mode: 0 = meet (spec default xMidYMid),
// 1 = none (stretch), 2 = slice (cover, clipped to the rect).
quint8 imageFitAttr(const QString& par) {
    const QString p = par.trimmed();
    if (p.startsWith(QLatin1String("none"))) return 1;
    if (p.contains(QLatin1String("slice"))) return 2;
    return 0;
}

// Place a decoded bitmap into `r` honoring the fit mode (SVG's
// preserveAspectRatio semantics; images may share the painter's clip).
void drawImageFitted(QPainter& p, const QImage& img, const QRectF& r,
                     quint8 fit) {
    if (r.isEmpty() || img.isNull()) return;
    // Small bitmaps get scaled up on large canvases; bilinear sampling keeps
    // overlays (zoning tints, icons) smooth like any vector renderer.
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (fit == 1) {
        p.drawImage(r, img);
        p.restore();
        return;
    }
    const qreal s =
        fit == 2 ? qMax(r.width() / img.width(), r.height() / img.height())
                 : qMin(r.width() / img.width(), r.height() / img.height());
    const QPointF tl =
        r.center() - QPointF(img.width() * s / 2.0, img.height() * s / 2.0);
    const QRectF target(tl, QSizeF(img.width() * s, img.height() * s));
    if (fit == 2) p.setClipRect(r, Qt::IntersectClip);
    p.drawImage(target, img);
    p.restore();
}


// ---------------------------------------------------------------------------
// Tree walker
// ---------------------------------------------------------------------------

// Inherited paint state (SVG attributes inherit through the group tree).
struct PaintState {
    bool hasFill = true;
    QColor fill{0, 0, 0};
    bool hasStroke = false;
    QColor stroke{0, 0, 0};
    double strokeWidth = 1.0;
    double opacity = 1.0;
    // Fill rule likewise inherits; the spec initial value is nonzero.
    bool fillEvenOdd = false;
};

void scanResources(QXmlStreamReader& xml, SvgResources& res) {
    auto cellShapes = [&](SvgResources& r, QVector<SvgNode>& shapes) {
        // Walk def-children with identity placement; PaintState defaults to
        // opaque black fill (SVG initial values). Defs are tiny: no budget.
        walkChildren(xml, QTransform(), PaintState(), r, shapes, nullptr);
    };
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) continue;
        const QString tag = xml.name().toString();
        const QXmlStreamAttributes a = xml.attributes();
        const QString id = a.value(QLatin1String("id")).toString();
        if (tag == QLatin1String("pattern") && !id.isEmpty() &&
            !res.patterns.contains(id)) {
            SvgPatternCell cell;
            cell.x = parseLength(readAttr(a, QLatin1String("x")), 0.0);
            cell.y = parseLength(readAttr(a, QLatin1String("y")), 0.0);
            cell.w = parseLength(readAttr(a, QLatin1String("width")), 10.0);
            cell.h = parseLength(readAttr(a, QLatin1String("height")), 10.0);
            if (!(cell.w > 0.0)) cell.w = 10.0;
            if (!(cell.h > 0.0)) cell.h = 10.0;
            cell.xform = readAttr(a, QLatin1String("patternTransform"));
            cellShapes(res, cell.shapes);
            res.patterns.insert(id, std::move(cell));
        } else if (tag == QLatin1String("marker") && !id.isEmpty() &&
                   !res.markers.contains(id)) {
            SvgMarkerCell cell;
            cell.refX = parseLength(readAttr(a, QLatin1String("refX")), 0.0);
            cell.refY = parseLength(readAttr(a, QLatin1String("refY")), 0.0);
            cell.w = parseLength(readAttr(a, QLatin1String("markerWidth")), 3.0);
            cell.h = parseLength(readAttr(a, QLatin1String("markerHeight")), 3.0);
            const QString orient = readAttr(a, QLatin1String("orient"));
            cell.autoOrient =
                orient.isEmpty() || orient == QLatin1String("auto") ||
                orient == QLatin1String("auto-start-reverse");
            if (!cell.autoOrient) cell.deg = orient.toDouble();
            cell.strokeUnits =
                readAttr(a, QLatin1String("markerUnits")) != QLatin1String("userSpaceOnUse");
            cellShapes(res, cell.shapes);
            res.markers.insert(id, std::move(cell));
        } else if (tag == QLatin1String("clipPath") && !id.isEmpty() &&
                   !res.clips.contains(id)) {
            SvgClipCell cell;
            cellShapes(res, cell.shapes);
            res.clips.insert(id, std::move(cell));
        } else if (tag == QLatin1String("mask") && !id.isEmpty() &&
                   !res.masks.contains(id)) {
            SvgMaskCell cell;
            cell.x = parseLength(readAttr(a, QLatin1String("x")), 0.0);
            cell.y = parseLength(readAttr(a, QLatin1String("y")), 0.0);
            cell.w = parseLength(readAttr(a, QLatin1String("width")), 0.0);
            cell.h = parseLength(readAttr(a, QLatin1String("height")), 0.0);
            cellShapes(res, cell.shapes);
            res.masks.insert(id, std::move(cell));
        } else if (tag == QLatin1String("filter") && !id.isEmpty() &&
                   !res.filters.contains(id)) {
            pittore::vector::FilterGraph g;
            g.id = id.toStdString();
            while (xml.readNextStartElement()) {
                const QString pt = xml.name().toString();
                if (!pt.startsWith(QLatin1String("fe"))) {
                    xml.skipCurrentElement();
                    continue;
                }
                pittore::vector::FePrimitive p;
                p.type = pt.toStdString();
                const QXmlStreamAttributes pa = xml.attributes();
                for (const QXmlStreamAttribute& at : pa) {
                    const QString k = at.name().toString();
                    const QString v = at.value().toString();
                    if (k == QLatin1String("result")) p.result = v.toStdString();
                    else if (k == QLatin1String("in")) p.in = v.toStdString();
                    else if (k == QLatin1String("in2")) p.in2 = v.toStdString();
                    else p.attrs[k.toStdString()] = v.toStdString();
                }
                // Fold light/function/merge children into the parent prim so
                // the engine model stays flat while losing nothing.
                while (xml.readNextStartElement()) {
                    const QString ct = xml.name().toString();
                    const QXmlStreamAttributes ca = xml.attributes();
                    if (ct == QLatin1String("feDistantLight") ||
                        ct == QLatin1String("fePointLight") ||
                        ct == QLatin1String("feSpotLight")) {
                        p.attrs["light.tag"] = ct.toStdString();
                        for (const QXmlStreamAttribute& at : ca)
                            p.attrs["light." + at.name().toString().toStdString()] =
                                at.value().toString().toStdString();
                    } else if (ct == QLatin1String("feFuncR") ||
                               ct == QLatin1String("feFuncG") ||
                               ct == QLatin1String("feFuncB") ||
                               ct == QLatin1String("feFuncA")) {
                        const std::string pre = ct.toStdString() + ".";
                        for (const QXmlStreamAttribute& at : ca)
                            p.attrs[pre + at.name().toString().toStdString()] =
                                at.value().toString().toStdString();
                    } else if (ct == QLatin1String("feMergeNode")) {
                        const QString vin =
                            ca.value(QLatin1String("in")).toString();
                        if (!vin.isEmpty()) {
                            if (!p.attrs.count("nodes")) p.attrs["nodes"] = "";
                            else p.attrs["nodes"] += ",";
                            p.attrs["nodes"] += vin.toStdString();
                        }
                    }
                    xml.skipCurrentElement();
                }
                g.prims.push_back(std::move(p));
            }
            res.filters.insert(id, std::move(g));
        } else if (tag == QLatin1String("meshgradient") && !id.isEmpty() &&
                   !res.meshes.contains(id)) {
            pittore::vector::MeshGradient m;
            m.rows = qMax(1, readAttr(a, QLatin1String("rows")).toInt());
            m.cols = qMax(1, readAttr(a, QLatin1String("cols")).toInt());
            m.isConical = readAttr(a, QLatin1String("conical")) == QLatin1String("1");
            m.conicalCx = parseLength(readAttr(a, QLatin1String("cx")), 0.5);
            m.conicalCy = parseLength(readAttr(a, QLatin1String("cy")), 0.5);
            while (xml.readNextStartElement()) {
                if (xml.name() != QLatin1String("meshpatch")) {
                    xml.skipCurrentElement();
                    continue;
                }
                pittore::vector::MeshPatch patch{};
                const QXmlStreamAttributes pa = xml.attributes();
                for (int k = 0; k < 4; k++) {
                    const QString ck =
                        pa.value(QString("c%1").arg(k)).toString();
                    const QStringList rgb = ck.split(QLatin1Char(','));
                    if (rgb.size() == 3) {
                        patch.c[(size_t)k][0] = (std::uint8_t)qBound(0, rgb[0].toInt(), 255);
                        patch.c[(size_t)k][1] = (std::uint8_t)qBound(0, rgb[1].toInt(), 255);
                        patch.c[(size_t)k][2] = (std::uint8_t)qBound(0, rgb[2].toInt(), 255);
                        patch.c[(size_t)k][3] = 255;
                    }
                }
                const QStringList pts =
                    readAttr(pa, QLatin1String("p")).split(QLatin1Char(' '),
                                                           Qt::SkipEmptyParts);
                for (int k = 0; k < 16 && k < pts.size(); k++) {
                    const QStringList xy = pts[k].split(QLatin1Char(','));
                    if (xy.size() == 2) {
                        patch.p[(size_t)k][0] = xy[0].toDouble();
                        patch.p[(size_t)k][1] = xy[1].toDouble();
                    }
                }
                m.patches.push_back(patch);
                xml.skipCurrentElement();
            }
            res.meshes.insert(id, std::move(m));
        } else if ((tag == QLatin1String("rect") || tag == QLatin1String("circle") ||
                    tag == QLatin1String("ellipse") || tag == QLatin1String("line") ||
                    tag == QLatin1String("polyline") || tag == QLatin1String("polygon") ||
                    tag == QLatin1String("path")) &&
                   !id.isEmpty() && !res.shapeRefs.contains(id)) {
            SvgShapeRef ref;
            ref.tag = tag;
            ref.attrs = a;  // rebuildable geometry for <textPath> targets
            res.shapeRefs.insert(id, std::move(ref));
            // Do not consume: keep descending so nested defs are still found.
        }
    }
}


void walkChildren(QXmlStreamReader& xml, const QTransform& tf, const PaintState& ps,
                  SvgResources& res, QVector<SvgNode>& out, WalkBudget* budget);

// Consecutive identical fully-opaque flat leaves are source-over idempotent:
// only the topmost contributes. The 100mb stress file stacks 1.8M identical
// red rects; dropping the covered copies keeps import O(1) instead of OOM.
bool samePathForCoalesce(const QPainterPath& a, const QPainterPath& b) {
    if (a.elementCount() != b.elementCount()) return false;
    if (a.elementCount() == 0) return false;
    for (int i = 0; i < a.elementCount(); ++i) {
        const QPainterPath::Element ea = a.elementAt(i);
        const QPainterPath::Element eb = b.elementAt(i);
        if (ea.type != eb.type) return false;
        if (ea.x != eb.x || ea.y != eb.y) return false;
    }
    return true;
}

bool sameMergePaint(const SvgNode& last, const SvgNode& cur) {
    if (last.group || cur.group) return false;
    if (last.image || cur.image) return false;  // bitmap ≠ vector run
    if (last.opacity != 1.0 || cur.opacity != 1.0) return false;
    if (!cur.hasFill || !last.hasFill) return false;
    if (cur.fill != last.fill) return false;
    if (cur.fill.alpha() != 255 || last.fill.alpha() != 255) return false;
    if (cur.gradient || last.gradient) return false;
    if (!cur.patternId.isEmpty() || !last.patternId.isEmpty()) return false;
    if (cur.mesh || last.mesh) return false;
    if (cur.filter || last.filter) return false;
    if (!cur.filterId.isEmpty() || !last.filterId.isEmpty()) return false;
    if (!cur.clipId.isEmpty() || !last.clipId.isEmpty()) return false;
    if (!cur.maskId.isEmpty() || !last.maskId.isEmpty()) return false;
    if (!cur.markerStart.isEmpty() || !last.markerStart.isEmpty()) return false;
    if (!cur.markerMid.isEmpty() || !last.markerMid.isEmpty()) return false;
    if (!cur.markerEnd.isEmpty() || !last.markerEnd.isEmpty()) return false;
    if (cur.hasStroke != last.hasStroke) return false;
    if (cur.hasStroke) {
        if (cur.stroke != last.stroke) return false;
        if (cur.strokeWidth != last.strokeWidth) return false;
        if (cur.stroke.alpha() != 255) return false;
    }
    if (cur.transform != last.transform) return false;
    return true;
}

bool canCoalesceIdentical(const SvgNode& last, const SvgNode& cur) {
    return sameMergePaint(last, cur) && samePathForCoalesce(last.path, cur.path);
}

// Push one leaf with dedup + budget. Returns true when the element was
// handled (pushed or coalesced away); false when dropped by overflow.
bool pushLeaf(QVector<SvgNode>& out, SvgNode&& node, WalkBudget* budget) {
    if (!out.isEmpty() && canCoalesceIdentical(out.last(), node)) return true;
    if (budget) {
        if (budget->overflow) return false;
        if (budget->leaves >= budget->limit) {
            budget->overflow = true;
            if (budget->diag) {
                budget->diag->overflow = true;
                budget->diag->overflowAt = budget->leaves;
            }
            return false;
        }
        if (budget->byteBudget > 0) {
            // Retained weight of this leaf: fixed node overhead, path
            // elements and any decoded bitmap pixels. Merged chunks stay
            // chunk-capped upstream, so the running total tracks memory
            // within a small factor.
            const qint64 est =
                512 + (qint64)node.path.elementCount() * 32 +
                (node.image ? (qint64)node.image->width() *
                                  node.image->height() * 4
                            : 0);
            if (budget->bytes + est > budget->byteBudget) {
                budget->overflow = true;
                if (budget->diag) {
                    budget->diag->overflow = true;
                    budget->diag->overflowAt = budget->leaves;
                }
                return false;
            }
            budget->bytes += est;
        }
        ++budget->leaves;
    }
    out.push_back(std::move(node));
    if (budget && budget->diag) ++budget->diag->leaves;
    return true;
}

// Merge consecutive same-paint opaque leaves into one node, subpaths
// appended in order: pixel-identical output for opaque paint, one layer
// instead of hundreds. The Brazil DDD map holds 5570 paths in a handful of
// same-fill runs; merging keeps the import at hundreds of layers instead of
// 6000+. Runs never cross group boundaries (panel structure preserved), and
// chunks stay spatially tight (union cap) so trimmed rasters and composite
// cells stay sparse instead of going full-canvas.
//
// Runs in a post-pass with one reserve per merged node, so every element
// moves once: O(n). (Merging incrementally inside pushLeaf re-copied the
// accumulator per leaf and turned the walk quadratic.)
void mergeConsecutiveLeaves(QVector<SvgNode>& out) {
    for (SvgNode& n : out) {
        if (n.group) mergeConsecutiveLeaves(n.children);
    }
    if (out.size() < 2) return;
    QVector<SvgNode> res;
    res.reserve(out.size());
    qsizetype i = 0;
    const qsizetype n = out.size();
    while (i < n) {
        SvgNode& first = out[i];
        if (first.group || first.fromText) {
            res.push_back(std::move(first));
            ++i;
            continue;
        }
        qsizetype j = i + 1;
        while (j < n && !out[j].group && !out[j].fromText &&
               sameMergePaint(first, out[j]) &&
               first.path.fillRule() == out[j].path.fillRule())
            ++j;
        if (j == i + 1) {
            res.push_back(std::move(first));
        } else {
            // Split huge runs into bounded chunks. Two bounds, both required
            // for editor speed:
            //  - element count: one 200k-element stroked path stalls a raster
            //    thread; chunks keep raster jobs balanced;
            //  - union growth: merging scattered same-color shapes balloons
            //    the trimmed image to the union bbox, turning every per-pixel
            //    composite blend and GPU upload full-canvas. A chunk whose
            //    union exceeds 16x its members' bbox area splits, so chunks
            //    stay spatially tight (adjacent municipalities merge; far-flung
            //    same-color states don't).
            // Chunks stay consecutive, so output is pixel-identical either way.
            constexpr int kMaxMergeElements = 20000;
            constexpr double kMaxUnionGrowth = 16.0;
            qsizetype k = i;
            while (k < j) {
                int total = 0;
                double memberArea = 0.0;
                QRectF box;
                bool hasBox = false;
                qsizetype m = k;
                while (m < j) {
                    const int ec = out[m].path.elementCount();
                    if (total + ec > kMaxMergeElements) break;
                    const QRectF b = out[m].path.boundingRect();
                    const double a = b.width() * b.height();
                    if (hasBox) {
                        const QRectF u = box.united(b);
                        const double denom =
                            memberArea > 0.0 ? memberArea : 1.0;
                        if (u.width() * u.height() > kMaxUnionGrowth * denom)
                            break;
                        box = u;
                    } else {
                        box = b;
                        hasBox = true;
                    }
                    memberArea += a;
                    total += ec;
                    ++m;
                }
                if (m == k) {
                    // Single path bigger than the cap: keep it whole.
                    ++m;
                    total = out[k].path.elementCount();
                }
                if (m == k + 1) {
                    res.push_back(std::move(out[k]));
                } else {
                    QPainterPath acc;
                    acc.setFillRule(out[k].path.fillRule());
                    acc.reserve(total);
                    for (qsizetype t = k; t < m; ++t) acc.addPath(out[t].path);
                    out[k].path = std::move(acc);
                    res.push_back(std::move(out[k]));
                }
                k = m;
            }
        }
        i = j;
    }
    out = std::move(res);
}

void walkElement(QXmlStreamReader& xml, const QTransform& parentTf,
                 const PaintState& parentPaint, SvgResources& res,
                 QVector<SvgNode>& out, WalkBudget* budget) {
    // Overflow fast-path: consume without building anything so a huge file
    // skips in O(n) streaming time instead of O(n) allocations.
    if (budget && budget->overflow) {
        if (budget->diag) ++budget->diag->cutAfterOverflow;
        xml.skipCurrentElement();
        return;
    }
    const QString tag = xml.name().toString();
    const QXmlStreamAttributes a = xml.attributes();

    QTransform tf = parentTf;
    QString tAttr = readAttr(a, QLatin1String("transform"));
    if (!tAttr.isEmpty()) {
        QTransform t;
        if (parseTransformList(tAttr, &t)) tf = tf * t;
    }

    PaintState ps = parentPaint;
    const QString fillStr = readAttr(a, QLatin1String("fill"));
    const QString strokeStr = readAttr(a, QLatin1String("stroke"));
    const QString fillOp = readAttr(a, QLatin1String("fill-opacity"));
    const QString strokeOp = readAttr(a, QLatin1String("stroke-opacity"));
    const QString opStr = readAttr(a, QLatin1String("opacity"));
    const QString swStr = readAttr(a, QLatin1String("stroke-width"));
    if (!fillStr.isEmpty()) {
        QColor c;
        if (parseColor(fillStr, &c)) {
            ps.hasFill = true;
            ps.fill = c;
        } else if (!fillStr.startsWith(QLatin1String("url("))) {
            ps.hasFill = false;  // fill="none"
        }
    }
    if (!strokeStr.isEmpty()) {
        QColor c;
        if (parseColor(strokeStr, &c)) {
            ps.hasStroke = true;
            ps.stroke = c;
        } else if (!strokeStr.startsWith(QLatin1String("url("))) {
            ps.hasStroke = false;
        }
    }
    if (!fillOp.isEmpty()) ps.fill.setAlphaF(qBound(0.0, parseLength(fillOp, 1.0) * ps.fill.alphaF(), 1.0));
    if (!strokeOp.isEmpty()) ps.stroke.setAlphaF(qBound(0.0, parseLength(strokeOp, 1.0) * ps.stroke.alphaF(), 1.0));
    if (!opStr.isEmpty()) ps.opacity *= qBound(0.0, parseLength(opStr, 1.0), 1.0);
    if (!swStr.isEmpty()) ps.strokeWidth = qMax(0.0, parseLength(swStr, 1.0));
    // Fill rule inherits like any presentation attribute; absent means the
    // spec initial value (nonzero). Glyph paths from <text> keep the
    // toolkit/font default and never take this path.
    const QString fillRuleStr = readAttr(a, QLatin1String("fill-rule"));
    if (!fillRuleStr.isEmpty()) {
        if (fillRuleStr.trimmed().compare(QLatin1String("nonzero"),
                                          Qt::CaseInsensitive) == 0)
            ps.fillEvenOdd = false;
        else if (fillRuleStr.trimmed().compare(QLatin1String("evenodd"),
                                               Qt::CaseInsensitive) == 0)
            ps.fillEvenOdd = true;
    }

    if (tag == QLatin1String("g")) {
        SvgNode node;
        node.group = true;
        const QString id = a.value(QLatin1String("id")).toString();
        node.name = id.isEmpty() ? QStringLiteral("Group") : id;
        node.opacity = ps.opacity;
        node.transform = tf;
        walkChildren(xml, tf, ps, res, node.children, budget);
        // Drop the row entirely when it has nothing (empty group).
        if (!node.children.isEmpty()) {
            out.push_back(std::move(node));
            if (budget && budget->diag) ++budget->diag->groups;
        }
        return;
    }

    if (tag == QLatin1String("defs")) {
        QHash<QString, std::shared_ptr<SvgGradient>> extra;
        collectDefs(xml, extra);
        for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
            if (!res.grads.contains(it.key())) res.grads.insert(it.key(), it.value());
        return;
    }

    // flowRoot: flowDiv text into flowRegion columns (text-in-shape).
    if (tag == QLatin1String("flowRoot")) {
        QList<QRectF> cols;
        QString flowText;
        while (!xml.atEnd()) {
            const auto tok = xml.readNext();
            if (tok == QXmlStreamReader::EndElement &&
                xml.name() == QLatin1String("flowRoot"))
                break;
            if (tok != QXmlStreamReader::StartElement) {
                if (tok == QXmlStreamReader::Characters) flowText += xml.text();
                continue;
            }
            const QString ct = xml.name().toString();
            if (ct == QLatin1String("flowRegion")) {
                while (xml.readNextStartElement()) {
                    if (xml.name() == QLatin1String("rect")) {
                        const QXmlStreamAttributes ra = xml.attributes();
                        cols.push_back(QRectF(
                            parseLength(readAttr(ra, QLatin1String("x")), 0.0),
                            parseLength(readAttr(ra, QLatin1String("y")), 0.0),
                            parseLength(readAttr(ra, QLatin1String("width")), 0.0),
                            parseLength(readAttr(ra, QLatin1String("height")), 0.0)));
                    }
                    xml.skipCurrentElement();
                }
            } else if (ct == QLatin1String("flowDiv") || ct == QLatin1String("flowPara") ||
                       ct == QLatin1String("flowSpan") || ct == QLatin1String("flowLine")) {
                // Descend: text accumulates via the Characters branch above.
            } else {
                xml.skipCurrentElement();
            }
        }
        QPainterPath path = flowTextGlyphs(a, flowText, cols);
        if (path.isEmpty()) return;
        SvgNode node;
        node.group = false;
        if (budget && budget->diag) ++budget->diag->flowTexts;
        node.fromText = true;
        node.name = a.value(QLatin1String("id")).toString();
        if (node.name.isEmpty()) node.name = tag;
        node.transform = tf;
        node.opacity = ps.opacity;
        node.path = path;
        node.hasFill = ps.hasFill;
        node.fill = ps.fill;
        node.hasStroke = ps.hasStroke;
        node.stroke = ps.stroke;
        node.strokeWidth = ps.strokeWidth;
        static_cast<void>(pushLeaf(out, std::move(node), budget));
        return;
    }

    // Shape elements: build the user-space path.
    QPainterPath path;
    QString textContent;
    QString textPathHref;
    std::shared_ptr<QImage> image;
    quint8 imageFit = 0;
    if (tag == QLatin1String("image")) {
        // Embedded raster: decode once, geometry = placement rect. Decode
        // failures count as dropped images (warning + degraded verdict).
        QString href = a.value(QLatin1String("href")).toString();
        if (href.isEmpty())
            href = a.value(QLatin1String("xlink:href")).toString();
        image = decodeImageDataUri(href, res);
        const double ix = parseLength(readAttr(a, QLatin1String("x")), 0.0);
        const double iy = parseLength(readAttr(a, QLatin1String("y")), 0.0);
        const double iw = parseLength(readAttr(a, QLatin1String("width")), 0.0);
        const double ih = parseLength(readAttr(a, QLatin1String("height")), 0.0);
        imageFit =
            imageFitAttr(readAttr(a, QLatin1String("preserveAspectRatio")));
        if (!image || !(iw > 0.0) || !(ih > 0.0)) {
            if (!image) ++res.imagesDropped;
            if (budget && budget->diag) ++budget->diag->skippedLeaves;
            xml.skipCurrentElement();
            return;
        }
        path.addRect(QRectF(ix, iy, iw, ih));
    } else if (tag == QLatin1String("text")) {
        // Structured read: plain runs, tspans and one textPath (consumes end).
        while (!xml.atEnd()) {
            const auto tok = xml.readNext();
            if (tok == QXmlStreamReader::EndElement &&
                xml.name() == QLatin1String("text"))
                break;
            if (tok == QXmlStreamReader::Characters) {
                textContent += xml.text();
            } else if (tok == QXmlStreamReader::StartElement) {
                const QString ct = xml.name().toString();
                if (ct == QLatin1String("textPath")) {
                    const QXmlStreamAttributes ta = xml.attributes();
                    const QString href =
                        ta.value(QLatin1String("href")).toString().isEmpty()
                            ? ta.value(QLatin1String("xlink:href")).toString()
                            : ta.value(QLatin1String("href")).toString();
                    if (textPathHref.isEmpty()) textPathHref = refTarget(href);
                    textContent += xml.readElementText();
                } else if (ct == QLatin1String("tspan") || ct == QLatin1String("tref")) {
                    textContent += xml.readElementText();
                } else {
                    xml.skipCurrentElement();
                }
            }
        }
        if (!textPathHref.isEmpty()) {
            path = textOnPathGlyphs(a, textContent, textPathHref, res);
            if (path.isEmpty())
                path = textGlyphPath(a, textContent, nullptr);
        } else {
            path = textGlyphPath(a, textContent, nullptr);
        }
    } else {
        auto get = [&](QLatin1String k) -> QString { return readAttr(a, k); };
        if (!buildShapeGeometry(tag, get, path)) {
            // Unknown/unsupported element — skip its subtree.
            if (budget && budget->diag) {
                ++budget->diag->skippedLeaves;
                noteCapped(budget->diag->skipTags, tag);
                // TEMP-DIAG (revert before commit): identify skipped leaves.
                std::fprintf(stderr, "[skip] tag=%s id=%s d=%.80s\n",
                             tag.toUtf8().constData(),
                             a.value(QLatin1String("id")).toString().toUtf8().constData(),
                             get(QLatin1String("d")).toUtf8().constData());
            }
            xml.skipCurrentElement();
            return;
        }
        // Resolved rule always wins over the toolkit default, so dense
        // self-overlapping map polygons rasterize per spec (nonzero unless
        // an evenodd is inherited or specified). Glyph paths never pass here.
        path.setFillRule(ps.fillEvenOdd ? Qt::OddEvenFill : Qt::WindingFill);
    }

    // All shape elements except <text>/<flowRoot> still sit at their start tag;
    // move past the body (which may hold <title>/<desc> children).
    if (tag != QLatin1String("text")) xml.skipCurrentElement();
    const bool isTextLeaf = (tag == QLatin1String("text"));
    if (path.isEmpty()) {
        // Blank text (whitespace runs) is benign; empty geometry otherwise
        // means a shape that drew nothing (empty d=, zero-area rect, ...).
        // Counted apart from skippedLeaves: nothing is lost, so it must not
        // flip the verdict to degraded.
        if (budget && budget->diag) {
            if (isTextLeaf) ++budget->diag->blankTexts;
            else ++budget->diag->emptyLeaves;
        }
        return;
    }
    if (budget && budget->diag) {
        if (isTextLeaf) {
            ++budget->diag->texts;
            if (!textPathHref.isEmpty()) ++budget->diag->textPaths;
        }
    }

    // Register rebuildable geometry for <textPath> targets (skipped when the
    // document has none: retaining 6KB d-strings per id'd path is pure waste
    // on map files).
    const QString nodeId = a.value(QLatin1String("id")).toString();
    const bool wantRefs = !budget || budget->needShapeRefs;
    if (wantRefs && !nodeId.isEmpty() && !res.shapeRefs.contains(nodeId) &&
        tag != QLatin1String("text")) {
        SvgShapeRef ref;
        ref.tag = tag;
        ref.attrs = a;
        res.shapeRefs.insert(nodeId, std::move(ref));
    }

    // Resolve url(#id) fills: gradients first, then pattern cells.
    std::shared_ptr<SvgGradient> grad;
    QString patternId;
    QString patternXform;
    const QString fillStr2 = readAttr(a, QLatin1String("fill"));
    if (fillStr2.startsWith(QLatin1String("url(#"))) {
        const QString id = fillStr2.mid(5, fillStr2.indexOf(QLatin1Char(')')) - 5);
        const auto it = res.grads.constFind(id);
        if (it != res.grads.constEnd()) {
            ps.hasFill = true;
            grad = *it;
            if (budget && budget->diag) ++budget->diag->gradUses;
        } else if (res.patterns.contains(id)) {
            ps.hasFill = true;
            patternId = id;
            patternXform = res.patterns.value(id).xform;
            if (budget && budget->diag) ++budget->diag->patternUses;
        } else if (budget && budget->diag && !id.isEmpty()) {
            // Dangling paint server: the shape keeps its inherited paint.
            noteCapped(budget->diag->missingRefs,
                       QStringLiteral("fill#") + id);
        }
    }
    // data-mesh="meshN" selects a retained mesh gradient (flat fallback stays).
    std::shared_ptr<pittore::vector::MeshGradient> mesh;
    const QString meshRef = readAttr(a, QLatin1String("data-mesh"));
    if (!meshRef.isEmpty()) {
        const auto it = res.meshes.constFind(meshRef);
        if (it != res.meshes.constEnd()) {
            mesh = std::make_shared<pittore::vector::MeshGradient>(it.value());
            if (budget && budget->diag) ++budget->diag->meshUses;
        } else if (budget && budget->diag) {
            noteCapped(budget->diag->missingRefs,
                       QStringLiteral("mesh#") + meshRef);
        }
    }

    SvgNode node;
    node.group = false;
    const bool isText = (tag == QLatin1String("text"));
    node.fromText = isText;
    node.name = nodeId.isEmpty() ? tag : nodeId;
    node.transform = tf;
    node.opacity = ps.opacity;
    node.path = path;
    node.hasFill = ps.hasFill;
    node.fill = ps.fill;
    node.gradient = grad;
    node.patternId = patternId;
    node.patternTransform = patternXform;
    node.mesh = mesh;
    node.hasStroke = ps.hasStroke;
    node.stroke = ps.stroke;
    node.strokeWidth = ps.strokeWidth;
    node.markerStart = refTarget(readAttr(a, QLatin1String("marker-start")));
    node.markerMid = refTarget(readAttr(a, QLatin1String("marker-mid")));
    node.markerEnd = refTarget(readAttr(a, QLatin1String("marker-end")));
    {
        const QString mall = readAttr(a, QLatin1String("marker"));
        if (!mall.isEmpty() && mall != QLatin1String("none")) {
            const QString t = refTarget(mall);
            if (!t.isEmpty()) node.markerStart = node.markerMid = node.markerEnd = t;
        }
    }
    node.clipId = refTarget(readAttr(a, QLatin1String("clip-path")));
    node.maskId = refTarget(readAttr(a, QLatin1String("mask")));
    node.filterId = refTarget(readAttr(a, QLatin1String("filter")));
    if (!node.filterId.isEmpty()) {
        const auto it = res.filters.constFind(node.filterId);
        if (it != res.filters.constEnd())
            node.filter =
                std::make_shared<pittore::vector::FilterGraph>(it.value());
    }
    if (budget && budget->diag) {
        SvgImportDiag* dd = budget->diag;
        if (!node.clipId.isEmpty()) {
            ++dd->clipUses;
            if (!res.clips.contains(node.clipId))
                noteCapped(dd->missingRefs,
                           QStringLiteral("clip#") + node.clipId);
        }
        if (!node.maskId.isEmpty()) {
            ++dd->maskUses;
            if (!res.masks.contains(node.maskId))
                noteCapped(dd->missingRefs,
                           QStringLiteral("mask#") + node.maskId);
        }
        if (!node.filterId.isEmpty()) {
            ++dd->filterUses;
            if (!node.filter)
                noteCapped(dd->missingRefs,
                           QStringLiteral("filter#") + node.filterId);
        }
        const QStringList mids = {node.markerStart, node.markerMid,
                                  node.markerEnd};
        bool anyMarker = false;
        for (const QString& mid : mids) {
            if (mid.isEmpty()) continue;
            anyMarker = true;
            if (!res.markers.contains(mid))
                noteCapped(dd->missingRefs,
                           QStringLiteral("marker#") + mid);
        }
        if (anyMarker) ++dd->markerUses;
    }
    if (image) {
        node.image = std::move(image);
        node.imageFit = imageFit;
        // The bitmap is the paint: no fill, stroke or markers of its own.
        node.hasFill = false;
        node.hasStroke = false;
        node.patternId.clear();
        node.markerStart.clear();
        node.markerMid.clear();
        node.markerEnd.clear();
    }
    static_cast<void>(pushLeaf(out, std::move(node), budget));
}

void walkChildren(QXmlStreamReader& xml, const QTransform& tf, const PaintState& ps,
                  SvgResources& res, QVector<SvgNode>& out, WalkBudget* budget) {
    while (xml.readNextStartElement()) {
        walkElement(xml, tf, ps, res, out, budget);
        // Overflow at top level: keep consuming cheaply so the reader stays
        // in sync; each remaining leaf is skipped without allocation.
        if (budget && budget->overflow && out.size() > 0) {
            // Do not break: the caller discards on overflow and the flatten
            // fallback re-parses from scratch. Continuing here only skips.
        }
    }
}

// Content bounds (user units, root transform applied) for the size fallback.
QRectF contentBounds(const QVector<SvgNode>& nodes) {
    QRectF b;
    for (const SvgNode& n : nodes) {
        if (n.group) b |= contentBounds(n.children);
        else b |= n.transform.mapRect(n.path.boundingRect());
    }
    return b;
}

void applyRoot(QVector<SvgNode>& nodes, const QTransform& root) {
    for (SvgNode& n : nodes) {
        n.transform = n.transform * root;  // user→root-unit, then→doc
        applyRoot(n.children, root);
    }
}

// ---------------------------------------------------------------------------
// Rasterizer
// ---------------------------------------------------------------------------

double transformScale(const QTransform& t) {
    const double sx = std::hypot(t.m11(), t.m12());
    const double sy = std::hypot(t.m21(), t.m22());
    return std::max(sx, std::max(sy, 1e-9));
}

QBrush makeBrush(const SvgNode& n, const QPainterPath& path) {
    if (n.gradient) {
        const QRectF b = path.boundingRect();
        if (n.gradient->radial) {
            QRadialGradient g(
                n.gradient->userSpace ? QPointF(n.gradient->cx, n.gradient->cy)
                                      : QPointF(b.x() + b.width() * n.gradient->cx,
                                                b.y() + b.height() * n.gradient->cy),
                n.gradient->userSpace ? n.gradient->r
                                      : std::max(b.width(), b.height()) * n.gradient->r);
            for (const auto& s : n.gradient->stops)
                g.setColorAt(qBound(0.0, s.offset, 1.0), s.color);
            return QBrush(g);
        }
        QLinearGradient g(
            n.gradient->userSpace ? QPointF(n.gradient->x1, n.gradient->y1)
                                  : QPointF(b.x() + b.width() * n.gradient->x1,
                                            b.y() + b.height() * n.gradient->y1),
            n.gradient->userSpace ? QPointF(n.gradient->x2, n.gradient->y2)
                                  : QPointF(b.x() + b.width() * n.gradient->x2,
                                            b.y() + b.height() * n.gradient->y2));
        for (const auto& s : n.gradient->stops)
            g.setColorAt(qBound(0.0, s.offset, 1.0), s.color);
        return QBrush(g);
    }
    return QBrush(n.fill);
}

// Collects one subtree in panel order: a group header row, then each child
// subtree from top-most (last painted) down. Leaves are rasterized later —
// each goes into its own independent QImage/QPainter, so the rasterizer can
// run them in parallel across the CPU cores.
struct RasterJob {
    const SvgNode* node = nullptr;
    const SvgResources* res = nullptr;
    int depth = 0;
    bool isGroup = false;
    bool isFlattened = false;  // group row with pre-flattened shared raster
    QImage pixels;      // leaf output (null when dropped or a group row)
    QPointF offset;     // leaf output
    std::shared_ptr<pittore::vector::ArtNode> art;  // leaf vector geometry
};

// Classic flattened row: every child a flattenable leaf, so the ArtNode
// fast path covers the whole row. Anything else (nested groups, clips,
// masks, text) needs the retained subtree for exact repaints.
bool flattenableLeaf(const SvgNode& n);
bool rowIsClassicFlat(const SvgNode& n) {
    for (const SvgNode& c : n.children)
        if (c.group || !flattenableLeaf(c)) return false;
    return true;
}

void collectRasterJobs(const QVector<SvgNode>& children, const SvgResources& res,
                       int depth, std::vector<RasterJob>& jobs) {
    for (auto it = children.crbegin(); it != children.crend(); ++it) {
        const SvgNode& n = *it;
        if (n.group && !n.flattened) {
            jobs.push_back(RasterJob{&n, &res, depth, true, false, {}, {}, {}});
            collectRasterJobs(n.children, res, depth + 1, jobs);
            continue;
        }
        if (n.group) {
            // Flattened group: one raster job for the shared image.
            jobs.push_back(RasterJob{&n, &res, depth, false, true, {}, {}, {}});
            continue;
        }
        jobs.push_back(RasterJob{&n, &res, depth, false, false, {}, {}, {}});
    }
}

// Convert a Qt path into the engine's unflattened segments, emitting a Close
// for every subpath that returns to its start (rects, ellipses, closed
// polygons and explicit 'Z' commands).
std::vector<pittore::vector::Segment> pathSegments(const QPainterPath& path) {
    using Seg = pittore::vector::Segment;
    std::vector<Seg> out;
    QPointF start, last;
    bool open = false;
    auto endSubpath = [&] {
        if (!open || std::abs(last.x() - start.x()) > 1e-6 ||
            std::abs(last.y() - start.y()) > 1e-6)
            return;
        // A straight return to the start becomes the close itself.
        if (!out.empty() && out.back().kind == Seg::Kind::LineTo) out.pop_back();
        Seg s;
        s.kind = Seg::Kind::Close;
        out.push_back(s);
    };
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        if (e.type == QPainterPath::MoveToElement) {
            endSubpath();
            Seg s;
            s.kind = Seg::Kind::MoveTo;
            s.x = static_cast<float>(e.x);
            s.y = static_cast<float>(e.y);
            out.push_back(s);
            start = last = QPointF(e.x, e.y);
            open = true;
        } else if (e.type == QPainterPath::LineToElement) {
            Seg s;
            s.kind = Seg::Kind::LineTo;
            s.x = static_cast<float>(e.x);
            s.y = static_cast<float>(e.y);
            out.push_back(s);
            last = QPointF(e.x, e.y);
        } else if (e.type == QPainterPath::CurveToElement &&
                   i + 2 < path.elementCount()) {
            const QPainterPath::Element c2 = path.elementAt(i + 1);
            const QPainterPath::Element end = path.elementAt(i + 2);
            Seg s;
            s.kind = Seg::Kind::CubicTo;
            s.c1x = static_cast<float>(e.x);
            s.c1y = static_cast<float>(e.y);
            s.c2x = static_cast<float>(c2.x);
            s.c2y = static_cast<float>(c2.y);
            s.x = static_cast<float>(end.x);
            s.y = static_cast<float>(end.y);
            out.push_back(s);
            last = QPointF(end.x, end.y);
            i += 2;
        }
    }
    endSubpath();
    return out;
}

void rgbaOf(const QColor& c, std::uint8_t out[4]) {
    const QColor s = c.isValid() ? c : QColor(0, 0, 0);
    out[0] = static_cast<std::uint8_t>(qBound(0, s.red(), 255));
    out[1] = static_cast<std::uint8_t>(qBound(0, s.green(), 255));
    out[2] = static_cast<std::uint8_t>(qBound(0, s.blue(), 255));
    out[3] = static_cast<std::uint8_t>(qBound(0, s.alpha(), 255));
}

// The paint of a node, with gradient coordinates resolved into user space the
// same way makeBrush() resolves them for the rasterizer, so the exported SVG
// shades identically.
void fillArtPaint(const SvgNode& n, pittore::vector::ArtPaint* paint) {
    paint->hasFill = n.hasFill;
    rgbaOf(n.fill, paint->fill);
    if (n.hasFill && n.gradient) {
        const QRectF b = n.path.boundingRect();
        const SvgGradient& src = *n.gradient;
        pittore::vector::ArtGradient& g = paint->gradient;
        paint->hasGradient = true;
        g.radial = src.radial;
        if (src.radial) {
            if (src.userSpace) {
                g.cx = src.cx;
                g.cy = src.cy;
                g.r = src.r;
            } else {
                g.cx = b.x() + b.width() * src.cx;
                g.cy = b.y() + b.height() * src.cy;
                g.r = std::max(b.width(), b.height()) * src.r;
            }
        } else if (src.userSpace) {
            g.x1 = src.x1;
            g.y1 = src.y1;
            g.x2 = src.x2;
            g.y2 = src.y2;
        } else {
            g.x1 = b.x() + b.width() * src.x1;
            g.y1 = b.y() + b.height() * src.y1;
            g.x2 = b.x() + b.width() * src.x2;
            g.y2 = b.y() + b.height() * src.y2;
        }
        g.stops.reserve(src.stops.size());
        for (const SvgGradient::Stop& s : src.stops) {
            pittore::vector::ArtStop stop;
            stop.pos = static_cast<float>(qBound(0.0, s.offset, 1.0));
            rgbaOf(s.color, stop.rgba);
            g.stops.push_back(stop);
        }
    }
    paint->hasStroke = n.hasStroke;
    rgbaOf(n.stroke, paint->stroke);
    paint->strokeWidth = std::max(0.01, n.strokeWidth);
    paint->patternId = n.patternId.toStdString();
    paint->hasPatternXform = false;
    if (!n.patternTransform.isEmpty()) {
        QTransform xf;
        if (parseTransformList(n.patternTransform, &xf)) {
            paint->hasPatternXform = true;
            paint->patternXform[0] = xf.m11();
            paint->patternXform[1] = xf.m12();
            paint->patternXform[2] = xf.m21();
            paint->patternXform[3] = xf.m22();
            paint->patternXform[4] = xf.dx();
            paint->patternXform[5] = xf.dy();
        }
    }
    paint->markerStart = n.markerStart.toStdString();
    paint->markerMid = n.markerMid.toStdString();
    paint->markerEnd = n.markerEnd.toStdString();
    paint->clipId = n.clipId.toStdString();
    paint->maskId = n.maskId.toStdString();
    paint->hasMesh = n.mesh && !n.mesh->patches.empty();
    if (paint->hasMesh) paint->mesh = *n.mesh;
    paint->hasFilter = n.filter && !n.filter->prims.empty();
    if (paint->hasFilter) paint->filter = *n.filter;
}

// Pattern tile → texture brush at the import scale (cell shapes paint with
// their own flat/gradient fills, so document patterns rasterize faithfully).
QBrush patternBrushFor(const SvgPatternCell& cell, double scale) {
    const int tw = qMax(1, qRound(cell.w * scale));
    const int th = qMax(1, qRound(cell.h * scale));
    QImage tile(tw, th, QImage::Format_ARGB32_Premultiplied);
    tile.fill(Qt::transparent);
    QPainter tp(&tile);
    tp.setRenderHint(QPainter::Antialiasing, true);
    tp.setRenderHint(QPainter::SmoothPixmapTransform, true);
    tp.scale(scale, scale);
    tp.translate(-cell.x, -cell.y);
    for (const SvgNode& s : cell.shapes) {
        if (s.image && !s.image->isNull()) {
            drawImageFitted(tp, *s.image,
                            s.transform.mapRect(s.path.boundingRect()),
                            s.imageFit);
            continue;
        }
        const QPainterPath sp = s.transform.map(s.path);
        if (s.hasFill) {
            tp.setBrush(s.gradient ? makeBrush(s, sp) : QBrush(s.fill));
        } else {
            tp.setBrush(Qt::NoBrush);
        }
        tp.setPen(Qt::NoPen);
        tp.drawPath(sp);
    }
    tp.end();
    QBrush b(QPixmap::fromImage(std::move(tile)));
    if (!cell.xform.isEmpty()) {
        QTransform pt;
        if (parseTransformList(cell.xform, &pt)) b.setTransform(pt);
    }
    return b;
}

// Mesh paint: engine-rasterized lattice stretched over its bbox, clipped to
// the shape (mesh coordinates are user-space, like the path).
void paintMeshOverlay(QPainter& p, const SvgNode& n) {
    if (!n.mesh || n.mesh->patches.empty()) return;
    double x0 = 1e100, y0 = 1e100, x1 = -1e100, y1 = -1e100;
    for (const auto& patch : n.mesh->patches)
        for (const auto& pt : patch.p) {
            x0 = qMin(x0, pt[0]);
            y0 = qMin(y0, pt[1]);
            x1 = qMax(x1, pt[0]);
            y1 = qMax(y1, pt[1]);
        }
    if (!(x1 > x0 && y1 > y0)) return;
    const int w = qBound(1, qRound(x1 - x0), 2048);
    const int h = qBound(1, qRound(y1 - y0), 2048);
    const std::vector<std::uint8_t> px = pittore::vector::rasterizeMesh(*n.mesh, w, h);
    QImage img((const uchar*)px.data(), w, h, QImage::Format_RGBA8888);
    p.save();
    p.setClipPath(n.path, Qt::IntersectClip);
    p.drawImage(QRectF(x0, y0, x1 - x0, y1 - y0), img);
    p.restore();
}

void paintCellShape(QPainter& p, const SvgNode& s, const QTransform& t) {
    const QPainterPath sp = t.map(s.transform.map(s.path));
    if (s.hasFill) p.setBrush(s.gradient ? makeBrush(s, sp) : QBrush(s.fill));
    else p.setBrush(Qt::NoBrush);
    if (s.hasStroke) {
        QPen pen(s.stroke, std::max(0.01, s.strokeWidth));
        pen.setCosmetic(false);
        p.setPen(pen);
        if (!s.hasFill) p.setBrush(Qt::NoBrush);
    } else {
        p.setPen(Qt::NoPen);
    }
    p.drawPath(sp);
}

// Marker ornaments at path vertices (orient auto follows the tangent).
void paintMarkers(QPainter& p, const SvgNode& n, const SvgResources& res) {
    if (n.markerStart.isEmpty() && n.markerMid.isEmpty() && n.markerEnd.isEmpty())
        return;
    const double sw = std::max(0.01, n.strokeWidth);
    const QList<QPolygonF> polys = n.path.toSubpathPolygons();
    for (const QPolygonF& poly : polys) {
        if (poly.size() < 2) continue;
        std::vector<std::pair<double, double>> pts;
        for (const QPointF& q : poly) pts.emplace_back(q.x(), q.y());
        const bool closed =
            QLineF(poly.first(), poly.last()).length() < 1e-6;
        const auto insts = pittore::vector::markersForPolyline(
            pts, closed, n.markerStart.toStdString(), n.markerMid.toStdString(),
            n.markerEnd.toStdString(), sw);
        for (const auto& inst : insts) {
            const auto it = res.markers.constFind(QString::fromStdString(inst.markerId));
            if (it == res.markers.constEnd()) continue;
            const SvgMarkerCell& cell = it.value();
            const double s = cell.strokeUnits ? inst.scale : 1.0;
            QTransform t;
            t.translate(inst.x, inst.y);
            t.rotate(inst.rotationDeg + cell.deg);
            t.scale(s, s);
            t.translate(-cell.refX, -cell.refY);
            p.save();
            p.setOpacity(qBound(0.0, n.opacity, 1.0));
            for (const SvgNode& s2 : cell.shapes) paintCellShape(p, s2, t);
            p.restore();
        }
    }
}

QPainterPath clipUserPath(const SvgNode& n, const SvgResources& res) {
    QPainterPath clip;
    if (n.clipId.isEmpty()) return clip;
    const auto it = res.clips.constFind(n.clipId);
    if (it == res.clips.constEnd()) return clip;
    for (const SvgNode& s : it.value().shapes)
        clip = clip.united(s.transform.map(s.path));
    return clip;
}

// Mask: luminance of the mask art scales the layer coverage (DestinationIn
// by hand so steep mask stacks stay exact).
void applyMaskAlpha(QImage& out, const SvgNode& n, const SvgResources& res,
                    const QRect& ir) {
    if (n.maskId.isEmpty()) return;
    const auto it = res.masks.constFind(n.maskId);
    if (it == res.masks.constEnd()) return;
    QImage mask(ir.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    QPainter mp(&mask);
    mp.setRenderHint(QPainter::Antialiasing, true);
    mp.translate(-ir.x(), -ir.y());
    mp.setTransform(n.transform, true);
    for (const SvgNode& s : it.value().shapes) {
        const QPainterPath sp = s.transform.map(s.path);
        mp.setBrush(s.gradient ? makeBrush(s, sp) : QBrush(Qt::white));
        mp.setPen(Qt::NoPen);
        mp.setOpacity(qBound(0.0, n.opacity, 1.0));
        mp.drawPath(sp);
    }
    mp.end();
    for (int y = 0; y < out.height(); y++) {
        const QRgb* mrow = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
        QRgb* orow = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < out.width(); x++) {
            const int lum =
                (qRed(mrow[x]) * 77 + qGreen(mrow[x]) * 150 + qBlue(mrow[x]) * 29) >> 8;
            const int k = lum * qAlpha(mrow[x]) / 255;
            orow[x] = qRgba(qRed(orow[x]) * k / 255, qGreen(orow[x]) * k / 255,
                            qBlue(orow[x]) * k / 255, qAlpha(orow[x]) * k / 255);
        }
    }
}

void applyNodeFilter(QImage& img, const pittore::vector::FilterGraph& g) {
    if (g.prims.empty() || img.isNull()) return;
    pittore::vector::RgbaImage src;
    src.w = img.width();
    src.h = img.height();
    src.px.resize((size_t)src.w * src.h * 4);
    for (int y = 0; y < src.h; y++) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < src.w; x++) {
            const int a = qAlpha(row[x]);
            size_t o = (size_t)(y * src.w + x) * 4;
            if (a == 0) {
                src.px[o] = src.px[o + 1] = src.px[o + 2] = src.px[o + 3] = 0;
            } else {
                src.px[o] = (std::uint8_t)(qRed(row[x]) * 255 / a);
                src.px[o + 1] = (std::uint8_t)(qGreen(row[x]) * 255 / a);
                src.px[o + 2] = (std::uint8_t)(qBlue(row[x]) * 255 / a);
                src.px[o + 3] = (std::uint8_t)a;
            }
        }
    }
    const pittore::vector::RgbaImage dst = pittore::vector::applyFilterGraph(g, src);
    if (dst.w != src.w || dst.h != src.h) return;
    for (int y = 0; y < src.h; y++) {
        QRgb* row = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < src.w; x++) {
            size_t o = (size_t)(y * src.w + x) * 4;
            const int a = dst.px[o + 3];
            row[x] = qRgba(dst.px[o] * a / 255, dst.px[o + 1] * a / 255,
                           dst.px[o + 2] * a / 255, a);
        }
    }
}

// Rasterize one leaf part into its own small image. Patterns, meshes,
// markers, clips, masks and retained filter graphs all paint here, so what
// the canvas shows matches the exported SVG. Returns false when the part has
// no drawable footprint (empty or absurdly large).
// The per-leaf paint body shared by rasterizePart (fresh trimmed painter)
// and the flattened-group rasterizer (shared painter, save/restored per
// leaf): opacity, transform, clip, brush, pen, path, mesh and markers.
// Single source of truth so the two raster paths can never diverge.
void paintLeafBody(QPainter& p, const SvgNode& n, const SvgResources& res) {
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setOpacity(qBound(0.0, n.opacity, 1.0));
    p.setTransform(n.transform, true);
    const QPainterPath clip = clipUserPath(n, res);
    if (!clip.isEmpty()) p.setClipPath(clip, Qt::IntersectClip);
    const double sc = transformScale(n.transform);
    if (n.image && !n.image->isNull()) {
        // Embedded raster: bitmap placement (no vector brush at all).
        drawImageFitted(p, *n.image, n.path.boundingRect(), n.imageFit);
    } else if (!n.patternId.isEmpty()) {
        const auto it = res.patterns.constFind(n.patternId);
        if (it != res.patterns.constEnd()) {
            QBrush pb = patternBrushFor(it.value(), sc);
            // Per-node patternTransform composes over the def matrix.
            if (!n.patternTransform.isEmpty()) {
                QTransform xf;
                if (parseTransformList(n.patternTransform, &xf))
                    pb.setTransform(xf * pb.transform());
            }
            p.setBrush(pb);
        } else if (n.hasFill) {
            p.setBrush(makeBrush(n, n.path));
        } else {
            p.setBrush(Qt::NoBrush);
        }
    } else if (n.hasFill) {
        p.setBrush(makeBrush(n, n.path));
    } else {
        p.setBrush(Qt::NoBrush);
    }
    if (n.hasStroke) {
        QPen pen(n.stroke, std::max(0.01, n.strokeWidth));
        pen.setCosmetic(false);
        p.setPen(pen);
        p.setBrush(n.hasFill || !n.patternId.isEmpty() ? p.brush() : Qt::NoBrush);
    } else {
        p.setPen(Qt::NoPen);
    }
    if (n.hasFill || !n.patternId.isEmpty() || n.hasStroke) {
        p.drawPath(n.path);
    }
    if (n.mesh) paintMeshOverlay(p, n);
    paintMarkers(p, n, res);
}

// Vector geometry behind one leaf's raster, mapped into the layer's source
// space: subtract the trimmed origin the raster sits at, so the layer's own
// offset/scale still move and resize the vector with the pixels.
std::shared_ptr<pittore::vector::ArtNode> makeLeafArt(const SvgNode& n,
                                                      const QPointF& origin) {
    auto node = std::make_shared<pittore::vector::ArtNode>();
    node->name = n.name.toStdString();
    node->segments = pathSegments(n.path);
    const QTransform local =
        n.transform * QTransform::fromTranslate(-origin.x(), -origin.y());
    node->matrix[0] = local.m11();
    node->matrix[1] = local.m12();
    node->matrix[2] = local.m21();
    node->matrix[3] = local.m22();
    node->matrix[4] = local.dx();
    node->matrix[5] = local.dy();
    node->opacity = qBound(0.0, n.opacity, 1.0);
    node->evenOdd = n.path.fillRule() == Qt::OddEvenFill;
    fillArtPaint(n, &node->paint);
    return node;
}

bool rasterizePart(const SvgNode& n, QImage* img, QPointF* offset,
                   std::shared_ptr<pittore::vector::ArtNode>* art,
                   const SvgResources& res) {
    const QRectF userBounds = n.path.boundingRect();
    const QRectF docBounds = n.transform.mapRect(userBounds);
    const double sc = transformScale(n.transform);
    double margin = n.hasStroke ? n.strokeWidth * sc / 2.0 + 1.0 : 1.0;
    if (!n.markerStart.isEmpty() || !n.markerMid.isEmpty() || !n.markerEnd.isEmpty())
        margin = qMax(margin, n.strokeWidth * sc * 4.0 + 2.0);
    // A plain embedded bitmap placed axis-aligned on integer pixels carries
    // no antialiased fringe past its rect, so rasterize it margin-free: the
    // padding is transparent, and a later smooth resample would fade the
    // true edge texels into it while a direct paint clamps at them (tile
    // simple-blit vs direct-paint mismatch). Anything that can paint past
    // the rect (stroke, markers, filters, mesh) or needs sub-pixel fringe
    // (rotated/skewed/fractional placement) keeps its margin.
    if (margin > 0.0 && n.image && !n.image->isNull() && !n.hasStroke &&
        n.markerStart.isEmpty() && n.markerMid.isEmpty() &&
        n.markerEnd.isEmpty() && !n.filter && !n.mesh &&
        (n.transform.type() == QTransform::TxTranslate ||
         n.transform.type() == QTransform::TxNone) &&
        docBounds.x() == std::floor(docBounds.x()) &&
        docBounds.y() == std::floor(docBounds.y()))
        margin = 0.0;
    const QRectF grown = docBounds.adjusted(-margin, -margin, margin, margin);
    const QRect ir = grown.toAlignedRect();
    if (ir.width() <= 0 || ir.height() <= 0 || ir.width() > 16384 ||
        ir.height() > 16384)
        return false;
    QImage out(ir.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(-ir.x(), -ir.y());
    paintLeafBody(p, n, res);
    p.end();
    applyMaskAlpha(out, n, res, ir);
    if (n.filter) applyNodeFilter(out, *n.filter);
    *img = std::move(out);
    *offset = QPointF(ir.x(), ir.y());
    // Bitmaps have no vector geometry to retain (and the rect path would
    // export as a painted shape); pixels carry them instead.
    if (art) *art = n.image ? nullptr : makeLeafArt(n, *offset);
    return true;
}

// A leaf is flattenable into a shared raster row when painting it is a
// plain sequential draw with no isolated effects and no double-apply skew:
// unit opacity (the per-layer path would apply it twice: baked raster plus
// layer opacity), no pattern/mesh/filter/clip/mask/markers (phase- or
// temp-dependent), text excluded (labels stay crisp vector rows) and
// bitmaps excluded (the row's retained geometry is vector-only).
bool flattenableLeaf(const SvgNode& n) {
    if (n.group || n.fromText) return false;
    if (n.opacity != 1.0) return false;
    if (n.image) return false;
    if (!n.patternId.isEmpty() || n.mesh || n.filter) return false;
    if (!n.filterId.isEmpty()) return false;
    if (!n.clipId.isEmpty() || !n.maskId.isEmpty()) return false;
    if (!n.markerStart.isEmpty() || !n.markerMid.isEmpty() ||
        !n.markerEnd.isEmpty())
        return false;
    return true;
}

// Rasterize a flattened node's leaves onto ONE shared image in paint order,
// each with its own brush via the same paintLeafBody the per-shape path
// uses. Pixel-identical to compositing the separate rasters (same order,
// same paints, unit opacities, Normal blends throughout the import).
// Union footprint of a shared row's descendant leaves (each grown by its
// stroke margin): the box its shared raster covers. Group structure is
// ignored (paint is already baked into the leaves). Null when the row paints
// nothing.
void accumulateRowUnion(const SvgNode& n, QRectF& unionBox, bool& hasBox) {
    if (n.group) {
        for (const SvgNode& c : n.children)
            accumulateRowUnion(c, unionBox, hasBox);
        return;
    }
    const double sc = transformScale(n.transform);
    const double margin =
        n.hasStroke ? n.strokeWidth * sc / 2.0 + 1.0 : 1.0;
    const QRectF grown = n.transform.mapRect(n.path.boundingRect())
                             .adjusted(-margin, -margin, margin, margin);
    if (grown.isEmpty()) return;
    unionBox = hasBox ? unionBox.united(grown) : grown;
    hasBox = true;
}

QRectF flatRowUnion(const SvgNode& g) {
    QRectF unionBox;
    bool hasBox = false;
    accumulateRowUnion(g, unionBox, hasBox);
    return unionBox;
}

void paintSharedLeaves(QPainter& p, const SvgNode& n, const SvgResources& res,
                       const QPointF& rowOrigin);

bool rasterizeFlattened(const SvgNode& g, QImage* img, QPointF* offset,
                        const SvgResources& res) {
    const QRectF unionBox = flatRowUnion(g);
    if (unionBox.isNull()) return false;
    const QRect ir = unionBox.toAlignedRect();
    if (ir.width() <= 0 || ir.height() <= 0 || ir.width() > 16384 ||
        ir.height() > 16384)
        return false;
    QImage out(ir.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(-ir.x(), -ir.y());
    paintSharedLeaves(p, g, res, QPointF(ir.x(), ir.y()));
    p.end();
    *img = std::move(out);
    *offset = QPointF(ir.x(), ir.y());
    return true;
}

// Paint a sealed subtree's descendant leaves in order. Plain leaves draw
// straight into the shared painter; leaves with clip/mask/filter paint
// through a trimmed temp (same as their per-shape path) so the shared row
// stays pixel-identical to separate layers.
void paintSharedLeaves(QPainter& p, const SvgNode& n, const SvgResources& res,
                       const QPointF& rowOrigin) {
    for (const SvgNode& c : n.children) {
        if (c.group) {
            paintSharedLeaves(p, c, res, rowOrigin);
            continue;
        }
        if (c.clipId.isEmpty() && c.maskId.isEmpty() && !c.filter) {
            p.save();
            paintLeafBody(p, c, res);
            p.restore();
            continue;
        }
        QImage tmp;
        QPointF off;
        if (!rasterizePart(c, &tmp, &off, nullptr, res)) continue;
        p.drawImage(off - rowOrigin, tmp);
    }
}

// Rescue pass, only when the row census already tripped a guard: seal
// every group into one shared row (exact paint, retained source for zoom
// rebakes). Sole wrappers are descended into instead of sealed, so a
// single root never collapses all structure. Normal files never reach
// here and keep full granularity.
void sealBigGroups(QVector<SvgNode>& out, int& sealed) {
    for (SvgNode& n : out) {
        if (!n.group || n.flattened) continue;
        if (out.size() == 1) {
            // Sole wrapper: sealing would hide everything; descend instead.
            sealBigGroups(n.children, sealed);
            continue;
        }
        n.flattened = true;
        n.opacity = 1.0;  // baked into the leaves; the row adds no more
        ++sealed;
    }
}

// Chunk maximal runs of loose sibling leaves into shared rows (order
// preserved, text leaves stay individual so labels remain crisp parts).
// Rescue-only companion to sealing: groups seal by structure, loose mass
// seals by count. Unions stay uncapped here; the area census decides if
// the result is affordable.
void chunkLooseRuns(QVector<SvgNode>& out, int& chunks) {
    constexpr qsizetype kChunk = 2048;
    for (SvgNode& n : out) {
        if (n.group && !n.flattened)
            chunkLooseRuns(n.children, chunks);
    }
    QVector<SvgNode> res;
    res.reserve(out.size());
    qsizetype i = 0;
    const qsizetype m = out.size();
    while (i < m) {
        if (out[i].group || out[i].fromText) {
            res.push_back(std::move(out[i]));
            ++i;
            continue;
        }
        qsizetype j = i;
        while (j < m && !out[j].group && !out[j].fromText && j - i < kChunk)
            ++j;
        if (j - i < 2) {
            // Lone leaf: keep it a real part.
            res.push_back(std::move(out[i]));
            ++i;
            continue;
        }
        SvgNode f;
        f.group = true;
        f.flattened = true;
        f.opacity = 1.0;
        f.name = out[i].name + QStringLiteral(" +%1").arg(j - i - 1);
        f.children.reserve(j - i);
        for (qsizetype t = i; t < j; ++t)
            f.children.push_back(std::move(out[t]));
        res.push_back(std::move(f));
        ++chunks;
        i = j;
    }
    out = std::move(res);
}

// Prints one subtree in panel order: the group header row, then each child
// subtree from top-most (last painted) down.
void appendPanelLayers(const QVector<SvgNode>& children, const SvgResources& res,
                       int depth, SvgImportResult& out) {
    std::vector<RasterJob> jobs;
    collectRasterJobs(children, res, depth, jobs);

    // Parallel rasterize the leaves (group rows need no work). Each job owns
    // its QImage and QPainter, so this is safe; tiny imports stay inline to
    // avoid thread-pool overhead.
    const std::size_t total = jobs.size();
    if (total >= 64) {
        unsigned hw = std::thread::hardware_concurrency();
        const std::size_t nthreads =
            std::min<std::size_t>(hw < 2 ? 2u : hw, 16);
        std::atomic<std::size_t> next{0};
        std::vector<std::thread> pool;
        pool.reserve(nthreads);
        for (std::size_t t = 0; t < nthreads; ++t) {
            pool.emplace_back([&] {
                while (true) {
                    const std::size_t i = next.fetch_add(1);
                    if (i >= total) return;
                    RasterJob& j = jobs[i];
                    if (j.isFlattened)
                        static_cast<void>(rasterizeFlattened(*j.node, &j.pixels,
                                                             &j.offset,
                                                             *j.res));
                    else if (!j.isGroup)
                        static_cast<void>(rasterizePart(*j.node, &j.pixels,
                                                        &j.offset, &j.art,
                                                        *j.res));
                }
            });
        }
        for (auto& t : pool) t.join();
    } else {
        for (RasterJob& j : jobs)
            if (j.isFlattened)
                static_cast<void>(rasterizeFlattened(*j.node, &j.pixels,
                                                     &j.offset, *j.res));
            else if (!j.isGroup)
                static_cast<void>(rasterizePart(*j.node, &j.pixels, &j.offset,
                                                &j.art, *j.res));
    }

    std::shared_ptr<SvgResources> resShared;
    for (const RasterJob& j : jobs) {
        if (j.isGroup) {
            SvgPartLayer row;
            row.name = j.node->name;
            row.depth = j.depth;
            row.group = true;
            out.layers.append(std::move(row));
            continue;
        }
        if (j.pixels.isNull()) continue;  // no drawable footprint
        SvgPartLayer layer;
        layer.name = j.node->name;
        layer.depth = j.depth;
        layer.group = false;
        layer.pixels = j.pixels;
        layer.offset = j.offset;
        layer.opacity = j.node->opacity;
        layer.art = j.art;
        if (j.isFlattened && j.node) {
            if (rowIsClassicFlat(*j.node)) {
                // Keep the members' geometry (same predicate as the
                // rasterizer) so zoom can re-rasterize the row at display
                // density.
                for (const SvgNode& c : j.node->children) {
                    if (c.group || !flattenableLeaf(c)) continue;
                    auto member = makeLeafArt(c, j.offset);
                    if (member && !member->isEmpty())
                        layer.flatArt.push_back(std::move(member));
                }
            } else {
                // Sealed general row: retain the whole subtree for exact
                // QPainter repaints (clips, masks, text included).
                layer.shared = std::make_shared<SvgNode>(*j.node);
                if (!resShared) resShared = std::make_shared<SvgResources>(res);
                layer.sharedRes = resShared;
            }
        }
        out.layers.append(std::move(layer));
    }
}

}  // namespace

// Exported path utilities for the live XML editor (svg_bridge): the
// definitions above live in file-local scope, so these thin wrappers provide
// the external linkage. Same TU, so the internal parsers stay reachable.
QPainterPath svgParsePathData(const QString& d) {
    return parseSvgPath(d);
}

std::vector<pittore::vector::Segment> svgPathSegments(const QPainterPath& path) {
    return pathSegments(path);
}

namespace {

// Direct-paint one leaf onto the flattened document painter. Common case
// paints straight through (same brush/pen/marker/mesh maths as the per-part
// rasterizer); masked/filtered leaves fall back to a trimmed temp composite
// so huge files stay correct without per-leaf storage.
void paintFlattenedLeaf(QPainter& docP, const SvgNode& n, const SvgResources& res) {
    docP.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const bool needsTemp = !n.maskId.isEmpty() || (n.filter && !n.filter->prims.empty());
    if (needsTemp) {
        QImage tmp;
        QPointF off;
        if (!rasterizePart(n, &tmp, &off, nullptr, res) || tmp.isNull()) return;
        docP.save();
        docP.setOpacity(1.0);
        docP.resetTransform();
        docP.drawImage(off, tmp);
        docP.restore();
        return;
    }
    docP.save();
    docP.resetTransform();
    docP.setTransform(n.transform, false);
    docP.setOpacity(qBound(0.0, n.opacity, 1.0));
    const QPainterPath clip = clipUserPath(n, res);
    if (!clip.isEmpty()) docP.setClipPath(clip, Qt::IntersectClip);
    if (n.image && !n.image->isNull()) {
        // Embedded raster: bitmap placement (no vector brush at all).
        drawImageFitted(docP, *n.image, n.path.boundingRect(), n.imageFit);
    } else if (!n.patternId.isEmpty()) {
        const auto it = res.patterns.constFind(n.patternId);
        if (it != res.patterns.constEnd()) {
            QBrush pb = patternBrushFor(it.value(), transformScale(n.transform));
            if (!n.patternTransform.isEmpty()) {
                QTransform xf;
                if (parseTransformList(n.patternTransform, &xf))
                    pb.setTransform(xf * pb.transform());
            }
            docP.setBrush(pb);
        } else if (n.hasFill) {
            docP.setBrush(makeBrush(n, n.path));
        } else {
            docP.setBrush(Qt::NoBrush);
        }
    } else if (n.hasFill) {
        docP.setBrush(makeBrush(n, n.path));
    } else {
        docP.setBrush(Qt::NoBrush);
    }
    if (n.hasStroke) {
        QPen pen(n.stroke, std::max(0.01, n.strokeWidth));
        pen.setCosmetic(false);
        docP.setPen(pen);
        if (!n.hasFill && n.patternId.isEmpty()) docP.setBrush(Qt::NoBrush);
    } else {
        docP.setPen(Qt::NoPen);
    }
    if (n.hasFill || !n.patternId.isEmpty() || n.hasStroke) docP.drawPath(n.path);
    if (n.mesh) paintMeshOverlay(docP, n);
    paintMarkers(docP, n, res);
    docP.restore();
}

void flattenChildren(QXmlStreamReader& xml, const QTransform& parentTf,
                     const PaintState& parentPaint, SvgResources& res,
                     QPainter& docP, const QTransform& root);
void flattenElement(QXmlStreamReader& xml, const QTransform& parentTf,
                    const PaintState& parentPaint, SvgResources& res,
                    QPainter& docP, const QTransform& root) {
    const QString tag = xml.name().toString();
    const QXmlStreamAttributes a = xml.attributes();
    QTransform tf = parentTf;
    const QString tAttr = readAttr(a, QLatin1String("transform"));
    if (!tAttr.isEmpty()) {
        QTransform t;
        if (parseTransformList(tAttr, &t)) tf = tf * t;
    }
    PaintState ps = parentPaint;
    const QString fillStr = readAttr(a, QLatin1String("fill"));
    const QString strokeStr = readAttr(a, QLatin1String("stroke"));
    const QString fillOp = readAttr(a, QLatin1String("fill-opacity"));
    const QString strokeOp = readAttr(a, QLatin1String("stroke-opacity"));
    const QString opStr = readAttr(a, QLatin1String("opacity"));
    const QString swStr = readAttr(a, QLatin1String("stroke-width"));
    if (!fillStr.isEmpty()) {
        QColor c;
        if (parseColor(fillStr, &c)) {
            ps.hasFill = true;
            ps.fill = c;
        } else if (!fillStr.startsWith(QLatin1String("url("))) {
            ps.hasFill = false;
        }
    }
    if (!strokeStr.isEmpty()) {
        QColor c;
        if (parseColor(strokeStr, &c)) {
            ps.hasStroke = true;
            ps.stroke = c;
        } else if (!strokeStr.startsWith(QLatin1String("url("))) {
            ps.hasStroke = false;
        }
    }
    if (!fillOp.isEmpty()) ps.fill.setAlphaF(qBound(0.0, parseLength(fillOp, 1.0) * ps.fill.alphaF(), 1.0));
    if (!strokeOp.isEmpty()) ps.stroke.setAlphaF(qBound(0.0, parseLength(strokeOp, 1.0) * ps.stroke.alphaF(), 1.0));
    if (!opStr.isEmpty()) ps.opacity *= qBound(0.0, parseLength(opStr, 1.0), 1.0);
    if (!swStr.isEmpty()) ps.strokeWidth = qMax(0.0, parseLength(swStr, 1.0));
    // Fill rule inherits; absent means the spec initial value (nonzero).
    const QString fillRuleStr = readAttr(a, QLatin1String("fill-rule"));
    if (!fillRuleStr.isEmpty()) {
        if (fillRuleStr.trimmed().compare(QLatin1String("nonzero"),
                                          Qt::CaseInsensitive) == 0)
            ps.fillEvenOdd = false;
        else if (fillRuleStr.trimmed().compare(QLatin1String("evenodd"),
                                               Qt::CaseInsensitive) == 0)
            ps.fillEvenOdd = true;
    }
    if (tag == QLatin1String("g")) {
        flattenChildren(xml, tf, ps, res, docP, root);
        return;
    }
    if (tag == QLatin1String("defs")) {
        xml.skipCurrentElement();
        return;
    }
    QPainterPath path;
    std::shared_ptr<QImage> image;
    quint8 imageFit = 0;
    if (tag == QLatin1String("text")) {
        QString textContent;
        QString textPathHref;
        while (!xml.atEnd()) {
            const auto tok = xml.readNext();
            if (tok == QXmlStreamReader::EndElement && xml.name() == QLatin1String("text")) break;
            if (tok == QXmlStreamReader::Characters) {
                textContent += xml.text();
            } else if (tok == QXmlStreamReader::StartElement) {
                const QString ct = xml.name().toString();
                if (ct == QLatin1String("textPath")) {
                    const QXmlStreamAttributes ta = xml.attributes();
                    const QString href = ta.value(QLatin1String("href")).toString().isEmpty()
                                             ? ta.value(QLatin1String("xlink:href")).toString()
                                             : ta.value(QLatin1String("href")).toString();
                    if (textPathHref.isEmpty()) textPathHref = refTarget(href);
                    textContent += xml.readElementText();
                } else if (ct == QLatin1String("tspan") || ct == QLatin1String("tref")) {
                    textContent += xml.readElementText();
                } else {
                    xml.skipCurrentElement();
                }
            }
        }
        if (!textPathHref.isEmpty()) {
            path = textOnPathGlyphs(a, textContent, textPathHref, res);
            if (path.isEmpty()) path = textGlyphPath(a, textContent, nullptr);
        } else {
            path = textGlyphPath(a, textContent, nullptr);
        }
    } else if (tag == QLatin1String("image")) {
        // Embedded raster: decoded bitmap over its placement rect (the
        // bitmap itself is the paint; no fill/stroke applies).
        QString href = a.value(QLatin1String("href")).toString();
        if (href.isEmpty())
            href = a.value(QLatin1String("xlink:href")).toString();
        image = decodeImageDataUri(href, res);
        const double ix = parseLength(readAttr(a, QLatin1String("x")), 0.0);
        const double iy = parseLength(readAttr(a, QLatin1String("y")), 0.0);
        const double iw = parseLength(readAttr(a, QLatin1String("width")), 0.0);
        const double ih = parseLength(readAttr(a, QLatin1String("height")), 0.0);
        imageFit =
            imageFitAttr(readAttr(a, QLatin1String("preserveAspectRatio")));
        if (!image || !(iw > 0.0) || !(ih > 0.0)) {
            if (!image) ++res.imagesDropped;
            xml.skipCurrentElement();
            return;
        }
        path.addRect(QRectF(ix, iy, iw, ih));
        xml.skipCurrentElement();
    } else if (tag == QLatin1String("flowRoot")) {
        xml.skipCurrentElement();
        return;
    } else {
        auto get = [&](QLatin1String k) -> QString { return readAttr(a, k); };
        if (!buildShapeGeometry(tag, get, path)) {
            xml.skipCurrentElement();
            return;
        }
        // Resolved rule always wins over the toolkit default (see walker).
        path.setFillRule(ps.fillEvenOdd ? Qt::OddEvenFill : Qt::WindingFill);
        xml.skipCurrentElement();
    }
    if (path.isEmpty()) return;
    SvgNode n;
    n.group = false;
    n.name = tag;
    n.transform = tf * root;
    n.opacity = ps.opacity;
    n.path = std::move(path);
    n.hasFill = ps.hasFill;
    n.fill = ps.fill;
    const QString fillStr2 = readAttr(a, QLatin1String("fill"));
    if (fillStr2.startsWith(QLatin1String("url(#"))) {
        const QString id = fillStr2.mid(5, fillStr2.indexOf(QLatin1Char(')')) - 5);
        const auto it = res.grads.constFind(id);
        if (it != res.grads.constEnd()) {
            n.hasFill = true;
            n.gradient = *it;
        } else if (res.patterns.contains(id)) {
            n.hasFill = true;
            n.patternId = id;
            n.patternTransform = res.patterns.value(id).xform;
        }
    }
    n.hasStroke = ps.hasStroke;
    n.stroke = ps.stroke;
    n.strokeWidth = ps.strokeWidth;
    n.markerStart = refTarget(readAttr(a, QLatin1String("marker-start")));
    n.markerMid = refTarget(readAttr(a, QLatin1String("marker-mid")));
    n.markerEnd = refTarget(readAttr(a, QLatin1String("marker-end")));
    n.clipId = refTarget(readAttr(a, QLatin1String("clip-path")));
    n.maskId = refTarget(readAttr(a, QLatin1String("mask")));
    n.filterId = refTarget(readAttr(a, QLatin1String("filter")));
    if (!n.filterId.isEmpty()) {
        const auto it = res.filters.constFind(n.filterId);
        if (it != res.filters.constEnd())
            n.filter = std::make_shared<pittore::vector::FilterGraph>(it.value());
    }
    if (image) {
        n.image = std::move(image);
        n.imageFit = imageFit;
        n.hasFill = false;
        n.hasStroke = false;
        n.patternId.clear();
        n.markerStart.clear();
        n.markerMid.clear();
        n.markerEnd.clear();
    }
    paintFlattenedLeaf(docP, n, res);
}

void flattenChildren(QXmlStreamReader& xml, const QTransform& parentTf,
                     const PaintState& parentPaint, SvgResources& res,
                     QPainter& docP, const QTransform& root) {
    while (xml.readNextStartElement()) flattenElement(xml, parentTf, parentPaint, res, docP, root);
}

bool svgPartsParseBudgeted(const QByteArray& xml, SvgSceneRoot* out, QString* error,
                           WalkBudget* budget, bool* overflowOut,
                           QByteArray* inOut, bool skipResWhenEmpty,
                           SvgImportDiag* diag = nullptr) {
    if (budget) budget->diag = diag;
    QElapsedTimer totalT;
    totalT.start();
    if (diag) {
        diag->bytesIn = xml.size();
        // Tag presence scans (linear, no DOM): what the normalize gate decides on.
        diag->preUses = xml.count("<use");
        diag->preSymbols = xml.count("<symbol");
        diag->preImages = xml.count("<image");
        diag->preStyles = xml.count("<style");
        diag->fontGlyphs = QGuiApplication::instance() != nullptr;
    }
    // Collapse first: a 100MB run of identical rects becomes ~600 bytes here,
    // so every scan below (ensure/normalize/gradients/resources/walk) runs on
    // the tiny result instead of the 100MB original. Safe: only opaque
    // identical self-closing shapes with whitespace gaps and ancestor
    // opacity 1 are dropped.
    const QByteArray collapsedPre = collapseIdenticalRuns(xml);
    const QByteArray& pre = collapsedPre.isEmpty() ? xml : collapsedPre;
    // Import bridge: expand <use>/<symbol> + inline stylesheets through the
    // toolkit-free DOM first, so clones and class-styled art parse as flat
    // geometry. Fast-pathed away for files without those features.
    QElapsedTimer normT;
    normT.start();
    const QByteArray norm = svgNormalizeForImport(svgEnsureBuiltinDefs(pre));
    const QByteArray inLocal = norm.isEmpty() ? pre : norm;
    if (inOut) *inOut = inLocal;
    const QByteArray& in = inOut ? *inOut : inLocal;
    if (diag) {
        diag->normMs = normT.elapsed();
        diag->bytesNorm = in.size();
        const bool wanted =
            diag->preUses > 0 || diag->preSymbols > 0 || diag->preStyles > 0;
        diag->normNote = !wanted ? QStringLiteral("not-needed")
                         : (in.size() != pre.size() ? QStringLiteral("expanded")
                                                    : QStringLiteral("over-cap-skipped"));
    }
    if (budget) budget->needShapeRefs = in.contains("<textPath");
    // First passes: gradients, then patterns/markers/clips/masks/filters/
    // meshes/shape-refs — every id reference resolves regardless of order.
    // Skip scans whose tags are absent: each scan is a full streaming pass
    // over potentially 100MB.
    SvgResources res;
    const bool hasGrad = !skipResWhenEmpty || in.contains("<linearGradient") ||
                         in.contains("<radialGradient");
    if (hasGrad) {
        QXmlStreamReader scan(in);
        scanGradients(scan, res.grads);
    }
    const bool hasRes = !skipResWhenEmpty || in.contains("<pattern") || in.contains("<marker") ||
                        in.contains("<clipPath") || in.contains("<mask") ||
                        in.contains("<filter") || in.contains("<meshgradient");
    if (hasRes) {
        QXmlStreamReader scan(in);
        scanResources(scan, res);
    } else if (!skipResWhenEmpty) {
        QXmlStreamReader scan(in);
        scanResources(scan, res);
    }

    QXmlStreamReader r(in);
    if (r.readNextStartElement() && r.name() == QLatin1String("svg")) {
        const QXmlStreamAttributes a = r.attributes();
        const QString w = readAttr(a, QLatin1String("width"));
        const QString h = readAttr(a, QLatin1String("height"));
        // 100% widths have no fixed size; let the viewBox (or content) decide.
        if (!w.isEmpty() && !w.endsWith(QLatin1Char('%')))
            out->widthPx = qMax(0.0, parseLength(w, 0.0));
        if (!h.isEmpty() && !h.endsWith(QLatin1Char('%')))
            out->heightPx = qMax(0.0, parseLength(h, 0.0));
        const QString vb = readAttr(a, QLatin1String("viewBox"));
        if (!vb.isEmpty()) {
            const std::vector<double> v = parseNumbers(vb);
            if (v.size() >= 4) {
                out->vbX = v[0];
                out->vbY = v[1];
                out->vbW = v[2];
                out->vbH = v[3];
            }
        }
        PaintState ps;
        QElapsedTimer walkT;
        walkT.start();
        walkChildren(r, QTransform(), ps, res, out->children, budget);
        if (diag) diag->walkMs = walkT.elapsed();
    }
    if (diag) {
        diag->gradDefs = res.grads.size();
        diag->patternDefs = res.patterns.size();
        diag->markerDefs = res.markers.size();
        diag->clipDefs = res.clips.size();
        diag->maskDefs = res.masks.size();
        diag->filterDefs = res.filters.size();
        diag->meshDefs = res.meshes.size();
        diag->images = res.images;
        diag->imagesDropped = res.imagesDropped;
        diag->imageBytes = res.imageBytes;
    }
    out->res = std::move(res);
    if (budget && budget->overflow) {
        if (overflowOut) *overflowOut = true;
        if (error) *error = QStringLiteral("SVG has too many parts for per-shape import");
        if (diag) {
            diag->overflow = true;
            diag->error = error ? *error : QString();
        }
        return false;
    }
    if (overflowOut) *overflowOut = false;
    if (r.hasError()) {
        if (error) *error = QStringLiteral("SVG parse error: %1").arg(r.errorString());
        if (diag && error) diag->error = *error;
        return false;
    }
    // Coalesce consecutive same-paint runs into single nodes (O(n) post-pass;
    // see mergeConsecutiveLeaves). Path-heavy maps like the Brazil DDD file
    // drop from thousands of layers to tens here.
    if (diag) diag->mergedFrom = out->children.size();
    mergeConsecutiveLeaves(out->children);
    if (diag) diag->mergedTo = out->children.size();
    if (out->children.isEmpty()) {
        if (error) *error = QStringLiteral("SVG contains no drawable elements");
        if (diag && error) diag->error = *error;
        return false;
    }
    return true;
}

}  // namespace

bool svgPartsParse(const QByteArray& xml, SvgSceneRoot* out, QString* error) {
    WalkBudget budget;
    bool overflow = false;
    return svgPartsParseBudgeted(xml, out, error, &budget, &overflow, nullptr, true);
}

bool svgFlattenSizeOk(int iw, int ih, qint64 pixelBudget) {
    if (iw <= 0 || ih <= 0 || iw > 16384 || ih > 16384) return false;
    return static_cast<qint64>(iw) * ih <= pixelBudget;
}

namespace {

// Flattened fallback: single raster layer for SVGs with more leaves than the
// per-part budget. Streams the document a second time painting directly onto
// one document-sized image (O(1) layers, O(1) peak nodes) so pathological
// files open in seconds instead of OOMing.
bool svgFlattenImport(const QByteArray& in, SvgResources& res, int iw, int ih,
                      const QTransform& root, SvgImportResult* out, int* dpiOut,
                      QString* error) {
    if (!svgFlattenSizeOk(iw, ih, svgRasterAreaBudget())) {
        if (error)
            *error = (iw <= 0 || ih <= 0 || iw > 16384 || ih > 16384)
                         ? QStringLiteral("SVG has an unsupported canvas size")
                         : QStringLiteral("SVG canvas is too large to rasterize");
        return false;
    }
    QImage doc(iw, ih, QImage::Format_ARGB32_Premultiplied);
    doc.fill(Qt::transparent);
    QPainter docP(&doc);
    docP.setRenderHint(QPainter::Antialiasing, true);
    {
        QXmlStreamReader r(in);
        if (r.readNextStartElement() && r.name() == QLatin1String("svg")) {
            PaintState ps;
            flattenChildren(r, QTransform(), ps, res, docP, root);
        }
        if (r.hasError()) {
            if (error) *error = QStringLiteral("SVG parse error: %1").arg(r.errorString());
            return false;
        }
    }
    docP.end();
    out->docSize = QSize(iw, ih);
    if (dpiOut) *dpiOut = 96;
    SvgPartLayer layer;
    layer.name = QStringLiteral("SVG");
    layer.depth = 0;
    layer.group = false;
    layer.pixels = std::move(doc);
    layer.offset = QPointF(0, 0);
    layer.opacity = 1.0;
    out->layers.append(std::move(layer));
    return true;
}

}  // namespace

namespace {

// Fidelity verdict + one-line report for the import log: a glance tells
// whether the SVG rendered fully or what was lost (overflow raster, skipped
// tags, dangling refs, unexpanded uses, dropped images).
void logSvgImportReport(const SvgImportDiag& d) {
    const bool usesLost = d.preUses > 0 &&
                          d.normNote == QLatin1String("over-cap-skipped");
    const bool boxText = d.texts > 0 && !d.fontGlyphs;
    const bool failed = !d.error.isEmpty();
    const bool degraded =
        !failed && (d.overflow || d.flattenFallback || d.skippedLeaves > 0 ||
                    !d.missingRefs.isEmpty() || d.imagesDropped > 0 || usesLost ||
                    boxText || d.rowCap || d.areaCap);
    auto mapText = [](const QHash<QString, int>& m) {
        QStringList parts;
        int shown = 0, hidden = 0;
        for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
            if (shown < 6) {
                parts.push_back(it.key() + QStringLiteral("x") +
                                 QString::number(it.value()));
                ++shown;
            } else {
                ++hidden;
            }
        }
        if (hidden > 0)
            parts.push_back(QStringLiteral("+%1 more").arg(hidden));
        return parts.join(QStringLiteral(","));
    };
    QString line = QStringLiteral("bytes=%1 norm=%2 leaves=%3 groups=%4 ")
                       .arg(d.bytesIn)
                       .arg(d.normNote)
                       .arg(d.leaves)
                       .arg(d.groups);
    if (d.preUses > 0 || d.preSymbols > 0)
        line += QStringLiteral("uses=%1 symbols=%2 ")
                    .arg(d.preUses)
                    .arg(d.preSymbols);
    if (d.texts > 0 || d.flowTexts > 0 || d.blankTexts > 0)
        line += QStringLiteral("texts=%1(+%2 on-path,+%3 flow,+%4 blank) ")
                    .arg(d.texts)
                    .arg(d.textPaths)
                    .arg(d.flowTexts)
                    .arg(d.blankTexts);
    if (d.overflow)
        line += QStringLiteral("overflow@%1 cut=%2 ")
                    .arg(d.overflowAt)
                    .arg(d.cutAfterOverflow);
    if (d.images > 0 || d.imagesDropped > 0)
        line += QStringLiteral("img=%1(+%2 dropped, %3KB) ")
                    .arg(d.images)
                    .arg(d.imagesDropped)
                    .arg(d.imageBytes / 1024);
    if (d.geometryBudget > 0)
        line += QStringLiteral("geom=%1/%2MB ")
                    .arg(d.geometryBytes / (1024 * 1024))
                    .arg(d.geometryBudget / (1024 * 1024));
    if (d.rowsCounted > 0)
        line += QStringLiteral("rows=%1 area=%2MP ")
                    .arg(d.rowsCounted)
                    .arg(d.rasterArea / 1000000.0, 0, 'f', 1);
    if (d.mergedTo > 0)
        line +=
            QStringLiteral("merged=%1->%2 ").arg(d.mergedFrom).arg(d.mergedTo);
    if (d.sealedGroups > 0 || d.chunkedRuns > 0)
        line += QStringLiteral("sealed=%1 chunks=%2 ")
                    .arg(d.sealedGroups)
                    .arg(d.chunkedRuns);
    const int layers = d.layersPixel + d.layersGroup;
    line += QStringLiteral("layers=%1(+%2 group,+%3 flat-art,+%4 shared) "
                           "doc=%5x%6 "
                           "ms=%7(norm=%8 walk=%9 raster=%10) verdict=%11")
                .arg(layers)
                .arg(d.layersGroup)
                .arg(d.layersFlatArt)
                .arg(d.layersShared)
                .arg(d.docW)
                .arg(d.docH)
                .arg(d.normMs + d.walkMs + d.rasterMs)
                .arg(d.normMs)
                .arg(d.walkMs)
                .arg(d.rasterMs)
                .arg(failed ? QStringLiteral("failed")
                            : (degraded ? QStringLiteral("degraded")
                                        : QStringLiteral("ok")));
    if (failed) {
        ::pittore::core::log::log_warning("[import] SVG %s",
                                          line.toUtf8().constData());
        ::pittore::core::log::log_warning("[import] SVG error: %s",
                                          d.error.toUtf8().constData());
        return;
    }
    ::pittore::core::log::log_info("[import] SVG %s",
                                   line.toUtf8().constData());
    if (d.overflow)
        ::pittore::core::log::log_warning(
            "[import] SVG over the geometry budget (%lldMB retained of "
            "%lldMB): single doc-size raster, zoom past 100%% upscales",
            (long long)(d.geometryBytes / (1024 * 1024)),
            (long long)(d.geometryBudget / (1024 * 1024)));
    if (d.rowCap)
        ::pittore::core::log::log_warning(
            "[import] SVG over the row guard (%d rows): single doc-size "
            "raster, zoom past 100%% upscales",
            d.rowsCounted);
    if (d.areaCap)
        ::pittore::core::log::log_warning(
            "[import] SVG over the raster-area guard (%lldMP): single doc-size "
            "raster, zoom past 100%% upscales",
            (long long)(d.rasterArea / 1000000));
    if (usesLost)
        ::pittore::core::log::log_warning(
            "[import] SVG has %d <use> refs but normalize skipped (over cap): "
            "cloned labels/shapes are missing",
            d.preUses);
    if (d.imagesDropped > 0)
        ::pittore::core::log::log_warning(
            "[import] SVG skipped %d <image> elements (undecodable, external "
            "ref or over the image budget)",
            d.imagesDropped);
    if (!d.skipTags.isEmpty())
        ::pittore::core::log::log_warning(
            "[import] SVG skipped unsupported elements: %s",
            mapText(d.skipTags).toUtf8().constData());
    if (d.emptyLeaves > 0)
        ::pittore::core::log::log_info(
            "[import] SVG dropped %d empty-geometry leaves (paints nothing)",
            d.emptyLeaves);
    if (!d.missingRefs.isEmpty())
        ::pittore::core::log::log_warning(
            "[import] SVG dangling refs (inherited paint kept): %s",
            mapText(d.missingRefs).toUtf8().constData());
    if (boxText)
        ::pittore::core::log::log_warning(
            "[import] SVG %d text leaves used box fallback (no font engine)",
            d.texts);
}

void countFlatRows(const QVector<SvgNode>& nodes, SvgImportDiag& d) {
    for (const SvgNode& n : nodes) {
        if (!n.group) continue;
        if (n.flattened) {
            ++d.flatRows;
            d.flatMembers += n.children.size();
        } else {
            countFlatRows(n.children, d);
        }
    }
}

// Pre-raster cost census, mirroring what collectRasterJobs will emit: one
// row per group header, leaf and flattened row, with raster area from the
// same boxes the rasterizer trims to. Lets huge files fall back before any
// QImage is allocated.
void estimateImportRows(const QVector<SvgNode>& nodes, int& rows,
                        qint64& area) {
    for (const SvgNode& n : nodes) {
        if (n.group && !n.flattened) {
            ++rows;
            estimateImportRows(n.children, rows, area);
            continue;
        }
        if (n.group) {
            const QRectF u = flatRowUnion(n);
            if (u.isNull()) continue;  // paints nothing: emits no layer
            ++rows;
            area += (qint64)std::ceil(u.width()) *
                    (qint64)std::ceil(u.height());
            continue;
        }
        ++rows;
        const double sc = transformScale(n.transform);
        const double margin =
            n.hasStroke ? n.strokeWidth * sc / 2.0 + 1.0 : 1.0;
        const QRectF b = n.transform.mapRect(n.path.boundingRect())
                             .adjusted(-margin, -margin, margin, margin);
        if (b.width() > 0.0 && b.height() > 0.0)
            area += (qint64)std::ceil(b.width()) *
                    (qint64)std::ceil(b.height());
    }
}

bool runFlattenFallback(const QByteArray& in, SvgResources& res, int iw,
                        int ih, const QTransform& root, SvgImportResult* out,
                        int* dpiOut, QString* error) {
    QElapsedTimer rasterT;
    rasterT.start();
    const bool ok = svgFlattenImport(in, res, iw, ih, root, out, dpiOut, error);
    out->diag.rasterMs = rasterT.elapsed();
    out->diag.flattenFallback = true;
    out->diag.docW = iw;
    out->diag.docH = ih;
    if (ok) {
        out->diag.layersPixel = out->layers.size();
        if (dpiOut) out->diag.dpi = *dpiOut;
    } else if (error) {
        out->diag.error = *error;
    }
    logSvgImportReport(out->diag);
    return ok;
}

}  // namespace

bool svgPartsImport(const QByteArray& xml, SvgImportResult* out, int* dpiOut,
                    QString* error) {
    SvgSceneRoot scene;
    WalkBudget budget;
    // Full imports are judged by retained weight, not leaf count: the live
    // editor entry keeps the small fixed cap, files take the byte budget.
    budget.limit = std::numeric_limits<int>::max();
    budget.byteBudget = svgGeometryBudget();
    out->diag.geometryBudget = budget.byteBudget;
    bool overflow = false;
    out->diag.dpi = 96;
    // Keep the normalized bytes alive for a potential flatten second pass.
    QByteArray inStore;
    {
        QString parseError;
        if (!svgPartsParseBudgeted(xml, &scene, &parseError, &budget, &overflow, &inStore,
                                   true, &out->diag)) {
            if (!overflow) {
                if (error) *error = parseError;
                out->diag.error = parseError;
                logSvgImportReport(out->diag);
                return false;
            }
            // Overflow: fall through to flattened import below; scene holds
            // the resources + size but a truncated child list.
        }
    }
    out->diag.geometryBytes = budget.bytes;

    // Document size: explicit width/height, else the viewBox, else the content
    // bounds. The document model uses whole-pixel sizes, so the final scale
    // maps the viewBox onto the rounded pixel canvas.
    double docW = scene.widthPx;
    double docH = scene.heightPx;
    const bool hasVb = scene.vbW > 0.0 && scene.vbH > 0.0;
    if (hasVb) {
        if (docW <= 0.0) docW = scene.vbW;
        if (docH <= 0.0) docH = scene.vbH;
    }
    if (overflow) {
        // Truncated child list: size must come from width/height/viewBox.
        // Content-bounds fallback would be wrong on a prefix.
        if (docW <= 0.0 || docH <= 0.0) {
            if (error) *error = QStringLiteral("SVG has too many parts for per-shape import");
            out->diag.error = error ? *error : QString();
            logSvgImportReport(out->diag);
            return false;
        }
        const int iw = qMax(1, qRound(docW));
        const int ih = qMax(1, qRound(docH));
        QTransform root;
        if (hasVb) {
            root.translate(-scene.vbX, -scene.vbY);
            root.scale(static_cast<double>(iw) / scene.vbW,
                       static_cast<double>(ih) / scene.vbH);
        }
        // Overflow is the designed fallback, not a failure: drop the
        // parse-stage message so the verdict reads degraded, and only
        // report a real error if the flatten pass itself fails.
        out->diag.error.clear();
        return runFlattenFallback(inStore, scene.res, iw, ih, root, out,
                                  dpiOut, error);
    }
    if (docW <= 0.0 || docH <= 0.0) {
        const QRectF b = contentBounds(scene.children);
        if (b.width() <= 0.0 || b.height() <= 0.0) {
            if (error) *error = QStringLiteral("SVG has no size or content");
            out->diag.error = error ? *error : QString();
            logSvgImportReport(out->diag);
            return false;
        }
        docW = b.width();
        docH = b.height();
    }
    const int iw = qMax(1, qRound(docW));
    const int ih = qMax(1, qRound(docH));
    QTransform root;
    if (hasVb) {
        root.translate(-scene.vbX, -scene.vbY);
        root.scale(static_cast<double>(iw) / scene.vbW,
                   static_cast<double>(ih) / scene.vbH);
    }
    applyRoot(scene.children, root);

    // Deliberately no run merging here: every authored group stays a group
    // row and every shape its own layer (flattening sibling runs into one
    // shared image row hid structure the panel must show). The row/area
    // census below still guards pathological files via the seal/chunk
    // rescue.
    {
        // Cost census before allocating a single raster: pathological files
        // fall back while still cheap.
        const qint64 areaBudget = svgRasterAreaBudget();
        int rows = 0;
        qint64 area = 0;
        estimateImportRows(scene.children, rows, area);
        if (rows > kMaxImportRows || area > areaBudget) {
            // Rescue: seal groups into shared rows (exact paint, retained
            // source for zoom rebakes), chunk loose-leaf mass the same way,
            // and recount. Only files that are still pathological after
            // sealing take the single raster.
            int sealed = 0, chunks = 0;
            sealBigGroups(scene.children, sealed);
            chunkLooseRuns(scene.children, chunks);
            out->diag.sealedGroups = sealed;
            out->diag.chunkedRuns = chunks;
            rows = 0;
            area = 0;
            estimateImportRows(scene.children, rows, area);
        }
        out->diag.rowsCounted = rows;
        out->diag.rasterArea = area;
        if (rows > kMaxImportRows || area > areaBudget) {
            if (rows > kMaxImportRows) out->diag.rowCap = true;
            if (area > areaBudget) out->diag.areaCap = true;
            return runFlattenFallback(inStore, scene.res, iw, ih, root, out,
                                      dpiOut, error);
        }
    }
    countFlatRows(scene.children, out->diag);

    out->docSize = QSize(iw, ih);
    if (dpiOut) *dpiOut = 96;
    QElapsedTimer rasterT;
    rasterT.start();
    appendPanelLayers(scene.children, scene.res, 0, *out);
    out->diag.rasterMs = rasterT.elapsed();
    out->diag.docW = iw;
    out->diag.docH = ih;
    if (dpiOut) out->diag.dpi = *dpiOut;
    for (const SvgPartLayer& l : out->layers) {
        if (l.group)
            ++out->diag.layersGroup;
        else
            ++out->diag.layersPixel;
        if (!l.flatArt.empty()) ++out->diag.layersFlatArt;
        if (l.shared) ++out->diag.layersShared;
    }
    if (out->layers.isEmpty()) {
        if (error) *error = QStringLiteral("SVG produced no importable parts");
        out->diag.error = error ? *error : QString();
        logSvgImportReport(out->diag);
        return false;
    }
    logSvgImportReport(out->diag);
    return true;
}

bool sharedRowBake(const SvgNode& root, const SvgResources& res,
                   const QTransform& place, double zoom, const QRectF& visible,
                   QImage* img, QPointF* origin, double* resampleOut,
                   QRectF* bakedBox) {
    if (img) *img = QImage();
    if (origin) *origin = QPointF();
    if (resampleOut) *resampleOut = 1.0;
    if (bakedBox) *bakedBox = QRectF();
    const double kf = std::clamp(std::ceil(zoom - 1e-9), 1.0, 16.0);
    if (kf <= 1.0) return false;
    // Union footprint through the live placement, culled to the visible
    // region when one is given (margin-grown boxes test exact: a culled
    // leaf cannot reach the baked box).
    struct Acc {
        QRectF box;
        bool has = false;
        double outScale = 1e-9;
        double peakSw = 0.0;
    };
    Acc acc;
    std::function<void(const SvgNode&)> walk = [&](const SvgNode& n) {
        if (n.group) {
            for (const SvgNode& c : n.children) walk(c);
            return;
        }
        if (n.path.isEmpty()) return;
        const QTransform placed =
            QTransform(n.transform.m11(), n.transform.m12(),
                       n.transform.m21(), n.transform.m22(), n.transform.dx(),
                       n.transform.dy()) *
            place;
        const double s =
            std::max({std::hypot(placed.m11(), placed.m12()),
                      std::hypot(placed.m21(), placed.m22()), 1e-9});
        const double margin =
            n.hasStroke ? n.strokeWidth * s / 2.0 + 1.0 : 1.0;
        const QRectF grown = placed.mapRect(n.path.boundingRect())
                                 .adjusted(-margin, -margin, margin, margin);
        if (grown.isEmpty()) return;
        if (!visible.isNull() && !grown.intersects(visible)) return;
        acc.outScale = std::max(acc.outScale, s);
        if (n.hasStroke) acc.peakSw = std::max(acc.peakSw, n.strokeWidth);
        acc.box = acc.has ? acc.box.united(grown) : grown;
        acc.has = true;
    };
    walk(root);
    if (!acc.has) return false;
    const double margin =
        acc.peakSw > 0.0 ? acc.peakSw * acc.outScale / 2.0 + 1.0 : 1.0;
    const QRect ir = acc.box.adjusted(-margin, -margin, margin, margin)
                         .toAlignedRect();
    if (ir.width() <= 0 || ir.height() <= 0 || ir.width() > 16384 ||
        ir.height() > 16384)
        return false;
    const double longEdge = std::max<double>(ir.width(), ir.height());
    double k = kf;
    if (longEdge > 0.0)
        k = std::max(1.0, std::min(kf, std::floor(2048.0 / longEdge)));
    if (k <= 1.0) return false;
    const int w = std::max(1, static_cast<int>(std::ceil(ir.width() * k)));
    const int h = std::max(1, static_cast<int>(std::ceil(ir.height() * k)));
    if (w > 16384 || h > 16384) return false;
    const QTransform frame = QTransform().translate(-ir.x(), -ir.y()) *
                             QTransform().scale(k, k);
    QImage out(QSize(w, h), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    std::function<void(const SvgNode&)> paint = [&](const SvgNode& n) {
        if (n.group) {
            for (const SvgNode& c : n.children) paint(c);
            return;
        }
        if (n.path.isEmpty()) return;
        const QTransform placed =
            QTransform(n.transform.m11(), n.transform.m12(),
                       n.transform.m21(), n.transform.m22(), n.transform.dx(),
                       n.transform.dy()) *
            place;
        // Same cull as the union walk (margin-grown, document space):
        // culled leaves cannot reach the box.
        if (!visible.isNull()) {
            const double psc = std::max(
                {std::hypot(placed.m11(), placed.m12()),
                 std::hypot(placed.m21(), placed.m22()), 1e-9});
            const double pmargin =
                n.hasStroke ? n.strokeWidth * psc / 2.0 + 1.0 : 1.0;
            if (!placed.mapRect(n.path.boundingRect())
                     .adjusted(-pmargin, -pmargin, pmargin, pmargin)
                     .intersects(visible))
                return;
        }
        SvgNode framed = n;  // shallow: path data stays shared
        framed.transform = placed * frame;
        if (framed.clipId.isEmpty() && framed.maskId.isEmpty() &&
            !framed.filter) {
            p.save();
            paintLeafBody(p, framed, res);
            p.restore();
            return;
        }
        QImage tmp;
        QPointF off;
        if (!rasterizePart(framed, &tmp, &off, nullptr, res)) return;
        p.drawImage(off, tmp);
    };
    paint(root);
    p.end();
    if (img) *img = std::move(out);
    if (origin) *origin = QPointF(ir.x(), ir.y());
    if (resampleOut) *resampleOut = k;
    if (bakedBox) *bakedBox = QRectF(ir);
    return true;
}

}  // namespace pittore::ui