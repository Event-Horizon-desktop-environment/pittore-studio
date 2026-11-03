#pragma once

// Resident-set reporting for import diagnostics. The project is Linux-only, so
// /proc/self/status is always the source; when it cannot be read (sandbox,
// permission) the helpers return 0 and the caller just logs a zero.

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace pittore::io::af_detail {

inline std::size_t procStatusKiB(const char* key) {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    std::size_t value = 0;
    const std::size_t keyLen = std::strlen(key);
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, key, keyLen) == 0) {
            std::sscanf(line + keyLen, " %zu", &value);
            break;
        }
    }
    std::fclose(f);
    return value;
}

// Current resident set size in MiB.
inline std::size_t rssMiB() { return procStatusKiB("VmRSS:") / 1024; }

// Peak resident set size in MiB since process start.
inline std::size_t peakRssMiB() { return procStatusKiB("VmHWM:") / 1024; }

}  // namespace pittore::io::af_detail
