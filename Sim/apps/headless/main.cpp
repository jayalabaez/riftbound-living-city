// LIVING CITY — headless simulation host.
//
// This binary is the whole point of architecture rule R1: the simulation builds and runs
// with no game engine present at all. Determinism, money conservation and replay are all
// verified here, in a process that never loads Unreal.
//
// This file also owns ALL file IO. The sim core never touches the filesystem (invariant I9);
// Replay serialises to and from byte buffers and this app moves those bytes to disk.
//
//   livingcity_headless run    --seed N --ticks N [--hash-every N] [--record FILE]
//                              [--csv FILE] [--print-final-hash] [--quiet]
//   livingcity_headless verify --log FILE [--quiet]
//   livingcity_headless bench  [--seed N] [--ticks N]
//   livingcity_headless selfcheck

#include "livingcity/sim/Sim.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace lc;

// ---------------------------------------------------------------- file IO
bool ReadFileBytes(const char* path, std::vector<u8>& out) {
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0) f = nullptr;
#else
    f = std::fopen(path, "rb");
#endif
    if (!f) return false;

    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) { std::fclose(f); return false; }

    out.resize(static_cast<std::size_t>(size));
    const std::size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

bool WriteFileBytes(const char* path, const std::vector<u8>& bytes) {
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "wb") != 0) f = nullptr;
#else
    f = std::fopen(path, "wb");
#endif
    if (!f) return false;
    const std::size_t put = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return put == bytes.size();
}

bool WriteTextFile(const char* path, const std::string& text) {
    std::vector<u8> bytes(text.begin(), text.end());
    return WriteFileBytes(path, bytes);
}

// ---------------------------------------------------------------- arg parsing
struct Args {
    const char* command       = nullptr;
    u64         seed          = 1337;
    u64         ticks         = 100000;
    u32         hashEvery     = 1000;
    const char* recordPath    = nullptr;
    const char* logPath       = nullptr;
    const char* csvPath       = nullptr;
    bool        printFinal    = false;
    bool        quiet         = false;
};

bool MatchValue(int argc, char** argv, int& i, const char* name, const char*& out) {
    if (std::strcmp(argv[i], name) == 0 && i + 1 < argc) { out = argv[++i]; return true; }
    const std::size_t n = std::strlen(name);
    if (std::strncmp(argv[i], name, n) == 0 && argv[i][n] == '=') { out = argv[i] + n + 1; return true; }
    return false;
}

