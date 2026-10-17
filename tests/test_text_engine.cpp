// test_text_engine.cpp — layout and rasterization through the system font
// stack. Pins the behaviour the Affinity text import relies on: family
// discovery, the line box metrics (ascender and line step taken from the face,
// not FreeType's pixel-rounded size), kerning and tracking in the measured
// width, explicit and wrapped line breaks, and that empty text has no ink.
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>

#include "engine/text/text_engine.h"
#include "test_util.h"

using namespace pittore::text;

namespace {

TextSpec base(const std::string& text) {
    TextSpec s;
    s.text = text;
    s.family = defaultFamily();
    s.size = 48.0f;
    return s;
}

std::optional<TextRaster> raster(const std::string& text) {
    return rasterize(base(text));
}

void test_family_discovery() {
    const std::string fam = defaultFamily();
    CHECK(!fam.empty());
    CHECK(hasFamily(fam));
    std::string upper = fam;
    for (char& c : upper)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK(hasFamily(upper));
    CHECK(!hasFamily("No Such Family Zuul 42"));
}

void test_empty_text_has_no_ink() {
    auto r = raster("");
    CHECK(r.has_value());
    if (r) CHECK(r->isEmpty());
}

void test_single_line_metrics() {
    auto r = raster("Hamburgefonstiv");
    CHECK(r.has_value());
    if (!r) return;
    CHECK(!r->isEmpty());
    CHECK(r->bounds.width() > 0);
    CHECK(r->bounds.height() > 0);
    CHECK(r->layoutWidth > 0.0f);
    CHECK(r->firstBaseline > 0.0f);
    CHECK(r->lineAdvance > 0.0f);
    if (r->capHeight) CHECK(*r->capHeight > 0.0f);
    // Ink sits above the first baseline; the box starts there.
    CHECK(r->bounds.top < r->firstBaseline);
}

void test_explicit_newline_adds_a_line() {
    auto one = raster("One");
    auto two = raster("One\nTwo");
    CHECK(one && two);
    if (!one || !two) return;
    CHECK_NEAR(two->firstBaseline, one->firstBaseline, 0.001);
    CHECK_NEAR(two->lineAdvance, one->lineAdvance, 0.001);
    CHECK(two->bounds.height() > one->bounds.height());
}

void test_wrap_breaks_at_word_boundaries() {
    const std::string words = "alpha beta gamma delta epsilon zeta eta theta";
    TextSpec s = base(words);
    CHECK(!s.wrapWidth.has_value());
    auto flat = rasterize(s);
    s.wrapWidth = 200.0f;
    auto wrapped = rasterize(s);
    CHECK(flat && wrapped);
    if (!flat || !wrapped) return;
    CHECK(!flat->isEmpty());
    CHECK(!wrapped->isEmpty());
    CHECK(wrapped->bounds.height() > flat->bounds.height());
    CHECK(wrapped->layoutWidth < flat->layoutWidth);
}

void test_tracking_is_charged_per_character() {
    TextSpec a = base("AB");
    TextSpec b = a;
    b.tracking = 10.0f;
    auto ra = rasterize(a);
    auto rb = rasterize(b);
    CHECK(ra && rb);
    if (!ra || !rb) return;
    CHECK_NEAR(rb->layoutWidth - ra->layoutWidth, 20.0f, 0.01);
    CHECK(rb->bounds.width() > ra->bounds.width());
}

void test_style_variants_still_rasterize() {
    TextSpec b = base("Bold");
    b.bold = true;
    TextSpec i = base("Italic");
    i.italic = true;
    auto rb = rasterize(b);
    auto ri = rasterize(i);
    CHECK(rb.has_value());
    CHECK(ri.has_value());
    if (rb) CHECK(!rb->isEmpty());
    if (ri) CHECK(!ri->isEmpty());
}

void test_utf8_text_rasterizes() {
    std::string s = "caf";
    s += "\xC3\xA9";  // é
    auto r = raster(s);
    CHECK(r.has_value());
    if (r) CHECK(!r->isEmpty());
}

void test_family_names_listed() {
    const std::vector<std::string> names = familyNames();
    CHECK(names.size() > 10);
    // Sorted case-insensitively and (case-insensitively) unique.
    for (std::size_t i = 1; i < names.size(); ++i) {
        std::string a = names[i - 1];
        std::string b = names[i];
        for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        CHECK(a < b);
    }
}

void test_layout_spans_and_carets() {
    TextSpec s = base("One\nTwo");
    std::optional<TextLayout> laid = layoutText(s);
    CHECK(laid.has_value());
    if (!laid) return;
    CHECK(laid->lines.size() == 2u);
    if (laid->lines.size() != 2u) return;
    // Explicit newlines are not part of a line's byte range.
    CHECK(laid->lines[0].start == 0u);
    CHECK(laid->lines[0].end == 3u);
    CHECK(laid->lines[1].start == 4u);
    CHECK(laid->lines[1].end == 7u);
    CHECK(laid->lineAdvance > 0.0f);
    CHECK(!laid->chars.empty());
    // Caret at the start of line 2 sits on that line's pen.
    const Caret c0 = caretAt(s, *laid, 4);
    CHECK_NEAR(c0.x, laid->lines[1].x, 0.01);
    CHECK_NEAR(c0.top, laid->lines[1].top, 0.01);
    // Caret at the end is the line's right edge; beyond it clamps there.
    const Caret ce = caretAt(s, *laid, 7);
    CHECK_NEAR(ce.x, laid->lines[1].x + laid->lines[1].width, 0.01);
    const Caret big = caretAt(s, *laid, 9999);
    CHECK_NEAR(big.x, ce.x, 0.01);
}

void test_leading_recorded_overrides_the_face_step() {
    // A document whose story records an explicit leading gets exactly that
    // line step, not the face's own line box.
    TextSpec s = base("One\nTwo");
    auto r = rasterize(s);
    CHECK(r.has_value());
    if (r) CHECK(r->lineCount == 2);
    s.emLeading = 0.5f;  // half the em: 24 px at size 48
    auto laid = layoutText(s);
    CHECK(laid.has_value());
    if (!laid) return;
    CHECK_NEAR(laid->lineAdvance, 24.0f, 0.01);
    CHECK(laid->lines.size() == 2u);
    // The step alone moves line 2; no paragraph spacing applies to '\n'.
    CHECK_NEAR(laid->lines[1].top, laid->lineAdvance, 0.01);
}

void test_paragraph_break_carries_the_recorded_gap() {
    TextSpec s = base("One\vTwo");
    s.paragraphGap = 15.0f;
    auto laid = layoutText(s);
    CHECK(laid.has_value());
    if (!laid) return;
    // The break splits lines and its byte stays out of both ranges.
    CHECK(laid->lines.size() == 2u);
    if (laid->lines.size() != 2u) return;
    CHECK(laid->lines[0].end == 3u);
    CHECK(laid->lines[1].start == 4u);
    // Line 2 sits one step plus the recorded paragraph spacing lower.
    TextSpec plain = base("One\nTwo");
    auto natural = layoutText(plain);
    CHECK(natural.has_value());
    if (!natural) return;
    CHECK_NEAR(laid->lines[1].top - laid->lines[0].top,
               natural->lines[1].top - natural->lines[0].top + 15.0f, 0.01);
}

void test_plain_newline_never_carries_the_gap() {
    TextSpec s = base("One\nTwo");
    s.paragraphGap = 15.0f;
    auto laid = layoutText(s);
    CHECK(laid.has_value());
    if (!laid) return;
    TextSpec plain = base("One\nTwo");
    auto natural = layoutText(plain);
    CHECK(natural.has_value());
    if (!natural) return;
    CHECK_NEAR(laid->lines[1].top - laid->lines[0].top,
               natural->lines[1].top - natural->lines[0].top, 0.01);
}

void test_hit_test_round_trips_character_pens() {
    TextSpec s = base("abc");
    std::optional<TextLayout> laid = layoutText(s);
    CHECK(laid.has_value());
    if (!laid) return;
    CHECK(laid->lines.size() == 1u);
    if (laid->lines.size() != 1u) return;
    const float midY = laid->lines[0].top + laid->lines[0].height * 0.5f;
    for (const TextLayout::CharPos& c : laid->chars) {
        // A point just right of a character's pen resolves to that character.
        CHECK(hitTest(s, *laid, c.x + 0.5f, midY) == c.byte);
    }
    // Past the right edge clamps to the end of the line.
    CHECK(hitTest(s, *laid, 10000.0f, midY) == laid->lines[0].end);
}

void test_wrapped_layout_spans_cover_the_run() {
    TextSpec s = base("alpha beta gamma delta epsilon zeta eta theta");
    s.wrapWidth = 200.0f;
    std::optional<TextLayout> laid = layoutText(s);
    CHECK(laid.has_value());
    if (!laid) return;
    CHECK(laid->lines.size() > 1u);
    if (laid->lines.empty()) return;
    CHECK(laid->lines.front().start == 0u);
    CHECK(laid->lines.back().end == s.text.size());
}

// --- Character-panel attributes --------------------------------------------

void test_hscale_stretches_advances() {
    TextSpec a = base("AB");
    TextSpec b = a;
    b.hScale = 2.0f;
    auto ra = rasterize(a);
    auto rb = rasterize(b);
    CHECK(ra && rb);
    if (!ra || !rb) return;
    CHECK_NEAR(rb->layoutWidth, ra->layoutWidth * 2.0f, 0.5f);
    CHECK(rb->bounds.width() > ra->bounds.width());
    CHECK_NEAR(rb->firstBaseline, ra->firstBaseline, 0.01);
}

void test_vscale_stretches_the_ink_but_not_the_pen() {
    TextSpec a = base("H");
    TextSpec b = a;
    b.vScale = 2.0f;
    auto ra = rasterize(a);
    auto rb = rasterize(b);
    CHECK(ra && rb);
    if (!ra || !rb) return;
    CHECK_NEAR(rb->layoutWidth, ra->layoutWidth, 0.01);
    CHECK(rb->bounds.height() > ra->bounds.height());
}

void test_baseline_shift_moves_the_ink() {
    TextSpec a = base("H");
    TextSpec b = a;
    b.baselineShift = 24.0f;
    auto ra = rasterize(a);
    auto rb = rasterize(b);
    CHECK(ra && rb);
    if (!ra || !rb) return;
    CHECK_NEAR(rb->firstBaseline, ra->firstBaseline, 0.01);
    CHECK_NEAR(static_cast<double>(rb->bounds.bottom),
               static_cast<double>(ra->bounds.bottom) - 24.0, 2.0);
}

void test_superscript_shrinks_and_raises() {
    TextSpec a = base("x");
    TextSpec b = a;
    b.superSub = 1;
    auto ra = rasterize(a);
    auto rb = rasterize(b);
    CHECK(ra && rb);
    if (!ra || !rb) return;
    CHECK(rb->bounds.height() < ra->bounds.height());
    CHECK(rb->bounds.bottom <= ra->bounds.bottom);
}

void test_all_caps_is_a_display_transform() {
    TextSpec lower = base("abc");
    lower.allCaps = true;
    TextSpec upper = base("ABC");
    auto rl = rasterize(lower);
    auto ru = rasterize(upper);
    CHECK(rl && ru);
    if (!rl || !ru) return;
    CHECK_NEAR(rl->layoutWidth, ru->layoutWidth, 0.5f);
    // The stored string is untouched, so caret offsets keep mapping.
    CHECK(lower.text == "abc");
}

void test_kerning_can_be_switched_off() {
    TextSpec on = base("AV");
    TextSpec off = on;
    off.kerning = false;
    auto ron = rasterize(on);
    auto roff = rasterize(off);
    CHECK(ron && roff);
    if (!ron || !roff) return;
    CHECK(roff->layoutWidth >= ron->layoutWidth - 0.01f);
}

void test_underline_and_strike_extend_bounds() {
    TextSpec a = base("H");
    auto ra = rasterize(a);
    CHECK(ra.has_value());
    if (!ra) return;
    CHECK(ra->colorRgba.empty());

    TextSpec u = a;
    u.underline = 1;
    auto ru = rasterize(u);
    CHECK(ru.has_value());
    if (ru) {
        CHECK(!ru->colorRgba.empty());
        CHECK(ru->colorRgba.size() == ru->coverage.size() * 4u);
        CHECK(ru->bounds.bottom > ra->bounds.bottom);
    }

    TextSpec s = a;
    s.strike = 1;
    auto rs = rasterize(s);
    CHECK(rs.has_value());
    if (rs) {
        CHECK(!rs->colorRgba.empty());
        CHECK(rs->bounds.bottom > ra->bounds.top);
    }
}

void test_background_box_sizes_and_paints() {
    TextSpec a = base("H");
    auto ra = rasterize(a);
    CHECK(ra.has_value());
    if (!ra) return;
    TextSpec b = a;
    b.backgroundColor = {1.0f, 1.0f, 0.0f, 1.0f};
    auto rb = rasterize(b);
    CHECK(rb.has_value());
    if (!rb) return;
    CHECK(!rb->colorRgba.empty());
    CHECK(rb->coverage.size() == rb->colorRgba.size() / 4u);
    // The highlight box reaches past the glyph ink.
    CHECK(rb->bounds.height() > ra->bounds.height());
}

void test_decorations_only_paint_when_enabled() {
    // A plain run must keep exactly its glyph bounds so imported-layer rasters
    // (and their probes) are unchanged by the panel's defaults.
    TextSpec plain = base("Hello");
    auto a = rasterize(plain);
    CHECK(a.has_value());
    if (a) CHECK(a->colorRgba.empty());
}

void run() {
    test_family_discovery();
    test_family_names_listed();
    test_empty_text_has_no_ink();
    test_single_line_metrics();
    test_explicit_newline_adds_a_line();
    test_wrap_breaks_at_word_boundaries();
    test_tracking_is_charged_per_character();
    test_style_variants_still_rasterize();
    test_utf8_text_rasterizes();
    test_layout_spans_and_carets();
    test_leading_recorded_overrides_the_face_step();
    test_paragraph_break_carries_the_recorded_gap();
    test_plain_newline_never_carries_the_gap();
    test_hit_test_round_trips_character_pens();
    test_wrapped_layout_spans_cover_the_run();
    test_hscale_stretches_advances();
    test_vscale_stretches_the_ink_but_not_the_pen();
    test_baseline_shift_moves_the_ink();
    test_superscript_shrinks_and_raises();
    test_all_caps_is_a_display_transform();
    test_kerning_can_be_switched_off();
    test_underline_and_strike_extend_bounds();
    test_background_box_sizes_and_paints();
    test_decorations_only_paint_when_enabled();
}

}  // namespace

TEST_MAIN_CALL(run)
