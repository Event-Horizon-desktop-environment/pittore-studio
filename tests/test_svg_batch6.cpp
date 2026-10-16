// SVG batch6 units: fonts, paint, grad, pattern, markers.
#include "engine/vector/svg/current_color.h"
#include "engine/vector/svg/font_family.h"
#include "engine/vector/svg/font_shorthand.h"
#include "engine/vector/svg/font_style.h"
#include "engine/vector/svg/font_weight.h"
#include "engine/vector/svg/glyph_orient.h"
#include "engine/vector/svg/grad_attrs.h"
#include "engine/vector/svg/grad_xform.h"
#include "engine/vector/svg/gradient_units.h"
#include "engine/vector/svg/important.h"
#include "engine/vector/svg/marker_attrs.h"
#include "engine/vector/svg/marker_orient.h"
#include "engine/vector/svg/pattern_attrs.h"
#include "engine/vector/svg/pattern_xform.h"
#include "engine/vector/svg/solid_color.h"
#include "engine/vector/svg/specificity.h"
#include "engine/vector/svg/spread_method.h"
#include "engine/vector/svg/stop_opacity.h"
#include "engine/vector/svg/text_deco.h"
#include "engine/vector/svg/text_length.h"
#include "test_util.h"

using namespace pittore::svg;

namespace {

void test_font() {
    CHECK(firstFamily("\"DejaVu\", serif") == "DejaVu");
    CHECK_EQ(fontWeightNum("bold"), 700);
    CHECK(isItalicStyle("oblique"));
    const DecoFlags d = parseDeco("underline line-through");
    CHECK(d.underline && d.strike);
    const TextLength t = parseTextLength("100", "spacingAndGlyphs");
    CHECK(t.has && t.spacingAndGlyphs);
    CHECK(!parseGlyphOrient("auto").autoMode == false);
    CHECK(!parseFontShorthand("italic bold 12px serif").family.empty());
}

void test_paint() {
    float o[4] = {0, 0, 0, 0};
    CHECK(solidRgba("red", "", o));
    CHECK_NEAR(stopAlpha("0.5"), 0.5f, 1e-6);
    CHECK(parseSpread("repeat") == Spread::Repeat);
    CHECK_NEAR(gradNum("50%", 0), 0.5, 1e-9);
    CHECK(parseGradientTransform("").a == 1);
    CHECK(isUserSpaceUnits("userSpaceOnUse"));
    CHECK(!splitImportant("red !important").value.empty());
    CHECK(specificity("#a") > specificity(".b"));
    const SolidColor sc = parseSolidColorElem("red", "0.5");
    CHECK(sc.valid);
    CHECK_NEAR(sc.rgba[0], 1.0f, 1e-6);
    CHECK_NEAR(sc.rgba[3], 0.5f, 1e-6);
    CHECK(!parseSolidColorElem("", "").valid);
    CHECK(!parseSolidColorElem("url(#a)", "").valid);
}

void test_refs() {
    CHECK(parsePatternRect("1", "2", "3", "4").w == 3);
    CHECK(parsePatternTransform("").a == 1);
    CHECK(parseMarkerArgs("1", "2", "3", "4", "").mw == 3);
    CHECK(parseMarkerOrient("auto").autoMode);
}

}  // namespace

int main() {
    test_font();
    test_paint();
    test_refs();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
