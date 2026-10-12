# Pittore Studio — Projectmap

> Ultra-detailed map of the repository for AI assistants and new contributors.
> Feed this file to any AI to orient it in minutes instead of hours.
> Source of truth for rules is `AGENTS.md`; this file maps _where_ and _what_.
> Generated 2026-10-08 from the working tree at `/home/matt/Documents/GitHub/pittore-studio`.
> Rule: `src/ui` depends on `src/engine`, never the reverse. No Qt headers in `src/engine/`.

## 0. How to use this document with an AI

- Paste this file plus `AGENTS.md` into context first, then ask the task.
- For a bug: point at the workflow in §11 (open/save, stroke, undo, drop, export, select, filter, vector).
- For a new feature: read the "Where to change" table in §12, then open only the listed files.
- For formats (`.psc`, `.ifp`, PSD, XCF, Affinity): read §6.1 first — verbatim round-trip policy is load-bearing.
- For kernels: read §5.4 (CPU = reference, GPU parity, how to add a kernel).
- Never loosen a failing assertion to get green. Reproduce, find root cause, fix.

## 1. Project snapshot

- Pittore Studio: layer-based photo editor for Linux, C++20 on Qt 6. Raster painting/retouching, vector persona, text, adjustment layers, filters, ICC-aware colour pipeline. GPU compute (CUDA or HIP) with CPU fallback; CPU is the reference. Status: alpha. Linux only — the build refuses to configure elsewhere. License: MIT (`LICENSE`).
- Repo scale: **1122 files** under `src tools tests docs packaging` (engine/vector 363, ui 420, tests 124, io 56, compute 55, filter 39, window 31, canvas 22, panels 22, tools 17, persona 36, export 6, ai 14, color 9, core 8, text 8, render 13).
- Entry: `src/app/main.cpp` builds the `painter` binary (`./build/src/app/painter`).
- Logs: `~/.config/PittoreStudio/log/` (`Pittore-UX.log`, `Pittore-Render.log`, `Pittore-GPU.log`, `Pittore-Tools.log`, `Pittore-Crash.log`).

## 2. Commands, build, tests, diagnostics

```sh
./build.sh                                   # bootstrap a machine, configure, compile
meson setup build                            # configure
ninja -C build                               # compile
./build/src/app/painter                      # run
just test                                    # build + run the full suite
ninja -C build && meson test -C build --print-errorlogs   # same, without just
meson test -C build <name> --print-errorlogs # single test
```

- `just models` downloads AI models (multi-GB, never bundle). `just install` installs system-wide.
- Meson options (`meson_options.txt`): `backend-cuda`, `backend-hip`, `cuda-root`, `hip-root`, `onnxruntime`, `onnxruntime-root`, `app`. Backends and ONNX Runtime auto-detect. Never require a GPU toolchain.
- SVG imports log one `[import] SVG ...` line with leaf counts, geometry MB, raster area, layer count, document size, `verdict=ok|degraded|failed`. Degraded paths log their own warning.
- Tests needing absent sample files must skip, never commit large binaries. File formats preserve unknown data on round trip where possible; never silently discard — report baked/dropped.

## 3. Hard rules (from AGENTS.md)

1. Diagnose and fix; never paper over a failing test or loosen an assertion.
2. Keep `just test` green; report actual results including unfixed failures.
3. One responsibility per file; prefer a new small file over growing a large one (several files already exceed 50 KB — do not grow them).
4. Match surrounding style; no drive-by refactors; diffs scoped to the task.
5. No emoji anywhere. Terse explanations: what changed, why, what was verified.
6. Legal (most serious defect class): do not copy/translate/paraphrase GPL/LGPL/AGPL (Krita, Inkscape, GIMP, darktable, LibreOffice) or proprietary code. Permissive code (MIT/BSD/Apache-2.0/ISC/Zlib) only with notice + attribution in third-party notices in the same change (URL + commit). Formats from public specs/observed behaviour; Affinity reverse-engineering extensions need maintainer approval. No embedded third-party bytes (templates, ICC, fonts, brushes, patterns) unless redistribution-licensed and recorded. Models on demand only, licence-checked, never non-commercial defaults (e.g. BRIA RMBG banned). Numeric constants original fits or cited papers. Product names descriptive only.
7. New dependency needs maintainer approval + licence + justification + notices entry.
8. Small focused commits, imperative subject < 72 chars. No build output, logs, models, `.bak`, machine paths. No history rewrite.
9. When unsure ask: format semantics, licensing, public API/project-format changes, rendered-output changes.

## 4. Layout

```
src/engine/    core types, compute kernels, IO codecs, filters, render, text, colour, vector, AI
src/ui/        Qt interface: panels, canvas, tools, dialogs, settings
src/app/       entry point (main.cpp)
tests/         unit, codec, GPU-parity, headless UI tests
tools/         developer utilities (run_tone_blend16)
docs/          FEATURES.md, BUILDING.md
packaging/     desktop file, metainfo, MIME, icons
```

- `src/engine/core`: `pixel.h` (linear RGBAf), `image.h` (host Image), `tile.h` (256px), `document.h` (minimal test doc), `log.h` (Qt-free logger), `parallel.h` (row/col splitters), `tonal_ops.h` (host tonal ops), `meson.build`.
- `src/engine/compute`: `backend.h` (interface), `factory.*`, `cpu_backend.*` (reference), `cuda_backend.cu/.h`, `hip_backend.cpp/.h`, `blend.*`, `adjust.h`, `oklab.h`, `dither.h`, `paint.h`, `layer_mask.*`, `warp.*`, `tone_blend.*`, `brushes/*`, `meson.build`.
- `src/engine/color`: `convert.*` (naive CMYK), `icc_convert.*` (LCMS2), `proof.*` (soft proof), `separate.*` (export separation), `meson.build`.
- `src/engine/render`: `layer_style.h`, `style/shared/style_plane.*`, `style/blur/*`, `style/sdf/*`, `style/blend/*`, `style/fx/*`, `style/apply/layer_style_apply.cpp`, `meson.build`.
- `src/engine/io`: `project.*` (.psc/.ifp), `psd.*`, `xcf.*`, `tiff.*`, `webp.*`, `zip.*`, `icc.h`, `icc_scan.*`, `af.h`, `af/*` (flat), `af_layers*/**` (layered), `meson.build`.
- `src/engine/filter`: `filters.h` (dispatcher), `core/*`, `registry/filter_registry.h`, per-category `defs_*` + `apply_*` (blur, blur_gallery, sharpen, noise, pixelate, distort, lens, stylize, artistic, brush_strokes defs-only, sketch, texture, render, threed, video, neural, other), `meson.build`.
- `src/engine/text`: `text_engine.h`, `shared/text_internal.h`, `face/text_face.cpp`, `shape/text_shape.cpp`, `layout/text_layout.cpp`, `raster/text_raster.cpp`, `family/text_family.cpp`, `meson.build`.
- `src/engine/vector`: `path.*`, `vector_shape.*`, `vector_art.*`, `vector_scene.h`, `svg_parse.*`, `svg_dom.*`, `svg_exchange.*`, `boolean.*`, `path_ops.*`, `clone.*`, `clip_mask.*`, `marker.*`, `pattern.*`, `mesh.*`, `filter_fe.*`, `grids.*`, `snap.*`, `transform_ops.*`, `spiro.*`, `calligraphy.*`, `connector.*`, `trace.*`, `box3d.*`, `spray.*`, `text_flow.*`, `palette.*`, `lpe/*`, `svg/*` (~140 one-concern files), `meson.build`.
- `src/engine/ai`: `bg_remove.h` (public API), `shared/*`, `session/*`, `preprocess/*`, `segment/*`, `encode/*`, `decode/*`, `select/*`, `refine/*`, `meson.build`.
- `src/ui`: `app_state*.cpp` (11 TUs), `main_window.h`, `canvas_view.h`, `window/*`, `canvas/*`, `persona/*`, `panels/*`, `tools/*`, `export/*`, dialogs, `settings.*`, `theme.*`, `keymap.*`, `icons.*`, `project_manager.*`, `meson.build`, `icons.qrc`, `brushes/*`.
- `src/app`: `main.cpp`, `meson.build`.
- `tools/`: `meson.build`, `run_tone_blend16.cpp`.
- `tests/`: 101 `test_*.cpp` + probes/benches, `test_util.h`, `gpu_parity.*`, `gpu_invariants.*`, `af_probe.h`, `test_all.cpp`, `meson.build`.
- Root: `meson.build`, `meson_options.txt`, `justfile`, `build.sh`, `AGENTS.md`, `README.md`, `LICENSE`, `ink.md`, `Reference/`, `Pittore-studio.zip` (artifact, not source).

## 5. Engine core, compute, colour, render

### 5.1 `src/engine/core/` — primitives (header-only, no Qt)

| File | Role | Key symbols |
|---|---|---|
| `core/pixel.h` | Working pixel: linear-light straight RGBA float, `float4`-compatible | `pittore::RGBAf` (alignas 16, `r,g,b,a`, `operator[]`) |
| `core/image.h` | Host row-major image, `shared_ptr` COW for undo | `pittore::Image`: `(w,h)`, `at`, `data`, `fill`, `clone` |
| `core/tile.h` | 256px tiling (one workgroup/wavefront chunk per tile) | `kTileSize=256`, `TileRect`, `TileGrid` |
| `core/document.h` | Minimal layer-stack doc for tests (editor uses richer doc in ui) | `pittore::Layer/Document`, `compose(backend, out)` |
| `core/log.h` | Qt-free logger to stderr + `~/.config/PittoreStudio/log/`; `PITTORE_*` env wins, legacy `INFINITY_*` honoured | `core::log::{log_info/warning/error, set_log_dir, strokeTrace}`, `PITTORE_LOG` |
| `core/parallel.h` | Row/col/item splitters, no OpenMP, bit-identical to serial | `parallel_rows/parallel_for/parallel_cols` |
| `core/tonal_ops.h` | Host tonal/geometry ops, linear [0,1]: histogram, Levels, Curves LUT, seeded noise, median, unsharp, rotate/flip | `computeHistogram/applyLevels/buildCurveLUT/applyAddNoise/applyMedianFilter/applyUnsharpMask/rotate90Cw/flipHorizontal` |
| `core/meson.build` | Header-only marker | — |

### 5.2 `src/engine/compute/` — backends + shared kernel math (lib `pittore-compute`)

- Interface `compute/backend.h`: `BackendType{CPU,CUDA,HIP}`, `Device`, `Buffer` (staging + upload/download + region), `PlacedLayer` (batched composite descriptor), `ComputeBackend` virtual kernel set with host-staging defaults.
- `factory.h/.cpp`: `enumerate_devices/make_backend/make_default_backend` (CUDA, then HIP, else CPU); guards `PITTORE_HAS_CUDA/HIP`.
- `cpu_backend.h/.cpp`: canonical reference. GPU must match it. `CpuBuffer` upload/download are no-ops.
- `cuda_backend.h/.cu` (~2571 lines): NVIDIA path, `kBlock=256`, `k_*` kernels including shared headers verbatim. `hip_backend.h/.cpp` (~2514 lines): 1:1 AMD port.
- Shared single-source math (host + device compile via `PITTORE_*_DEVICE`): `blend.h/.cpp` (27 PSD-compatible modes, Dissolve hashed on doc coords), `adjust.h` (14 live-adjustment kinds), `oklab.h` (tone-blend perceptual space), `dither.h` (Bayer-8, doc-coord deterministic), `paint.h` (brush umbrella), `layer_mask.h/.cpp` (opaque-grey coverage in R, never alpha-carried), `warp.h/.cpp` (liquify 4px mesh + per-dab subgrid), `tone_blend.h/.cpp` (reharmonization transfer).
- `brushes/`: `dab` (paint), `erase` (+`bg_erase.h` core), `clone`, `heal` (largest TU, diffusion solver), `composite`, `mip`, `flood`, `replace`, `smudge` (dirty-brush carry), `stamp` (+`pattern.h` procedural tiles), `tone`, `history/history_dab.h`, `blur/blur_dab.h`, `adjust/adjust_dab.h`, `selection_mask`, `tip/tip.h`, `texture/texture.h`, `mask/mask.h`, `mixer/mixer.h`, `loaders` (clean-room GBR/GIH/ABR tip parsers). Convention: `*_host(dst,w,h,cx,cy,radius,...,bboxOut,selection?)`, exclusive bbox, touched-bool.

### 5.3 `src/engine/color/` (lib `pittore-color`, host-side only)

