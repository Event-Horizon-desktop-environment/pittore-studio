# Pittore Studio

<img width="2400" height="600" alt="pittore-studio-banner" src="https://github.com/user-attachments/assets/0ee74a44-6dcf-4f0f-8f49-97837959ef65" />


A layer-based photo editor for Linux. Raster painting and retouching, vector
shapes and paths, type, adjustment layers, filters, and an ICC-aware colour
pipeline, written in C++20 on Qt 6.

Rendering goes through the GPU when you have an NVIDIA or AMD card and falls
back to a CPU path when you don't. Both backends implement the same kernels,
and the test suite checks that they agree.

**Status:** alpha. The engine and the test suite are in decent shape; the
interface still moves around and you should expect rough edges. Linux only —
the build refuses to configure anywhere else.

## Features

- Layer documents with groups, masks, clipping, adjustment layers and a full
  undo history, saved in a single-file `.psc` project format (legacy `.ifp` still opens).
- 135 filters in 17 categories, 15 adjustment kinds, 28 blend modes, and layer
  styles (shadow, glow, overlay, stroke, bevel, blur).
- Selection, retouching and painting tooling: marquees, lasso and magic wand,
  object selection, clone and healing brushes, patch, content-aware move,
  liquify, mixer and history brushes, dodge/burn/sponge.
- Vector persona: pen tools, boolean path operations, around 40 parametric
  shapes, SVG import and export, QR codes.
- Four personas — Pixel, Vector, Draw, Color — each with its own tool set and
  options bar.
- Text built on HarfBuzz shaping and FreeType rasterization, with character
  and paragraph panels and type masks.
- Colour management through Little CMS: ICC profiles on import, CMYK
  conversion, soft proofing, and a mismatch policy when a file's profile
  disagrees with the working space.
- Optional AI background removal, object selection and refinement. The models
  are downloaded on demand, never bundled.

[Full feature list, including what is unfinished →](docs/FEATURES.md)

## Building

The one-command route:

```sh
./build.sh
```

It detects your distro, installs whatever is missing, then configures and
compiles. It never installs GPU drivers or AMD tooling — HIP is picked up only
if `hipcc` is already on your machine.

For the manual route, exact package names per distro, the meson options and
the common build problems, see **[docs/BUILDING.md](docs/BUILDING.md)**.

Summary of the manual path:

```sh
meson setup build
ninja -C build
./build/src/app/painter
```

### Tests

```sh
just test
```

Without `just`:

```sh
ninja -C build && meson test -C build --print-errorlogs
```

The suite covers the engine, the CPU/GPU kernel parity checks and a set of
headless UI tests. Some tests look for sample files that are not in this
repository; they skip themselves when the file is absent.

### Optional extras

```sh
just models          # download background-removal models (multi-GB)
just install         # system-wide install into /usr/bin
```

GPU backends and the ONNX Runtime are auto-detected. Force or disable them
with the meson options documented in the build guide.

## Repository layout

```
src/engine/    core types, compute kernels, IO codecs, filters, render,
               text, colour, vector, AI
src/ui/        Qt interface: panels, canvas, tools, dialogs, settings
src/app/       entry point
tests/         unit, codec, GPU-parity and headless UI tests
tools/         small developer utilities
docs/          features and build instructions
```

Build files are `meson.build` throughout; `justfile` wraps the common
commands; `build.sh` bootstraps a machine from scratch.

## Contributing

Read `docs/BUILDING.md`, get a green `just test`, and keep it green. The code
is organized so that each file does one thing; keep new code to a single
obvious responsibility.

## License

MIT — see [LICENSE](LICENSE).

The bundled Lucide icons keep their own ISC license in
`src/ui/icons/lucide/LICENSE.lucide`. Qt, HarfBuzz, Little CMS and the rest
come from the system rather than from this repository, and downloaded AI
models carry the license shown when you fetch them.
