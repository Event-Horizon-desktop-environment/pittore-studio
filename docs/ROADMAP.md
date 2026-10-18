# Roadmap

What the editor does not do yet. Compiled by walking every menu, panel and
engine module against the documented feature set of a complete professional
photo editor; grouped by area, not ordered by priority.

- **Placeholder** — the menu entry or panel exists in the UI but is disabled
  or inert; nothing is implemented behind it.
- **Partial** — it works, with known gaps.
- Everything not marked is absent entirely.

## Input and shortcuts

- Pixel select-all has no binding — `Ctrl+A` selects all layers.
- `Ctrl+Alt+Z` toggles the last state instead of stepping backward through
  history one step at a time.
- `Ctrl+Shift+F` is the vector Fill and Stroke; the Fade command is absent.
- `Delete` removes the selected layer; clearing the selected pixels has no
  working key.
- Unbound today: feather selection, desaturate, color balance as a dialog,
  lock layer, lock transparent pixels, select the layer above/below, select
  the top/bottom layer, add to the panel layer selection, next/previous and
  first/last brush preset, blend-mode cycling, the remaining blend-mode
  letter family, brush flow on `Shift`+digit, panel function keys, last
  filter with its dialog, guide locking, page and corner scrolling, text
  size, kerning, leading and alignment.
- Missing hold-key behaviours: `Alt` as a temporary eyedropper while
  painting, `Ctrl+Space` / `Alt+Space` click zoom, `Shift`+click to connect
  a straight paint stroke.
- `Caps Lock` precision cursor is not implemented.
- New layers are always created without a naming dialog; there is no key
  split between dialog and no-dialog creation.
- One keymap only — no shortcut profiles. Workspaces can carry overrides,
  but there is no profile switcher.
- Dialog-internal key sets (Liquify modes, camera raw) have no equivalents;
  those modes live in the tool options.

## Documents, capture and metadata

- RAW decode and a develop workflow: absent. No de-Bayer, no camera
  profiles, no white-balance solve; the camera raw filter is a classical
  tone filter on already-decoded RGB.
- Tethered capture and scanner acquisition: absent.
- DICOM and medical/film formats: absent.
- PDF and EPS import: absent. PDF export exists as a minimal vector writer.
- Document templates: absent; the start page offers fixed presets only.
- Metadata editing: absent. File Info is a placeholder, and the export
  copyright/GPS controls are disabled.
- Content credentials (C2PA signed provenance): absent.

## Document commands

- Image Size and Canvas Size entries are placeholders; resizing is reachable
  only through the crop/resize interactions and the export resampler.
- Arbitrary rotation is a placeholder; 90-degree rotations and flips are
  wired.
- Crop, Trim, Reveal All and Duplicate entries are placeholders; cropping
  exists as a tool.
- Auto Tone, Auto Contrast and Auto Color are placeholders.
- Apply Image, Calculations, Variables and Analysis are placeholders.
- In Image ▸ Adjustments only Levels and Curves open dialogs; the rest is
  placeholders: Brightness/Contrast, Exposure, Vibrance, Hue/Saturation,
  Color Balance, Black & White, Photo Filter, Channel Mixer, Color Lookup,
  Invert, Posterize, Threshold, Gradient Map, Selective Color,
  Shadows/Highlights, HDR Toning, Desaturate, Match Color, Replace Color,
  Equalize. Non-destructive adjustment layers cover part of this list.

## Colour and image modes

- Mode conversion covers RGB, Grayscale and CMYK. Bitmap, Duotone, Indexed,
  Lab and Multichannel are placeholders, and the bit-depth rows in the Mode
  menu are placeholders.
- Spot and special inks, overprint, ink limit and a separations preview:
  absent.
- LUT loading: 3D LUT parsing and application exist with no UI to load a
  LUT.
- HDR Toning: a dead function behind a placeholder entry.
- The canvas is SDR — no HDR preview path. HDR stills export exists (PQ/HLG
  PNG presets with cICP).
- OpenColorIO input/output transforms: absent.
- A tone-mapping operator: absent.
- CMYK editing remains partial: RGB round trips at the appearance boundary
  for non-normal brush composites, vector and text paint, and layer
  compositing.

## Layers and compositing

- Smart objects: the layer kind exists in the enum but is never constructed.
  Open-as, embed/link, edit-contents and rasterize are placeholders.
- Merge Down, Merge Visible and Flatten Image are disabled entries, and the
  `Ctrl+E` / `Ctrl+Shift+E` keymap ids have no consumer.
- New Fill Layer (solid, gradient, pattern), Layer via Cut and the Align
  rows are placeholders.
- Layer comps (named layer-state variants): absent.
- Blend ranges ("blend if"), knockout groups and compound masks: absent.
- Mask as rubylith, Alt-click mask preview, eye-icon isolate and thumbnail
  add/subtract: absent.
- Artboards: in progress. Model, operations, panel and persistence exist;
  the tool and the Layer menu entries are still incomplete.
- Isolate Layers and Find Layers are placeholders.
- Stamp visible and transform-again: absent (stamp visible also has no key).

## Selection and masking

- Color Range, Focus Area, Sky and Objects are placeholders.
- Load Selection, Save Selection, Grow, Similar, Transform Selection and
  Reselect are placeholders.
- The Channels panel is a static list; saving a selection as a channel and
  loading one back are no-ops, and channel add/subtract/intersect from
  thumbnails is absent.
- Sky replacement reports itself unavailable.
- Selection edge quality is limited to the Modify and Select-and-Mask paths;
  there is no focus-area or depth-based selection.

## Painting and retouching