| File | Role | Key symbols |
|---|---|---|
| `color/convert.*` | Naive profile-free CMYK↔RGB fallback | `color::{CmykF, rgbToCmyk, cmykToRgb}` |
| `color/icc_convert.*` | LCMS2 CMYK↔sRGB (pimpl, 16-bit path, per-decode instance) | `color::{CmykToSrgb, SrgbToCmyk}` |
| `color/proof.*` | Soft-proof manager (intents, BPC, gamut alarm; one per thread) | `color::{ProofIntent, ProofManager::applyProof}` |
| `color/separate.*` | RGBA16 → CMYK ink planes at write time | `color::separateRgba16` |
| `color/meson.build` | Optional lcms2>=2.9, `-DPITTORE_LCMS2` | `pittore_color_dep` |

Colour rule: respect embedded ICC + working-space mismatch policy. Never silently treat unmanaged pixels as sRGB — call sites branch explicitly.

### 5.4 CPU = reference / GPU parity; how to add a kernel

1. Scalar math in a shared header compiling on host + device (ternaries, plain C math, `::powf` on device).
2. Declare on `ComputeBackend` with host-staging default; implement reference in `cpu_backend.cpp` (`parallel_rows`, bit-identical loops).
3. Add `k_*` kernel + override in `cuda_backend.cu`, port 1:1 to `hip_backend.cpp`.
4. Extend `tests/gpu_parity.cpp` (`run_gpu_parity`, driven by `test_cuda.cpp`/`test_hip.cpp`); keep `just test` green.

### 5.5 `src/engine/render/` (lib `pittore-render`, CPU, toolkit-free)

- `render/layer_style.h`: public style API (shadow/glow/satin/overlay/stroke/bevel/blur), `outset()` growth, `applyLayerStyle`. Used by `.af` `FiEf` bake and the Layer Style dialog — one implementation.
- Internals (`detail::`): `style/shared/style_plane.*`, `style/blur/*` (3-box gaussian), `style/sdf/*` (distance transform), `style/blend/*`, `style/fx/*` (per-effect builders), `style/apply/layer_style_apply.cpp` (orchestration).

## 6. Engine IO, filters, text, vector, AI

### 6.1 `src/engine/io/` — codecs (pure C++ byte buffers; zlib required; webp/tiff/jpeg/zstd optional)

| Format | Decode | Encode | Notes |
|---|---|---|---|
| Native `.psc` / legacy `.ifp` | `projectDecode/projectScan` | `projectEncode` | v1/v2 (+v4 for PSD foreign blocks); trailing `icc`; `PsdBlock{sig,key,data,padding}` verbatim (`project.cpp:492`) |
| PSD | `psdDecode`, `psdDecodeLayers` (raw/RLE, 8/16b RGB/gray/CMYK/Lab) | `psdEncodeRgba`, `psdEncodeLayers` | `RawBlock` verbatim; never stores regenerated `luni/lsct/masks`; stand-in adjustments skipped; ICC `0x040F` |
| XCF v0/v1 | `xcfDecode` (RGB/gray/indexed→RGBA16) | `xcfEncodeRgba` (single-layer RLE) | flat only |
| TIFF | `tiffDecodeRgba16`, banded + subsampled streaming, `tiffProbeFile` | `tiffEncodeRgba16`, `tiffEncodeExport` (bits/gray/alpha/LZW/ZIP/CMYK+embed) | ICCPROFILE tag dir 0 |
| WebP | `webpDecodeRgba16` | `webpEncodeRgba16` (lossless/lossy) | container ICC via `icc_scan` |
| ZIP | `zipRead/zipFind` | `zipWrite` | dup names keep last |
| Container ICC | `containerIccProfile(Name)` | — | JPEG APP2, PNG iCCP, WebP ICCP |
| Affinity flat | `afProbe/afDecode/afDecodeDocument/afDecodePngRgba16` (largest PNG preview) | — | preview-only; placements reported |
| Affinity layered | `afDecodeLayers` → `AfLayersDoc` | `afBuildTemplateDoc` + `afEncodeLayers` → `AfEncodeResult{bytes,skipped}` | masks live; effects/filters baked; adjustments skipped+logged; template-derived writer, `Value::wire` byte-identical re-emit |

Files: `project.*`, `psd.*`, `xcf.*`, `tiff.*`, `webp.*`, `zip.*`, `icc.h`, `icc_scan.*`, `af.h`, `af/shared/af_preview.*`, `af/png/af_png.*`, `af/meta/af_meta.cpp`, `af/doc/af_document.cpp`, `af_layers.h`, `af_layers/container/af_archive.*`, `af_layers/graph/af_graph.* + af_graph_parser.h + af_values.cpp`, `af_layers/geom/af_geom.* + af_homography.*`, `af_layers/image/af_image.* + af_tiles.cpp + af_raster.cpp`, `af_layers/filter/af_blur.* + af_distort.* + af_fx.* + af_live.*`, `af_layers/vector/af_shapes.*`, `af_layers/walker/af_walker*.cpp + af_document.cpp`, `af_layers/emit/af_emit.* + af_write.*`, `meson.build`.

### 6.2 `src/engine/filter/` — define → register → apply

1. Define: `defs_<cat>.h` returns `vector<FilterDef>` (`id/name/category/params`; kind 0 scalar, 1 choice, 2 toggle).
2. Register: `registry/filter_registry.h::allFilterDefs/findFilter/defaultParams`.
3. Access: `core/filter_params.h::pv/p2v..p14v`; types in `core/filter_types.h`.
4. Apply: `apply_<cat>.h::apply<Cat>(img, scratch, id, par)->bool`; `filters.h::applyFilter` tries Blur→…→Other in order. Shared math in `core/filter_detail.h::detail`. All CPU, `parallel_rows/cols`.
- Quirk: `brush_strokes/` has defs only; its 8 ids are implemented in `apply_artistic.h` (7) + `apply_stylize.h::accented_edges`. `apply_neural.h` forward-declares umbrella `applyFilter` for internal delegation. Categories: blur(11), blur_gallery(5), sharpen(6), noise(5), pixelate(7), distort(11), lens(3), stylize(11), artistic(15), brush_strokes(8 defs), sketch(14), texture(6), render(8), threed(2), video(2), neural(15 heuristic), other(6).

### 6.3 `src/engine/text/` — font stack (fontconfig → HarfBuzz → FreeType)

`text_engine.h` (public: `Align/OTFeature/TextSpec/TextRaster/LineSpan/Caret/TextLayout`, `familyNames/layoutText/caretAt/hitTest/rasterize`), `shared/text_internal.h` (detail), `face/text_face.cpp`, `shape/text_shape.cpp`, `layout/text_layout.cpp`, `raster/text_raster.cpp`, `family/text_family.cpp`, `meson.build`. Single family/style/size LTR, kerning, wrap, align → 8-bit coverage.

### 6.4 `src/engine/vector/` — vector persona (toolkit-free)

Top: `path.*` (scanline rasterizer), `vector_shape.*` (anchors/live shapes/gradients), `vector_art.*` (retained art + SVG writer + IFP codec), `vector_scene.h` (legacy flat scene), `svg_parse.*` (flat parser), `svg_dom.*` (DOM + cascade, unknown attrs preserved), `svg_exchange.*` (DXF/PDF writers, extension host, CLI verbs), `boolean.*` (1e-9 polygon ops), `path_ops.*`, `clone.*`, `clip_mask.*`, `marker.*`, `pattern.*`, `mesh.*`, `filter_fe.*` (fe* CPU + 250-entry gallery stub), `grids.*`, `snap.*`, `transform_ops.*`, `spiro.*`, `calligraphy.*`, `connector.*` (A* routing), `trace.*` (marching-squares + centerline), `box3d.*`, `spray.*`, `text_flow.*`, `palette.*` (GPL/ASE/ACB-subset), `lpe/*` (live path effects, one TU per effect), `meson.build`.
`svg/` (~140 one-concern files): pipeline `xml_reader → css_parse/style → length/transform/path_data/color_parse → scene → render_item/bounds → raster_tile/compose/scene_image → export_svg/svg_save + import_bridge`; `vector_scene_check` parity gate; `thread_pool` rows/tiles; `tag_count/depth_guard/ref_guard/size_guard/utf16_check/bom_strip/zip_magic` DoS guards. See §13 file index for every name.

### 6.5 `src/engine/ai/` — ONNX segmentation/SAM (optional `PITTORE_HAS_ONNX`; models never bundled)

`bg_remove.h` (sole public header: `onnx_available/version`, `SegmentResult/segment_rgba8`, `SamEncodings/encode_rgba8`, `SamDecodeResult/decode_point/points/mask_points/mask/refine_mask/auto_subject/segment_rgba8_pair`, `guided_refine_alpha/align_alpha_to_reference/clear_session_cache`), `shared/*`, `session/*` (per-path session cache), `preprocess/*`, `segment/ai_segment.cpp`, `encode/ai_encode.cpp`, `decode/*`, `select/ai_select.cpp`, `refine/ai_refine.cpp`, `meson.build`.

## 7. UI state core (`src/ui`: AppState, documents, projects, settings)

### 7.1 `AppState` / `DocumentItem` (`app_state.h` ~2395 lines, `app_state.cpp` ~7608 lines + 10 split TUs)

- `app_state.h`: `TaskContext/ScreenMode/ChromeVisibility`; `TextItem` (live-text spec); `LayerItem` (Kind Pixel/Text/Shape/Adjustment/Group/SmartObject/Frame; mask block; adjustment block + LUT; tone-blend; `pixels/offset/scale`; deferred RGBA8; heightMap; live-filter + styled caches; thumbnails; `art` geometry; PSD raw blocks); `DocumentItem` (title/paths/size/dpi/colorMode/icc/dirty, zoom/pan/rotation, layers index-0-top, history, selection rect+mask+stamp, guides, annotations, slices, `composite` QImage, proxy fields, backend; `rebuildComposite/recompositeRegion/renderRegion`, group/visibility queries, snapshot undo); `AppState: QObject` (tools+options, stamp/pattern/hose libraries, stroke state, colours, modes, snap bitmask, proof toggles, prefs, documents, projects/import/export, selection/layers, vector commits, type API, undo gestures; ~25 signals: `toolChanged/documentsChanged/activeDocumentChanged/layersChanged/historyChanged/selectionChanged/...`).
- Splits: `app_state_detail.*` (shared internals), `app_state_open.cpp` (all open paths incl. PSD/SVG/AF layered), `app_state_save.cpp` (save, recovery snapshot, session JSON, mismatch policy), `app_state_layers.cpp` (placement/selection/grouping/reorder), `app_state_mask.cpp` (masks/clipping), `app_state_adjust.cpp` (adjustment layers), `app_state_stroke.cpp` (dab execution), `app_state_stamp.cpp` (stamp/wash/flood/clone/heal/patch), `app_state_smudge.cpp`, `app_state_selection.cpp`, `app_state_undo.cpp` (snapshot COW undo, `begin/commit/discardUndoStep`), `app_state_text.cpp` (live text).
- Any user-visible mutation must go through history. `composite_gpu.cpp` is the GPU delegate of `rebuildComposite`.

### 7.2 Projects, import/export, colour policy

- `project_manager.h/.cpp`: `ProjectLayerMeta/ProjectFileData`, `projectsRootDir` (`~/Pictures/Pittore Studio/Projects`, `$PITTORE_PROJECTS_DIR`), `projectPathForName`, `isNativeProjectSuffix` (psc + legacy ifp), `saveProjectFile/loadProjectFile` (atomic `QSaveFile`), RGBA64 bridges, `qimageFromFile(Capped)` (PSD/PSB/XCF built-ins, KRA `mergedimage.png`, AF preview, WebP/TIFF, Qt plugins; proxy subsample), `probeImageSize/proxyFactorForPixels`, `suffixIsOpenable/dropHasOpenableFiles`, `imageOpenFilter/imageExportFilter`, `saveImageFile`, `scanProjects`, `ProjectManagerDialog` (start page: thumbnail grid + New Project form, drag-drop open).
- `image_ops.h` (header-only `imageops`): Image-menu pixel math (autoTone/Color, gradientMap, CubeLut, selectiveColor, shadowsHighlights, hdrToning, matchColor, affine resample…).
- `embedded_icc.h`: `embeddedProfileName(path, decoded)` — container scan first, `QColorSpace` fallback.
- `color_mismatch.*`: `ImportProfileChoice` (UseEmbedded/Convert/Keep/Discard/Cancel), `askImportedProfile` modal (UseEmbedded default).
- `color_mode.*`: Image ▸ Mode (`resolveCmykProfile`, `convertDocumentMode`; RGB-internal, one undo step).

### 7.3 Settings, theme, icons, keymap, brushes

