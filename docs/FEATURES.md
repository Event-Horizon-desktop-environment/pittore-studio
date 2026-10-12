# Features

What Pittore Studio does today, and where the edges are. Anything marked
*partial* works but has gaps worth knowing about before you rely on it.

## Documents

- Layered documents with raster layers, groups, layer masks, clipping masks
  and adjustment layers.
- Non-destructive adjustment stack: the adjustment stays editable until you
  flatten or export.
- Full undo/redo history, plus autosave and crash recovery.
- Native `.psc` project format — one file per project (legacy `.ifp` still opens).
- Document modes offered at new-document time: RGB at 8, 16 or 32-bit,
  Grayscale/8 and CMYK at 8 or 16-bit. The renderer keeps an RGB appearance image and
  CMYK documents also retain per-layer device ink samples for file round trips.

## Colour

- ICC profile handling on import, with a configurable policy when the file's
  profile does not match the working space (embed, convert, or ask).
- CMYK conversion through Little CMS (`liblcms2`), falling back to naive
  conversion when it is not installed. Imported CMYK PSD layer data is retained
  as per-layer ink samples; native projects persist that data in v5 files.
- Image ▸ Mode converts to RGB, Grayscale or CMYK as one undo step. CMYK
  runs every colour through a destination profile (the document's own, the
  configured path, or a system probe) and back, so the canvas shows the
  gamut-reduced picture that will print; the mode tag and profile bytes
  travel with the document — PSD import and `.psc` persistence included.
- CMYK export: a CMYK-tagged document writes layered PSD (mode 4, five
  channels, embedded ICC) and TIFF (separated ink planes plus embedded
  profile) through its destination profile, flattening in appearance space
  before separating.
- Soft proofing against a destination profile.
- Wide-gamut display support and per-theme interface swatches.

Little CMS is an optional dependency. Build without it and the application
still works, but profiled conversion and soft proofing report themselves as
unavailable rather than doing the work.

## Painting and retouching

| Area | Tools |
|---|---|
| Selection | Rectangular and elliptical marquees, single row/column, lasso, polygonal and magnetic lasso, selection brush, quick selection, magic wand, object selection |
| Crop | Crop, perspective crop, slice |
| Healing | Spot healing, healing brush, patch, remove, content-aware move, red eye |
| Cloning | Clone stamp, pattern stamp |
| Painting | Brush, pencil, colour replacement, mixer brush, history brush, art history brush |
| Adjusting | Dodge, burn, sponge, blur, sharpen, smudge, adjustment brush |
| Filling | Gradient, paint bucket, flood fill |
| Other | Eyedropper, colour sampler, ruler, note, count, liquify |

Brushes are dab-based with tip textures, spacing, scattering, texture and
wet-mix settings. Strokes render through the same kernels on CPU and GPU.

## Vector

- Pen tools: pen, freeform pen, curvature pen, content-aware tracing.
- Path editing: add/delete/convert anchor points, path and direct selection,
  node, corner, contour and knife tools.
- Boolean path operations (union, subtract, intersect, divide).
- Around 40 parametric shapes, from basic geometry to badges, arrows,
  callouts and a QR code generator.
- SVG import and export.

## Text

- Horizontal and vertical type, plus type-mask tools that turn text into a
  selection.
- HarfBuzz shaping with kerning and OpenType features; FreeType rasterizing.
- Character and paragraph panels: family, style, size, leading, tracking,
  alignment, hyphenation.
- Fonts come from the system through fontconfig. Nothing is bundled.

## Filters

135 filters across 17 categories:

Blur, Blur Gallery, Sharpen, Distort, Lens, Noise, Pixelate, Sketch,
Stylize, Artistic, Brush Strokes, Texture, Render, 3D, Video, Neural, Other.

Every filter is a table entry (`src/engine/filter/*/defs_*.h`) with named
parameters and ranges, so adding one is a data change rather than a new
dialog. Filters open in a preview dialog and can be re-applied with the last
settings.

*Partial:* the Filter Gallery and Neural filters are wired up but a few
entries are placeholders.

## Layer styles

Drop shadow, inner shadow, outer glow, inner glow, colour overlay, gradient
overlay, stroke, bevel and layer blur. All of them derive from the layer's
own alpha, so a style follows the layer's shape as you edit it.

## Blending

28 blend modes shared by the compositor and the GPU kernels, including group
pass-through.

