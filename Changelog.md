# Changelog

All notable changes to Pittore Studio are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## Unreleased

### Fixed

- **SVG import: gradient strokes.** `stroke="url(#id)"` was silently
  dropped, so rings, outlines and other stroke-only artwork imported as
  empty shapes. The importer now resolves the paint server (inheriting it
  through groups), paints it into the layer raster, and retains it in the
  vector geometry and `.psc` round trip; the SVG writer emits the gradient
  back as a reference.
- **SVG import: `clip-path` on a group.** A clip on a `<g>` was ignored, so
  its contents painted outside the clip — bars overrunning a clipped disc,
  letterform tops left unclipped. Clip refs now accumulate down the group
  tree and intersect at paint time (ancestor clips plus the element's own),
  group transforms map the clip's user space, and the retained art carries
  the effective clip so the canvas live-repaint keeps the clipped bake.

### Tests

- The soft-proof suites (`test_proof`, `proof-preview`) resolve their CMYK
  fixture from the repository (`tests/fixtures/photocraft-coated-cmyk.icc`,
  CC0-1.0, synthetic — provenance and full licence text recorded beside it)
  instead of `/usr/share/ghostscript/iccprofiles/default_cmyk.icc`, so they
  no longer skip on machines without Ghostscript. `PITTORE_CMYK_PROFILE`
  overrides the path to proof against a real press profile instead.

## 0.1.1 — 2026-10-13

### Added

- **Native CMYK editing (partial).** CMYK documents now retain per-layer
  device ink samples alongside the RGB appearance image, so paint that
  belongs in ink space stays in ink space:
  - Normal source-over brush and pencil dabs (build-up and wash) and
    normal-composite bitmap stamp tips paint directly into the inks and
    derive their preview through the document profile.
  - Color Replacement and Paint Bucket fills update device inks; Magic
    Eraser and alpha-only selection edits preserve them.
  - Tonal brushes, healing and cloning re-separate only the region they
    changed, leaving untouched device samples alone; accepted destructive
    filters re-separate through the document profile, and a cancelled
    filter preview restores the original inks.
  - Non-normal brush composites, vector and text paint, and layer
    compositing still cross the RGB appearance boundary.
- **CMYK/16 document mode** in the new-document dialog, alongside the
  existing RGB/8, RGB/16, RGB/32, Grayscale/8 and CMYK/8.
- **CMYK file round trips.**
  - `.psc` projects gain a v5 optional per-layer native CMYK payload;
    v1–v4 files decode unchanged and files that need no newer fields still
    encode at the oldest compatible version.
  - CMYK PSD/PSB layer and composite inks are retained on import and
    re-emitted on export.
  - New TIFF reader for separated data (`tiffDecodeCmykFile`): 8/16-bit
    CMYK and CMYKA, stripped or tiled, contiguous or planar-separate, all
    eight orientations, ICC profile carried alongside.
  - A shared destination-profile resolver feeds the Image ▸ Mode
    conversion, the layered PSD writer and the TIFF export path, so all
    three agree on which profile separates the document.
- **Auto Refine.** One-shot AI refinement of the current selection or
  mask: segment, remove speckles, decay haze, smooth, decontaminate and
  write back to the layer, with a run report (model, timing, matte
  coverage, stages that ran). The pipeline lives in a new
  `window_ai.cpp` command file next to Select Subject, Remove Background
  and Enhance Edges.
- **Refine dialog improvements.**
  - *Shift* slider to displace the edge after feathering (contract or
    expand).
  - *Smart radius*: one narrow-band pass everywhere plus a wide-band pass
    accepted only where the mask edge is soft, so hard boundaries keep a
    tight treatment while hair and fur ramps get the wide one.
  - Pressed brush footprints preview as translucent discs while you paint
    in the dialog, instead of only after the re-solve.
- **New matte cleanup operators** in the selection mask library, used by
  the Refine pipeline: detached-island speckle removal, small enclosed
  hole filling, low-confidence haze decay driven by local texture, and
  smart-radius edge snapping.
- **Guided upsampling of AI masks.** Model-resolution masks are lifted to
  full resolution with joint bilateral upsampling (Kopf et al. 2007),
  weighting coarse taps by position and colour in the full-resolution
  guide so contours land on real image edges instead of the model grid.
- **Pigment-mix dab compositing.** Geometric mean, parallel (harmonic)
  and Allanon mean blends for brush dabs, giving watercolour-style
  glazing where overlaps average instead of cover. Exposed as a brush
  composite option and mapped from imported preset bundles.
- **Paper grain textures.** A grain evaluator for brush textures —
  tile sample, subtractive brightness, mid-grey-pivot contrast,
  neutral-point remap, cutoff window and strength combine — with
  pressure-driven strength and automatic invert on the eraser. Opt-in per
  preset; existing textures keep the legacy combine.
- **Brush engine additions:** auto-spacing coefficient (size-law
  spacing stride), clean-room `.pat` pattern decoder (depth 1–4, malformed
  input fails cleanly), and sharpness / soften / darken dynamics carried
  through from imported presets.
- **Richer preset bundle import:** pressure child-sensor curves are read
  from the correct sensor block instead of whichever curve comes first,
  all sensor entries are collected in order, and auto spacing, composite
  operation, darken, sharpness and softness now map into the preset model.
  Approximations are still recorded in the import notes.
- **Threaded SVG import.** Large SVGs parse and rasterize off the GUI
  thread with cooperative progress and cancel; a cancelled import aborts
  quietly and a closed dialog drops the result unread.
- **MCP bridge tools:** `run_command` (headless-safe command ids —
  `auto_refine`, `select_subject`, `remove_background`, `enhance_edges`,
  `deselect`, `invert_selection`; dialog-opening commands are refused),
  `run_info` (last AI run report) and `export_layer_png`. Bridge reads
  allow long-polling so multi-minute AI commands no longer trip the
  socket timeout.
- **Refine lab test harness** (`tests/Refine-app`): a headless-capable
  refine window with its own MCP server and a tuned hair preset, plus
  `refine_probe` and `upsample_probe` executables.

### Changed

- **HiDPI rendering above scale 1 is correct.** Canvas tiles snap in
  device pixels, the checkerboard is baked per device pixel ratio, zoom
  modes account for the ratio, pointer math keeps sub-pixel precision,
  icons ship 1x/2x variants, and the HiDPI policy is applied before the
  `QApplication` constructor runs.
- **Affinity import.** Fill rasters (`FRst`) become masked solid fills
  instead of opaque layers, and the flattened background layer is only
  kept when it actually carries something the recovered stack lacks —
  a clean import no longer leaves a redundant "Background (flattened)"
  row (logged as such).
- **Colour decontamination** now propagates confident foreground colour
  outward by weighted diffusion and blends edge pixels toward it, instead
  of unmixing the backdrop out of low-alpha pixels; amount and radius are
  parameterised.
- **Open-image path** decodes flat CMYK PSD and TIFF sources through the
  retained-ink path when the file opens at full resolution inside the
  document memory budget; proxied files and unsupported TIFF layouts use
  the appearance path as before.
- **Wash stroke baking** re-uploads the touched pixels, re-renders the
  baked region and emits `regionModified`, so a baked wash stroke is
  visible immediately rather than at the next zoom.
- README carries a banner image; version strings updated to 0.1.1.

### Fixed

- Build failure in `app_state_open.cpp` when opening flat CMYK sources.
- Unused-parameter warnings in the no-ONNX AI stubs (`decodePrompt`,
  `segment_rgba8`, and the encoder/decoder paths), which otherwise fire on
  every build that links no ONNX Runtime.

### Packaging and CI

- GitHub Actions upgraded: `checkout@v6`, `upload-artifact@v5`,
  `download-artifact@v5`, `action-gh-release@v3`.
- Fedora RPM builds move from Fedora 41 to Fedora 44.
- Snap builds pin `snapcore/action-build` to a Node 24 commit until
  upstream ships a tag.
- Release workflow tags the release on `workflow_dispatch` (reusing an
  existing tag), publishes only `pittore-studio*` artifacts with
  `fail_on_unmatched_files`, and the previous "latest" prerelease becomes
  the `rolling` release.
- Test execution is disabled in CI: the full meson suite (notably the
  heavy tools-ux cases) times out on hosted runners. `just test` remains
  the pre-merge gate and must be run locally.

### Tests

- New suites: `test_color_mode`, `test_svg_import_async`, CMYK cases in
  `test_tiff`, expanded `test_selection_ops`, `test_selection_mask`,
  `test_project_file`, `test_psd_export`, `test_brush`, `test_tools_ux`.

## 0.1.0

Initial release.
