// HIP plugin entry points: the C ABI surface of libpittore-hip.so.
// The loader (hip_loader.cpp) finds these with dlsym, so the main binary
// never links libamdhip64 and starts fine on machines without ROCm.
#include <vector>

#include "engine/compute/hip_backend.h"

extern "C" {

// Backend bound to the first usable HIP device; nullptr when none.
// Ownership passes to the caller (same heap: one build, one libstdc++).
pittore::compute::ComputeBackend* pittore_hip_create() {
    return pittore::compute::make_hip_backend().release();
}

// Fill up to `cap` Devices, return the total count. A null `out` (or
// cap 0) only counts, so the loader sizes its vector first.
int pittore_hip_enumerate(pittore::compute::Device* out, int cap) {
    const std::vector<pittore::compute::Device> ds =
        pittore::compute::hip_devices();
    int n = 0;
    for (const auto& d : ds) {
        if (out != nullptr && n < cap) out[n] = d;
        ++n;
    }
    return n;
}
}
