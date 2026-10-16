# Pittore Studio — developer command file
# Usage: just [recipe] | just --list | just install  (no sudo prefix:
# only the install step itself escalates for your password)
set shell := ["bash", "-o", "pipefail", "-c"]

# Toolkits live outside PATH on some distros (/opt/cuda, /opt/rocm), and sudo
# scrubs PATH outright — either way ninja ends up running bare `nvcc`/`hipcc`
# and failing. Prepend the standard locations so user-shell builds resolve.
export PATH := "/opt/cuda/bin:/opt/rocm/bin:" + env_var("PATH")

# Build parallelism cap: ninja defaults to core-count jobs, but big TUs
# (app_state.cpp runs ~1GB RSS with debug info) make uncapped parallelism
# OOM ordinary desktops. Budget ~3GB per job from MemAvailable, clamped to
# [1, nproc]; PITTORE_JOBS overrides outright. The same budget caps test
# runtime parallelism below (meson test defaults to core-count processes,
# and GPU/Qt test binaries are far heavier per process than the 3GB/job
# compile budget assumes — never raise one without the other).
# UI tests link the single pittore_ui_dep static lib (only two probe
# harnesses still compile UI sources directly); the remaining per-job cost
# is debug info + parallel links, which is what the cap bounds.
JOBS := env_var_or_default("PITTORE_JOBS", `n=$(nproc); j=$(awk '/MemAvailable/{print int($2/3072000)}' /proc/meminfo); [ "${j:-0}" -lt 1 ] && j=1; [ "$j" -gt "$n" ] && j=$n; echo "$j"`)

# Configure (first run only) + build only what changed into ./build.
# Plain ninja tracks header/source mtimes, so repeat runs are no-ops when
# nothing changed. Do NOT reconfigure here: `meson setup --reconfigure`
# re-probes Qt/CUDA/HIP and regenerates the manifest on every run, which
# needlessly widens multi-hundred-target rebuilds.
# GPU backends are auto-detected: CUDA when nvcc exists, HIP when hipcc exists.
# Override with:  meson configure build -Dbackend-cuda=disabled
build:
	test -f build/build.ninja || meson setup build --prefix=/usr
	ninja -C build -j{{JOBS}}

# Re-run Meson configuration (added/renamed files, new options, Qt/deps
# changes), then build. Use this instead of `build` when the file list or
# build options changed.
reconf:
	meson setup --reconfigure build
	ninja -C build -j{{JOBS}}

# Build and run the full test suite (engine + kernel parity). Test
# processes are capped by the same JOBS budget as the build: meson test
# otherwise runs one process per core, and concurrent GPU/Qt test binaries
# spike RAM far past what the desktops this cap protects can absorb.
test: build
	meson test -C build --print-errorlogs --num-processes {{JOBS}}

# Build ONLY the application, skipping the test binaries. This is the fast
# edit-run loop: with no changes it is a no-op, with changes it compiles
# just those TUs and relinks one binary. Use `build` when you need the
# tests too.
#
# Configures for release, because `just install` ships this and a debug binary
# is only useful while you are actively working on it. Switch back to debug
# with:  meson configure build -Dbuildtype=debugoptimized
app:
	test -f build/build.ninja && meson configure build -Dbuildtype=release || meson setup build --prefix=/usr -Dbuildtype=release
	ninja -C build -j{{JOBS}} src/app/painter src/app/pittore-mcp

# Single-binary test runner — send this to a friend.
test-all: build
	./build/tests/test-all

# Run the app (engine milestone shell).
run: app
	./build/src/app/painter

# Print the background-removal model catalogue and its download URLs.
models-list: app
	./build/src/app/painter --list-models

# Download every background-removal model into the per-user cache (multi-GB).
# Set PITTORE_MODELS_DIR to redirect the cache (default: ~/.local/share/PittoreStudio/models).
models: app
	./build/src/app/painter --fetch-models

# Download a single model:  just model birefnet-portrait
model id: app
	./build/src/app/painter --fetch-model {{id}}

# Build first (as you), then install system-wide into /usr/bin.
# Run WITHOUT sudo: only the install step escalates for your password.
# --no-rebuild matters: a rebuild under sudo would run with sudo's scrubbed
# PATH (no nvcc/hipcc) and fail even though user-shell builds work fine.
# Only the app binary installs, so the `app` target is sufficient.
#
# This always installs a RELEASE build: shipping a debug binary is not useful
# to anyone but you. Build a debug tree locally with `./build.sh --buildtype=debug`
# and switch it back to release with `meson configure build -Dbuildtype=release`.
install:
    # sudo-safe: when the whole command runs as root (`sudo just install`),
    # de-escalate the build to the invoking user. A root build runs with
    # sudo's scrubbed environment and litters build/ with root-owned objects,
    # so the install that follows can ship a stale binary (and later
    # user-shell builds trip over the root-owned files). Only this install
    # step needs root. Plain `just install` is unchanged: SUDO_USER is unset
    # outside sudo, so it builds as you as before.
    if [ -n "${SUDO_USER:-}" ] && [ "$(id -u)" = 0 ] && [ "$SUDO_USER" != root ]; then sudo -u "$SUDO_USER" just app; else just app; fi
    sudo meson install -C build --no-rebuild

# Remove what 'install' placed (ninja uninstall runs a script, no rebuild).
uninstall:
	sudo ninja -C build uninstall

# Wipe the build directory.
clean:
	rm -rf build

# List which compute backends are compiled in.
backends:
	meson configure build | grep -E 'backend-(cuda|hip)|app'

# Print the AI model catalogue, download URLs and the current model cache dir.
ai-catalog: app
	./build/src/app/painter --list-models

# Install the system ONNX Runtime (CUDA execution provider) for GPU background
# removal. Needs your password once via sudo; then this reconfigures the build
# to auto-detect it and rebuilds. Until this runs (or until -Donnxruntime-root
# points at a CUDA-capable ONNX Runtime), inference uses the CPU runtime.
deps-ai:
	sudo pacman -S --needed onnxruntime-cuda
	meson configure build -Donnxruntime-root=
	ninja -C build -j{{JOBS}}