- `settings.h/.cpp`: TOML `~/.config/PittoreStudio/Settings.toml` (`AppSettings`: theme, compute, AI models, autosave/session, canvas Show flags, nudge/grid/slices, colours, tablet/cursor, new-doc defaults, workingProfile, mismatch policy default AlwaysAsk=3, export defaults, undoLimit, proof, cmykProfile, recents). `applySettings` swaps backend + recomposites.
- `theme.h/.cpp`: `UiTheme` (Black/DarkGray/MediumGray/LightGray), `ThemeColors`, `paletteFor/styleSheetFor/canvasSurround`.
- `icons.h/.cpp` + `icons.qrc`: Lucide ISC recoloured icons + procedural fallbacks (`glyphPixmap/toolIcon/chromeIcon`; no `fx.svg` by design).
- `keymap.h/.cpp`: `keymap.json` beside Settings, `defaultKeymapEntries`, `keyEventMatchesShortcut`.
- `brushes/`: `pressure_curve.h`, `brush_library.h` (presets + disk), `brush_preview.h` (real-kernel thumbnails), `bundle_import.h` (third-party `.bundle` mapping), `sensor_drives.h` + `sensor_drives_json.h` + `sensor_drives_dialog.h`.
- `ui/meson.build`: Qt6 Widgets layer, moc headers, all `app_state*.cpp` + canvas + window + persona + panels + tools + export sources; static lib `pittore-ui`.

## 8. Window shell, menus, dialogs, canvas

### 8.1 `main_window.h` + `window/` (MainWindow owns no editing state; R24 anatomy A–F)

- `window/shell/window_shell.cpp`: ctor (dock nesting, `setAcceptDrops(true)`), build order DocumentArea→MenuBar→Keymap→Corner→Docks→StatusBar, persona/workspace/state/shortcuts/theme/crash wiring.
- `window/shell/document_area.cpp`: central `OptionsBar` + `DocumentTabBar` + `CanvasView` + task-bar wiring. `DocumentTabBar: QTabBar` accepts file drops → `openProjectFile` as new tabs (Photoshop behaviour); canvas viewport drops place as layers; non-file drags fall through for tab reorder. Brush tool shows brushes panel.
- `window/shell/window_session.cpp`: R98 crash safety (marker, autosave, `.psc` preferred/`.ifp` fallback, clean-quit session).
- `window/shared/window_helpers.*`: `kSpringDwellMs=220`, `FloatingPanelWindow`, `renderLayerForExport`.
- `window/state/window_state.cpp`: corner switcher, status bar (zoom/doc stats/device), `wireState`, `syncUndoRedo/DocumentTabs`, theme/screen/chrome.
- `window/state/window_shortcuts.cpp`: R21 keyboard grammar (app event filter, keymap carriers, spring tools, space/ctrl latches, two-digit opacity, brush-size keys).
- `window/docks/window_docks.cpp`: left Tools dock + right top/bottom rail groups, `Layers`/`History` leads, floating Character/Paragraph.
- `window/menus/`: `menu_bar.cpp` (fan-out), `menu_actions.cpp` (primitives + keymap overlay), `menu_file/edit/image/layer/type/select/filter/view/object/path/plugins/window/help.cpp` (builders only), `ops/menu_ops_edit.cpp` (Edit behaviour), `window/menus/menu_window.cpp` (arrange, workspaces, panels).
- `window/commands/window_commands.cpp`: central `runCommand(id)` router + `maskedSelectionCopy/refineSelection`; `window_ai.cpp`: `runAiBackgroundRemoval/runEnhanceEdges`.
- `window/dialogs/`: `dialogs_file.cpp` (new/open/place/save/export + window drops), `dialogs_filter.cpp` (tonal/filter sessions + guides), `dialogs_export.cpp` (slices + About), `dialogs_ui.cpp` (toolbar/workspace/shortcuts/spotlight/prefs/proof/CMYK), `dialogs_liquify.cpp` (liquify guard).

### 8.2 `canvas_view.h` + `canvas/` (`CanvasView: QAbstractScrollArea`; overlay R20 never composited; `paintDocument` is the Vulkan-swap target)

Public: zoom family + rotation, Show toggles, `viewToDocument/documentToView`, brush-cursor probes, `MoveSnap` snapping, liquify API, `lockReplacePalette/airbrushTick`, stylus math; signals `zoomChanged/cursorMoved/colorSampled/copy/paste/pasteInPlace/clear/placeRequested/rotationChanged/layerPickedFromCanvas`; `eventFilter` headless-testable.
- `canvas/shell/canvas_shell.cpp` (ctor: rulers, task bar, drop target, ants/airbrush/hover/zoom timers), `canvas/shell/canvas_events.cpp` (viewport eventFilter + Zoom R14 menu + `acceptDropImage`: SVG places as shapes, PSD/PSB/.af open as own doc, raster places).
- `canvas/shared/canvas_helpers.*` (tool predicates, liquify brush, `CanvasRuler`, lasso/selection helpers, `isCodecImportSuffix/dropMimeHasImage`), `canvas/shared/symmetry.h` (pure doc-space mirror math).
- `canvas/paint/`: `canvas_brush.cpp` (`DabToolOpts` per-event cache, symmetry, gap-fill), `canvas_document.cpp` (region `paintDocument`, `paintSegmented`, proof cache), `canvas_overlay.cpp` (brush ring, marquee, handles, guides/grid), `canvas_liquify.cpp` (mesh stroke), `canvas_annotations.cpp` (pins/notes/count/slices, 7px hits).
- `canvas/interactions/`: `canvas_mouse_press/move/release.cpp` (gesture router → live update → one-history-step commit), `canvas_hover.cpp` (outline cache, double-click-to-edit), `canvas_cursor.cpp`.
- `canvas/pen/canvas_pen.cpp` (Pen/Freehand/Curvature sessions, ribbon, tablet latches), `canvas/pen/canvas_vector_tools.cpp` (drag vector tools).
- `canvas/move/canvas_move.cpp` (body/handle drags, auto-select, snap guides).
- `canvas/ai/canvas_ai_select.cpp` (SAM embeddings per layer+model, fused decode, hover settle).
- `canvas/text/canvas_text.cpp` (live text-layer sessions + Type-Mask modes).
- `canvas/view/canvas_zoom.cpp` (view maths, settle rebake), `canvas/view/canvas_rulers.cpp` (guide drag-out).

### 8.3 Toolbar, options, task bar, workspace, spotlight

- `options_bar.*`: per-tool strip from `OptionSpec` rows; `commandTriggered(ToolId,id)`.
- `tools_panel.*`: vertical strip, flyouts, Shift-cycle, quick-mask + screen-mode footer, FG/BG wells.
- `tools/`: `ids/tool_ids.h` (~130 `ToolId`), `defs/tool_defs.h` (sections/groups), `table/tool_table.*` (order = strip order; add a tool = one line) + `tool_lookup.cpp`, `options/tool_options.h` (schema) + `options_registry.cpp` (section dispatch) + `options_selection/crop/retouch/draw/navgen.cpp` + `option_builders.*`, `log/tool_log.*` (per-tool logs + crash breadcrumb).
- `tool_registry.*`: compat stub re-exporting the above.
- `contextual_task_bar.*`: floating viewport strip per `TaskContext` (Selection/Text/…), emits command ids.
- `workspace.*`: named layouts (`saveState` + panel sets), built-ins (Essentials/Photography/Painting/Graphic-and-Web/Typography/Motion/Color-Grading).
- `spotlight.*`: quick search over menus + tools + panels.
- `panels.*` + `panels/registry/*`: dispatch contract + `allPanels()` table (~30 panels); adding a panel = one `PanelInfo`.

## 9. Panels, persona, export, dialogs, app, tools, tests, docs, packaging, root

### 9.1 `panels/` (factory per panel, never cross-include)

layers (stack + `kLayerDragMime` re-parent), channels, paths, adjustments, properties, color, swatches, history, info, navigator, histogram, brushes (+preview), character*/paragraph* (floating), actions, libraries, aimodels, vector extras (Align/Transform/XML/ObjectProps/FilterEditor/Symbols/DocProps/Trace/Extensions/Pages/Markers/LPE), `shared/panel_helpers.*`, `registry/panel_creators.h + panels_registry.cpp`.

### 9.2 `persona/` (Pixel/Vector/Draw/Color)

`persona.h/.cpp` (QtCore-only mapping, testable), `persona_bar.*` (tab bar), `persona_manager.*` (switching), `persona_wire.*`, `appearance_panel.*`, `stroke_panel.*`, `vector_build.*` (one-undo creation tail), `vector_shapes.*` (~40 live shapes), `vector_pen.*` (`PenMode/PenPath/BrushStroke`), `vector_path_ops/node/point_ops/edit/gradient/profile/raster/view/qr.*` (QR via libqrencode, honest refusal without backend).

### 9.3 `export/` (`ExportDialog` in `export_dialog.h`)

`shell/export_shell.cpp` (left target tree + right settings/preview), `settings/export_settings.cpp` (sync/collect/presets/scale/matte), `presets/export_presets.cpp`, `shared/export_helpers.*` (formats, PNG/TIFF writers, CRC/BE utils), `io/export_writers.cpp` (`exportBaseName/writeExportImage/writeExportVector`).

### 9.4 Dialogs + bridges (`src/ui/*.h/.cpp`)

`filter_dialog` (135 filters + gallery + `FilterSession`), `liquify_dialog` (+canvas + freeze mask), `refine_dialog` (`RefineResult`), `layer_style_dialog` (`StyleEffect` order), `tone_dialogs` (Levels/Curves/Noise/Median/Unsharp via single-undo `ToneSession`), `tone_blend_dialog` (live group sliders), `curve_editor` (shared 0..1 surface), `preferences_dialog` (+AdvancedHooks, keymap editing), `ai_models` (catalogue + downloader + CLI), `svg_bridge` + `svg_parts` (SVG→layers importer), `selection_mask` + `selection_ops` (outline/smooth/bbox/largest-component + interchange), `mask_finish` (density×feather), `live_filter` (Smart-filter recipes), `psd_export` (`buildLayeredPsdDoc`), `af_export` (`buildAfLayersDoc` + bake notes), `proof_preview`, `dpi_pixmap.h`, `brush_popup_scale.h`, `color_grade_dialog` (adjustment-backed grading).

### 9.5 `src/app/`, `tools/`

- `app/main.cpp`: `--list-models/--fetch-models/--fetch-model`, HiDPI + Fusion, logging, `AppState` + theme, `MainWindow(1680×1000)` + `exec()`. `app/meson.build`: `painter` executable + desktop/metainfo/mime/icon install; engine-only when Qt6 missing.
- `tools/meson.build` + `tools/run_tone_blend16.cpp`: `run_tone_blend16` CLI harness for `applyToneBlend` on raw RGB16.

### 9.6 `tests/` (101 `test_*.cpp` + probes/benches; `test_util.h` zero-dep harness; `gpu_parity.*`; `test_all.cpp` runner)

`meson.build`: `test_deps=[compute,io,vector,text,color]`; `ui_core_sources` = 11 `app_state*.cpp` + `tool_log` + `logging` shared by AppState harnesses; host-tests loop; Qt-gated UI tests `-fPIC`, offscreen, scratch `XDG_CONFIG_HOME`, `PITTORE_SOURCE_ROOT` fixtures; probes/benches built not registered. Key: `test_canvas_drop` (drop ICC policy), `test_chrome_state` (no-doc contract + popup geometry), `test_layer_stack_ui`, `test_history`, `test_color_policy/mode`, `test_curves_ui`, `test_filter/layer_style/liquify/grade/refine_dialog`, `test_psd_export/psd_save/af_export_ui/ifp_layers/live_filter/mask_ops/selection_ops`, `test_persona/vector_paint_ui/tools_ui/tools_ux/tool_log/docks_ui/preferences_ui/export_ui/place_ui/session/project/import_export`, `test_cuda/hip/filter_gpu` parity, `test_svg_batch* + svg_final/xml/value/scene/pool`.

### 9.7 `docs/`, `packaging/`, root

- `docs/FEATURES.md` (feature truth + "Not there yet"), `docs/BUILDING.md` (deps + loss table + distro lines + meson/just/test/AI/install notes).
- `packaging/linux/studio.pittore.painter.desktop` (`Exec=painter %F`), `studio.pittore.painter.metainfo.xml` (MIT AppStream), `mime/painter.xml` (`application/x-painter-project` `*.psc/*.ifp`), `icons/hicolor/scalable/apps/painter.svg`, `packaging/README.md`.
- Root: `meson.build` (0.1.1 MIT C++20, Linux-only), `meson_options.txt` (cuda/hip/onnx/app), `justfile` (build/reconf/test/app/models/backends/install/clean), `build.sh` (bootstrap; never installs GPU toolchains), `AGENTS.md` (agent rules), `README.md`, `LICENSE`, `ink.md` (scratch/spec), `Reference/` (reference material, not compiled), `Pittore-studio.zip` (artifact).

