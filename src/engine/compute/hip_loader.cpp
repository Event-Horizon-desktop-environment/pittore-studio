// dlopen-based loader for libpittore-hip.so (see hip_loader.h). The plugin
// links libamdhip64, which has no static redistributable build, so linking
// it into the main binary would stop the app starting on machines without
// ROCm. Loading it here keeps the CPU fallback working everywhere while
// AMD machines with the ROCm HIP runtime installed light the GPU path up.
// Linux-only (like the rest of the project): /proc/self/exe locates the
// install and build trees.
#include "engine/compute/hip_loader.h"

#include <dlfcn.h>

#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace pittore::compute {
namespace {

constexpr char kPluginName[] = "libpittore-hip.so";

using CreateFn = ComputeBackend* (*)();
using EnumerateFn = int (*)(Device*, int);

struct Plugin {
    void* handle = nullptr;
    CreateFn create = nullptr;
    EnumerateFn enumerate = nullptr;

    bool ready() const {
        return handle != nullptr && create != nullptr &&
               enumerate != nullptr;
    }
};

// Exe directory via /proc/self/exe; empty when unreadable.
std::string exe_dir() {
    char path[4096];
    const ssize_t n = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return {};
    std::string p(path, static_cast<std::size_t>(n));
    const std::size_t slash = p.rfind('/');
    return slash == std::string::npos ? std::string{} : p.substr(0, slash);
}

void* try_open(const std::string& path) {
    if (path.empty()) return nullptr;
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

const Plugin& plugin() {
    static const Plugin p = [] {
        Plugin out;
        // Explicit override for tests and relocated installs.
        const char* env_plugin = ::getenv("PITTORE_HIP_PLUGIN");
        out.handle =
            try_open(env_plugin != nullptr ? env_plugin : "");
        const std::string dir = exe_dir();
        const std::string rel[] = {
            dir + "/" + kPluginName,
            // Meson build tree: test binaries live in build/tests, the
            // app in build/src/app, the plugin in build/src/engine/compute.
            dir + "/../src/engine/compute/" + kPluginName,
            dir + "/../engine/compute/" + kPluginName,
            // Installed layout: <prefix>/bin/painter with the plugin
            // under <prefix>/lib*/pittore (libdir varies by distro).
            dir + "/../lib/pittore/" + kPluginName,
            dir + "/../lib64/pittore/" + kPluginName,
            dir + "/../lib/x86_64-linux-gnu/pittore/" + kPluginName,
            "/usr/lib/pittore/" + std::string(kPluginName),
            "/usr/lib64/pittore/" + std::string(kPluginName),
            "/usr/lib/x86_64-linux-gnu/pittore/" + std::string(kPluginName),
            "/usr/local/lib/pittore/" + std::string(kPluginName),
            "/usr/local/lib64/pittore/" + std::string(kPluginName),
        };
        for (const auto& c : rel) {
            if (out.handle != nullptr) break;
            out.handle = try_open(c);
        }
        if (out.handle == nullptr)
            out.handle = try_open(kPluginName);  // LD_LIBRARY_PATH
        if (out.handle != nullptr) {
            out.create =
                reinterpret_cast<CreateFn>(::dlsym(out.handle,
                                                  "pittore_hip_create"));
            out.enumerate = reinterpret_cast<EnumerateFn>(
                ::dlsym(out.handle, "pittore_hip_enumerate"));
        }
        return out;
    }();
    return p;
}

}  // namespace

std::vector<Device> hip_plugin_devices() {
    const Plugin& p = plugin();
    if (!p.ready()) return {};
    const int n = p.enumerate(nullptr, 0);
    if (n <= 0) return {};
    std::vector<Device> out(static_cast<std::size_t>(n));
    p.enumerate(out.data(), n);
    return out;
}

std::unique_ptr<ComputeBackend> hip_plugin_make_backend() {
    const Plugin& p = plugin();
    if (!p.ready()) return nullptr;
    return std::unique_ptr<ComputeBackend>(p.create());
}

}  // namespace pittore::compute
