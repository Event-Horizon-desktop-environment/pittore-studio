#pragma once
// CMYK fixture lookup for the soft-proof suites (test_proof,
// test_proof_preview). The profile lives in the repository
// (tests/fixtures/photocraft-coated-cmyk.icc, CC0-1.0 — see the LICENSE
// file beside it), so the proof tests no longer depend on a system
// Ghostscript install; PITTORE_CMYK_PROFILE overrides it for machines
// that want to proof against a real press profile instead.

#include <cstdlib>
#include <filesystem>
#include <string>

namespace pittore_test {

// Path to the CMYK proof fixture, whether or not it exists. Resolution
// order: $PITTORE_CMYK_PROFILE, then the vendored file relative to the
// source root baked in by meson, then relative to the working directory.
inline std::string cmykFixturePath() {
    if (const char* env = std::getenv("PITTORE_CMYK_PROFILE")) {
        if (*env) return std::string(env);
    }
#ifdef PITTORE_SOURCE_ROOT
    const std::string vendored = std::string(PITTORE_SOURCE_ROOT) +
                                 "/tests/fixtures/photocraft-coated-cmyk.icc";
    if (std::filesystem::exists(vendored)) return vendored;
#endif
    return "tests/fixtures/photocraft-coated-cmyk.icc";
}

inline bool hasCmykFixture() {
    return std::filesystem::exists(cmykFixturePath());
}

}  // namespace pittore_test
