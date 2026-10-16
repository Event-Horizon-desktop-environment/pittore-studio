#pragma once

// Tiny engine logger, no Qt.
// PITTORE_LOG always writes (stderr + file). Logging stays on in alpha;
// PITTORE_DEBUG=0 silences verbose lines, anything else (or unset) logs.
// Lines starting with [tag] fan out to per-category files under
// <ConfigLocation>/PittoreStudio/log (or PITTORE_LOG_DIR): paint/brush/dab/
// erase/stroke -> Paint, render/flush/region/composite/rebuild/TIMING ->
// Render, gpu/cuda/hip/transfer/upload/download/buffer -> GPU,
// prefs/settings/ui/app/env/build/theme -> UX.
// Everything else lands in pittore_debug.log.

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace pittore::core::log {

// New PITTORE_* env wins; old INFINITY_* still honoured for migration.
inline const char* pittoreEnv(const char* pittoreKey, const char* legacyKey) {
    const char* e = std::getenv(pittoreKey);
    if (e && *e) return e;
    return std::getenv(legacyKey);
}

// Steady-clock ms since first call. Per-session timing anchor, not wall time.
inline std::uint64_t epoch_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count());
}

inline bool enabled() {
    static const bool on = [] {
        const char* e = pittoreEnv("PITTORE_DEBUG", "INFINITY_DEBUG");
        return !(e && *e && std::string(e) == "0");
    }();
    return on;
}

// Log dir, made on demand. PITTORE_LOG_DIR wins; GUI overrides via set_log_dir().
inline std::string& log_dir() {
    static std::string dir = [] {
        const char* env = pittoreEnv("PITTORE_LOG_DIR", "INFINITY_LOG_DIR");
        if (env && *env) return std::string(env);
        std::string p;
        const char* home = std::getenv("HOME");
        if (home && *home) p = std::string(home) + "/.config/PittoreStudio/log";
        else p = "PittoreStudio/log";
#ifdef _WIN32
        _mkdir(p.c_str());
#else
        std::size_t slash = p.find('/');
        while (slash != std::string::npos) {
            const std::string part = p.substr(0, slash);
            if (!part.empty()) ::mkdir(part.c_str(), 0755);
            slash = p.find('/', slash + 1);
        }
        if (!p.empty()) ::mkdir(p.c_str(), 0755);
#endif
        return p;
    }();
    return dir;
}

inline void set_log_dir(const char* d) {
    if (!d || !*d) return;
    log_dir() = std::string(d);
#ifndef _WIN32
    std::size_t slash = log_dir().find('/');
    while (slash != std::string::npos) {
        const std::string part = log_dir().substr(0, slash);
        if (!part.empty()) ::mkdir(part.c_str(), 0755);
        slash = log_dir().find('/', slash + 1);
    }
    if (!log_dir().empty()) ::mkdir(log_dir().c_str(), 0755);
#endif
}

inline std::string default_log_path() {
    return log_dir() + "/pittore_debug.log";
}

inline FILE*& file() {
    static FILE* f = nullptr;
    if (!f) f = std::fopen(default_log_path().c_str(), "a");
    return f;
}

enum class Route : unsigned char { Default, Paint, Render, Gpu, Ux };

