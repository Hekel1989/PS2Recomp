#include "ps2_coverage.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    // One byte per 4-byte instruction slot of 32 MB EE RAM (8 MB, zeroed at
    // load).  A byte (not a bit) keeps the first-hit check branch-predictable
    // and the address space small enough that a direct index beats hashing.
    constexpr uint32_t kRamMask = 0x1FFFFFFFu;
    constexpr size_t kSlotCount = 0x2000000u / 4u;
    std::atomic<uint8_t> g_hitMap[kSlotCount];

    std::mutex g_hitsMutex;
    std::vector<uint32_t> g_hits;
    std::atomic<bool> g_active{false};
    std::atomic<uint64_t> g_nextFlushNs{0};
    std::atomic<uint64_t> g_vsyncTick{0};

    // Optional first-hit trace: PS2X_COVERAGE_TRACE=1 logs every newly hit
    // address with the EE vsync, optionally restricted to a window with
    // PS2X_COVERAGE_TRACE_FROM/_TO.  Unset = no output and no clock reads.
    struct CoverageTraceConfig
    {
        bool enabled = false;
        uint64_t from = 0;
        uint64_t to = UINT64_MAX;
    };

    const CoverageTraceConfig &coverageTraceConfig()
    {
        static const CoverageTraceConfig config = []
        {
            CoverageTraceConfig parsed;
            const char *env = std::getenv("PS2X_COVERAGE_TRACE");
            parsed.enabled = env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
            if (const char *from = std::getenv("PS2X_COVERAGE_TRACE_FROM"); from != nullptr)
            {
                parsed.from = std::strtoull(from, nullptr, 10);
            }
            if (const char *to = std::getenv("PS2X_COVERAGE_TRACE_TO"); to != nullptr)
            {
                parsed.to = std::strtoull(to, nullptr, 10);
            }
            return parsed;
        }();
        return config;
    }

    uint64_t steadyNowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    const char *coverageOutPath()
    {
        static const char *const path = []
        {
            const char *value = std::getenv("PS2X_COVERAGE_OUT");
            return (value != nullptr && value[0] != '\0') ? value : nullptr;
        }();
        return path;
    }
} // namespace

void ps2CoverageHit(uint32_t address)
{
    const uint32_t slot = (address & kRamMask) >> 2u;
    std::atomic<uint8_t> &flag = g_hitMap[slot];
    if (flag.load(std::memory_order_relaxed) != 0u)
    {
        return;
    }

    flag.store(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_hitsMutex);
        g_hits.push_back(address & kRamMask);
    }
    g_active.store(true, std::memory_order_relaxed);

    const CoverageTraceConfig &trace = coverageTraceConfig();
    if (trace.enabled)
    {
        const uint64_t tick = g_vsyncTick.load(std::memory_order_relaxed);
        if (tick >= trace.from && tick <= trace.to)
        {
            std::fprintf(stderr, "[coverage-hit] vsync=%llu addr=0x%06x\n",
                         static_cast<unsigned long long>(tick), address & kRamMask);
        }
    }
}

void ps2CoverageSetVSync(uint64_t tick)
{
    g_vsyncTick.store(tick, std::memory_order_relaxed);
}

void ps2CoverageFlush()
{
    if (!g_active.load(std::memory_order_relaxed))
    {
        return;
    }

    const char *path = coverageOutPath();
    if (path == nullptr)
    {
        return;
    }

    std::vector<uint32_t> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_hitsMutex);
        snapshot = g_hits;
    }
    std::sort(snapshot.begin(), snapshot.end());
    snapshot.erase(std::unique(snapshot.begin(), snapshot.end()), snapshot.end());

    // Write to a temp file and rename so a reader never sees a partial dump.
    const std::string tmp = std::string(path) + ".tmp";
    FILE *file = std::fopen(tmp.c_str(), "w");
    if (file == nullptr)
    {
        return;
    }
    for (const uint32_t address : snapshot)
    {
        std::fprintf(file, "0x%06x\n", address);
    }
    std::fclose(file);
    std::rename(tmp.c_str(), path);
}

void ps2CoveragePeriodicFlush()
{
    if (!g_active.load(std::memory_order_relaxed))
    {
        return;
    }

    constexpr uint64_t kFlushIntervalNs = 2ull * 1000 * 1000 * 1000;
    const uint64_t now = steadyNowNs();
    uint64_t expected = g_nextFlushNs.load(std::memory_order_relaxed);
    if (now < expected ||
        !g_nextFlushNs.compare_exchange_strong(expected, now + kFlushIntervalNs,
                                              std::memory_order_relaxed))
    {
        return;
    }

    ps2CoverageFlush();
}