## 10. Line-level reading guide (how deep this map goes)

Per-file line counts shift; use this to orient inside the big TUs rather than as a fixed census:

- `app_state.h` ~2395 (model + AppState API + signals), `app_state.cpp` ~7608 (core orchestration), `app_state_open.cpp` ~1291 (every open path), `app_state_layers.cpp` ~944 (stack surgery), `project_manager.cpp` ~1649 (import/export + start page), `icons.cpp` ~1575 (icon set), `cuda_backend.cu` ~2571 + `hip_backend.cpp` ~2514 (device ports), `heal.cpp` ~1123 (diffusion solver), `tone_blend.cpp` ~562 + `adjust.h` ~556 (shared math), `brushes/brush_library.h` ~922 + `bundle_import.h` ~1092 (presets), `layers_panel` + brushes panel 1000+ lines each.
- Line-by-line semantics live in the sources; the tables above give the owning TU + key symbols per concern so an AI can open exactly one file and read it fully. For "every line" depth, read the TU top-to-bottom with its header: constructors → public API → internals → signal emissions.

## 11. End-to-end workflows (which files fire in order)

- Open: dialog `dialogs_file::openProjectFile` → `AppState::openProject/openImageFile` (`app_state_open.cpp`) → codec (`engine/io/*`) → `resolveImportedProfile` (`app_state_save.cpp` + `color_mismatch` dialog) → `addDocument` → `syncDocumentTabs` (`window_state.cpp`) + `canvas_->zoomToFit`.
- Tab-strip drop: `DocumentTabBar::dropEvent` (`document_area.cpp`) → `openProjectFile` per file (new tabs); raw `hasImage` → `addDocument` + `placeImageLayer` + `zoomToFit`. Canvas drop instead: `canvas_events::eventFilter` → `acceptDropImage`/`placeSvgParts`/`openImageFile`.
- Stroke: `mousePress` (`canvas_mouse_press`) → `beginStrokeState` + `beginUndoStep` → `mouseMove` dabs (`canvas_brush` → `AppState::paintDab`, `DabToolOpts` cached per event) → `mouseRelease` → `commitUndoStep` → `rebuildComposite` (CPU or `composite_gpu.cpp`) → `refresh/paintEvent`.
- Undo: `beginUndoStep` (nesting depth) → `DocumentSnapshot` (shared-pointer pixels) → `commit/discard` (`app_state_undo.cpp`); `undo/redo` restores snapshot; `historyChanged` syncs Edit labels.
- Filter: `filterDialog(filterId)` (`dialogs_filter.cpp`) → `FilterDialog` preview (`filter_dialog.cpp`) → engine `applyFilter` (`engine/filter/filters.h` + category `apply_*`) → one history step on accept.
- Export: `exportActiveDocument` → `ExportDialog` (`export/*`) → `writeExportImage/Vector` (`export_writers.cpp`); layered PSD/AF via `psd_export`/`af_export` + `exportLayeredPsd/AfLayers`.
- AI select: `runAiBackgroundRemoval/runEnhanceEdges` (`window_ai.cpp`) or canvas `runAiObjectSelect/runAiQuickSelect` (`canvas_ai_select.cpp`) → `ai_encode` (embeddings cached per layer+model) → `ai_decode*` (fused, largest component) → selection mask → ants outline.
- Vector: persona bar → manager swaps tools (`persona/*`) → canvas pen/vector tools (`canvas_pen/vector_tools`) → `addVectorPathLayer/applyVectorNode` (one undo) → `vector_raster` re-bake or direct-geometry paint (`vector_view` eligibility).

## 12. Where to change (task → files)

| Task | Files |
|---|---|
| New tool | `tools/table/tool_table.cpp` (one line), `tools/ids/tool_ids.h` if new id, `tools/options/options_*.cpp` bar, `canvas/interactions/canvas_mouse_*.cpp` gesture, `tools_panel` order |
| New panel | `panels/<name>/<name>_panel.cpp` + `panels/registry/panel_creators.h` + `panels_registry.cpp` (`PanelInfo`) |
| New filter | `engine/filter/<cat>/defs_<cat>.h` + `apply_<cat>.h`, registry picks it up; dialog via `filter_dialog.cpp` |
| New kernel | §5.4: shared header → `backend.h` default → `cpu_backend.cpp` → `cuda_backend.cu` → `hip_backend.cpp` → `tests/gpu_parity.cpp` |
| New format support | `engine/io/*` codec + `project_manager.cpp` (`suffixIsOpenable`, `qimageFromFile`, filters) + `app_state_open.cpp` dispatch |
| New brush behaviour | `engine/compute/brushes/<tool>/*` + `app_state_stroke/stamp/smudge.cpp` + parity test |
| Theme/icons/shortcuts | `theme.*`, `icons.* (+icons.qrc)`, `keymap.*` + `window_shortcuts.cpp` carriers |
| Menus | `window/menus/menu_*.cpp` builder + `ops/` or `commands/` handler |
| Settings | `settings.*` (+ `preferences_dialog.cpp` page) |
| AI model | `ai_models.*` catalogue + `engine/ai/*` runtime + `just models` |

## 13. Full file index (all 1122 files; generated)

> Machine-generated `find` listing. Per-file roles for the load-bearing files are in §5–§9 above.