namespace tag_tables {

struct Entry {
    const char* token;
    Route route;
};

// [tag] -> file, prefix-matched. First hit wins, specific before short.
constexpr Entry kTags[] = {
    {"transfer", Route::Gpu},
    {"download", Route::Gpu},
    {"upload", Route::Gpu},
    {"rebuild", Route::Render},
    {"composite", Route::Render},
    {"region", Route::Render},
    {"flush", Route::Render},
    {"TIMING", Route::Render},
    {"render", Route::Render},
    {"stroke", Route::Paint},
    {"brush", Route::Paint},
    {"dab", Route::Paint},
    {"erase", Route::Paint},
    {"paint", Route::Paint},
    {"gpu", Route::Gpu},
    {"cuda", Route::Gpu},
    {"hip", Route::Gpu},
    {"buffer", Route::Gpu},
    {"prefs", Route::Ux},
    {"settings", Route::Ux},
    {"theme", Route::Ux},
    {"build", Route::Ux},
    {"env", Route::Ux},
    {"app", Route::Ux},
    {"ui", Route::Ux},
    {"import", Route::Ux},
    {"export", Route::Ux},
    {"layer", Route::Ux},
};

constexpr Entry kPrefixes[] = {
    {"paint:", Route::Paint},
    {"brush:", Route::Paint},
    {"dab:", Route::Paint},
    {"erase:", Route::Paint},
    {"stroke:", Route::Paint},
    {"render:", Route::Render},
    {"flush:", Route::Render},
    {"region:", Route::Render},
    {"composite:", Route::Render},
    {"rebuild:", Route::Render},
    {"gpu:", Route::Gpu},
    {"cuda:", Route::Gpu},
    {"hip:", Route::Gpu},
    {"transfer:", Route::Gpu},
    {"upload:", Route::Gpu},
    {"download:", Route::Gpu},
    {"buffer:", Route::Gpu},
    {"prefs:", Route::Ux},
    {"settings:", Route::Ux},
    {"app:", Route::Ux},
    {"ui:", Route::Ux},
    {"env:", Route::Ux},
    {"import:", Route::Ux},
    {"export:", Route::Ux},
    {"layer:", Route::Ux},
};

}  // namespace tag_tables

inline Route route_for_tokens(const std::string_view tok, const tag_tables::Entry* table,
                              std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::string_view needle(table[i].token);
        if (tok.substr(0, needle.size()) == needle) return table[i].route;
    }
    return Route::Default;
}

inline Route route_of(const char* body) {
    if (!body || !*body) return Route::Default;
    if (body[0] == '[') {
        const char* close = std::strchr(body, ']');
        if (!close) return Route::Default;
        const std::string_view tag(body + 1, static_cast<std::size_t>(close - body - 1));
        return route_for_tokens(tag, tag_tables::kTags, std::size(tag_tables::kTags));
    }
    return route_for_tokens(body, tag_tables::kPrefixes, std::size(tag_tables::kPrefixes));
}

// File for a route, inside the log dir.
inline std::string route_log_path(Route r) {
    const char* name = "pittore_debug.log";
    switch (r) {
        case Route::Paint: name = "Pittore-Paint.log"; break;
        case Route::Render: name = "Pittore-Render.log"; break;
        case Route::Gpu: name = "Pittore-GPU.log"; break;
        case Route::Ux: name = "Pittore-UX.log"; break;
        case Route::Default: break;
    }
    return log_dir() + "/" + name;
}

// Open-on-first-use handle per route. Default reuses file().
inline FILE*& route_file_handle(Route r) {
    static FILE* paint = nullptr;
    static FILE* render = nullptr;
    static FILE* gpu = nullptr;
    static FILE* ux = nullptr;
    switch (r) {
        case Route::Paint: if (!paint) paint = std::fopen(route_log_path(r).c_str(), "a"); return paint;
        case Route::Render: if (!render) render = std::fopen(route_log_path(r).c_str(), "a"); return render;
        case Route::Gpu: if (!gpu) gpu = std::fopen(route_log_path(r).c_str(), "a"); return gpu;
        case Route::Ux: if (!ux) ux = std::fopen(route_log_path(r).c_str(), "a"); return ux;
        case Route::Default:
        default:
            return file();
    }
}

inline FILE* file_for(const char* body) { return route_file_handle(route_of(body)); }

// Drop cached handles so the next write reopens fresh.
// Lets the GUI reset logs without stale fds.
inline void reset_file() {
    if (FILE* old = file()) {
        std::fclose(old);
        file() = nullptr;
    }
}

inline void reset_route_files() {
    for (Route r : {Route::Paint, Route::Render, Route::Gpu, Route::Ux}) {
        FILE*& h = route_file_handle(r);
        if (h) {
            std::fclose(h);
            h = nullptr;
        }
    }
}

inline std::mutex& mutex() {
    static std::mutex m;
    return m;
}

// Per-dab trace gate: REMOVED — all stroke/dab/flush/region logs write
// unconditionally. Per-call cost (mutex + timestamp + file write) applies
// to fast strokes; reintroduce a gate here if paint profiling shows it.
inline bool strokeTrace() {
    return true;
}

// Transfer-trace gate: REMOVED — per-event GPU region upload/download
// logs write unconditionally (same cost caveat as strokeTrace above).
// Whole-buffer transfers are bulk traffic: one per layer per cold
// composite on the thousand-layer documents, so they use bulkTrace().
inline bool transferTrace() {
    return true;
}

