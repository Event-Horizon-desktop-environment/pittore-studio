#include "ui/tools/log/tool_log.h"

#include <QDateTime>
#include <QDir>
#include <QString>

#include <atomic>
#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <unordered_map>

#ifndef _WIN32
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "engine/core/log.h"
#include "ui/logging.h"
#include "ui/tools/defs/tool_defs.h"

namespace pittore::ui {
namespace {

// ---------------------------------------------------------------------------
// Handles. One open file per tool, opened on first use and flushed per line:
// the whole point is that the tail of a tool's log survives a crash.
// ---------------------------------------------------------------------------
std::mutex& toolMutex() {
    static std::mutex m;
    return m;
}

// Keyed by ToolId. A failed open is cached as null too, so a read-only config
// directory cannot turn every line into a fresh fopen() attempt.
std::unordered_map<int, FILE*>& toolFiles() {
    static std::unordered_map<int, FILE*> files;
    return files;
}

FILE*& rollupFile() {
    static FILE* f = nullptr;
    return f;
}

std::string toolsDir() {
    return log_dir().toStdString() + "/tools";
}

std::string crashPath() { return log_dir().toStdString() + "/Pittore-Crash.log"; }

// "Rectangular Marquee" -> "rectangular-marquee"; digits and letters survive,
// everything else collapses to one dash so the filename stays shell-friendly.
std::string slugFor(ToolId id) {
    std::string out;
    for (const char* p = toolDef(id).name; p && *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c >= '0' && c <= '9') {
            out.push_back(static_cast<char>(c));
        } else if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (c >= 'a' && c <= 'z') {
            out.push_back(static_cast<char>(c));
        } else if (!out.empty() && out.back() != '-') {
            out.push_back('-');
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) out = "tool";
    return out;
}

// tools/006-lasso.log — the id prefix keeps the directory sortable by id and
// makes two same-slug tools (there are none today) unambiguous anyway.
std::string pathFor(ToolId id) {
    char prefix[8];
    std::snprintf(prefix, sizeof prefix, "%03d", static_cast<int>(id));
    return toolsDir() + "/" + prefix + "-" + slugFor(id) + ".log";
}

FILE* openToolFile(ToolId id) {
    auto& files = toolFiles();
    const int key = static_cast<int>(id);
    const auto it = files.find(key);
    if (it != files.end()) return it->second;

    const std::string path = pathFor(id);
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos)
        QDir().mkpath(QString::fromStdString(path.substr(0, slash)));
    FILE* f = std::fopen(path.c_str(), "a");
    files.emplace(key, f);
    return f;
}

FILE* openRollup() {
    FILE*& f = rollupFile();
    if (!f) f = std::fopen((log_dir().toStdString() + "/Pittore-Tools.log").c_str(), "a");
    return f;
}

void writeFlush(FILE* f, const std::string& line) {
    if (!f) return;
    std::fwrite(line.data(), 1, line.size(), f);
    std::fputc('\n', f);
    std::fflush(f);  // never let a crash eat the tail of a tool's log
}

const char* sectionName(ToolSection s) {
    switch (s) {
        case ToolSection::Selection: return "Selection";
        case ToolSection::CropMeasure: return "CropMeasure";
        case ToolSection::RetouchPaint: return "RetouchPaint";
        case ToolSection::DrawType: return "DrawType";
        case ToolSection::Navigation: return "Navigation";
        case ToolSection::Generative: return "Generative";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Crash breadcrumb. Plain fixed buffers + one atomic id: the signal handler
// may only read these, never format a QString or take the log mutex.
// Written by every log line, so at any instant they name the last thing the
// app was doing with a tool.
// ---------------------------------------------------------------------------
std::atomic<int> g_crashTool{-1};
std::atomic<bool> g_crashing{false};
char g_crashToolName[64] = "(no tool yet)";
char g_crashStage[192] = "startup";
char g_crashPath[512] = "";
int g_crashFd = -1;

}  // namespace

void tool_log_breadcrumb(ToolId id, const char* stage) {
    if (g_crashTool.load(std::memory_order_relaxed) != static_cast<int>(id)) {
        // The slow, allocating part — never reached from the handler itself,
        // which only ever reads what this wrote.
        const std::string path = pathFor(id);
        std::snprintf(g_crashToolName, sizeof g_crashToolName, "%s", toolDef(id).name);
        std::snprintf(g_crashPath, sizeof g_crashPath, "%s", path.c_str());
        g_crashTool.store(static_cast<int>(id), std::memory_order_release);
    }
    std::snprintf(g_crashStage, sizeof g_crashStage, "%s", stage ? stage : "(none)");
}

namespace {

void emitLine(ToolId id, const char* stage, const char* detail) {
    tool_log_breadcrumb(id, stage);
    if (!logging_debug()) return;  // PITTORE_DEBUG=0: breadcrumb stays, lines go

    const QByteArray ts = QDateTime::currentDateTime()
                              .toString(QStringLiteral("HH:mm:ss.zzz"))
                              .toUtf8();
    const std::string ms = std::to_string(::pittore::core::log::epoch_ms());
    const std::string body =
        std::string(stage ? stage : "(none)") +
        (detail && *detail ? std::string(": ") + detail : std::string());

    // Per-tool file gets the bare stage; the rollup and stderr also name the
    // tool, so the console stays readable when several tools log at once.
    const std::string head =
        std::string("[tool ") + ts.constData() + " ms=" + ms + "] ";
    const std::string mine = head + body;
    const std::string ours = head + "[" + toolDef(id).name + "] " + body;

    std::lock_guard<std::mutex> lk(toolMutex());
    writeFlush(openToolFile(id), mine);
    writeFlush(openRollup(), ours);
    std::fwrite(ours.data(), 1, ours.size(), stderr);
    std::fputc('\n', stderr);
}

// ---------------------------------------------------------------------------
// Crash dump. Only write() and snprintf on a static buffer: no malloc, no Qt,
// no locks. The breadcrumb is written first, so even a handler that trips over
// the backtrace has already named the guilty tool + stage on disk.
// ---------------------------------------------------------------------------
const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGBUS: return "SIGBUS (bad memory access)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGFPE: return "SIGFPE (arithmetic fault)";
        case SIGILL: return "SIGILL (illegal instruction)";
        default: return "unrecognised fatal signal";
    }
}

#ifndef _WIN32
void emitBytes(int fd, const void* buf, std::size_t n) {
    // write() is warn_unused_result, but a crashing process cannot act on
    // the outcome: consume the return value and keep the dump best effort.
    [[maybe_unused]] const ssize_t unused = ::write(fd, buf, n);
}
#endif

void writeCrash(const char* header) {
    char buf[1536];
    int n = std::snprintf(
        buf, sizeof buf,
        "\n[fatal] %s\n"
        "  tool:     %s (id=%d)\n"
        "  stage:    %s\n"
        "  tool log: %s\n"
        "  uptime:   %llu ms\n"
        "  next:     the last line of that tool log is the stage that was running.\n",
        header, g_crashToolName, g_crashTool.load(), g_crashStage, g_crashPath,
        static_cast<unsigned long long>(::pittore::core::log::epoch_ms()));
    if (n < 0) return;
    if (n > static_cast<int>(sizeof buf)) n = static_cast<int>(sizeof buf);
#ifndef _WIN32
    const int fd = g_crashFd >= 0 ? g_crashFd : 2;
    emitBytes(fd, buf, static_cast<std::size_t>(n));
    if (fd != 2) {
        emitBytes(2, buf, static_cast<std::size_t>(n));
    }
#else
    std::fwrite(buf, 1, static_cast<std::size_t>(n), stderr);
#endif
}

void writeBacktrace() {
#ifndef _WIN32
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    if (n <= 0) return;
    const int fd = g_crashFd >= 0 ? g_crashFd : 2;
    static const char kTag[] = "\n  backtrace (innermost frame first):\n";
    emitBytes(fd, kTag, sizeof kTag - 1);
    ::backtrace_symbols_fd(frames, n, fd);
#endif
}

void crashHandler(int sig) {
    if (!g_crashing.exchange(true)) {
        writeCrash(signalName(sig));
        writeBacktrace();
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void terminateHandler() {
    if (!g_crashing.exchange(true)) {
        char header[768];
        std::snprintf(header, sizeof header, "std::terminate (no exception visible)");
        // Supported by the major standard libraries: the in-flight exception
        // can be rethrown here to describe it.
        try {
            throw;
        } catch (const std::exception& e) {
            std::snprintf(header, sizeof header,
                          "std::terminate: uncaught std::exception: %s", e.what());
        } catch (...) {
            std::snprintf(header, sizeof header,
                          "std::terminate: uncaught non-std exception");
        }
        writeCrash(header);
        writeBacktrace();
    }
    std::abort();
}

void openCrashFd() {
#ifndef _WIN32
    if (g_crashFd >= 0) {
        ::close(g_crashFd);
        g_crashFd = -1;
    }
    g_crashFd = ::open(crashPath().c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
#endif
}

}  // namespace

void tool_log_reset() {
    {
        std::lock_guard<std::mutex> lk(toolMutex());
        for (auto& entry : toolFiles())
            if (entry.second) std::fclose(entry.second);
        toolFiles().clear();
        if (FILE* r = rollupFile()) std::fclose(r);
        rollupFile() = nullptr;
    }
    const std::string base = log_dir().toStdString();
    QDir(QString::fromStdString(base + "/tools")).removeRecursively();
    QDir().mkpath(QString::fromStdString(base + "/tools"));
    std::remove((base + "/Pittore-Tools.log").c_str());
    std::remove(crashPath().c_str());
    if (g_crashFd >= 0) openCrashFd();  // keep the fd on the fresh file
}

void tool_log(ToolId id, const char* stage, const char* detail) {
    emitLine(id, stage, detail);
}

void tool_logf(ToolId id, const char* stage, const char* fmt, ...) {
    char buf[768];
    buf[0] = '\0';
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emitLine(id, stage, buf);
}

ToolSetupTrace::ToolSetupTrace(ToolId id, const char* reason) : id_(id) {
    const ToolDef& def = toolDef(id);
    const char key = def.key ? def.key : '-';
    tool_logf(id_, "setup/begin", "reason=%s section=%s key=%c",
              reason ? reason : "switch", sectionName(def.section), key);
}

ToolSetupTrace::~ToolSetupTrace() { tool_log(id_, "setup/end", "completed"); }

void install_tool_crash_logging() {
    static std::once_flag once;
    std::call_once(once, [] {
        openCrashFd();
#ifndef _WIN32
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_handler = crashHandler;
        sigemptyset(&sa.sa_mask);
        for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL})
            ::sigaction(sig, &sa, nullptr);
#else
        for (int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) std::signal(sig, crashHandler);
#endif
        std::set_terminate(terminateHandler);
    });
}

}  // namespace pittore::ui
