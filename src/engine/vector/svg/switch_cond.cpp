// Static switch evaluation. Table mirrors the importer's real coverage
// (see docs/svg2.md); anything unlisted fails closed.
#include "engine/vector/svg/switch_cond.h"

#include <cctype>

namespace pittore::svg {
namespace {

// SVG 1.1 feature URIs the static importer can render. Both singular and
// plural spellings accepted (authors vary; the selected branch renders the
// same either way).
bool featureSupported(const std::string& uri) {
    static const char* kYes[] = {
        "http://www.w3.org/TR/SVG11/feature#SVG-static",
        "http://www.w3.org/TR/SVG11/feature#SVGDOM-static",
        "http://www.w3.org/TR/SVG11/feature#CoreAttribute",
        "http://www.w3.org/TR/SVG11/feature#Structure",
        "http://www.w3.org/TR/SVG11/feature#ConditionalProcessing",
        "http://www.w3.org/TR/SVG11/feature#Image",
        "http://www.w3.org/TR/SVG11/feature#Style",
        "http://www.w3.org/TR/SVG11/feature#ViewportAttribute",
        "http://www.w3.org/TR/SVG11/feature#Shape",
        "http://www.w3.org/TR/SVG11/feature#Text",
        "http://www.w3.org/TR/SVG11/feature#PaintAttribute",
        "http://www.w3.org/TR/SVG11/feature#OpacityAttribute",
        "http://www.w3.org/TR/SVG11/feature#GraphicsAttribute",
        "http://www.w3.org/TR/SVG11/feature#Markers",
        "http://www.w3.org/TR/SVG11/feature#Gradient",
        "http://www.w3.org/TR/SVG11/feature#Gradients",
        "http://www.w3.org/TR/SVG11/feature#Pattern",
        "http://www.w3.org/TR/SVG11/feature#Patterns",
        "http://www.w3.org/TR/SVG11/feature#Clip",
        "http://www.w3.org/TR/SVG11/feature#Clips",
        "http://www.w3.org/TR/SVG11/feature#Mask",
        "http://www.w3.org/TR/SVG11/feature#Masks",
        "http://www.w3.org/TR/SVG11/feature#Filter",
        "http://www.w3.org/TR/SVG11/feature#Filters",
        "http://www.w3.org/TR/SVG11/feature#Xlink",
        "http://www.w3.org/TR/SVG11/feature#Extensibility",
        nullptr,
    };
    for (int i = 0; kYes[i]; ++i) {
        if (uri == kYes[i]) return true;
    }
    return false;
}

std::string trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() &&
           (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r'))
        ++b;
    size_t e = s.size();
    while (e > b &&
           (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' ||
            s[e - 1] == '\r'))
        --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

// BCP47 basic filtering: range matches the tag or a '-' prefix of it.
bool langMatch(const std::string& range, const std::string& tag) {
    const std::string r = lower(trim(range));
    if (r.empty()) return false;
    if (r == "*") return true;
    const std::string t = lower(tag);
    if (r == t) return true;
    return t.size() > r.size() && t.compare(0, r.size(), r) == 0 &&
           t[r.size()] == '-';
}

}  // namespace

bool switchChildOk(const SwitchCond& cond, const std::string& systemLocale) {
    // Space-separated feature URIs: every one must hold.
    {
        size_t i = 0;
        while (i < cond.requiredFeatures.size()) {
            while (i < cond.requiredFeatures.size() &&
                   std::isspace((unsigned char)cond.requiredFeatures[i]))
                ++i;
            size_t j = i;
            while (j < cond.requiredFeatures.size() &&
                   !std::isspace((unsigned char)cond.requiredFeatures[j]))
                ++j;
            if (j > i && !featureSupported(cond.requiredFeatures.substr(i, j - i)))
                return false;
            i = j;
        }
    }
    // No extension namespace is supported.
    if (!trim(cond.requiredExtensions).empty()) return false;
    // Comma-separated language ranges.
    if (trim(cond.systemLanguage).empty()) return true;
    size_t i = 0;
    while (i <= cond.systemLanguage.size()) {
        const size_t c = cond.systemLanguage.find(',', i);
        const std::string range = cond.systemLanguage.substr(
            i, c == std::string::npos ? c : c - i);
        if (langMatch(range, systemLocale)) return true;
        if (c == std::string::npos) break;
        i = c + 1;
    }
    return false;
}

}  // namespace pittore::svg
