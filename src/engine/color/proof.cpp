// Soft-proof manager over LittleCMS2. Without -DPITTORE_LCMS2 this is a
// compiled-in stub that reports unavailable, so the rest of the tree never
// needs #ifdefs at call sites.

#include "engine/color/proof.h"

#include <cstdio>
#include <filesystem>
#include <utility>

#ifdef PITTORE_LCMS2
#include <lcms2.h>
#endif

namespace pittore::color {

struct ProofManager::Impl {
    bool haveLcms = false;
#ifdef PITTORE_LCMS2
    void* ctx = nullptr;  // cmsContext, untyped so the header stays clean
#endif
    std::string srcPath;    // empty = built-in sRGB
    std::string proofPath;  // empty = unset
    ProofIntent intent = ProofIntent::Relative;
    ProofIntent proofIntent = ProofIntent::Relative;
    bool bpc = true;
    bool gamutCheck = false;
    float alarm[3] = {1.0f, 0.0f, 1.0f};
    // Live cached transform + the fingerprint it was built for. Cache holds a
    // single entry: sliders re-run tiles through it and never rebuild it, so
    // any settings change is exactly one rebuild by construction.
    std::string fingerprint;
#ifdef PITTORE_LCMS2
    cmsHPROFILE srcProfile = nullptr;
    cmsHPROFILE displayProfile = nullptr;
    cmsHPROFILE proofProfile = nullptr;
    cmsHTRANSFORM xform = nullptr;
#endif
};

namespace {

std::string fileStamp(const std::string& path) {
    // mtime+size: cheap invalidation if the profile file is replaced.
    try {
        const auto t = std::filesystem::last_write_time(path);
        const auto s = std::filesystem::file_size(path);
        return path + "@" + std::to_string(t.time_since_epoch().count()) +
               "+" + std::to_string(s);
    } catch (const std::filesystem::filesystem_error&) {
        return path + "@?";
    }
}

}  // namespace

ProofManager::ProofManager() : impl_(new Impl()) {
#ifdef PITTORE_LCMS2
    impl_->ctx = cmsCreateContext(nullptr, nullptr);
    impl_->haveLcms = impl_->ctx != nullptr;
#endif
}

ProofManager::~ProofManager() {
#ifdef PITTORE_LCMS2
    if (impl_->xform) cmsDeleteTransform(impl_->xform);
    if (impl_->srcProfile) cmsCloseProfile(impl_->srcProfile);
    if (impl_->displayProfile) cmsCloseProfile(impl_->displayProfile);
    if (impl_->proofProfile) cmsCloseProfile(impl_->proofProfile);
    if (impl_->ctx) cmsDeleteContext(static_cast<cmsContext>(impl_->ctx));
#endif
    delete impl_;
}

bool ProofManager::available() const { return impl_->haveLcms; }

std::string ProofManager::setSourceProfile(const std::string& path) {
    if (!impl_->haveLcms) return "proof unavailable: built without LCMS2";
    if (!path.empty() && !std::filesystem::exists(path))
        return "source profile not found: " + path;
    impl_->srcPath = path;
    impl_->fingerprint.clear();  // force rebuild on next apply
    return "";
}

std::string ProofManager::setProofProfile(const std::string& path) {
    if (!impl_->haveLcms) return "proof unavailable: built without LCMS2";
    if (!std::filesystem::exists(path))
        return "proof profile not found: " + path;
    impl_->proofPath = path;
    impl_->fingerprint.clear();
    return "";
}

void ProofManager::setIntent(ProofIntent intent, ProofIntent proofIntent) {
    impl_->intent = intent;
    impl_->proofIntent = proofIntent;
    impl_->fingerprint.clear();
}

void ProofManager::setBlackPointCompensation(bool on) {
    impl_->bpc = on;
    impl_->fingerprint.clear();
}

void ProofManager::setGamutCheck(bool on, float alarmR, float alarmG,
                                 float alarmB) {
    impl_->gamutCheck = on;
    impl_->alarm[0] = alarmR;
    impl_->alarm[1] = alarmG;
    impl_->alarm[2] = alarmB;
    impl_->fingerprint.clear();
}

std::size_t ProofManager::cacheSize() const {
#ifdef PITTORE_LCMS2
    return impl_->xform ? 1 : 0;
#else
    return 0;
#endif
}

#ifdef PITTORE_LCMS2

namespace {

int toLcmsIntent(ProofIntent i) {
    switch (i) {
        case ProofIntent::Perceptual: return INTENT_PERCEPTUAL;
        case ProofIntent::Relative: return INTENT_RELATIVE_COLORIMETRIC;
        case ProofIntent::Saturation: return INTENT_SATURATION;
        case ProofIntent::Absolute: return INTENT_ABSOLUTE_COLORIMETRIC;
    }
    return INTENT_RELATIVE_COLORIMETRIC;
}

std::string buildFingerprint(const ProofManager::Impl& st) {
    const std::string src = st.srcPath.empty() ? "builtin-srgb"
                                               : fileStamp(st.srcPath);
    std::string fp = "src=" + src + "|proof=" + fileStamp(st.proofPath);
    fp += "|intent=" + std::to_string(static_cast<int>(st.intent)) + "," +
          std::to_string(static_cast<int>(st.proofIntent));
    fp += st.bpc ? "|bpc=1" : "|bpc=0";
    fp += st.gamutCheck ? "|gamut=1" : "|gamut=0";
    if (st.gamutCheck) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "|alarm=%.4g,%.4g,%.4g", st.alarm[0],
                      st.alarm[1], st.alarm[2]);
        fp += buf;
    }
    return fp;
}