bool ParseArgs(int argc, char** argv, Args& a) {
    if (argc < 2) return false;
    a.command = argv[1];

    for (int i = 2; i < argc; ++i) {
        const char* v = nullptr;
        if (MatchValue(argc, argv, i, "--seed", v))            { a.seed      = std::strtoull(v, nullptr, 10); }
        else if (MatchValue(argc, argv, i, "--ticks", v))      { a.ticks     = std::strtoull(v, nullptr, 10); }
        else if (MatchValue(argc, argv, i, "--hash-every", v)) { a.hashEvery = static_cast<u32>(std::strtoul(v, nullptr, 10)); }
        else if (MatchValue(argc, argv, i, "--record", v))     { a.recordPath = v; }
        else if (MatchValue(argc, argv, i, "--log", v))        { a.logPath    = v; }
        else if (MatchValue(argc, argv, i, "--csv", v))        { a.csvPath    = v; }
        else if (std::strcmp(argv[i], "--print-final-hash") == 0) { a.printFinal = true; }
        else if (std::strcmp(argv[i], "--quiet") == 0)            { a.quiet = true; }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

void Usage() {
    std::fprintf(stderr,
        "LIVING CITY headless simulation\n\n"
        "  run      --seed N --ticks N [--hash-every N] [--record FILE] [--csv FILE]\n"
        "           [--print-final-hash] [--quiet]\n"
        "  verify   --log FILE [--quiet]\n"
        "  bench    [--seed N] [--ticks N]\n"
        "  selfcheck\n\n"
        "exit codes: 0 = ok, 1 = failure/divergence, 2 = usage error\n");
}

// ---------------------------------------------------------------- commands
int CmdRun(const Args& a) {
    SimConfig cfg;
    cfg.worldSeed          = a.seed;
    cfg.checkpointInterval = a.hashEvery;
    cfg.recordInputLog     = true;   // always record: checkpoints are what verify compares

    Simulation sim(cfg);

    std::string csv;
    if (a.csvPath) sim.Telem().WriteCsvHeader(csv);

    const auto start = std::chrono::steady_clock::now();
    for (u64 i = 0; i < a.ticks; ++i) {
        sim.Step();
        if (a.csvPath) sim.Telem().AppendCsvRow(csv);
    }
    const auto end = std::chrono::steady_clock::now();

    const u64 elapsedNs = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    const u64 finalHash = sim.ComputeWorldHash();

    if (!a.quiet) {
        const double seconds  = static_cast<double>(elapsedNs) / 1e9;
        const double perSec   = seconds > 0.0 ? static_cast<double>(a.ticks) / seconds : 0.0;
        // 20 ticks == 1 simulated second, so realtime multiple is perSec / 20.
        std::printf("ticks        : %llu\n", static_cast<unsigned long long>(a.ticks));
        std::printf("seed         : %llu\n", static_cast<unsigned long long>(a.seed));
        std::printf("wall time    : %.3f s\n", seconds);
        std::printf("throughput   : %.0f ticks/s  (%.0fx real time)\n", perSec, perSec / 20.0);
        std::printf("final tick   : %llu\n", static_cast<unsigned long long>(sim.CurrentTick()));
        std::printf("checkpoints  : %llu\n",
                    static_cast<unsigned long long>(sim.Log().Checkpoints().size()));
        std::printf("world hash   : %016llx\n", static_cast<unsigned long long>(finalHash));
        std::printf("conservation : %s\n", sim.Accounts().ConservationHolds() ? "HOLDS" : "VIOLATED");
    }

    if (a.printFinal) {
        std::printf("%016llx\n", static_cast<unsigned long long>(finalHash));
    }

    if (!sim.Accounts().ConservationHolds()) {
        std::fprintf(stderr, "FAIL: money conservation violated (invariant I3)\n");
        return 1;
    }

    if (a.recordPath) {
        const std::vector<u8> bytes = sim.Log().Serialise();
        if (!WriteFileBytes(a.recordPath, bytes)) {
            std::fprintf(stderr, "FAIL: could not write replay log to %s\n", a.recordPath);
            return 1;
        }
        if (!a.quiet) {
            std::printf("recorded     : %s (%zu bytes)\n", a.recordPath, bytes.size());
        }
    }

    if (a.csvPath && !WriteTextFile(a.csvPath, csv)) {
        std::fprintf(stderr, "FAIL: could not write telemetry csv to %s\n", a.csvPath);
        return 1;
    }

    return 0;
}

int CmdVerify(const Args& a) {
    if (!a.logPath) { std::fprintf(stderr, "verify requires --log FILE\n"); return 2; }

    std::vector<u8> bytes;
    if (!ReadFileBytes(a.logPath, bytes)) {
        std::fprintf(stderr, "FAIL: could not read %s\n", a.logPath);
        return 1;
    }

    InputLog    recorded;
    std::string error;
    if (!InputLog::Parse(bytes, recorded, error)) {
        std::fprintf(stderr, "FAIL: could not parse replay log: %s\n", error.c_str());
        return 1;
    }

    const auto& checkpoints = recorded.Checkpoints();
    if (checkpoints.empty()) {
        std::fprintf(stderr, "FAIL: replay log contains no checkpoints to verify against\n");
        return 1;
    }

    // Re-run the same seed, feeding the recorded commands back in, and record fresh
    // checkpoints. If anything in the sim is non-deterministic, these will diverge.
    const TickIndex lastTick = checkpoints.back().tick;

    SimConfig cfg;
    cfg.worldSeed          = recorded.WorldSeed();
    cfg.checkpointInterval = checkpoints.size() > 1
        ? static_cast<u32>(checkpoints[1].tick - checkpoints[0].tick)
        : static_cast<u32>(checkpoints[0].tick);
    cfg.recordInputLog = true;

    Simulation sim(cfg);
    sim.BeginReplay(recorded);

    // Step until the simulation REACHES the last recorded tick - do not count steps from
    // zero. The world does not start at tick 0: it opens at SimConfig::startHour, so a run
    // recorded over 20,000 ticks has its last checkpoint at tick 524,000, and counting to
    // 524,000 would replay the world twenty-six times further than it was ever recorded.
    // Every checkpoint past the end of the log then compares against nothing and the harness
    // reports a divergence that is entirely its own doing.
    const TickIndex startTick = sim.CurrentTick();
    if (lastTick <= startTick) {
        std::fprintf(stderr, "FAIL: the log's last checkpoint (tick %llu) is at or before "
                             "the world's start tick (%llu) - it cannot be replayed\n",
                     static_cast<unsigned long long>(lastTick),
                     static_cast<unsigned long long>(startTick));
        return 1;
    }
    while (sim.CurrentTick() < lastTick) sim.Step();

    const Divergence d = CompareCheckpoints(recorded, sim.Log());

    if (d.diverged) {
        std::fprintf(stderr, "\n=== DETERMINISM FAILURE ===\n");
        std::fprintf(stderr, "  first divergent tick : %llu\n",
                     static_cast<unsigned long long>(d.firstDivergentTick));
        std::fprintf(stderr, "  expected hash        : %016llx\n",
                     static_cast<unsigned long long>(d.expectedHash));
        std::fprintf(stderr, "  actual hash          : %016llx\n",
                     static_cast<unsigned long long>(d.actualHash));
        std::fprintf(stderr, "  checkpoints compared : %llu\n",
                     static_cast<unsigned long long>(d.comparedCount));
        std::fprintf(stderr, "===========================\n");
        return 1;
    }

    if (!a.quiet) {
        std::printf("replay verified: %llu checkpoints over %llu ticks, no divergence\n",
                    static_cast<unsigned long long>(d.comparedCount),
                    static_cast<unsigned long long>(lastTick));
    }
    return 0;
}

int CmdBench(const Args& a) {
    SimConfig cfg;
    cfg.worldSeed          = a.seed;
    cfg.checkpointInterval = 0;      // no replay checkpoints; snapshot hashing still runs
    cfg.recordInputLog     = false;

    Simulation sim(cfg);

    // Warm up so first-touch page faults do not pollute the number.
    for (int i = 0; i < 10000; ++i) sim.Step();

    ResetAllocStats();
    const AllocStats before = GetAllocStats();

    const auto start = std::chrono::steady_clock::now();
    for (u64 i = 0; i < a.ticks; ++i) sim.Step();
    const auto end = std::chrono::steady_clock::now();

    const AllocStats after = GetAllocStats();

    const double seconds = std::chrono::duration<double>(end - start).count();
    const double perSec  = seconds > 0.0 ? static_cast<double>(a.ticks) / seconds : 0.0;

    std::printf("ticks           : %llu\n", static_cast<unsigned long long>(a.ticks));
    std::printf("wall time       : %.4f s\n", seconds);
    std::printf("throughput      : %.0f ticks/s\n", perSec);
    std::printf("realtime factor : %.0fx  (target >= 1000x)\n", perSec / 20.0);
    std::printf("ns per tick     : %.1f\n",
                perSec > 0.0 ? 1e9 / perSec : 0.0);

    if (AllocTrackingEnabled()) {
        const u64 allocs = after.allocations - before.allocations;
        std::printf("steady-state allocations : %llu %s\n",
                    static_cast<unsigned long long>(allocs),
                    allocs == 0 ? "(I5 holds)" : "(I5 VIOLATED)");
        if (allocs != 0) return 1;
    } else {
        std::printf("steady-state allocations : not tracked in this build\n");
    }

    return 0;
}

// A fast smoke test used by CI before the heavier runs.
int CmdSelfcheck() {
    SimConfig cfg;
    cfg.worldSeed          = 42;
    cfg.checkpointInterval = 100;
    cfg.recordInputLog     = true;

    Simulation a(cfg);
    Simulation b(cfg);

    for (int i = 0; i < 5000; ++i) { a.Step(); b.Step(); }

    if (a.ComputeWorldHash() != b.ComputeWorldHash()) {
        std::fprintf(stderr, "FAIL: two in-process runs with the same seed diverged\n");
        return 1;
    }
    if (!a.Accounts().ConservationHolds()) {
        std::fprintf(stderr, "FAIL: money conservation violated\n");
        return 1;
    }

    SimConfig other = cfg;
    other.worldSeed = 43;
    Simulation c(other);
    for (int i = 0; i < 5000; ++i) c.Step();

    if (a.ComputeWorldHash() == c.ComputeWorldHash()) {
        std::fprintf(stderr, "FAIL: different seeds produced the same world — "
                             "the seed is not reaching the simulation\n");
        return 1;
    }

    std::printf("selfcheck ok: same seed converges, different seed diverges, "
                "conservation holds\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!ParseArgs(argc, argv, a)) { Usage(); return 2; }

    if (std::strcmp(a.command, "run") == 0)       return CmdRun(a);
    if (std::strcmp(a.command, "verify") == 0)    return CmdVerify(a);
    if (std::strcmp(a.command, "bench") == 0)     return CmdBench(a);
    if (std::strcmp(a.command, "selfcheck") == 0) return CmdSelfcheck();

    std::fprintf(stderr, "unknown command: %s\n\n", a.command);
    Usage();
    return 2;
}