- Erase-to-history: absent.
- Frequency-separation retouching: absent.
- Straight-line connect, temporary eyedropper and the precision cursor are
  absent (also under Input).

## Text and vector

- Warp text: absent; the options-bar action falls through to the
  not-implemented path.
- Glyphs panel, match font, the top-level OpenType menu, anti-alias rows and
  orientation rows are placeholders. (OpenType feature toggles do exist in
  the Character panel.)
- Spell check reports itself unavailable.
- Create Work Path and Convert to Shape are placeholders.
- Linked symbols/instances: absent; only tiled clones exist.

## Filters and processing

- Vanishing-point editing: placeholder.
- Pin-based freeform warp and perspective warp: absent; the Transform menu
  routes Warp to Liquify. The only perspective tool is perspective crop.
- Content-aware scale and pattern preview: absent.
- Auto-Blend Layers only cross-fades overlap edges; no content-aware
  blending.
- Sky replacement: unavailable (see Selection).
- Lens correction: manual sliders only; no lens profile database.
- The filter gallery is a searchable single-filter picker, not a stacked
  effects gallery.
- The Neural group ships fifteen classical entries with no model inference:
  eight are parametric approximations whose controls are synthetic (depth
  blur, caricature, harmonization, landscape mixer, makeup transfer, smart
  portrait, style transfer, super zoom), one is effectively a stub (sketch
  to portrait), and six are content-aware classical effects (colorize,
  color transfer, JPEG artifact removal, photo restoration,
  photo-to-sketch, skin smoothing).
- Harmonization is only a colour-transfer approximation; there is no
  reference-image path.

## Multi-shot work

- Panorama merge, HDR merge, focus merge, astro stacking and stack modes:
  absent — there is no multi-shot source concept to merge.
- Several features are blocked on this: HDR Toning, the JPEG gain-map export
  option and HDR merge import paths all have no upstream source.

## Generative and AI

- Generative fill is implemented but inactive until a generative model is
  present in the model store; no generative model is bundled.
- Generative expand reports itself unavailable. Generative upscale,
  variants and background generation: absent.
- Model-backed tools that do exist: subject selection, select-and-mask
  refinement, background removal. Depth estimation and face detection for
  portrait tools are absent.

## Automation and extensibility

- Actions: the panel is a static stub — no recording, playback, editing or
  persistence.
- Batch across files, a save-and-close image processor and batch-rename
  workflows: placeholders or absent.
- No in-app scripting console. Subprocess extensions with SVG exchange
  exist, and CLI verbs exist but are not wired into the app entry point.
- No third-party plugin loader of any kind; Browse Plugins and Manage
  Plugins are placeholders. The plugin API is documented as not stable.
- Variables / data-driven graphics: placeholder.
- Extensions ▸ Export PDF with Links, Export DXF and Command Line entries
  are placeholders.

## Output and printing

- Print and Print One Copy are placeholders: no print dialog, no
  colour-managed printing, no proof printing, marks or bleed.
- Animated GIF and APNG export: absent (no animation model or timeline).
- PSD export is raster-only: layer styles do not survive the round trip.
- Built-in PSD, XCF and WebP encoders carry neither DPI nor ICC profile.
- TIFF orientation handling is partial: the streaming RGB decoder handles
  top-left only; other orientations fall back to the buffered decoder.
- HEIF, AVIF and JPEG XL support depends on runtime Qt image plugins; none
  is built in.

## Settings and preferences

Settings ships ten pages (General, Interface, Canvas, Cursors, Documents,
Export, Performance, Machine Learning, Shortcuts, Advanced). Missing against
a complete editor:

- Ruler and document units: no unit or origin preference. The measure tools
  carry their own unit pickers; everything else is pixels.
- Colour conversion options: rendering intent, black point compensation and
  dither are not exposed.
- Type preferences: type units, smart quotes, missing-glyph protection and
  localised font names: absent.
- File handling: a PSD save-compatibility mode and background saving are
  absent (layered PSD compression RLE/ZIP does exist).
- Scratch disk selection and a cache purge: absent. The Advanced page shows
  file locations only.
- History log file and snapshot preferences: absent (the undo limit exists).
- Camera raw defaults: absent with RAW support.
- A 30-bit/HDR display toggle: absent.
- Tool defaults: the preset well's Reset Tool, Reset All Tools and New Tool
  Preset items are inert (see Interface design).
- Menu customisation (hide, recolour or rebind individual menu items):
  absent; the Menus entry opens the Shortcuts page.
- UI language: no selector, and no translations ship (strings are
  translatable).
- Accessibility: no dedicated options (screen-reader descriptions,
  high-contrast handling).

## Interface design

- Preset management: no unified preset manager. Brushes import and export
  (.abr); patterns, gradients and layer styles have no load, save, rename or
  export UI, and tool presets have no list at all.
- A Layout persona slot is reserved but unpopulated.
- On-canvas colour picker HUD: absent; the resize HUD is the only on-canvas
  HUD.
- Dialogs follow the tool-options style; mode-based interfaces that live
  inside a dialog elsewhere (Liquify, camera raw) have no equivalent here
  (also under Input).

## Platform and interface

- Linux only by design: no Windows or macOS build.
- Touch input: absent (stylus and tablet events exist).
- No animation or timeline.
- View ▸ Proof Setup preset rows and New Guide Layout are placeholders;
  Window ▸ arrange rows are placeholders.
- Single shortcut profile (see Input).
- Vulkan or direct swap-chain output: not implemented.

## Assets, services and collaboration

- The Libraries panel is a static stub: no asset library, imports, search
  or sync.
- Cloud documents, share-for-review comments and version history: absent.
