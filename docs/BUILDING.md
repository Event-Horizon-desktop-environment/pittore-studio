# Building

**This project is Linux only.** The meson configuration calls `error()` on
anything that is not Linux, so there is no Windows or macOS path to get wrong
— if `meson setup` runs, you are on a supported platform.

## Requirements

| | |
|---|---|
| OS | Linux (any distro with the packages below) |
| Meson | **1.1.0 or newer** |
| Compiler | GCC or Clang with C++20 |
| Build | Ninja, pkg-config |
| Qt | Qt 6 (Widgets, Svg, Network) |
| Optional | CUDA toolkit, ROCm/HIP, ONNX Runtime |

Required libraries: `zlib`, `harfbuzz`, `freetype2`, `fontconfig`,
`tomlplusplus`, `Qt6Widgets`, `Qt6Network`, `Qt6Svg`.

Optional libraries: `lcms2`, `libwebp`, `libtiff-4`, `libjpeg`, `libzstd`,
`libqrencode`, `onnxruntime`. They build when present and are skipped with a
warning when not, but they gate real features. Worth installing all of them:

| Missing | What you lose |
|---|---|
| `lcms2` | ICC profile conversion and soft proofing; conversion falls back to naive math |
| `libwebp`, `libtiff-4`, `libjpeg` | WebP, TIFF and JPEG decoding and encoding |
| `libzstd` | Affinity archive compression |
| `libqrencode` | The vector QR Code tool |
| `onnxruntime` | GPU background removal and object selection |

## Quick start

```sh
./build.sh
```

`build.sh` reads `/etc/os-release`, installs whatever is missing from your
distro's repository, configures `./build` and compiles. It never installs GPU
drivers or anything AMD — HIP is compiled in only if `hipcc` is already
installed.

Useful flags:

```sh
./build.sh --no-deps                 # never install, just build
./build.sh --buildtype=release       # optimised, NDEBUG (default: debugoptimized)
./build.sh --buildtype=debug         # unoptimised, for debugging
./build.sh --help
```

Builds are `debugoptimized` (`-O2 -g`) unless you say otherwise. This matters:
the Affinity import runs ~7x slower unoptimised (33s instead of 4.5s on a
115-layer document), because it is one long chain of per-pixel work.

## Manual build

### Arch / Manjaro

```sh
sudo pacman -S --needed base-devel meson ninja git pkgconf \
  zlib harfbuzz freetype2 fontconfig tomlplusplus lcms2 \
  qt6-base qt6-svg \
  libwebp tiff libjpeg-turbo zstd libqrencode
```

### Fedora

```sh
sudo dnf install gcc-c++ meson ninja-build git pkgconf \
  zlib-ng-compat-devel harfbuzz-devel freetype-devel fontconfig-devel \
  tomlplusplus-devel lcms2-devel \
  qt6-qtbase-devel qt6-qtsvg-devel \
  libwebp-devel libtiff-devel libjpeg-turbo-devel libzstd-devel \
  qrencode-devel
```

`zlib` comes from `zlib-ng-compat-devel` and QR from `qrencode-devel` — there
are no packages called `zlib-devel` or `libqrencode-devel` here, which is the
usual reason a Fedora build fails.

### Debian / Ubuntu

```sh
sudo apt-get install build-essential meson ninja-build git pkg-config \
  zlib1g-dev libharfbuzz-dev libfreetype-dev libfontconfig-dev \
  libtomlplusplus-dev liblcms2-dev \
  qt6-base-dev libqt6svg6-dev \
  libwebp-dev libtiff-dev libjpeg-dev libzstd-dev libqrencode-dev
```

**Mind the meson version.** Stable Debian and older Ubuntu releases ship a
meson older than 1.1.0. Check with `meson --version`; if it is below 1.1.0
install a newer one:

```sh
pipx install --system-site-packages meson   # or: pip install --user meson
```

### openSUSE

```sh
sudo zypper install gcc-c++ meson ninja git pkgconf \
  zlib-devel harfbuzz-devel freetype2-devel fontconfig-devel \
  tomlplusplus-devel lcms2-devel \
  qt6-base-devel libqt6-qt6svg-devel \
  libwebp-devel libtiff-devel libjpeg8-devel libzstd-devel \
  libqrencode-devel
```

### Then

```sh
meson setup build
ninja -C build
./build/src/app/painter
```

Cap parallel jobs by available RAM: the test tree compiles app_state.cpp
(~1GB per TU) into every test binary, so core-count ninja OOMs — e.g.
`ninja -C build -j4`. (`just build` sizes `-j` from MemAvailable
automatically; `PITTORE_JOBS` overrides.)

