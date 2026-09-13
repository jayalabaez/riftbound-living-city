#include "livingcity/core/Core.h"

#include <cstdarg>
#include <cstring>
#include <new>

namespace lc {

// ---------------------------------------------------------------- logging
namespace {

LogSink g_sink = nullptr;

void DefaultSink(LogLevel level, const char* msg) {
    const char* tag = "INFO ";
    switch (level) {
        case LogLevel::Debug: tag = "DEBUG"; break;
        case LogLevel::Info:  tag = "INFO "; break;
        case LogLevel::Warn:  tag = "WARN "; break;
        case LogLevel::Error: tag = "ERROR"; break;
    }
    std::fprintf(level >= LogLevel::Warn ? stderr : stdout, "[%s] %s\n", tag, msg);
}

} // namespace

void SetLogSink(LogSink sink) { g_sink = sink; }

void Log(LogLevel level, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    (g_sink ? g_sink : &DefaultSink)(level, buf);
}

// ---------------------------------------------------------------- assertions
[[noreturn]] void FailAssert(const char* expr, const char* file, int line, const char* msg) {
    std::fprintf(stderr, "\n=== LIVING CITY ASSERTION FAILED ===\n");
    std::fprintf(stderr, "  expr : %s\n", expr ? expr : "(null)");
    std::fprintf(stderr, "  at   : %s:%d\n", file ? file : "(null)", line);
    if (msg) std::fprintf(stderr, "  msg  : %s\n", msg);
    std::fprintf(stderr, "====================================\n");
    std::fflush(stderr);
    std::abort();
}

// ---------------------------------------------------------------- allocation tracking (I5)
namespace {
AllocStats g_alloc;
} // namespace

AllocStats GetAllocStats() { return g_alloc; }
void       ResetAllocStats() { g_alloc = AllocStats{}; }

#if defined(LC_TRACK_ALLOCATIONS)
bool AllocTrackingEnabled() { return true; }
#else
bool AllocTrackingEnabled() { return false; }
#endif

namespace detail {
void NoteAlloc(std::size_t bytes) {
    ++g_alloc.allocations;
    g_alloc.bytes += bytes;
}
void NoteFree() { ++g_alloc.frees; }
} // namespace detail

} // namespace lc

// Global operator new/delete replacement is compiled ONLY into the headless and test
// binaries. Never under UBT — replacing these inside an Unreal module would fight UE's
// own allocator. Scripts/Build-Sim.ps1 defines LC_TRACK_ALLOCATIONS; LivingCitySim.Build.cs
// deliberately does not. See DECISIONS.md D-005.
#if defined(LC_TRACK_ALLOCATIONS)

void* operator new(std::size_t size) {
    if (size == 0) size = 1;
    void* p = std::malloc(size);
    if (!p) {
        std::fprintf(stderr, "out of memory allocating %zu bytes\n", size);
        std::abort();
    }
    lc::detail::NoteAlloc(size);
    return p;
}

void* operator new[](std::size_t size) { return operator new(size); }

void operator delete(void* p) noexcept {
    if (p) { lc::detail::NoteFree(); std::free(p); }
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

#endif // LC_TRACK_ALLOCATIONS