// Bulk-traffic gate: whole-layer cache fills and their whole-buffer
// uploads fire once per layer per cold composite — the hundreds of
// thousands of lines the svg-scale regression pins to zero. Off unless
// PITTORE_BULK_TRACE=1; per-event stroke/region logs stay always-on.
inline bool bulkTrace() {
    static const bool on = [] {
        const char* e = pittoreEnv("PITTORE_BULK_TRACE", "INFINITY_BULK_TRACE");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    return on;
}

// Slow-event threshold (ms): region/rebuild logs are trace-gated above, but
// any event at or over this always reports with its stage breakdown, so real
// slowness in a session is attributable without log spam. Override with
// PITTORE_SLOW_EVENT_MS (default 8 = the single-digit-ms budget).
inline double slowEventMs() {
    static const double v = [] {
        const char* e = pittoreEnv("PITTORE_SLOW_EVENT_MS", "INFINITY_SLOW_EVENT_MS");
        return e && *e ? std::atof(e) : 8.0;
    }();
    return v;
}

}  // namespace pittore::core::log

// Write one line: PITTORE_LOG("[paint] dab x=%.1f", x).
// Always formatted + written (stderr + file), so timing chains survive release.
#define PITTORE_LOG(fmt, ...)                                                        \
    do {                                                                              \
        std::lock_guard<std::mutex> lk_(::pittore::core::log::mutex());              \
        const auto now_ = std::chrono::system_clock::now();                            \
        const auto t_ = std::chrono::system_clock::to_time_t(now_);                    \
        std::tm tmv_;                                                                 \
        ::localtime_r(&t_, &tmv_);                                                    \
        char ts_[32];                                                                 \
        std::snprintf(ts_, sizeof(ts_), "%02d:%02d:%02d.%03d", tmv_.tm_hour,          \
                      tmv_.tm_min, tmv_.tm_sec,                                       \
                      static_cast<int>(std::chrono::duration_cast<                     \
                                           std::chrono::milliseconds>(                 \
                                           now_.time_since_epoch())                    \
                                           .count() %                                  \
                                       1000));                                         \
        std::fprintf(stderr, "[core %s ms=%llu] " fmt "\n", ts_,                      \
                     static_cast<unsigned long long>(::pittore::core::log::epoch_ms()), \
                     ##__VA_ARGS__);                                                   \
        if (FILE* f_ = ::pittore::core::log::file_for(fmt); f_)                       \
            std::fprintf(f_, "[core %s ms=%llu] " fmt "\n", ts_,                      \
                         static_cast<unsigned long long>(                              \
                             ::pittore::core::log::epoch_ms()),                       \
                         ##__VA_ARGS__);                                               \
    } while (0)

// Always-on error log. Backend/render failures must show on default runs.
namespace pittore::core::log {

inline void log_error(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk_(::pittore::core::log::mutex());
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[core ERROR ms=%llu] %s\n",
                 static_cast<unsigned long long>(epoch_ms()), buf);
    std::fflush(stderr);
    if (FILE* f_ = ::pittore::core::log::file_for(buf); f_) {
        std::fprintf(f_, "[core ERROR ms=%llu] %s\n",
                     static_cast<unsigned long long>(epoch_ms()), buf);
        std::fflush(f_);
    }
}

inline void log_warning(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk_(::pittore::core::log::mutex());
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[core WARN ms=%llu] %s\n",
                 static_cast<unsigned long long>(epoch_ms()), buf);
    std::fflush(stderr);
    if (FILE* f_ = ::pittore::core::log::file_for(buf); f_) {
        std::fprintf(f_, "[core WARN ms=%llu] %s\n",
                     static_cast<unsigned long long>(epoch_ms()), buf);
        std::fflush(f_);
    }
}

inline void log_info(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk_(::pittore::core::log::mutex());
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[core INFO ms=%llu] %s\n",
                 static_cast<unsigned long long>(epoch_ms()), buf);
    std::fflush(stderr);
    if (FILE* f_ = ::pittore::core::log::file_for(buf); f_) {
        std::fprintf(f_, "[core INFO ms=%llu] %s\n",
                     static_cast<unsigned long long>(epoch_ms()), buf);
        std::fflush(f_);
    }
}

}  // namespace pittore::core::log