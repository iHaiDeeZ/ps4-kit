#include "report.h"

#include <sys/stat.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

extern "C" {
int sceKernelDebugOutText(int channel, const char* text);
uint64_t sceKernelReadTsc(void);
uint64_t sceKernelGetTscFrequency(void);
}

namespace kittest {
namespace {

std::mutex g_lock;
FILE* g_file = nullptr;
std::atomic<int> g_failures{0};

void emit(const char* line) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_file) {
        mkdir("/data/ps4-kit", 0777);
        g_file = fopen("/data/ps4-kit/kit-test.log", "w");
    }
    if (g_file) {
        fputs(line, g_file);
        fputc('\n', g_file);
        fflush(g_file);  // a crash or hang must not take the lines before it along
    }
    char klog[600];
    snprintf(klog, sizeof klog, "[kit-test] %s\n", line);
    sceKernelDebugOutText(0, klog);
}

}  // namespace

void report(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    emit(line);
}

void check(bool ok, const char* name, const char* fmt, ...) {
    char detail[400], line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "%s %s: %s", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) g_failures.fetch_add(1);
    emit(line);
}

int failures() { return g_failures.load(); }

uint64_t now_us() {
    static const uint64_t freq = sceKernelGetTscFrequency();
    return static_cast<uint64_t>(static_cast<unsigned __int128>(sceKernelReadTsc()) * 1000000u / freq);
}

}  // namespace kittest