## Meson options

| Option | Default | What it does |
|---|---|---|
| `backend-cuda` | `auto` | CUDA (NVIDIA) backend; on when `nvcc` is found (PATH or a standard install root) |
| `backend-hip` | `auto` | HIP (AMD) backend; on when `hipcc` is found |
| `cuda-root` | auto | CUDA toolkit root, overriding PATH and the standard roots |
| `cuda-archs` | `native` | GPU targets nvcc compiles for; `native` probes the build GPU, a GPU-less builder (CI) passes a comma list such as `sm_75,sm_86,sm_120,+ptx` |
| `hip-root` | auto | ROCm root when it is not on `PATH` |
| `app` | `auto` | Build the Qt application; off when Qt6 or `lcms2` is missing |
| `onnxruntime` | `auto` | ONNX Runtime for GPU inference |
| `onnxruntime-root` | auto | Prefix of an ONNX Runtime install |

```sh
# examples
meson setup build -Dbackend-cuda=disabled        # AMD-only machine
meson setup build -Dapp=disabled                 # engine and tests only
meson configure build -Donnxruntime-root=/opt/onnxruntime
```

Check what a configured build picked up:

```sh
just backends
```

## Commands

The `justfile` wraps the common operations. Install `just` from your
repository (`pacman -S just`, `dnf install just`) or with
`cargo install just`.

| Command | Does |
|---|---|
| `just build` | Configure if needed, build everything including tests |
| `just app` | Build only the application (fast edit/run loop) |
| `just run` | Build and launch |
| `just test` | Build, then run the whole suite |
| `just test-all` | Single-binary test runner |
| `just reconf` | Re-run meson (new files, changed options) |
| `just backends` | Show which GPU backends are compiled in |
| `just models` | Download background-removal models |
| `just model <id>` | Download one model |
| `just install` | Install to `/usr/bin` (only the install step asks for sudo) |
| `just uninstall` | Remove it again |
| `just clean` | Delete the build directory |

Without `just`, the same thing by hand:

```sh
meson setup build --prefix=/usr
ninja -C build
meson test -C build --print-errorlogs
sudo meson install -C build
```

## Tests

```sh
just test
```

Around 80 tests: engine units, codec round-trips, CPU/GPU kernel parity and
headless UI tests. The exact count moves as tests are added, and GPU tests
compile and run only for the backend that is available, so it varies a little
between machines.

Some tests look for sample files that are not in this repository (large TIFFs,
`.af` documents, an ICC profile). They are pointed at by `PITTORE_*`
environment variables and **skip cleanly when the variable is unset or the
file is missing** — a fresh clone runs the whole suite without them.

## AI models

Background removal, object selection and refinement run on ONNX models that
you download yourself. Nothing downloads automatically and nothing is
bundled.

```sh
just models                     # everything, multi-GB
just model birefnet-portrait    # one of them
just models-list                # catalogue with URLs and sizes
```

Models are cached under `~/.local/share/PittoreStudio/models`; set
`PITTORE_MODELS_DIR` to move the cache.

GPU inference additionally needs an ONNX Runtime built with CUDA:

```sh
just deps-ai                    # Arch: installs onnxruntime-cuda and reconfigures
```

On other distros point meson at your own build with
`-Donnxruntime-root=/path/to/onnxruntime`. Without it the models run on the
CPU, which works but is slower.

## Install and uninstall

```sh
just install      # builds, then installs — only this step escalates
just uninstall
```

The install step is deliberately separated from the build: running ninja
under `sudo` scrubs `PATH`, so `hipcc` disappears and a GPU build that
worked a second ago fails.

## Troubleshooting

**Configure fails on `meson_version`** — your meson is older than 1.1.0. See
the distro notes above.

**Every compile fails after you removed a compiler wrapper** — the build dir
still holds the dead wrapper path. Wipe it: `just clean`, then configure
again.

**The application target is missing** — Qt6 or `lcms2` was not found. Install
them and reconfigure: `just reconf`.

**CUDA not picked up** — `nvcc` was searched on `PATH` plus the standard
install roots (`/usr/local/cuda/bin`, `/opt/cuda/bin`, `/usr/lib/cuda/bin`).
For any other layout, pass the root explicitly:
`meson setup build -Dcuda-root=/path/to/cuda`.

**HIP not picked up** — `hipcc` is not on `PATH`; build.sh never installs
ROCm. Install it yourself, then `just reconf`.

**Stale or confusing configuration state** — start over:

```sh
just clean && meson setup build
```
