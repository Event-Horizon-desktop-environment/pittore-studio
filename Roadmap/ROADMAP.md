# Adjustment Parity Roadmap

Goal: remake every Affinity Colour Grading adjustment in Pittore, including the
preset ("mode") lists Affinity ships with each one.

Reference screenshots in this folder, one per adjustment, named
`NN-<adjustment>-presets.png`. They are Affinity 3.3.1 UI captures, used only to
read control names, ranges and preset labels. The maths below is our own, fitted
to the documented control semantics — no code or numeric constants are taken
from Affinity.

## Status

| # | Adjustment        | Pittore kind            | Engine | Params UI | Presets | Screenshot |
|---|-------------------|-------------------------|--------|-----------|---------|------------|
| 1 | Recolour          | `AdjustmentKind::Recolour` (15) | done | done | fitted | `05-recolour-presets.png` |
| 2 | Levels            | `Levels` (2)            | done | done | fitted | `01-levels-presets.png` |
| 3 | White Balance     | `WhiteBalance` (11)     | done | done | fitted | `02-white-balance-presets.png` |
| 4 | HSL               | `HueSaturation` (6)     | partial | done | fitted | `03-hsl-presets-a.png`, `04-hsl-presets-b.png` |
| 5 | Black & White     | `BlackWhite` (12)       | done | done | fitted | `06-black-and-white-presets.png` |
| 6 | Brightness/Contrast | `BrightnessContrast` (1) | done | done | fitted | `07-brightness-contrast-presets.png` |
| 7 | Posterise         | `Posterize` (9)         | done | done | fitted | `08-posterise-presets.png` |
| 8 | Vibrance          | `Vibrance` (5)          | done | done | fitted | `09-vibrance-presets.png` |

"fitted" means the preset list is implemented and selectable, but its numbers
are our own fits rather than values read out of Affinity. Phase 2 covers both
the chips UI and the exact-value question.

Affinity's Colour Grading list also carries Exposure, Threshold, Curves, Channel
Mixer, Gradient Map, Selective Colour, Colour Balance, Invert, Soft Proof, LUT
and Lens Filter. Of those, Gradient Map, Selective Colour, Soft Proof, LUT and
Lens Filter have no Pittore kind at all. They are out of scope for this pass
unless the maintainer says otherwise.

### HSL gap

Affinity's HSL is **per hue range** (Reds, Yellows, Greens, Cyans, Blues,
Magentas), each with its own Hue/Saturation/Lightness. Pittore's
`HueSaturation` applies one global shift. That is why the preset list is
dominated by per-range names ("Red Saturation", "Brighten Cyans"). Closing this
gap means widening `AdjustmentKind::HueSaturation` from 3 params to 19, which
is a document-format change and needs the maintainer's call.

## Phase 1 — Recolour (in progress)

Pittore has no Recolour. This is the one kind the Affinity fixture
`2021-disney-birthday-cards_embedded.af` actually uses, and the decoder currently
skips it (`[af] skip RcRA (RcRA): layer kind is out of scope`).

### Model

Affinity exposes three controls: Hue (0–360 degrees), Saturation (0–100%),
Lightness (0%). Observed defaults: Hue 0, Saturation 100%, Lightness 0. The
single decoded layer in the fixture carries Hue 16.4, Saturation 100,
Lightness 5.

Our fit: Recolour maps every pixel to one hue at one saturation, keeping the
source's HSL lightness as the tonal carrier, then applies a lightness offset.

```
h = p[0] / 360
s = clamp01(p[1])
l = clamp01(rgb_to_lightness(r, g, b) + p[2])
rgb = hsl_to_rgb(h, s, l)
```

`rgb_to_lightness` and `hsl_to_rgb` already exist in `adjust.h`, so the kernel
is small and lands in the shared CPU/GPU header — CUDA and HIP pick it up for
free because both call `adjust::apply()`.

Params: `p[0]` hue degrees 0..360, `p[1]` saturation 0..1, `p[2]` lightness
-1..1. Defaults 0, 1.0, 0.

### Work items

- [x] `AdjustmentKind::Recolour = 15` plus `recolour()` and its `apply()` case
      in `src/engine/compute/adjust.h`, and the parameter-semantics comment.
- [x] `adjustmentKindName` / `adjustmentParamDescs` in
      `src/ui/app_state_adjust.cpp`.
- [x] `setAdjustmentDefaults` in `src/ui/app_state_detail.cpp` (saturation 1.0,
      not 0 — the zero loop does not land on a usable default here).
- [x] Button in `src/ui/panels/adjustments/adjustments_panel.cpp`.
- [x] Entry in the Layer > New Adjustment Layer menu,
      `src/ui/window/menus/menu_layer.cpp`.
- [x] A "recolor" glyph in `src/ui/icons.cpp`.
- [x] Recolour presets (below), fitted and selectable in the Properties panel.

### Presets — `05-recolour-presets.png`

