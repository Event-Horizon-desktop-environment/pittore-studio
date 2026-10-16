// SVG batch7 units: boxes, filters, refs, cycles, chunks.
#include "engine/vector/svg/aspect_align.h"
#include "engine/vector/svg/chunk_split.h"
#include "engine/vector/svg/circle_args.h"
#include "engine/vector/svg/clip_rule.h"
#include "engine/vector/svg/clip_units.h"
#include "engine/vector/svg/current_color.h"
#include "engine/vector/svg/dxdy_list.h"
#include "engine/vector/svg/ellipse_args.h"
#include "engine/vector/svg/filter_in.h"
#include "engine/vector/svg/filter_rect.h"
#include "engine/vector/svg/filter_units.h"
#include "engine/vector/svg/href_cycle.h"
#include "engine/vector/svg/id_clash.h"
#include "engine/vector/svg/id_index.h"
#include "engine/vector/svg/image_fit.h"
#include "engine/vector/svg/image_rect.h"
#include "engine/vector/svg/inherit_flag.h"
#include "engine/vector/svg/lang_space.h"
#include "engine/vector/svg/line_args.h"
#include "engine/vector/svg/mask_rect.h"
#include "engine/vector/svg/mask_units.h"
#include "engine/vector/svg/ns_name.h"
#include "engine/vector/svg/path_length.h"
#include "engine/vector/svg/pattern_units.h"
#include "engine/vector/svg/poly_points.h"
#include "engine/vector/svg/rect_args.h"
#include "engine/vector/svg/rotate_list.h"
#include "engine/vector/svg/switch_cond.h"
#include "engine/vector/svg/unknown_keep.h"
#include "engine/vector/svg/xml_reader.h"
#include "test_util.h"

using namespace pittore::svg;

namespace {

void test_boxes() {
    CHECK(parseRectArgs("1", "2", "3", "4", "", "", 100, 100).w == 3);
    CHECK(parseCircleArgs("1", "1", "2", 100, 100).r == 2);
    CHECK(parseEllipseArgs("1", "1", "2", "3", 100, 100).ry == 3);
    CHECK(parseLineArgs("0", "0", "1", "1", 10, 10).x2 == 1);
    CHECK(parseImageRect("1", "2", "3", "4", 100, 100).h == 4);
    CHECK(parseAspect("xMidYMid meet").align == 5);
    CHECK(parseAspect("none").align == 0);
    CHECK_EQ(parsePolyPoints("0,0 1,1").size(), 2u);
    CHECK(pathLengthScale("", 5) == 1);
    CHECK_EQ(parseRotateList("1 2").size(), 2u);
    CHECK_EQ(parsePosList("1 2", 10).size(), 2u);
}

void test_units() {
    CHECK(isClipEvenOdd("evenodd"));
    CHECK(clipIsUserSpace(""));
    CHECK(parseMaskRect("", "", "", "").w == 1.2);
    CHECK(!maskIsAlpha("luminance"));
    CHECK(parseFilterRect("", "", "", "").h == 1.2);
    CHECK(!filterIsUserSpace("objectBoundingBox"));
    CHECK(!primIsUserSpace(""));
    CHECK(isMagicInput("SourceGraphic"));
    CHECK(patternIsUserSpace("userSpaceOnUse"));
    CHECK(preservesSpace("preserve"));
    CHECK(inheritsProp("color"));
}

void test_refs() {
    std::string out;
    CHECK(resolveCurrentColor("currentColor", "red", out));
    CHECK(localName("xlink:href") == "href");
    CHECK(prefixOf("xlink:href") == "xlink");
    CHECK(!remapIds({{"id", "a"}}, "-1").empty());
    CHECK(hasHrefCycle({{"a", "b"}, {"b", "a"}}));
    CHECK(!hasHrefCycle({{"a", "b"}}));
    CHECK_EQ(splitRanges(10, 3).size(), 3u);
    CHECK(needsThreads(10000));
    const XmlRead r = readXml("<svg><rect id=\"a\"/></svg>");
    CHECK(r.ok && !indexIds(*r.root).empty());
    CHECK(isKnownAttr("rect", "x"));
}

void test_switch() {
    CHECK(switchChildOk(
        {"http://www.w3.org/TR/SVG11/feature#Shape", "", ""}, "en-US"));
    CHECK(!switchChildOk(
        {"http://www.w3.org/TR/SVG11/feature#Script", "", ""}, "en-US"));
    CHECK(!switchChildOk(
        {"http://www.w3.org/TR/SVG11/feature#Shape",
         "http://example.invalid/ext", ""},
        "en-US"));
    CHECK(switchChildOk({"", "", "en-US, fr"}, "en-US"));
    CHECK(switchChildOk({"", "", "EN-us"}, "en-US"));
    CHECK(!switchChildOk({"", "", "fr"}, "en-US"));
    CHECK(switchChildOk({"", "", ""}, ""));
    CHECK(!switchChildOk(
        {"http://www.w3.org/TR/SVG11/feature#Nope", "", ""}, "en-US"));
}

}  // namespace

int main() {
    test_boxes();
    test_units();
    test_refs();
    test_switch();
    const int rc = pittore_test::failures() == 0 ? 0 : 1;
    std::printf("  checks=%d failures=%d\n", pittore_test::checks(),
                pittore_test::failures());
    return rc;
}
