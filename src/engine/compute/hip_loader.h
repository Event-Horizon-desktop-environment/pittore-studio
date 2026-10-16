#pragma once
// Runtime loader for libpittore-hip.so. The HIP backend ships as a plugin
// so the app and tools start on machines without ROCm installed: dlopen
// fails there and every call below degrades to "no HIP devices". Declared
// separately from factory.h: this is the loading mechanism, not the
// backend-selection interface.

#include <memory>
#include <vector>

#include "engine/compute/backend.h"

namespace pittore::compute {

// Devices reported by the HIP plugin on this machine (empty when the
// plugin or the ROCm runtime is absent, or no AMD GPU is present).
std::vector<Device> hip_plugin_devices();

// Backend bound to the first usable HIP device, or nullptr when the
// plugin cannot provide one.
std::unique_ptr<ComputeBackend> hip_plugin_make_backend();

}  // namespace pittore::compute
