# Third-party notices

Components consumed under their own licenses. Nothing below is vendored
into this repository; each is used at build/packaging time or carries its
own license file where it lives in the tree.

## NVIDIA CUDA Toolkit (build-time only)

- What: nvcc and the CUDA runtime headers/libraries used to compile
  `src/engine/compute/cuda_backend.cu` when `-Dbackend-cuda` is enabled.
- License: NVIDIA CUDA Toolkit EULA (proprietary; accepted by whoever
  installs the toolkit to run the build). The only toolkit code that
  reaches shipped binaries is the CUDA runtime, linked statically
  (`libcudart_static.a`), which the EULA's Attachment A lists as
  redistributable (https://docs.nvidia.com/cuda/eula/). No toolkit files
  are vendored in this repository.
- Why a system library is insufficient: no Linux distribution ships
  prebuilt Pittore CUDA kernels; the `.cu` source must be compiled by
  nvcc for the target GPU architectures.
- Source: https://developer.nvidia.com/cuda-toolkit — CI installs it
  from NVIDIA's official per-distro repositories (Ubuntu, Fedora) or the
  distro package (Arch `cuda`). Developers install it locally; without it
  the build transparently falls back to the CPU backend.
- Runtime note: shipped binaries link the runtime statically and talk
  only to `libcuda.so.1` from the user's installed NVIDIA driver. No
  toolkit is needed on user machines; without a driver the app runs the
  CPU backend.

## AMD ROCm HIP (build-time only)

- What: hipcc and the HIP headers/libraries used to compile
  `src/engine/compute/hip_backend.cpp` when `-Dbackend-hip` is enabled.
- License: MIT (`ROCm/hip` LICENSE.md,
  https://github.com/ROCm/hip/blob/develop/LICENSE.md). The only ROCm
  code that reaches shipped binaries is the device code hipcc bundles
  into our kernels at compile time; the host runtime (`libamdhip64`)
  has no static redistributable build, so the backend ships as a plugin
  (`libpittore-hip.so`) that the app `dlopen`s. No ROCm files are
  vendored in this repository.
- Why a system library is insufficient: no Linux distribution ships
  prebuilt Pittore HIP kernels; the source must be compiled by hipcc
  for the target AMD architectures (one `--offload-arch` image each —
  HIP has no driver-JIT fallback, so the shipped list in CI covers
  GCN5.1/CDNA/RDNA1-4 explicitly).
- Source: https://www.amd.com/en/developer/resources/rocm-hub.html —
  CI installs it from AMD's official per-distro repositories (Ubuntu,
  Fedora) or the distro package (Arch `hip-runtime-amd`). Developers
  install it locally; without it the build transparently falls back to
  the CPU backend.
- Runtime note: the app starts everywhere (the plugin simply fails to
  load where ROCm is absent and the CPU backend is used). On AMD
  machines the GPU path lights up once the small `hip-runtime-amd`
  package is installed (same arrangement Blender uses: it is a
  `Recommends`/`optdepends`, never a hard dependency). Without it the
  app runs the CPU backend.
