#include "engine/compute/factory.h"

#include "engine/compute/cpu_backend.h"

#if defined(PITTORE_HAS_CUDA)
#include "engine/compute/cuda_backend.h"
#endif
#if defined(PITTORE_HAS_HIP)
#include "engine/compute/hip_loader.h"
#endif

namespace pittore::compute {

std::vector<Device> enumerate_devices() {
    std::vector<Device> out;
    out.push_back(cpu_device());
#if defined(PITTORE_HAS_CUDA)
    for (auto& d : cuda_devices()) out.push_back(d);
#endif
#if defined(PITTORE_HAS_HIP)
    for (auto& d : hip_plugin_devices()) out.push_back(d);
#endif
    return out;
}

std::unique_ptr<ComputeBackend> make_backend(BackendType type) {
    switch (type) {
        case BackendType::CPU:
            return std::make_unique<CpuBackend>();
        case BackendType::CUDA:
#if defined(PITTORE_HAS_CUDA)
            return make_cuda_backend();
#else
            return nullptr;
#endif
        case BackendType::HIP:
#if defined(PITTORE_HAS_HIP)
            return hip_plugin_make_backend();
#else
            return nullptr;
#endif
    }
    return nullptr;
}

std::unique_ptr<ComputeBackend> make_default_backend() {
#if defined(PITTORE_HAS_CUDA)
    if (auto c = make_cuda_backend()) return c;
#endif
#if defined(PITTORE_HAS_HIP)
    if (auto h = hip_plugin_make_backend()) return h;
#endif
    return std::make_unique<CpuBackend>();
}

}  // namespace pittore::compute