void closeCached(ProofManager::Impl& st) {
    if (st.xform) {
        cmsDeleteTransform(st.xform);
        st.xform = nullptr;
    }
    if (st.srcProfile) {
        cmsCloseProfile(st.srcProfile);
        st.srcProfile = nullptr;
    }
    if (st.displayProfile) {
        cmsCloseProfile(st.displayProfile);
        st.displayProfile = nullptr;
    }
    if (st.proofProfile) {
        cmsCloseProfile(st.proofProfile);
        st.proofProfile = nullptr;
    }
}

}  // namespace

std::string ProofManager::applyProof(const float* src, float* dst,
                                     std::uint32_t w, std::uint32_t h) {
    Impl& st = *impl_;
    if (src == nullptr || dst == nullptr) return "null pixel buffer";
    if (w == 0 || h == 0) return "empty region";
    if (st.proofPath.empty()) return "no proof profile set";

    const std::string fp = buildFingerprint(st);
    if (st.xform == nullptr || fp != st.fingerprint) {
        closeCached(st);
        cmsContext ctx = static_cast<cmsContext>(st.ctx);
        st.srcProfile = st.srcPath.empty()
                            ? cmsCreate_sRGBProfileTHR(ctx)
                            : cmsOpenProfileFromFileTHR(ctx,
                                                        st.srcPath.c_str(),
                                                        "r");
        st.displayProfile = cmsCreate_sRGBProfileTHR(ctx);
        st.proofProfile =
            cmsOpenProfileFromFileTHR(ctx, st.proofPath.c_str(), "r");
        if (!st.srcProfile || !st.displayProfile || !st.proofProfile) {
            closeCached(st);
            return "failed to open profile for proof transform";
        }
        cmsUInt32Number flags = cmsFLAGS_SOFTPROOFING;
        if (st.bpc) flags |= cmsFLAGS_BLACKPOINTCOMPENSATION;
        if (st.gamutCheck) {
            flags |= cmsFLAGS_GAMUTCHECK;
            cmsUInt16Number alarm[cmsMAXCHANNELS] = {0, 0, 0};
            for (int c = 0; c < 3; ++c) {
                const float v = st.alarm[c] < 0.0f
                                    ? 0.0f
                                    : (st.alarm[c] > 1.0f ? 1.0f
                                                          : st.alarm[c]);
                alarm[c] = static_cast<cmsUInt16Number>(v * 65535.0f + 0.5f);
            }
            cmsSetAlarmCodesTHR(ctx, alarm);
        }
        // Extra channels (alpha) pass through untouched by the CMM.
        st.xform = cmsCreateProofingTransformTHR(
            ctx, st.srcProfile, TYPE_RGBA_FLT, st.displayProfile,
            TYPE_RGBA_FLT, st.proofProfile, toLcmsIntent(st.intent),
            toLcmsIntent(st.proofIntent), flags);
        if (!st.xform) {
            closeCached(st);
            return "failed to create proofing transform";
        }
        st.fingerprint = fp;
    }

    if (src == dst) {
        cmsDoTransform(st.xform, const_cast<float*>(src), dst,
                       static_cast<cmsUInt32Number>(w) * h);
    } else {
        // Row walk keeps each cmsDoTransform call bounded.
        for (std::uint32_t y = 0; y < h; ++y) {
            cmsDoTransform(st.xform, const_cast<float*>(src + y * w * 4),
                           dst + y * w * 4, w);
        }
    }
    if (st.gamutCheck) {
        // The 16-bit alarm codes above only take effect for integer output
        // formats: the float pipeline ignores cmsSetAlarmCodes and emits a
        // whole-pixel -1 sentinel for out-of-gamut instead (cross-checked
        // against the 8-bit path over a 17^3 sRGB sweep: the sentinel is
        // always exactly -1 on all three channels and always agrees with
        // the 8-bit alarm verdict). Substitute the configured alarm here.
        // Small single-channel negatives are interpolation undershoot —
        // identical with and without the flag — and must not trigger it.
        float alarm[3];
        for (int c = 0; c < 3; ++c)
            alarm[c] = st.alarm[c] < 0.0f
                           ? 0.0f
                           : (st.alarm[c] > 1.0f ? 1.0f : st.alarm[c]);
        const std::size_t n = static_cast<std::size_t>(w) * h;
        for (std::size_t i = 0; i < n; ++i) {
            float* px = dst + i * 4;
            if (px[0] == -1.0f && px[1] == -1.0f && px[2] == -1.0f) {
                px[0] = alarm[0];
                px[1] = alarm[1];
                px[2] = alarm[2];
            }
        }
    }
    return "";
}

#else  // !PITTORE_LCMS2

std::string ProofManager::applyProof(const float*, float*, std::uint32_t,
                                     std::uint32_t) {
    return "proof unavailable: built without LCMS2";
}

#endif

}  // namespace pittore::color