## Tools and interface

- Four personas — Pixel, Vector, Draw, Color — each swapping the tool set and
  rebuilding the options bar around it.
- Panels: Layers, Channels, Paths, Properties, Adjustments, Color, Swatches,
  Stroke, Appearance, History, Histogram, Navigator, Info, Brushes, Brush
  Preview, Character, Paragraph, Actions, Libraries, AI Models.
- Dockable, tabbed, restorable workspace layouts with saved presets.
- Canvas: zoom ladder, rotation, rulers, guides, grid, marching ants,
  transform and crop overlays.
- Keyboard shortcuts configurable from the preferences dialog.

## File formats

**Open:** `.psc` project (legacy `.ifp` still opens), PSD/PSB, XCF, KRA, Affinity (`.af`, `.afphoto`,
`.afdesign`, `.afpub`), SVG, and — when compiled in — WebP, TIFF and JPEG,
plus anything the installed Qt image plugins can read.

**Save:** PSD/PSB, XCF, SVG, PNG, JPEG, TIFF, WebP, GIF, and the rest of the
Qt writer set.

*Partial:* layered export to third-party formats is best-effort. Text and
adjustment layers are rasterized on export where the target format has no
equivalent, and linked sources are skipped rather than flattened.

## AI features

- Background removal.
- Object selection.
- Selection refinement and segmentation.

The models are ONNX files fetched on demand into the per-user cache; nothing
is bundled and nothing downloads by itself. GPU inference needs ONNX Runtime
built with CUDA; without it the models run on the CPU.

```sh
just models          # download everything
just model birefnet-portrait
```

## AI bridge (MCP, partial)

AI assistants can drive the live session through Model Context Protocol.
The app listens on a per-user local socket; the `pittore-mcp` shim (stdio,
installed alongside the app) translates MCP into bridge commands. The app
must be open — without a live session every tool call fails honestly.

```json
{
  "mcpServers": {
    "pittore-studio": { "command": "pittore-mcp" }
  }
}
```

Tools: `ping`, `list_documents`, `document_info` (active document metadata
and layer stack), `composite_png` (rendered canvas as an image, long side
capped), `list_filters` (every engine filter with its parameters),
`new_document`, `add_shape` (rectangle/ellipse/triangle), `add_text`,
`apply_filter`, `set_layer_visible`, `set_layer_blend`, `set_layer_opacity`,
`set_active_layer`, `undo`, `redo`, `save_project`, `open_image`,
`export_png`. Tool system: `list_tools` (every editor tool),
`list_tool_options` (all options with ranges, defaults and live values),
`set_tool_option`, `list_brushes` (factory plus custom presets),
`select_brush`, and `paint_stroke` (round-brush dabs along a polyline, one
undo step; full brush dynamics stay pointer-side). Reads are free; mutations reuse the panels' undo-safe entry
points, run on the GUI thread, and must stay fast (a call blocks the UI
while it runs). Only this user can reach the socket; there is no network
listener.

## Performance

- Tile-based compositing so only the changed region is recomposited.
- CUDA and HIP backends written from the same kernel source as the CPU path.
- A GPU parity test suite that compares CPU and GPU output pixel for pixel.
- Proxy previews for very large documents.

## Not there yet

- Normal source-over brush and pencil dabs in build-up or wash mode, plus
  normal-composite bitmap stamp tips in all color modes, blend directly into
  retained CMYK ink samples and derive their RGB preview through the document
  profile.
  Color Replacement and Paint Bucket fills also update device inks; Magic
  Eraser and alpha-only selection edits preserve them. Accepted destructive
  filters re-separate through the document profile; cancelled filter previews
  restore the original inks. RGB-based raster edits such as tonal brushes,
  healing and cloning re-separate only their changed regions, preserving
  untouched device samples. Non-normal brush composites, vector/text paint and
  layer compositing still cross the RGB appearance boundary. This is partial
  native CMYK editing, not full pipeline coverage.
  CMYK TIFF native samples are retained when a supported image opens
  at full resolution within the document's memory budget; proxied files and
  unsupported TIFF layouts use the appearance path. Lab, Bitmap, Duotone,
  Indexed and Multichannel modes remain disabled placeholders.
- No animation or timeline.
- Plugin API is declared but not stable.
- No Windows or macOS build; the meson configuration errors out on anything
  that is not Linux.