| Preset          | Fitted values (hue, saturation, lightness) |
|-----------------|--------------------------------------------|
| (Default)       | 0, 100%, 0 |
| Coarse Sepia    | 32, 55%, -2% |
| Fire            | 18, 95%, +2% |
| Night           | 225, 55%, -12% |
| Purple Hour     | 285, 55%, 0 |
| Blue Hour       | 215, 65%, -5% |
| Wine            | 345, 70%, -8% |

The panel only shows names and thumbnails, so these are fits, not read-out
values. The RE route below would give exact numbers instead.

## Phase 2 — Presets for every existing kind

Affinity's Adjustments panel renders each adjustment as an expandable section
holding a grid of two-thumbnail (before/after) preset chips. Pittore's
Adjustments panel is a flat 3-across icon grid with no preset support at all.

Maintainer decision (recorded): build the preset values now, fitted by eye,
rather than waiting for the RE work to produce exact numbers.

Work items:

- [x] A preset model: `{ kind, name, params[16] }`, one table per kind, in
      `src/ui/adjustment_presets.h/.cpp`. Entry 0 of every table is the reset
      state, so it matches `setAdjustmentDefaults`.
- [x] A Preset dropdown in the Properties panel header, which applies a whole
      parameter set as one undoable step (`AppState::setAdjustmentParams`).
      An exact match marks the current entry; a hand-edited layer reads
      "Custom".
- [ ] Thumbnail chips in the Adjustments panel itself, matching the screenshot
      layout (collapsed header with disclosure triangle and icon, then
      labelled before/after thumbnails). Still open.

Preset names, transcribed from the screenshots:

**Levels** — `01-levels-presets.png`
(Default), Darken, Lighten, Raise Black Level, Lower White Level, Increase
Gamma, Decrease Gamma, Highlight Burn

**White Balance** — `02-white-balance-presets.png`
(Default), Warm, Cool, Magenta Tint, Green Tint, Cool Magenta, Warm Green,
Warm Crush, Cool Crush

**HSL** — `03-hsl-presets-a.png`, `04-hsl-presets-b.png`
(Default), Saturate, Desaturate, Invert Hue, Red Saturation, Yellow Saturation,
Green Saturation, Cyan Saturation, Blue Saturation, Magenta Saturation, Brighten
Reds, Brighten Yellows, Brighten Greens, Brighten Cyans, Brighten Blues,
Brighten Magentas, Autumnal, Brighten Sky Detail

**Black & White** — `06-black-and-white-presets.png`
(Default), Cold, Warm, Crunch, Darken Blues/Cyans, Red/Blue Contrast

**Brightness / Contrast** — `07-brightness-contrast-presets.png`
(Default), Lighter, Darker, Lower Contrast, Increase Contrast, Saturate,
Desaturate, Muted Colours 01, Muted Colours 02, Subtle Punch

**Posterise** — `08-posterise-presets.png`
(Default), Harsh, Soft, Muted

**Vibrance** — `09-vibrance-presets.png`
(Default), Full Vibrance, No Vibrance, Half Vibrance

Note that several preset names imply controls Pittore's kind does not have:
"Saturate"/"Desaturate" under Brightness/Contrast, and the HSL per-range names.
Those are fitted as approximations today and are flagged in the table comments
in `src/ui/adjustment_presets.cpp`. Closing the HSL gap would let the per-range
names become real.

## Phase 3 — Colour Grading opens a left-side panel

Requested as: "like the artboard, when colour grading is clicked it opens a left
side panel".

Maintainer decision (recorded): keep the Color Grading modal dialog, and have
clicking it also raise the existing Adjustments panel on the left dock.

Work items:

- [x] `MainWindow::raisePanelOnLeft(id)` in
      `src/ui/window/docks/window_docks.cpp`: re-homes a panel to the left rail
      beside Tools (joining an existing left column when there is one rather
      than splitting the tools rail a second time), and leaves a torn-out
      floating panel where the user put it.
- [x] `MainWindow::colorGradeDialog()` calls it before `exec()`.
- [x] Selecting the Color persona raises the Adjustments panel the same way
      (`src/ui/persona/persona_wire.cpp`), after the persona's panel set is
      applied. `showPanel` also leaves an already-left-docked panel in place,
      so repeated Color clicks do not bounce it back into the right stack.

Note: the dialog is modal, so the left panel is visible but not interactive
until the dialog closes. Making the dialog non-modal is a separate decision.

## Reverse-engineering still owed

Full detail belongs in the RE brief for the other agent. Items this roadmap
depends on:

- `RcRA` field list and semantics — gives exact preset numbers for Phase 1 and
  lets the decoder stop skipping the layer.
- `AdjR` base record layout.
- Artboard margin/guide record, so artboard chrome can draw margins.

## Provenance

Screenshots are Affinity product UI, captured locally for reference. They are
kept untracked. Do not commit them, and do not copy Affinity code, constants or
artwork. Preset names are short functional labels and are used descriptively.