- `docs/BUILDING.md`
- `docs/FEATURES.md`
- `packaging/linux/icons/hicolor/scalable/apps/painter.svg`
- `packaging/linux/mime/painter.xml`
- `packaging/linux/studio.pittore.painter.desktop`
- `packaging/linux/studio.pittore.painter.metainfo.xml`
- `packaging/README.md`
- `src/app/main.cpp`
- `src/app/meson.build`
- `src/engine/ai/bg_remove.h`
- `src/engine/ai/decode/ai_decode.cpp`
- `src/engine/ai/decode/ai_decode.h`
- `src/engine/ai/decode/ai_decode_pub.cpp`
- `src/engine/ai/encode/ai_encode.cpp`
- `src/engine/ai/meson.build`
- `src/engine/ai/preprocess/ai_preprocess.cpp`
- `src/engine/ai/refine/ai_refine.cpp`
- `src/engine/ai/segment/ai_segment.cpp`
- `src/engine/ai/select/ai_select.cpp`
- `src/engine/ai/session/ai_runtime.cpp`
- `src/engine/ai/session/ai_session.cpp`
- `src/engine/ai/shared/ai_preprocess.h`
- `src/engine/ai/shared/ai_session.h`
- `src/engine/color/convert.cpp`
- `src/engine/color/convert.h`
- `src/engine/color/icc_convert.cpp`
- `src/engine/color/icc_convert.h`
- `src/engine/color/meson.build`
- `src/engine/color/proof.cpp`
- `src/engine/color/proof.h`
- `src/engine/color/separate.cpp`
- `src/engine/color/separate.h`
- `src/engine/compute/adjust.h`
- `src/engine/compute/backend.h`
- `src/engine/compute/blend.cpp`
- `src/engine/compute/blend.h`
- `src/engine/compute/brushes/adjust/adjust_dab.h`
- `src/engine/compute/brushes/blur/blur_dab.h`
- `src/engine/compute/brushes/clone/clone.cpp`
- `src/engine/compute/brushes/clone/clone.h`
- `src/engine/compute/brushes/composite/composite.cpp`
- `src/engine/compute/brushes/composite/composite.h`
- `src/engine/compute/brushes/dab/dab.cpp`
- `src/engine/compute/brushes/dab/dab.h`
- `src/engine/compute/brushes/erase/bg_erase.h`
- `src/engine/compute/brushes/erase/erase.cpp`
- `src/engine/compute/brushes/erase/erase.h`
- `src/engine/compute/brushes/flood/flood.cpp`
- `src/engine/compute/brushes/flood/flood.h`
- `src/engine/compute/brushes/heal/heal.cpp`
- `src/engine/compute/brushes/heal/heal.h`
- `src/engine/compute/brushes/history/history_dab.h`
- `src/engine/compute/brushes/loaders/loaders.cpp`
- `src/engine/compute/brushes/loaders/loaders.h`
- `src/engine/compute/brushes/mask/mask.h`
- `src/engine/compute/brushes/mip/mip.h`
- `src/engine/compute/brushes/mixer/mixer.h`
- `src/engine/compute/brushes/replace/replace.cpp`
- `src/engine/compute/brushes/replace/replace.h`
- `src/engine/compute/brushes/selection_mask/selection_mask.h`
- `src/engine/compute/brushes/smudge/smudge.cpp`
- `src/engine/compute/brushes/smudge/smudge.h`
- `src/engine/compute/brushes/stamp/pattern.h`
- `src/engine/compute/brushes/stamp/stamp.cpp`
- `src/engine/compute/brushes/stamp/stamp.h`
- `src/engine/compute/brushes/texture/texture.h`
- `src/engine/compute/brushes/tip/tip.h`
- `src/engine/compute/brushes/tone/tone.cpp`
- `src/engine/compute/brushes/tone/tone.h`
- `src/engine/compute/cpu_backend.cpp`
- `src/engine/compute/cpu_backend.h`
- `src/engine/compute/cuda_backend.cu`
- `src/engine/compute/cuda_backend.h`
- `src/engine/compute/dither.h`
- `src/engine/compute/factory.cpp`
- `src/engine/compute/factory.h`
- `src/engine/compute/hip_backend.cpp`
- `src/engine/compute/hip_backend.h`
- `src/engine/compute/layer_mask.cpp`
- `src/engine/compute/layer_mask.h`
- `src/engine/compute/meson.build`
- `src/engine/compute/oklab.h`
- `src/engine/compute/paint.h`
- `src/engine/compute/tone_blend.cpp`
- `src/engine/compute/tone_blend.h`
- `src/engine/compute/warp.cpp`
- `src/engine/compute/warp.h`
- `src/engine/core/document.h`
- `src/engine/core/image.h`
- `src/engine/core/log.h`
- `src/engine/core/meson.build`
- `src/engine/core/parallel.h`
- `src/engine/core/pixel.h`
- `src/engine/core/tile.h`
- `src/engine/core/tonal_ops.h`
- `src/engine/filter/artistic/apply_artistic.h`
- `src/engine/filter/artistic/defs_artistic.h`
- `src/engine/filter/blur/apply_blur.h`
- `src/engine/filter/blur/defs_blur.h`
- `src/engine/filter/blur_gallery/apply_blur_gallery.h`
- `src/engine/filter/blur_gallery/defs_blur_gallery.h`
- `src/engine/filter/brush_strokes/defs_brush_strokes.h`
- `src/engine/filter/core/filter_detail.h`
- `src/engine/filter/core/filter_params.h`
- `src/engine/filter/core/filter_types.h`
- `src/engine/filter/distort/apply_distort.h`
- `src/engine/filter/distort/defs_distort.h`
- `src/engine/filter/filters.h`
- `src/engine/filter/lens/apply_lens.h`
- `src/engine/filter/lens/defs_lens.h`
- `src/engine/filter/meson.build`
- `src/engine/filter/neural/apply_neural.h`
- `src/engine/filter/neural/defs_neural.h`
- `src/engine/filter/noise/apply_noise.h`
- `src/engine/filter/noise/defs_noise.h`
- `src/engine/filter/other/apply_other.h`
- `src/engine/filter/other/defs_other.h`
- `src/engine/filter/pixelate/apply_pixelate.h`
- `src/engine/filter/pixelate/defs_pixelate.h`
- `src/engine/filter/registry/filter_registry.h`
- `src/engine/filter/render/apply_render.h`
- `src/engine/filter/render/defs_render.h`
- `src/engine/filter/sharpen/apply_sharpen.h`
- `src/engine/filter/sharpen/defs_sharpen.h`
- `src/engine/filter/sketch/apply_sketch.h`
- `src/engine/filter/sketch/defs_sketch.h`
- `src/engine/filter/stylize/apply_stylize.h`
- `src/engine/filter/stylize/defs_stylize.h`
- `src/engine/filter/texture/apply_texture.h`
- `src/engine/filter/texture/defs_texture.h`
- `src/engine/filter/threed/apply_threed.h`
- `src/engine/filter/threed/defs_threed.h`
- `src/engine/filter/video/apply_video.h`
- `src/engine/filter/video/defs_video.h`
- `src/engine/io/af/doc/af_document.cpp`
- `src/engine/io/af.h`
- `src/engine/io/af_layers/container/af_archive.cpp`
- `src/engine/io/af_layers/container/af_archive.h`
- `src/engine/io/af_layers/emit/af_emit.cpp`
- `src/engine/io/af_layers/emit/af_emit.h`
- `src/engine/io/af_layers/emit/af_write.cpp`
- `src/engine/io/af_layers/emit/af_write.h`
- `src/engine/io/af_layers/filter/af_blur.cpp`
- `src/engine/io/af_layers/filter/af_blur.h`
- `src/engine/io/af_layers/filter/af_distort.cpp`
- `src/engine/io/af_layers/filter/af_distort.h`
- `src/engine/io/af_layers/filter/af_fx.cpp`
- `src/engine/io/af_layers/filter/af_fx.h`
- `src/engine/io/af_layers/filter/af_live.cpp`
- `src/engine/io/af_layers/filter/af_live.h`
- `src/engine/io/af_layers/geom/af_geom.cpp`
- `src/engine/io/af_layers/geom/af_geom.h`
- `src/engine/io/af_layers/geom/af_homography.cpp`
- `src/engine/io/af_layers/geom/af_homography.h`
- `src/engine/io/af_layers/graph/af_graph.cpp`
- `src/engine/io/af_layers/graph/af_graph.h`
- `src/engine/io/af_layers/graph/af_graph_parser.h`
- `src/engine/io/af_layers/graph/af_values.cpp`
- `src/engine/io/af_layers.h`
- `src/engine/io/af_layers/image/af_image.cpp`
- `src/engine/io/af_layers/image/af_image.h`
- `src/engine/io/af_layers/image/af_raster.cpp`
- `src/engine/io/af_layers/image/af_tiles.cpp`
- `src/engine/io/af_layers/vector/af_shapes.cpp`
- `src/engine/io/af_layers/vector/af_shapes.h`
- `src/engine/io/af_layers/walker/af_document.cpp`
- `src/engine/io/af_layers/walker/af_walker_decode.cpp`
- `src/engine/io/af_layers/walker/af_walker_emit.cpp`
- `src/engine/io/af_layers/walker/af_walker.h`
- `src/engine/io/af/meta/af_meta.cpp`
- `src/engine/io/af/png/af_png.cpp`
- `src/engine/io/af/png/af_png.h`
- `src/engine/io/af/shared/af_preview.cpp`
- `src/engine/io/af/shared/af_preview.h`
- `src/engine/io/icc.h`
- `src/engine/io/icc_scan.cpp`
- `src/engine/io/icc_scan.h`
- `src/engine/io/meson.build`
- `src/engine/io/project.cpp`
- `src/engine/io/project.h`
- `src/engine/io/psd.cpp`
- `src/engine/io/psd.h`
- `src/engine/io/tiff.cpp`
- `src/engine/io/tiff.h`
- `src/engine/io/webp.cpp`
- `src/engine/io/webp.h`
- `src/engine/io/xcf.cpp`
- `src/engine/io/xcf.h`
- `src/engine/io/zip.cpp`
- `src/engine/io/zip.h`
- `src/engine/render/layer_style.h`
- `src/engine/render/meson.build`
- `src/engine/render/style/apply/layer_style_apply.cpp`
- `src/engine/render/style/blend/style_blend.cpp`
- `src/engine/render/style/blend/style_blend.h`
- `src/engine/render/style/blur/style_blur.cpp`
- `src/engine/render/style/blur/style_blur.h`
- `src/engine/render/style/fx/style_fx.cpp`
- `src/engine/render/style/fx/style_fx.h`
- `src/engine/render/style/sdf/style_sdf.cpp`
- `src/engine/render/style/sdf/style_sdf.h`
- `src/engine/render/style/shared/style_plane.cpp`
- `src/engine/render/style/shared/style_plane.h`
- `src/engine/text/face/text_face.cpp`
- `src/engine/text/family/text_family.cpp`
- `src/engine/text/layout/text_layout.cpp`
- `src/engine/text/meson.build`
- `src/engine/text/raster/text_raster.cpp`
- `src/engine/text/shape/text_shape.cpp`
- `src/engine/text/shared/text_internal.h`
- `src/engine/text/text_engine.h`
- `src/engine/vector/boolean.cpp`
- `src/engine/vector/boolean.h`
- `src/engine/vector/box3d.cpp`
- `src/engine/vector/box3d.h`
- `src/engine/vector/calligraphy.cpp`
- `src/engine/vector/calligraphy.h`
- `src/engine/vector/clip_mask.cpp`
- `src/engine/vector/clip_mask.h`
- `src/engine/vector/clone.cpp`
- `src/engine/vector/clone.h`
- `src/engine/vector/connector.cpp`
- `src/engine/vector/connector.h`
- `src/engine/vector/filter_fe.cpp`
- `src/engine/vector/filter_fe.h`
- `src/engine/vector/grids.cpp`
- `src/engine/vector/grids.h`
- `src/engine/vector/lpe/lpe_convert.cpp`
- `src/engine/vector/lpe/lpe.cpp`
- `src/engine/vector/lpe/lpe_deform.cpp`
- `src/engine/vector/lpe/lpe_drag.cpp`
- `src/engine/vector/lpe/lpe_generate.cpp`
- `src/engine/vector/lpe/lpe.h`
- `src/engine/vector/lpe/lpe_offset.cpp`
- `src/engine/vector/marker.cpp`
- `src/engine/vector/marker.h`
- `src/engine/vector/mesh.cpp`
- `src/engine/vector/mesh.h`
- `src/engine/vector/meson.build`
- `src/engine/vector/palette.cpp`
- `src/engine/vector/palette.h`
- `src/engine/vector/path.cpp`
- `src/engine/vector/path.h`
- `src/engine/vector/path_ops.cpp`
- `src/engine/vector/path_ops.h`
- `src/engine/vector/pattern.cpp`
- `src/engine/vector/pattern.h`
- `src/engine/vector/snap.cpp`
- `src/engine/vector/snap.h`
- `src/engine/vector/spiro.cpp`
- `src/engine/vector/spiro.h`
- `src/engine/vector/spray.cpp`
- `src/engine/vector/spray.h`
- `src/engine/vector/svg/affine_apply.cpp`
- `src/engine/vector/svg/affine_apply.h`
- `src/engine/vector/svg/affine_build.cpp`
- `src/engine/vector/svg/affine_build.h`
- `src/engine/vector/svg/affine_inv.cpp`
- `src/engine/vector/svg/affine_inv.h`
- `src/engine/vector/svg/alpha_mix.cpp`
- `src/engine/vector/svg/alpha_mix.h`
- `src/engine/vector/svg/angle.cpp`
- `src/engine/vector/svg/angle.h`
- `src/engine/vector/svg/arc_flags.cpp`
- `src/engine/vector/svg/arc_flags.h`
- `src/engine/vector/svg/aspect_align.cpp`
- `src/engine/vector/svg/aspect_align.h`
- `src/engine/vector/svg/attr_escape.cpp`
- `src/engine/vector/svg/attr_escape.h`
- `src/engine/vector/svg/attr_trim.cpp`
- `src/engine/vector/svg/attr_trim.h`
- `src/engine/vector/svg/attr_write.cpp`
- `src/engine/vector/svg/attr_write.h`
- `src/engine/vector/svg/bbox_merge.cpp`
- `src/engine/vector/svg/bbox_merge.h`
- `src/engine/vector/svg/bezier_flat.cpp`
- `src/engine/vector/svg/bezier_flat.h`
- `src/engine/vector/svg/bidi_flag.cpp`
- `src/engine/vector/svg/bidi_flag.h`
- `src/engine/vector/svg/blend.cpp`
- `src/engine/vector/svg/blend.h`
- `src/engine/vector/svg/bom_strip.cpp`
- `src/engine/vector/svg/bom_strip.h`
- `src/engine/vector/svg/bounds.cpp`
- `src/engine/vector/svg/bounds.h`
- `src/engine/vector/svg/box.cpp`
- `src/engine/vector/svg/box.h`
- `src/engine/vector/svg/cdata_wrap.cpp`
- `src/engine/vector/svg/cdata_wrap.h`
- `src/engine/vector/svg/chunk_split.cpp`
- `src/engine/vector/svg/chunk_split.h`
- `src/engine/vector/svg/circle_args.cpp`
- `src/engine/vector/svg/circle_args.h`
- `src/engine/vector/svg/class_split.cpp`
- `src/engine/vector/svg/class_split.h`
- `src/engine/vector/svg/clip_path.cpp`
- `src/engine/vector/svg/clip_path.h`
- `src/engine/vector/svg/clip_rule.cpp`
- `src/engine/vector/svg/clip_rule.h`
- `src/engine/vector/svg/clip_units.cpp`
- `src/engine/vector/svg/clip_units.h`
- `src/engine/vector/svg/color_interp.cpp`
- `src/engine/vector/svg/color_interp.h`
- `src/engine/vector/svg/color_parse.cpp`
- `src/engine/vector/svg/color_parse.h`
- `src/engine/vector/svg/comment_skip.cpp`
- `src/engine/vector/svg/comment_skip.h`
- `src/engine/vector/svg/compose.cpp`
- `src/engine/vector/svg/compose.h`
- `src/engine/vector/svg/contrast_pick.cpp`
- `src/engine/vector/svg/contrast_pick.h`
- `src/engine/vector/svg/css_parse.cpp`
- `src/engine/vector/svg/css_parse.h`
- `src/engine/vector/svg/current_color.cpp`
- `src/engine/vector/svg/current_color.h`
- `src/engine/vector/svg/dash.cpp`
- `src/engine/vector/svg/dash.h`
- `src/engine/vector/svg/decl_merge.cpp`
- `src/engine/vector/svg/decl_merge.h`
- `src/engine/vector/svg/depth_guard.cpp`
- `src/engine/vector/svg/depth_guard.h`
- `src/engine/vector/svg/display.cpp`
- `src/engine/vector/svg/display.h`
- `src/engine/vector/svg_dom.cpp`
- `src/engine/vector/svg_dom.h`
- `src/engine/vector/svg/dxdy_list.cpp`
- `src/engine/vector/svg/dxdy_list.h`
- `src/engine/vector/svg/ellipse_args.cpp`
- `src/engine/vector/svg/ellipse_args.h`
- `src/engine/vector/svg_exchange.cpp`
- `src/engine/vector/svg_exchange.h`
- `src/engine/vector/svg/export_svg.cpp`
- `src/engine/vector/svg/export_svg.h`
- `src/engine/vector/svg/fe_blur.cpp`
- `src/engine/vector/svg/fe_blur.h`
- `src/engine/vector/svg/fe_buffer.cpp`
- `src/engine/vector/svg/fe_buffer.h`
- `src/engine/vector/svg/fe_color.cpp`
- `src/engine/vector/svg/fe_color.h`
- `src/engine/vector/svg/fe_composite.cpp`
- `src/engine/vector/svg/fe_composite.h`
- `src/engine/vector/svg/fill_rule.cpp`
- `src/engine/vector/svg/fill_rule.h`
- `src/engine/vector/svg/filter_in.cpp`
- `src/engine/vector/svg/filter_in.h`
- `src/engine/vector/svg/filter_params.cpp`
- `src/engine/vector/svg/filter_params.h`
- `src/engine/vector/svg/filter_rect.cpp`
- `src/engine/vector/svg/filter_rect.h`
- `src/engine/vector/svg/filter_region.cpp`
- `src/engine/vector/svg/filter_region.h`
- `src/engine/vector/svg/filter_units.cpp`
- `src/engine/vector/svg/filter_units.h`
- `src/engine/vector/svg/font_family.cpp`
- `src/engine/vector/svg/font_family.h`
- `src/engine/vector/svg/font_shorthand.cpp`
- `src/engine/vector/svg/font_shorthand.h`
- `src/engine/vector/svg/font_size.cpp`
- `src/engine/vector/svg/font_size.h`
- `src/engine/vector/svg/font_style.cpp`
- `src/engine/vector/svg/font_style.h`
- `src/engine/vector/svg/font_weight.cpp`
- `src/engine/vector/svg/font_weight.h`
- `src/engine/vector/svg/glyph_orient.cpp`
- `src/engine/vector/svg/glyph_orient.h`
- `src/engine/vector/svg/grad_attrs.cpp`
- `src/engine/vector/svg/grad_attrs.h`
- `src/engine/vector/svg/gradient.cpp`
- `src/engine/vector/svg/gradient.h`
- `src/engine/vector/svg/gradient_units.cpp`
- `src/engine/vector/svg/gradient_units.h`
- `src/engine/vector/svg/grad_xform.cpp`
- `src/engine/vector/svg/grad_xform.h`
- `src/engine/vector/svg/grain_split.cpp`
- `src/engine/vector/svg/grain_split.h`
- `src/engine/vector/svg/href_cycle.cpp`
- `src/engine/vector/svg/href_cycle.h`
- `src/engine/vector/svg/hsl_build.cpp`
- `src/engine/vector/svg/hsl_build.h`
- `src/engine/vector/svg/id_clash.cpp`
- `src/engine/vector/svg/id_clash.h`
- `src/engine/vector/svg/id_index.cpp`
- `src/engine/vector/svg/id_index.h`
- `src/engine/vector/svg/image_fit.cpp`
- `src/engine/vector/svg/image_fit.h`
- `src/engine/vector/svg/image_rect.cpp`
- `src/engine/vector/svg/image_rect.h`
- `src/engine/vector/svg/image_ref.cpp`
- `src/engine/vector/svg/image_ref.h`
- `src/engine/vector/svg/important.cpp`
- `src/engine/vector/svg/important.h`
- `src/engine/vector/svg/import_bridge.cpp`
- `src/engine/vector/svg/import_bridge.h`
- `src/engine/vector/svg/indent_write.cpp`
- `src/engine/vector/svg/indent_write.h`
- `src/engine/vector/svg/inherit_flag.cpp`
- `src/engine/vector/svg/inherit_flag.h`
- `src/engine/vector/svg/iri.cpp`
- `src/engine/vector/svg/iri.h`
- `src/engine/vector/svg/lang_space.cpp`
- `src/engine/vector/svg/lang_space.h`
- `src/engine/vector/svg/length.cpp`
- `src/engine/vector/svg/length.h`
- `src/engine/vector/svg/letter_space.cpp`
- `src/engine/vector/svg/letter_space.h`
- `src/engine/vector/svg/line_args.cpp`
- `src/engine/vector/svg/line_args.h`
- `src/engine/vector/svg/line_break.cpp`
- `src/engine/vector/svg/line_break.h`
- `src/engine/vector/svg/line_height.cpp`
- `src/engine/vector/svg/line_height.h`
- `src/engine/vector/svg/list_split.cpp`
- `src/engine/vector/svg/list_split.h`
- `src/engine/vector/svg/luma_gray.cpp`
- `src/engine/vector/svg/luma_gray.h`
- `src/engine/vector/svg/marker_attrs.cpp`
- `src/engine/vector/svg/marker_attrs.h`
- `src/engine/vector/svg/marker_frame.cpp`
- `src/engine/vector/svg/marker_frame.h`
- `src/engine/vector/svg/marker_orient.cpp`
- `src/engine/vector/svg/marker_orient.h`
- `src/engine/vector/svg/mask_luma.cpp`
- `src/engine/vector/svg/mask_luma.h`
- `src/engine/vector/svg/mask_rect.cpp`
- `src/engine/vector/svg/mask_rect.h`
- `src/engine/vector/svg/mask_units.cpp`
- `src/engine/vector/svg/mask_units.h`
- `src/engine/vector/svg/named_color.cpp`
- `src/engine/vector/svg/named_color.h`
- `src/engine/vector/svg/ns_name.cpp`
- `src/engine/vector/svg/ns_name.h`
- `src/engine/vector/svg/number_list.cpp`
- `src/engine/vector/svg/number_list.h`
- `src/engine/vector/svg/num_unit.cpp`
- `src/engine/vector/svg/num_unit.h`
- `src/engine/vector/svg/opacity.cpp`
- `src/engine/vector/svg/opacity.h`
- `src/engine/vector/svg/overflow.cpp`
- `src/engine/vector/svg/overflow.h`
- `src/engine/vector/svg/paint_order.cpp`
- `src/engine/vector/svg/paint_order.h`
- `src/engine/vector/svg/paint_server.cpp`
- `src/engine/vector/svg/paint_server.h`
- `src/engine/vector/svg_parse.cpp`
- `src/engine/vector/svg_parse.h`
- `src/engine/vector/svg/parse_stats.cpp`
- `src/engine/vector/svg/parse_stats.h`
- `src/engine/vector/svg/path_data.cpp`
- `src/engine/vector/svg/path_data.h`
- `src/engine/vector/svg/path_length.cpp`
- `src/engine/vector/svg/path_length.h`
- `src/engine/vector/svg/pattern_attrs.cpp`
- `src/engine/vector/svg/pattern_attrs.h`
- `src/engine/vector/svg/pattern_tile.cpp`
- `src/engine/vector/svg/pattern_tile.h`
- `src/engine/vector/svg/pattern_units.cpp`
- `src/engine/vector/svg/pattern_units.h`
- `src/engine/vector/svg/pattern_xform.cpp`
- `src/engine/vector/svg/pattern_xform.h`
- `src/engine/vector/svg/pct_of.cpp`
- `src/engine/vector/svg/pct_of.h`
- `src/engine/vector/svg/png_stripes.cpp`
- `src/engine/vector/svg/png_stripes.h`
- `src/engine/vector/svg/point_xform.cpp`
- `src/engine/vector/svg/point_xform.h`
- `src/engine/vector/svg/poly_close.cpp`
- `src/engine/vector/svg/poly_close.h`
- `src/engine/vector/svg/poly_points.cpp`
- `src/engine/vector/svg/poly_points.h`
- `src/engine/vector/svg/quad_split.cpp`
- `src/engine/vector/svg/quad_split.h`
- `src/engine/vector/svg/raster_tile.cpp`
- `src/engine/vector/svg/raster_tile.h`
- `src/engine/vector/svg/rect_args.cpp`
- `src/engine/vector/svg/rect_args.h`
- `src/engine/vector/svg/rect_corners.cpp`
- `src/engine/vector/svg/rect_corners.h`
- `src/engine/vector/svg/ref_guard.cpp`
- `src/engine/vector/svg/ref_guard.h`
- `src/engine/vector/svg/render_item.cpp`
- `src/engine/vector/svg/render_item.h`
- `src/engine/vector/svg/rgb_hex.cpp`
- `src/engine/vector/svg/rgb_hex.h`
- `src/engine/vector/svg/rotate_list.cpp`
- `src/engine/vector/svg/rotate_list.h`
- `src/engine/vector/svg/row_split.cpp`
- `src/engine/vector/svg/row_split.h`
- `src/engine/vector/svg/scene.cpp`
- `src/engine/vector/svg/scene.h`
- `src/engine/vector/svg/scene_image.cpp`
- `src/engine/vector/svg/scene_image.h`
- `src/engine/vector/svg/sheet_lookup.cpp`
- `src/engine/vector/svg/sheet_lookup.h`
- `src/engine/vector/svg/size_guard.cpp`
- `src/engine/vector/svg/size_guard.h`
- `src/engine/vector/svg/smooth_hint.cpp`
- `src/engine/vector/svg/smooth_hint.h`
- `src/engine/vector/svg/solid_color.cpp`
- `src/engine/vector/svg/solid_color.h`
- `src/engine/vector/svg/specificity.cpp`
- `src/engine/vector/svg/specificity.h`
- `src/engine/vector/svg/spread_method.cpp`
- `src/engine/vector/svg/spread_method.h`
- `src/engine/vector/svg/stop_list.cpp`
- `src/engine/vector/svg/stop_list.h`
- `src/engine/vector/svg/stop_opacity.cpp`
- `src/engine/vector/svg/stop_opacity.h`
- `src/engine/vector/svg/stripe_join.cpp`
- `src/engine/vector/svg/stripe_join.h`
- `src/engine/vector/svg/stroke_style.cpp`
- `src/engine/vector/svg/stroke_style.h`
- `src/engine/vector/svg/style.cpp`
- `src/engine/vector/svg/style.h`
- `src/engine/vector/svg/style_join.cpp`
- `src/engine/vector/svg/style_join.h`
- `src/engine/vector/svg/svg_save.cpp`
- `src/engine/vector/svg/svg_save.h`
- `src/engine/vector/svg/tag_count.cpp`
- `src/engine/vector/svg/tag_count.h`
- `src/engine/vector/svg/tag_match.cpp`
- `src/engine/vector/svg/tag_match.h`
- `src/engine/vector/svg/text_anchor.cpp`
- `src/engine/vector/svg/text_anchor.h`
- `src/engine/vector/svg/text_deco.cpp`
- `src/engine/vector/svg/text_deco.h`
- `src/engine/vector/svg/text_escape.cpp`
- `src/engine/vector/svg/text_escape.h`
- `src/engine/vector/svg/text_length.cpp`
- `src/engine/vector/svg/text_length.h`
- `src/engine/vector/svg/text_measure.cpp`
- `src/engine/vector/svg/text_measure.h`
- `src/engine/vector/svg/text_xform.cpp`
- `src/engine/vector/svg/text_xform.h`
- `src/engine/vector/svg/thread_pool.cpp`
- `src/engine/vector/svg/thread_pool.h`
- `src/engine/vector/svg/transform.cpp`
- `src/engine/vector/svg/transform.h`
- `src/engine/vector/svg/unknown_keep.cpp`
- `src/engine/vector/svg/unknown_keep.h`
- `src/engine/vector/svg/use_count.cpp`
- `src/engine/vector/svg/use_count.h`
- `src/engine/vector/svg/use_expand.cpp`
- `src/engine/vector/svg/use_expand.h`
- `src/engine/vector/svg/utf16_check.cpp`
- `src/engine/vector/svg/utf16_check.h`
- `src/engine/vector/svg/validate.cpp`
- `src/engine/vector/svg/validate.h`
- `src/engine/vector/svg/vector_effect.cpp`
- `src/engine/vector/svg/vector_effect.h`
- `src/engine/vector/svg/vector_scene_check.cpp`
- `src/engine/vector/svg/vector_scene_check.h`
- `src/engine/vector/svg/viewbox.cpp`
- `src/engine/vector/svg/viewbox.h`
- `src/engine/vector/svg/white_wrap.cpp`
- `src/engine/vector/svg/white_wrap.h`
- `src/engine/vector/svg/word_split.cpp`
- `src/engine/vector/svg/word_split.h`
- `src/engine/vector/svg/writing_mode.cpp`
- `src/engine/vector/svg/writing_mode.h`
- `src/engine/vector/svg/xml_reader.cpp`
- `src/engine/vector/svg/xml_reader.h`
- `src/engine/vector/svg/zip_magic.cpp`
- `src/engine/vector/svg/zip_magic.h`
- `src/engine/vector/text_flow.cpp`
- `src/engine/vector/text_flow.h`
- `src/engine/vector/trace.cpp`
- `src/engine/vector/trace.h`
- `src/engine/vector/transform_ops.cpp`
- `src/engine/vector/transform_ops.h`
- `src/engine/vector/vector_art.cpp`
- `src/engine/vector/vector_art.h`
- `src/engine/vector/vector_scene.h`
- `src/engine/vector/vector_shape.cpp`
- `src/engine/vector/vector_shape.h`
- `src/meson.build`
- `src/Resources/pittore-studio-icon.svg`
- `src/ui/af_export.cpp`
- `src/ui/af_export.h`
- `src/ui/ai_models.cpp`
- `src/ui/ai_models.h`
- `src/ui/app_state_adjust.cpp`
- `src/ui/app_state.cpp`
- `src/ui/app_state_detail.cpp`
- `src/ui/app_state_detail.h`
- `src/ui/app_state.h`
- `src/ui/app_state_layers.cpp`
- `src/ui/app_state_mask.cpp`
- `src/ui/app_state_open.cpp`
- `src/ui/app_state_save.cpp`
- `src/ui/app_state_selection.cpp`
- `src/ui/app_state_smudge.cpp`
- `src/ui/app_state_stamp.cpp`
- `src/ui/app_state_stroke.cpp`
- `src/ui/app_state_text.cpp`
- `src/ui/app_state_undo.cpp`
- `src/ui/brushes/brush_library.h`
- `src/ui/brushes/brush_preview.h`
- `src/ui/brushes/bundle_import.h`
- `src/ui/brushes/pressure_curve.h`
- `src/ui/brushes/sensor_drives_dialog.h`
- `src/ui/brushes/sensor_drives.h`
- `src/ui/brushes/sensor_drives_json.h`
- `src/ui/brush_popup_scale.h`
- `src/ui/canvas/ai/canvas_ai_select.cpp`
- `src/ui/canvas/interactions/canvas_cursor.cpp`
- `src/ui/canvas/interactions/canvas_hover.cpp`
- `src/ui/canvas/interactions/canvas_mouse_move.cpp`
- `src/ui/canvas/interactions/canvas_mouse_press.cpp`
- `src/ui/canvas/interactions/canvas_mouse_release.cpp`
- `src/ui/canvas/move/canvas_move.cpp`
- `src/ui/canvas/paint/canvas_annotations.cpp`
- `src/ui/canvas/paint/canvas_brush.cpp`
- `src/ui/canvas/paint/canvas_document.cpp`
- `src/ui/canvas/paint/canvas_liquify.cpp`
- `src/ui/canvas/paint/canvas_overlay.cpp`
- `src/ui/canvas/pen/canvas_pen.cpp`
- `src/ui/canvas/pen/canvas_vector_tools.cpp`
- `src/ui/canvas/shared/canvas_helpers.cpp`
- `src/ui/canvas/shared/canvas_helpers.h`
- `src/ui/canvas/shared/symmetry.h`
- `src/ui/canvas/shell/canvas_events.cpp`
- `src/ui/canvas/shell/canvas_shell.cpp`
- `src/ui/canvas/text/canvas_text.cpp`
- `src/ui/canvas/view/canvas_rulers.cpp`
- `src/ui/canvas/view/canvas_zoom.cpp`
- `src/ui/canvas_view.h`
- `src/ui/color_grade_dialog.cpp`
- `src/ui/color_grade_dialog.h`
- `src/ui/color_mismatch.cpp`
- `src/ui/color_mismatch.h`
- `src/ui/color_mode.cpp`
- `src/ui/color_mode.h`
- `src/ui/composite_gpu.cpp`
- `src/ui/contextual_task_bar.cpp`
- `src/ui/contextual_task_bar.h`
- `src/ui/curve_editor.cpp`
- `src/ui/curve_editor.h`
- `src/ui/dpi_pixmap.h`
- `src/ui/embedded_icc.h`
- `src/ui/export_dialog.h`
- `src/ui/export/io/export_writers.cpp`
- `src/ui/export/presets/export_presets.cpp`
- `src/ui/export/settings/export_settings.cpp`
- `src/ui/export/shared/export_helpers.cpp`
- `src/ui/export/shared/export_helpers.h`
- `src/ui/export/shell/export_shell.cpp`
- `src/ui/filter_dialog.cpp`
- `src/ui/filter_dialog.h`
- `src/ui/font_preview.cpp`
- `src/ui/font_preview.h`
- `src/ui/icons.cpp`
- `src/ui/icons.h`
- `src/ui/icons/lucide/actions.svg`
- `src/ui/icons/lucide/adj-brush.svg`
- `src/ui/icons/lucide/adjustments.svg`
- `src/ui/icons/lucide/anchor-add.svg`
- `src/ui/icons/lucide/anchor-conv.svg`
- `src/ui/icons/lucide/anchor-del.svg`
- `src/ui/icons/lucide/area.svg`
- `src/ui/icons/lucide/artboard.svg`
- `src/ui/icons/lucide/art-history.svg`
- `src/ui/icons/lucide/blur.svg`
- `src/ui/icons/lucide/box3d.svg`
- `src/ui/icons/lucide/brushes.svg`
- `src/ui/icons/lucide/brush.svg`
- `src/ui/icons/lucide/bucket.svg`
- `src/ui/icons/lucide/burn.svg`
- `src/ui/icons/lucide/calligraphy.svg`
- `src/ui/icons/lucide/ca-move.svg`
- `src/ui/icons/lucide/channels.svg`
- `src/ui/icons/lucide/character.svg`
- `src/ui/icons/lucide/check.svg`
- `src/ui/icons/lucide/chevron-down.svg`
- `src/ui/icons/lucide/chevron-right.svg`
- `src/ui/icons/lucide/clone.svg`
- `src/ui/icons/lucide/close.svg`
- `src/ui/icons/lucide/color.svg`
- `src/ui/icons/lucide/columns.svg`
- `src/ui/icons/lucide/connector.svg`
- `src/ui/icons/lucide/contour.svg`
- `src/ui/icons/lucide/corner.svg`
- `src/ui/icons/lucide/count.svg`
- `src/ui/icons/lucide/crop-persp.svg`
- `src/ui/icons/lucide/crop.svg`
- `src/ui/icons/lucide/crop-vector.svg`
- `src/ui/icons/lucide/direct-sel.svg`
- `src/ui/icons/lucide/dodge.svg`
- `src/ui/icons/lucide/edittoolbar.svg`
- `src/ui/icons/lucide/eraser-bg.svg`
- `src/ui/icons/lucide/eraser-magic.svg`
- `src/ui/icons/lucide/eraser.svg`
- `src/ui/icons/lucide/eraser-vector.svg`
- `src/ui/icons/lucide/eyedropper.svg`
- `src/ui/icons/lucide/eye.svg`
- `src/ui/icons/lucide/flood-vector.svg`
- `src/ui/icons/lucide/frame.svg`
- `src/ui/icons/lucide/gen-bg.svg`
- `src/ui/icons/lucide/gen-fill.svg`
- `src/ui/icons/lucide/gradient.svg`
- `src/ui/icons/lucide/grip.svg`
- `src/ui/icons/lucide/group.svg`
- `src/ui/icons/lucide/hand.svg`
- `src/ui/icons/lucide/heal.svg`
- `src/ui/icons/lucide/histogram.svg`
- `src/ui/icons/lucide/history-br.svg`
- `src/ui/icons/lucide/history.svg`
- `src/ui/icons/lucide/import.svg`
- `src/ui/icons/lucide/info.svg`
- `src/ui/icons/lucide/knife.svg`
- `src/ui/icons/lucide/lasso-mag.svg`
- `src/ui/icons/lucide/lasso-poly.svg`
- `src/ui/icons/lucide/lasso.svg`
- `src/ui/icons/lucide/layers.svg`
- `src/ui/icons/lucide/libraries.svg`
- `src/ui/icons/lucide/LICENSE.lucide`
- `src/ui/icons/lucide/link.svg`
- `src/ui/icons/lucide/liquify.svg`
- `src/ui/icons/lucide/lock.svg`
- `src/ui/icons/lucide/lpe.svg`
- `src/ui/icons/lucide/lq-bloat.svg`
- `src/ui/icons/lucide/lq-clone.svg`
- `src/ui/icons/lucide/lq-forward.svg`
- `src/ui/icons/lucide/lq-freeze.svg`
- `src/ui/icons/lucide/lq-mirror.svg`
- `src/ui/icons/lucide/lq-pucker.svg`
- `src/ui/icons/lucide/lq-push-left.svg`
- `src/ui/icons/lucide/lq-push-right.svg`
- `src/ui/icons/lucide/lq-reconstruct.svg`
- `src/ui/icons/lucide/lq-smooth.svg`
- `src/ui/icons/lucide/lq-thaw.svg`
- `src/ui/icons/lucide/lq-turbulence.svg`
- `src/ui/icons/lucide/lq-twirl-ccw.svg`
- `src/ui/icons/lucide/lq-twirl-cw.svg`
- `src/ui/icons/lucide/marker.svg`
- `src/ui/icons/lucide/marquee-col.svg`
- `src/ui/icons/lucide/marquee-ell.svg`
- `src/ui/icons/lucide/marquee-rect.svg`
- `src/ui/icons/lucide/marquee-row.svg`
- `src/ui/icons/lucide/mask.svg`
- `src/ui/icons/lucide/measure.svg`
- `src/ui/icons/lucide/menu.svg`
- `src/ui/icons/lucide/mesh.svg`
- `src/ui/icons/lucide/minus.svg`
- `src/ui/icons/lucide/mixer.svg`
- `src/ui/icons/lucide/move.svg`
- `src/ui/icons/lucide/navigator.svg`
- `src/ui/icons/lucide/newlayer.svg`
- `src/ui/icons/lucide/node.svg`
- `src/ui/icons/lucide/note.svg`
- `src/ui/icons/lucide/object-sel.svg`
- `src/ui/icons/lucide/pages.svg`
- `src/ui/icons/lucide/paragraph.svg`
- `src/ui/icons/lucide/patch.svg`
- `src/ui/icons/lucide/path-sel.svg`
- `src/ui/icons/lucide/paths.svg`
- `src/ui/icons/lucide/pattern.svg`
- `src/ui/icons/lucide/pencil.svg`
- `src/ui/icons/lucide/pen-curve.svg`
- `src/ui/icons/lucide/pen-free.svg`
- `src/ui/icons/lucide/pen.svg`
- `src/ui/icons/lucide/place.svg`
- `src/ui/icons/lucide/plus.svg`
- `src/ui/icons/lucide/point-xform.svg`
- `src/ui/icons/lucide/properties.svg`
- `src/ui/icons/lucide/quickmask.svg`
- `src/ui/icons/lucide/quick-sel.svg`
- `src/ui/icons/lucide/README.txt`
- `src/ui/icons/lucide/red-eye.svg`
- `src/ui/icons/lucide/remove.svg`
- `src/ui/icons/lucide/replace.svg`
- `src/ui/icons/lucide/rotate-view.svg`
- `src/ui/icons/lucide/ruler.svg`
- `src/ui/icons/lucide/sampler.svg`
- `src/ui/icons/lucide/screenmode.svg`
- `src/ui/icons/lucide/sel-brush.svg`
- `src/ui/icons/lucide/shape-arrow.svg`
- `src/ui/icons/lucide/shape-builder.svg`
- `src/ui/icons/lucide/shape-callout-ell.svg`
- `src/ui/icons/lucide/shape-callout-rect.svg`
- `src/ui/icons/lucide/shape-cat.svg`
- `src/ui/icons/lucide/shape-chevron.svg`
- `src/ui/icons/lucide/shape-circulararrow.svg`
- `src/ui/icons/lucide/shape-cloud.svg`
- `src/ui/icons/lucide/shape-cog.svg`
- `src/ui/icons/lucide/shape-crescent.svg`
- `src/ui/icons/lucide/shape-cross.svg`
- `src/ui/icons/lucide/shape-custom.svg`
- `src/ui/icons/lucide/shape-diamond.svg`
- `src/ui/icons/lucide/shape-donut.svg`
- `src/ui/icons/lucide/shape-doublearrow.svg`
- `src/ui/icons/lucide/shape-doublestar.svg`
- `src/ui/icons/lucide/shape-ell.svg`
- `src/ui/icons/lucide/shape-heart.svg`
- `src/ui/icons/lucide/shape-hexagon.svg`
- `src/ui/icons/lucide/shape-line.svg`
- `src/ui/icons/lucide/shape-octagon.svg`
- `src/ui/icons/lucide/shape-parallelogram.svg`
- `src/ui/icons/lucide/shape-pie.svg`
- `src/ui/icons/lucide/shape-poly.svg`
- `src/ui/icons/lucide/shape-qr.svg`
- `src/ui/icons/lucide/shape-rect.svg`
- `src/ui/icons/lucide/shape-rounded.svg`
- `src/ui/icons/lucide/shape-rtriangle.svg`
- `src/ui/icons/lucide/shape-segment.svg`
- `src/ui/icons/lucide/shape-shield.svg`
- `src/ui/icons/lucide/shape-sparkle.svg`
- `src/ui/icons/lucide/shape-spiral.svg`
- `src/ui/icons/lucide/shape-squarestar.svg`
- `src/ui/icons/lucide/shape-star.svg`
- `src/ui/icons/lucide/shape-sun.svg`
- `src/ui/icons/lucide/shape-tear.svg`
- `src/ui/icons/lucide/shape-ticket.svg`
- `src/ui/icons/lucide/shape-trapezoid.svg`
- `src/ui/icons/lucide/shape-tri.svg`
- `src/ui/icons/lucide/sharpen.svg`
- `src/ui/icons/lucide/slice-sel.svg`
- `src/ui/icons/lucide/slice.svg`
- `src/ui/icons/lucide/smart.svg`
- `src/ui/icons/lucide/smudge.svg`
- `src/ui/icons/lucide/sparkle.svg`
- `src/ui/icons/lucide/sponge.svg`
- `src/ui/icons/lucide/spot-heal.svg`
- `src/ui/icons/lucide/spray.svg`
- `src/ui/icons/lucide/stroke-width.svg`
- `src/ui/icons/lucide/style-picker.svg`
- `src/ui/icons/lucide/swatches.svg`
- `src/ui/icons/lucide/trace.svg`
- `src/ui/icons/lucide/transparency.svg`
- `src/ui/icons/lucide/trash.svg`
- `src/ui/icons/lucide/tweak.svg`
- `src/ui/icons/lucide/type-mask.svg`
- `src/ui/icons/lucide/type-mask-v.svg`
- `src/ui/icons/lucide/type.svg`
- `src/ui/icons/lucide/type-vert.svg`
- `src/ui/icons/lucide/vector-brush.svg`
- `src/ui/icons/lucide/wand.svg`
- `src/ui/icons/lucide/zoom.svg`
- `src/ui/icons.qrc`
- `src/ui/image_ops.h`
- `src/ui/keymap.cpp`
- `src/ui/keymap.h`
- `src/ui/layer_style_dialog.cpp`
- `src/ui/layer_style_dialog.h`
- `src/ui/liquify_dialog.cpp`
- `src/ui/liquify_dialog.h`
- `src/ui/live_filter.cpp`
- `src/ui/live_filter.h`
- `src/ui/logging.cpp`
- `src/ui/logging.h`
- `src/ui/main_window.h`
- `src/ui/mask_finish.cpp`
- `src/ui/mask_finish.h`
- `src/ui/meson.build`
- `src/ui/options_bar.cpp`
- `src/ui/options_bar.h`
- `src/ui/panels/actions/actions_panel.cpp`
- `src/ui/panels/adjustments/adjustments_panel.cpp`
- `src/ui/panels/aimodels/aimodels_panel.cpp`
- `src/ui/panels/brushes/brushes_panel.cpp`
- `src/ui/panels/channels/channels_panel.cpp`
- `src/ui/panels/character/character_panel.cpp`
- `src/ui/panels/color/color_panel.cpp`
- `src/ui/panels.cpp`
- `src/ui/panels.h`
- `src/ui/panels/histogram/histogram_panel.cpp`
- `src/ui/panels/history/history_panel.cpp`
- `src/ui/panels/info/info_panel.cpp`
- `src/ui/panels/layers/layers_panel.cpp`
- `src/ui/panels/libraries/libraries_panel.cpp`
- `src/ui/panels/navigator/navigator_panel.cpp`
- `src/ui/panels/paragraph/paragraph_panel.cpp`
- `src/ui/panels/paths/paths_panel.cpp`
- `src/ui/panels/properties/properties_panel.cpp`
- `src/ui/panels/registry/panel_creators.h`
- `src/ui/panels/registry/panels_registry.cpp`
- `src/ui/panels/shared/panel_helpers.cpp`
- `src/ui/panels/shared/panel_helpers.h`
- `src/ui/panels/swatches/swatches_panel.cpp`
- `src/ui/panels/vector/vector_panels.cpp`
- `src/ui/persona/appearance_panel.cpp`
- `src/ui/persona/appearance_panel.h`
- `src/ui/persona/persona_bar.cpp`
- `src/ui/persona/persona_bar.h`
- `src/ui/persona/persona.cpp`
- `src/ui/persona/persona.h`
- `src/ui/persona/persona_manager.cpp`
- `src/ui/persona/persona_manager.h`
- `src/ui/persona/persona_wire.cpp`
- `src/ui/persona/persona_wire.h`
- `src/ui/persona/stroke_panel.cpp`
- `src/ui/persona/stroke_panel.h`
- `src/ui/persona/vector_build.cpp`
- `src/ui/persona/vector_build.h`
- `src/ui/persona/vector_edit.cpp`
- `src/ui/persona/vector_edit.h`
- `src/ui/persona/vector_gradient.cpp`
- `src/ui/persona/vector_gradient.h`
- `src/ui/persona/vector_node.cpp`
- `src/ui/persona/vector_node.h`
- `src/ui/persona/vector_path_ops.cpp`
- `src/ui/persona/vector_path_ops.h`
- `src/ui/persona/vector_pen.cpp`
- `src/ui/persona/vector_pen.h`
- `src/ui/persona/vector_point_ops.cpp`
- `src/ui/persona/vector_point_ops.h`
- `src/ui/persona/vector_profile.cpp`
- `src/ui/persona/vector_profile.h`
- `src/ui/persona/vector_qr.cpp`
- `src/ui/persona/vector_qr.h`
- `src/ui/persona/vector_raster.cpp`
- `src/ui/persona/vector_raster.h`
- `src/ui/persona/vector_shapes.cpp`
- `src/ui/persona/vector_shapes.h`
- `src/ui/persona/vector_view.cpp`
- `src/ui/persona/vector_view.h`
- `src/ui/preferences_dialog.cpp`
- `src/ui/preferences_dialog.h`
- `src/ui/project_manager.cpp`
- `src/ui/project_manager.h`
- `src/ui/proof_preview.cpp`
- `src/ui/proof_preview.h`
- `src/ui/psd_export.cpp`
- `src/ui/psd_export.h`
- `src/ui/refine_dialog.cpp`
- `src/ui/refine_dialog.h`
- `src/ui/selection_mask.cpp`
- `src/ui/selection_mask.h`
- `src/ui/selection_ops.cpp`
- `src/ui/settings.cpp`
- `src/ui/settings.h`
- `src/ui/spotlight.cpp`
- `src/ui/spotlight.h`
- `src/ui/svg_bridge.cpp`
- `src/ui/svg_bridge.h`
- `src/ui/svg_parts.cpp`
- `src/ui/svg_parts.h`
- `src/ui/theme.cpp`
- `src/ui/theme.h`
- `src/ui/tone_blend_dialog.cpp`
- `src/ui/tone_blend_dialog.h`
- `src/ui/tone_dialogs.cpp`
- `src/ui/tone_dialogs.h`
- `src/ui/tool_registry.cpp`
- `src/ui/tool_registry.h`
- `src/ui/tools/defs/tool_defs.h`
- `src/ui/tools/ids/tool_ids.h`
- `src/ui/tools/log/tool_log.cpp`
- `src/ui/tools/log/tool_log.h`
- `src/ui/tools/options/option_builders.cpp`
- `src/ui/tools/options/option_builders.h`
- `src/ui/tools/options/options_crop.cpp`
- `src/ui/tools/options/options_draw.cpp`
- `src/ui/tools/options/options_navgen.cpp`
- `src/ui/tools/options/options_registry.cpp`
- `src/ui/tools/options/options_retouch.cpp`
- `src/ui/tools/options/options_selection.cpp`
- `src/ui/tools/options/section_options.h`
- `src/ui/tools/options/tool_options.h`
- `src/ui/tools_panel.cpp`
- `src/ui/tools_panel.h`
- `src/ui/tools/table/tool_lookup.cpp`
- `src/ui/tools/table/tool_table.cpp`
- `src/ui/tools/table/tool_table.h`
- `src/ui/window/commands/window_ai.cpp`
- `src/ui/window/commands/window_commands.cpp`
- `src/ui/window/dialogs/dialogs_export.cpp`
- `src/ui/window/dialogs/dialogs_file.cpp`
- `src/ui/window/dialogs/dialogs_filter.cpp`
- `src/ui/window/dialogs/dialogs_liquify.cpp`
- `src/ui/window/dialogs/dialogs_ui.cpp`
- `src/ui/window/docks/window_docks.cpp`
- `src/ui/window/menus/menu_actions.cpp`
- `src/ui/window/menus/menu_bar.cpp`
- `src/ui/window/menus/menu_edit.cpp`
- `src/ui/window/menus/menu_file.cpp`
- `src/ui/window/menus/menu_filter.cpp`
- `src/ui/window/menus/menu_help.cpp`
- `src/ui/window/menus/menu_image.cpp`
- `src/ui/window/menus/menu_layer.cpp`
- `src/ui/window/menus/menu_object.cpp`
- `src/ui/window/menus/menu_path.cpp`
- `src/ui/window/menus/menu_plugins.cpp`
- `src/ui/window/menus/menu_select.cpp`
- `src/ui/window/menus/menu_type.cpp`
- `src/ui/window/menus/menu_view.cpp`
- `src/ui/window/menus/menu_window.cpp`
- `src/ui/window/menus/ops/menu_ops_edit.cpp`
- `src/ui/window/shared/window_helpers.cpp`
- `src/ui/window/shared/window_helpers.h`
- `src/ui/window/shell/document_area.cpp`
- `src/ui/window/shell/window_session.cpp`
- `src/ui/window/shell/window_shell.cpp`
- `src/ui/window/state/window_shortcuts.cpp`
- `src/ui/window/state/window_state.cpp`
- `src/ui/workspace.cpp`
- `src/ui/workspace.h`
- `tests/ablate_probe.cpp`
- `tests/af_open_probe.cpp`
- `tests/af_probe.h`
- `tests/bench_app.cpp`
- `tests/bench_gpu.cpp`
- `tests/bench_move.cpp`
- `tests/calib_probe.cpp`
- `tests/engine_bench.cpp`
- `tests/gpu_invariants.cpp`
- `tests/gpu_invariants.h`
- `tests/gpu_parity.cpp`
- `tests/gpu_parity.h`
- `tests/log_cost_probe.cpp`
- `tests/meson.build`
- `tests/object_select_probe.cpp`
- `tests/ref_fit_probe.cpp`
- `tests/row_update_probe.cpp`
- `tests/scratch_ifp_check.cpp`
- `tests/subject_tuning_probe.cpp`
- `tests/svg_cmp_bench.cpp`
- `tests/svg_toggle_probe.cpp`
- `tests/test_adjust.cpp`
- `tests/test_af.cpp`
- `tests/test_af_emit.cpp`
- `tests/test_af_export.cpp`
- `tests/test_af_export_ui.cpp`
- `tests/test_af_shapes.cpp`
- `tests/test_af_text.cpp`
- `tests/test_ai_models.cpp`
- `tests/test_ai_segment.cpp`
- `tests/test_ai_ui.cpp`
- `tests/test_all.cpp`
- `tests/test_blend.cpp`
- `tests/test_blur.cpp`
- `tests/test_brush.cpp`
- `tests/test_buffer.cpp`
- `tests/test_canvas_drop.cpp`
- `tests/test_chrome_state.cpp`
- `tests/test_clone.cpp`
- `tests/test_cmyk_convert.cpp`
- `tests/test_color_mode.cpp`
- `tests/test_color_policy.cpp`
- `tests/test_cuda.cpp`
- `tests/test_curves_ui.cpp`
- `tests/test_device.cpp`
- `tests/test_docks_ui.cpp`
- `tests/test_document.cpp`
- `tests/test_export_ui.cpp`
- `tests/test_filter_gpu.cpp`
- `tests/test_filters.cpp`
- `tests/test_filter_ui.cpp`
- `tests/test_font_preview.cpp`
- `tests/test_font_search.cpp`
- `tests/test_grade_dialog.cpp`
- `tests/test_grayscale.cpp`
- `tests/test_hip.cpp`
- `tests/test_history.cpp`
- `tests/test_ifp_layers.cpp`
- `tests/test_image_ops.cpp`
- `tests/test_import_export.cpp`
- `tests/test_keymap.cpp`
- `tests/test_layer_stack_ui.cpp`
- `tests/test_layer_style_ui.cpp`
- `tests/test_liquify_ui.cpp`
- `tests/test_live_audit.cpp`
- `tests/test_live_filter.cpp`
- `tests/test_mask_ops.cpp`
- `tests/test_paint_tools.cpp`
- `tests/test_perf.cpp`
- `tests/test_persona.cpp`
- `tests/test_place.cpp`
- `tests/test_place_ui.cpp`
- `tests/test_preferences_ui.cpp`
- `tests/test_project.cpp`
- `tests/test_project_file.cpp`
- `tests/test_proof.cpp`
- `tests/test_proof_preview.cpp`
- `tests/test_psd.cpp`
- `tests/test_psd_export.cpp`
- `tests/test_psd_save.cpp`
- `tests/test_refine_dialog.cpp`
- `tests/test_replace_color.cpp`
- `tests/test_selection_mask.cpp`
- `tests/test_selection_ops.cpp`
- `tests/test_session.cpp`
- `tests/test_settings.cpp`
- `tests/test_spot_heal.cpp`
- `tests/test_svg_batch2.cpp`
- `tests/test_svg_batch3.cpp`
- `tests/test_svg_batch4.cpp`
- `tests/test_svg_batch5.cpp`
- `tests/test_svg_batch6.cpp`
- `tests/test_svg_batch7.cpp`
- `tests/test_svg_batch8.cpp`
- `tests/test_svg_batch9.cpp`
- `tests/test_svg_batch.cpp`
- `tests/test_svg_final.cpp`
- `tests/test_svg_pool.cpp`
- `tests/test_svg_scene.cpp`
- `tests/test_svg_value.cpp`
- `tests/test_svg_xml.cpp`
- `tests/test_text_dblclick.cpp`
- `tests/test_text_engine.cpp`
- `tests/test_tiff.cpp`
- `tests/test_tile.cpp`
- `tests/test_tonal_ops.cpp`
- `tests/test_tonal_ui.cpp`
- `tests/test_tone_blend.cpp`
- `tests/test_tone_blend_group.cpp`
- `tests/test_tool_log.cpp`
- `tests/test_tools_ui.cpp`
- `tests/test_tools_ux.cpp`
- `tests/test_type_tool_ui.cpp`
- `tests/test_util.h`
- `tests/test_vector_boolean.cpp`
- `tests/test_vector_engine.cpp`
- `tests/test_vector_paint_ui.cpp`
- `tests/test_vector_path.cpp`
- `tests/test_vector_shape.cpp`
- `tests/test_vector_svg.cpp`
- `tests/test_warp.cpp`
- `tests/test_xcf.cpp`
- `tests/test_zip.cpp`
- `tests/zz_svg_thumb_repro.cpp`
- `tools/meson.build`
- `tools/run_tone_blend16.cpp`

