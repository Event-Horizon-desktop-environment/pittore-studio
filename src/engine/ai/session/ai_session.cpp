#include "engine/ai/bg_remove.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "engine/core/log.h"

#ifdef PITTORE_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#endif

#include "engine/ai/shared/ai_session.h"
#include "engine/ai/shared/ai_preprocess.h"

namespace pittore::ai {
namespace ai_detail {

#ifdef PITTORE_HAS_ONNX
std::mutex gMutex;
Ort::Env& ortEnv() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "painter");
    return env;
}

std::unordered_map<std::string, std::unique_ptr<OrtSession>>& sessionCache() {
    static std::unordered_map<std::string, std::unique_ptr<OrtSession>> cache;
    return cache;
}

// One shared CPU arena for all inference sessions: extend-by-exact-need
// instead of power-of-two doubling, so transient spikes stay near true use
// and the retained high-water does not overshoot far above it. Sessions
// opt in at creation; any failure here keeps the default allocators.
void registerBoundedCpuArena() {
    static std::once_flag once;
    std::call_once(once, [] {
        try {
            Ort::MemoryInfo cpuArena = Ort::MemoryInfo::CreateCpu(
                OrtArenaAllocator, OrtMemTypeDefault);
            Ort::ArenaCfg cfg(0, 1, -1, -1);
            ortEnv().CreateAndRegisterAllocator(cpuArena, cfg);
        } catch (...) {
        }
    });
}
OrtSession* getOrLoadSessionLocked(const std::string& path, const char* label,
                                   std::string& err, double msNow) {
    auto iter = sessionCache().find(path);
    if (iter != sessionCache().end()) return iter->second.get();
    try {
        Ort::SessionOptions opts;
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        registerBoundedCpuArena();
        try {
            opts.AddConfigEntry("session.use_env_allocators", "1");
        } catch (...) {
            // Unknown key on this ORT build: sessions fall back to
            // their own default allocators.
        }
        std::string provider = "CPU";
        try {
            // Works when the linked ORT build ships a CUDA execution
            // provider (onnxruntime-cuda); throws cleanly on CPU builds.
            opts.AppendExecutionProvider("CUDA", {});
            provider = "CUDA";
        } catch (const std::exception& e) {
            PITTORE_LOG("[ai] CUDA provider unavailable, falling back to CPU: %s",
                         e.what());
        } catch (...) {
            PITTORE_LOG("[ai] CUDA provider unavailable (unknown error), using CPU");
        }
        auto made = std::make_unique<OrtSession>();
        made->session = Ort::Session(ortEnv(), path.c_str(), opts);
        made->provider = std::move(provider);
        OrtSession* session = made.get();
        sessionCache()[path] = std::move(made);
        PITTORE_LOG("[ai] %s loaded: %.50s provider=%s ORT=%s (load %.0f ms)",
                     label, path.c_str(), session->provider.c_str(),
                     Ort::GetVersionString().c_str(), msNow);
        return session;
    } catch (const std::exception& e) {
        err = std::string("Failed to load ") + path + ": " + e.what();
        return nullptr;
    } catch (...) {
        err = std::string("Failed to load ") + path + " (unknown error).";
        return nullptr;
    }
}

#endif

}  // namespace ai_detail
}  // namespace pittore::ai
