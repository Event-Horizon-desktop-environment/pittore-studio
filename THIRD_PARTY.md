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
