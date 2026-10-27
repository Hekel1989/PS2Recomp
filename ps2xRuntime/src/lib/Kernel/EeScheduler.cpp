#include "runtime/ee_scheduler.h"

#include "ps2_coverage.h"
#include "ps2_log.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_pace.h"
#include "runtime/ee_time.h"
#include "runtime/ps2_gs_host_hooks.h"
#include "runtime/ps2_diag_shares.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <unwind.h>
#include <dlfcn.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    // PS2X(dmac-dstat-mask): DMAC channels 0..9 and their D_STAT register.
    constexpr uint32_t kDmacChannelCount = 10u;
    constexpr uint32_t kDmacChannelMask = (1u << kDmacChannelCount) - 1u;
    constexpr uint32_t kDmacMaskShift = 16u;
    constexpr uint32_t kDStatAddress = 0x1000E010u;

    // Emulated-time pacing (see EeScheduler::sampleHostPacing): sample the
    // wall/CPU/stall clocks at this interval.  One clock_gettime() per sample
    // is cheap (a few hundred per second); sampling per dispatch was
    // measurable on the menu (~13% of the frame budget).
    constexpr uint64_t kPaceSamplePeriodNs = 2000000ull; // 2 ms
    // A live movie demuxes or serves a picture every frame.  If neither has
    // happened for this many guest vsyncs, the MPEG object is left over from a
    // skipped/ended movie and must not suppress debt repayment any longer.
    // Guest ticks, not wall time: excluded host stalls do not age the movie.
    constexpr uint64_t kPaceMovieIdleTicks = 120u; // 2 s
    // Checkpoints between host-clock probes once the cycle deadline has
    // passed.  A checkpoint is one backward branch (~tens of cycles), so 64
    // of them are far below a millisecond: the vDSO read stops dominating
    // the path without letting the VBlank chain drift.
    constexpr uint32_t kDeadlineProbeInterval = 64u;

    // Developer A/B switch: 0 keeps the old rule (all excluded time, including
    // OS descheduling, goes through the slow-motion debt).  The default repays
    // short descheduling immediately; see runtime/ee_pace.h.
    bool descheduleRepayEnabled() noexcept
    {
        static const bool enabled = []()
        {
            const char *value = std::getenv("PS2X_EE_DESCHED_REPAY");
            return value == nullptr || std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    // Host-only work and guest waits reported by the executor thread.  Both are
    // written and read on the same thread, so no atomics are needed.
    thread_local uint64_t t_hostStallNs = 0u;
    thread_local uint64_t t_guestWaitNs = 0u;

    // Diagnostic: PS2X_EE_SCHED=1 logs scheduler thread dispatches and guest
    // invocation push/pop with pc/sp/ra, enough to spot stack aliasing between
    // invocation stacks and thread stacks.
    bool schedTraceEnabled()
    {
        static const bool enabled = std::getenv("PS2X_EE_SCHED") != nullptr;
        return enabled;
    }

    // Diagnostic: PS2X_EE_SEMA_TRACE=1 logs semaphore create/delete and every
    // successful signal or blocking wait. Used to diagnose frame-handshake
    // deadlocks (main thread waiting for the display worker and vice versa).
    bool semaTraceEnabled()
    {
        static const bool enabled = std::getenv("PS2X_EE_SEMA_TRACE") != nullptr;
        return enabled;
    }

    // Diagnostic: PS2X_EE_THREAD_DUMP_AT=<vsync>[,<vsync>...] prints every
    // thread's PC and wait state when the vsync counter reaches each value.
    // Runs on the scheduler thread, so it works with host presentation too.
    struct EeThreadDumpConfig
    {
        std::array<uint64_t, 8> at{};
        size_t count{0};
    };

    const EeThreadDumpConfig &eeThreadDumpConfig()
    {
        static const EeThreadDumpConfig config = [] {
            EeThreadDumpConfig parsed;
            const char *env = std::getenv("PS2X_EE_THREAD_DUMP_AT");
            if (env != nullptr)
            {
                const char *p = env;
                while (*p != '\0' && parsed.count < parsed.at.size())
                {
                    char *end = nullptr;
                    const unsigned long long v = std::strtoull(p, &end, 10);
                    if (end == p)
                    {
                        ++p;
                        continue;
                    }
                    parsed.at[parsed.count++] = v;
                    p = (*end == ',') ? end + 1 : end;
                }
                std::sort(parsed.at.begin(), parsed.at.begin() + parsed.count);
            }
            return parsed;
        }();
        return config;
    }

    // Diagnostic: PS2X_EE_SIGNAL_DUMP=1 makes SIGUSR2 request the same dump
    // (with PS2X_EE_RAM_DUMP, also the RAM file) at the next scheduler pass,
    // for a hang whose vsync is not known in advance.  SIGUSR1 is the IOP's.
    std::atomic<bool> s_eeSignalDumpRequested{false};
    void onEeSignalDump(int) { s_eeSignalDumpRequested.store(true, std::memory_order_relaxed); }

    bool eeSignalDumpEnabled()
    {
        static const bool enabled = [] {
            if (std::getenv("PS2X_EE_SIGNAL_DUMP") == nullptr)
                return false;
            std::signal(SIGUSR2, onEeSignalDump);
            return true;
        }();
        return enabled;
    }

    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    // EE cycles elapsed for a host-clock duration.  Whole seconds are scaled
    // separately from the remainder so long gaps cannot overflow.
    uint64_t hostDurationToEeCycles(std::chrono::nanoseconds elapsed)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const auto wholeSeconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
        const auto remainder = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed - wholeSeconds);
        return static_cast<uint64_t>(wholeSeconds.count()) * EeScheduler::kEeClockHz +
               static_cast<uint64_t>(remainder.count()) * EeScheduler::kEeClockHz / kNanosecondsPerSecond;
    }

    // CPU time consumed by the executor thread.  Unlike steady_clock, this
    // does not advance while the OS has descheduled the thread, so host load
    // cannot inflate emulated time.
    uint64_t threadCpuNowNs()
    {
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
        timespec ts{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        {
            return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                   static_cast<uint64_t>(ts.tv_nsec);
        }
#endif
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    uint64_t steadyNowNs()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // Diagnostic: PS2X_EE_TIME_TRACE=1 prints once a second how emulated pace
    // (wall time minus reported host stalls) and unrepaid stall debt relate to
    // the guest clock.  Used to tell "the guest is executing" from "the guest
    // clock ran ahead while the host rendered".
    bool eeTimeTraceEnabled()
    {
        static const bool enabled = std::getenv("PS2X_EE_TIME_TRACE") != nullptr;
        return enabled;
    }

    // Diagnostic: PS2X_EE_VBLANK_TRACE=<start>[,<count>] prints one line per
    // VBlank with the host wall/CPU time the interval took, the VU1 fence wait
    // before the present, the present call itself and the emulated clock.  A
    // frame that misses its VBlank shows up as a wall interval of two periods
    // (or a long before/present time) at the vsync where no new picture
    // appeared.  Optional PS2X_EE_VBLANK_WATCH=<hexaddr>[,<hexaddr>...] appends
    // the guest u32 at each address (for the game's own frame counter/target).
    // Off by default; the clock reads only happen while tracing.
    struct VBlankTraceConfig
    {
        bool enabled = false;
        uint64_t start = 0u;
        uint64_t count = 0u;
        std::vector<uint32_t> watch;
    };

    const VBlankTraceConfig &vblankTraceConfig()
    {
        static const VBlankTraceConfig config = [] {
            VBlankTraceConfig parsed;
            const char *env = std::getenv("PS2X_EE_VBLANK_TRACE");
            if (env == nullptr || env[0] == '\0' || env[0] == '0')
                return parsed;
            char *end = nullptr;
            parsed.start = std::strtoull(env, &end, 10);
            parsed.count = 600u;
            if (end != nullptr && *end == ',')
                parsed.count = std::strtoull(end + 1, nullptr, 10);
            parsed.enabled = parsed.count != 0u;

            const char *watch = std::getenv("PS2X_EE_VBLANK_WATCH");
            if (watch != nullptr)
            {
                const char *p = watch;
                while (*p != '\0')
                {
                    while (*p == ',' || *p == ' ' || *p == '\t')
                        ++p;
                    if (*p == '\0')
                        break;
                    char *watchEnd = nullptr;
                    const unsigned long long address = std::strtoull(p, &watchEnd, 16);
                    if (watchEnd == p)
                    {
                        ++p;
                        continue;
                    }
                    parsed.watch.push_back(static_cast<uint32_t>(address) & 0x01FFFFFFu);
                    p = (*watchEnd == ',') ? watchEnd + 1 : watchEnd;
                }
            }
            return parsed;
        }();
        return config;
    }

    // Diagnostic: PS2X_EE_SWITCH_STATS=1 counts dispatcher transfers by reason
    // and measures how long the throw-to-catch unwind takes.  Used to decide
    // whether a fiber switch is worth implementing; no clock is read when unset.
    bool eeSwitchStatsEnabled()
    {
        static const bool enabled = std::getenv("PS2X_EE_SWITCH_STATS") != nullptr;
        return enabled;
    }

    constexpr uint32_t kTransferBlock = 0u;
    constexpr uint32_t kTransferYield = 1u;
    constexpr uint32_t kTransferExit = 2u;
    constexpr uint32_t kTransferInvocation = 3u;
    constexpr uint32_t kDispatchReturn = 4u;
    constexpr uint32_t kTraceStart = 5u;
    constexpr uint32_t kDispatchNone = 0xFFu;
    constexpr uint64_t kSwitchTraceLimit = 500000u;
    constexpr const char *kTransferNames[6] = {"block", "yield", "exit", "inv", "return", "start"};
    constexpr std::size_t kFiberStackBytes = 1u * 1024u * 1024u;

    // Developer switch: PS2X_EE_FIBERS=0 forces the old exception-based
    // transfer path for A/B and regression testing.  Enabled by default.
    bool eeFibersEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_EE_FIBERS");
            return value == nullptr || value[0] != '0' || value[1] != '\0';
        }();
        return enabled;
    }

    // Diagnostic: PS2X_EE_SWITCH_TRACE=1 logs one line per transfer decision
    // ("vsync=... from=... to=... reason=...").  Capped to keep a runaway
    // movie trace finite; the cap is far above a menu/title comparison window.
    bool eeSwitchTraceEnabled()
    {
        static const bool enabled = std::getenv("PS2X_EE_SWITCH_TRACE") != nullptr;
        return enabled;
    }

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

void ps2x::time::noteHostStall(std::chrono::nanoseconds duration) noexcept
{
    if (duration.count() > 0)
    {
        t_hostStallNs += static_cast<uint64_t>(duration.count());
    }
}

void ps2x::time::noteEmulatedWait(std::chrono::nanoseconds duration) noexcept
{
    if (duration.count() > 0)
    {
        t_guestWaitNs += static_cast<uint64_t>(duration.count());
    }
}

bool ps2x::time::descheduleRepayEnabled() noexcept
{
    return ::descheduleRepayEnabled();
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
    m_currentFiber = nullptr;
    m_fibers.clear();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    // Clear a stop request left over from an earlier run at the start of the
    // reset, not at the end: a requestStop() that arrives while the reset is
    // in progress must stay pending.  Tests that time out a wedged guest call
    // requestStop() from another thread while the scheduler thread is still
    // initialising; clearing the flag afterwards lost the stop and the join
    // never returned.
    m_stopRequested.store(false, std::memory_order_release);
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_dmacEnableCache = 0u;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_hostAnchorCycle = m_eeCycle;
    m_paceLastWallNs = m_paceLastCpuNs = m_paceLastStallNs = m_paceLastWaitNs = 0u;
    m_paceAdvanceNs = 0u;
    m_paceDebtNs = 0u;
    m_deadlineProbeCountdown = 0u;
    m_deadlineDueObserved = false;
    m_snapshotPublishCountdown = 0u;
    m_paceGuestCpuNs = m_paceWaitNs = m_paceStalledNs = m_paceDescheduledNs = 0u;
    m_paceShortDeschedNs = m_paceMovieNs = 0u;
    m_paceInitialized = false;
    m_transferStartNs = 0u;
    m_transferReason = 0u;
    m_transferActive = false;
    for (uint32_t reason = 0u; reason < 4u; ++reason)
    {
        m_switchCounts[reason] = 0u;
        m_switchNs[reason] = 0u;
        m_switchLastCounts[reason] = 0u;
        m_switchLastNs[reason] = 0u;
    }
    m_switchLastGuestNs = 0u;
    m_currentFiber = nullptr;
    m_dispatchReason = kDispatchReturn;
    m_pendingFiberFree = 0;
    m_fiberException = nullptr;
    m_fiberFunction = nullptr;
    m_fiberContext = nullptr;
    m_fibers.clear();
    m_traceFromId = 0;
    m_traceReason = kTraceStart;
    m_traceCount = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        {
            static size_t dumpNext = 0;
            const EeThreadDumpConfig &dump = eeThreadDumpConfig();
            const bool signalled = eeSignalDumpEnabled() &&
                                   s_eeSignalDumpRequested.exchange(false, std::memory_order_relaxed);
            for (bool pending = signalled;
                 pending || (dumpNext < dump.count && m_vsyncTick >= dump.at[dumpNext]);
                 pending = false)
            {
                const uint64_t requested = pending ? m_vsyncTick : dump.at[dumpNext++];
                std::cerr << "[ee-threads] vsync=" << m_vsyncTick << " requested=" << requested
                          << " running=" << m_currentThreadId << std::endl;
                for (const auto &[id, item] : m_threads)
                {
                    if (id < 0)
                    {
                        continue;
                    }
                    const R5900Context &context = item.activeContext();
                    std::cerr << "[ee-threads] tid=" << id
                              << " status=" << static_cast<int>(item.status)
                              << " wait=" << static_cast<int>(item.wait.reason)
                              << " waitid=" << waitObjectId(item.wait)
                              << " prio=" << item.currentPriority
                              << " pc=0x" << std::hex << context.pc
                              << " ra=0x" << getRegU32(&context, 31)
                              << " sp=0x" << getRegU32(&context, 29)
                              << " entry=0x" << item.entry
                              << std::dec << std::endl;
                    // PS2X_EE_THREAD_DUMP_REGS=1 adds the argument/saved
                    // registers and f20-f22, for loops that never return.
                    static const bool dumpRegs = std::getenv("PS2X_EE_THREAD_DUMP_REGS") != nullptr;
                    if (dumpRegs)
                    {
                        std::cerr << "[ee-threads] tid=" << id << " regs" << std::hex;
                        static const int kRegs[] = {2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23, 30};
                        for (const int reg : kRegs)
                        {
                            std::cerr << " r" << std::dec << reg << std::hex << "=0x" << getRegU32(&context, reg);
                        }
                        std::cerr << std::dec << " f20=" << context.f[20] << " f21=" << context.f[21]
                                  << " f22=" << context.f[22] << std::endl;
                    }
                }
                // With PS2X_EE_RAM_DUMP=<prefix>, the last requested dump also
                // writes EE RAM to <prefix>v<vsync>.bin.
                const char *ramPrefix = std::getenv("PS2X_EE_RAM_DUMP");
                if (ramPrefix != nullptr && (pending || dumpNext == dump.count) && m_rdram != nullptr)
                {
                    const std::string file = std::string(ramPrefix) + "v" + std::to_string(m_vsyncTick) + ".bin";
                    if (FILE *out = std::fopen(file.c_str(), "wb"))
                    {
                        std::fwrite(m_rdram, 1, PS2_RAM_SIZE, out);
                        std::fclose(out);
                        std::cerr << "[ee-threads] wrote " << file << std::endl;
                    }
                }
            }
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                traceSwitch(0);
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
                m_traceReason = kTransferInvocation;
                traceSwitch(owner->id);
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
                endTransfer();
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (schedTraceEnabled())
                {
                    R5900Context &parent = running->activeContext();
                    std::cerr << "[ee-sched] inv-pop tid=" << running->id
                              << " kind=" << static_cast<int>(completed.kind)
                              << " donepc=0x" << std::hex << completed.context.pc
                              << " donesp=0x" << getRegU32(&completed.context, 29)
                              << " parentpc=0x" << parent.pc
                              << " parentsp=0x" << getRegU32(&parent, 29)
                              << " parentra=0x" << getRegU32(&parent, 31)
                              << std::dec << std::endl;
                }
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                        endTransfer();
                    }
                }
                continue;
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            m_traceFromId = running->id;
            m_traceReason = kTransferInvocation;
            traceSwitch(running->id);
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }

        if (schedTraceEnabled())
        {
            std::cerr << "[ee-sched] dispatch tid=" << running->id
                      << " inv=" << running->invocations.size()
                      << " prio=" << running->currentPriority
                      << " pc=0x" << std::hex << context.pc
                      << " sp=0x" << getRegU32(&context, 29)
                      << " ra=0x" << getRegU32(&context, 31)
                      << std::dec << std::endl;
        }

        try
        {
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            if (eeFibersEnabled())
            {
                dispatchOnFiber(function, context);
            }
            else
            {
                function(m_rdram, &context, &m_runtime);
            }
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            endTransfer();
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    accountCycles(cycles);

    // Coverage dump: no-op (one relaxed load) unless the recompiled code has
    // hit a coverage marker at least once.
    ps2CoveragePeriodicFlush();

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        return true;
    }

    if (deadlineProbeDue())
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::sampleHostPacing(uint64_t nowWallNs) noexcept
{
    const uint64_t cpuNow = threadCpuNowNs();
    if (!m_paceInitialized)
    {
        // Lazy anchor for executors that never call reset() (syscall tests).
        m_paceLastWallNs = nowWallNs;
        m_paceLastCpuNs = cpuNow;
        m_paceLastStallNs = t_hostStallNs;
        m_paceLastWaitNs = t_guestWaitNs;
        m_paceAdvanceNs = 0u;
        m_paceDebtNs = 0u;
        m_paceGuestCpuNs = m_paceWaitNs = m_paceStalledNs = m_paceDescheduledNs = 0u;
        m_paceShortDeschedNs = m_paceMovieNs = 0u;
        m_paceInitialized = true;
        return;
    }

    const uint64_t wallDelta = nowWallNs - m_paceLastWallNs;
    const uint64_t cpuDelta = cpuNow - m_paceLastCpuNs;
    const uint64_t stallDelta = t_hostStallNs - m_paceLastStallNs;
    const uint64_t waitDelta = t_guestWaitNs - m_paceLastWaitNs;
    m_paceLastWallNs = nowWallNs;
    m_paceLastCpuNs = cpuNow;
    m_paceLastStallNs = t_hostStallNs;
    m_paceLastWaitNs = t_guestWaitNs;

    // Time the executor spent off-CPU without being blocked in a guest wait is
    // host contention/scheduling, not emulated time: under `taskset -c 0-1`
    // plus a PNG per vsync this was tens of ms per frame and let the guest
    // clock run ~9x its execution rate, which is what froze the post-skip
    // transition.  Guest waits (DelayThread/WaitVSync/semaphore sleeps) do
    // count, so idle threads still reach their timer deadlines.
    ps2x::time::PaceSample sample{};
    sample.wallNs = wallDelta;
    sample.cpuNs = cpuDelta;
    sample.stallNs = stallDelta;
    sample.waitNs = waitDelta;
    const bool deschedRepay = ps2x::time::descheduleRepayEnabled();
    sample.movieActive = deschedRepay
                             ? m_runtime.movieAudioActiveAt(m_vsyncTick, kPaceMovieIdleTicks)
                             : m_runtime.movieAudioActive();
    sample.repayShortDesched = deschedRepay;
    const ps2x::time::PaceStep step = ps2x::time::stepPace(sample, m_paceDebtNs);

    m_paceStalledNs += std::min(wallDelta, stallDelta);
    m_paceDescheduledNs += step.descheduledNs;
    m_paceWaitNs += waitDelta;
    m_paceGuestCpuNs += step.guestCpuNs;
    m_paceShortDeschedNs += step.shortDeschedNs;
    if (sample.movieActive)
    {
        m_paceMovieNs += wallDelta;
    }
    m_paceDebtNs = step.debtNs;
    m_paceAdvanceNs += step.advanceNs;
}

void EeScheduler::beginTransfer(uint32_t reason) noexcept
{
    if (!eeSwitchStatsEnabled())
    {
        return;
    }
    m_transferReason = reason;
    m_transferStartNs = steadyNowNs();
    m_transferActive = true;
}

void EeScheduler::endTransfer() noexcept
{
    if (!m_transferActive)
    {
        return;
    }
    m_transferActive = false;
    const uint64_t elapsed = steadyNowNs() - m_transferStartNs;
    m_switchCounts[m_transferReason] += 1u;
    m_switchNs[m_transferReason] += elapsed;
}

void EeScheduler::fiberEntryThunk(void *argument)
{
    auto *self = static_cast<EeScheduler *>(argument);
    try
    {
        self->m_fiberFunction(self->m_rdram, self->m_fiberContext, &self->m_runtime);
    }
    catch (...)
    {
        // Preserve both guest exceptions (which the old path let propagate to
        // the dispatcher) and any stray EeDispatcherTransfer for the caller.
        self->m_fiberException = std::current_exception();
    }
}

ps2x::ee::Fiber &EeScheduler::acquireFiber(int threadId)
{
    auto it = m_fibers.find(threadId);
    if (it == m_fibers.end())
    {
        it = m_fibers.emplace(threadId, std::make_unique<ps2x::ee::Fiber>("ee", kFiberStackBytes)).first;
    }
    return *it->second;
}

void EeScheduler::releaseFiber(int threadId) noexcept
{
    const auto it = m_fibers.find(threadId);
    if (it != m_fibers.end() && m_currentFiber == it->second.get())
    {
        return;
    }
    m_fibers.erase(threadId);
}

void EeScheduler::dispatchOnFiber(PS2Runtime::RecompiledFunction function, R5900Context &context)
{
    ps2x::ee::Fiber &fiber = acquireFiber(m_currentThreadId);
    m_fiberFunction = function;
    m_fiberContext = &context;
    m_fiberException = nullptr;
    m_dispatchReason = kDispatchReturn;
    m_currentFiber = &fiber;
    fiber.prepare(&EeScheduler::fiberEntryThunk, this);
    fiber.switchIn();
    m_currentFiber = nullptr;
    if (m_dispatchReason != kDispatchReturn)
    {
        endTransfer();
    }
    if (m_pendingFiberFree != 0)
    {
        const int id = m_pendingFiberFree;
        m_pendingFiberFree = 0;
        releaseFiber(id);
    }
    if (m_fiberException)
    {
        std::exception_ptr exception = m_fiberException;
        m_fiberException = nullptr;
        std::rethrow_exception(exception);
    }
}

namespace
{
    // Diagnostic: PS2X_EE_FIBER_AUDIT=1 walks the fiber's host stack at every
    // fiber transfer and reports each new chain that contains a frame with
    // C++ exception tables (a try/catch or an object with a destructor).  The
    // exception path ran those clean-ups while unwinding; the fiber path
    // abandons the stack, so every reported chain must be checked by hand.
    // Addresses are module-relative for addr2line.
    bool eeFiberAuditEnabled()
    {
        static const bool enabled = std::getenv("PS2X_EE_FIBER_AUDIT") != nullptr;
        return enabled;
    }

    struct FiberAuditWalk
    {
        uintptr_t top = 0;
        std::array<uintptr_t, 48> ips{};
        std::array<bool, 48> hasTables{};
        size_t count = 0;
    };

    _Unwind_Reason_Code fiberAuditFrame(_Unwind_Context *context, void *argument)
    {
        auto *walk = static_cast<FiberAuditWalk *>(argument);
        if (_Unwind_GetCFA(context) >= walk->top || walk->count >= walk->ips.size())
        {
            return _URC_END_OF_STACK;
        }
        walk->ips[walk->count] = _Unwind_GetIP(context);
        walk->hasTables[walk->count] = _Unwind_GetLanguageSpecificData(context) != nullptr;
        ++walk->count;
        return _URC_NO_REASON;
    }

    void auditFiberTransfer(const void *stackTop, uint32_t reason)
    {
        FiberAuditWalk walk;
        walk.top = reinterpret_cast<uintptr_t>(stackTop);
        _Unwind_Backtrace(&fiberAuditFrame, &walk);

        std::string key;
        bool anyTables = false;
        for (size_t i = 0; i < walk.count; ++i)
        {
            if (walk.hasTables[i])
            {
                anyTables = true;
                key += std::to_string(walk.ips[i]);
                key += ',';
            }
        }
        if (!anyTables)
        {
            return;
        }

        static std::mutex mutex;
        static std::set<std::string> seen;
        std::lock_guard lock(mutex);
        if (seen.size() >= 500u || !seen.insert(key).second)
        {
            return;
        }
        std::cerr << "[ee-fiber-audit] reason=" << reason << " frames:";
        for (size_t i = 0; i < walk.count; ++i)
        {
            Dl_info info{};
            uintptr_t offset = walk.ips[i];
            if (dladdr(reinterpret_cast<void *>(walk.ips[i]), &info) != 0 && info.dli_fbase != nullptr)
            {
                offset -= reinterpret_cast<uintptr_t>(info.dli_fbase);
            }
            std::cerr << ' ' << (walk.hasTables[i] ? "*" : "") << "0x" << std::hex << offset << std::dec;
        }
        std::cerr << std::endl;
    }
} // namespace

[[noreturn]] void EeScheduler::switchToDispatcher(uint32_t reason)
{
    // The dispatcher itself (a completion/onComplete callback) still uses the
    // exception path; it has no guest fiber to abandon and its stack frames
    // must unwind normally.
    if (m_currentThreadId != 0 && m_traceFromId == 0)
    {
        m_traceFromId = m_currentThreadId;
    }
    m_traceReason = reason;
    if (m_currentFiber == nullptr)
    {
        beginTransfer(reason);
        throw EeDispatcherTransfer{};
    }
    if (eeFiberAuditEnabled())
    {
        auditFiberTransfer(m_currentFiber->stackTop(), reason);
    }
    beginTransfer(reason);
    m_dispatchReason = reason;
    m_currentFiber->switchOut();
}

void EeScheduler::traceSwitch(int toThreadId) noexcept
{
    if (m_traceReason == kDispatchNone)
    {
        return;
    }
    if (!eeSwitchTraceEnabled())
    {
        m_traceReason = kDispatchNone;
        m_traceFromId = 0;
        return;
    }
    if (m_traceCount >= kSwitchTraceLimit)
    {
        if (m_traceCount == kSwitchTraceLimit)
        {
            std::fprintf(stderr, "[ee-switch-trace] truncated after %llu switches\n",
                         static_cast<unsigned long long>(kSwitchTraceLimit));
        }
        ++m_traceCount;
        m_traceReason = kDispatchNone;
        m_traceFromId = 0;
        return;
    }
    const char *reason = m_traceReason < 6u ? kTransferNames[m_traceReason] : "?";
    std::fprintf(stderr,
                 "[ee-switch-trace] vsync=%llu from=%d to=%d reason=%s\n",
                 static_cast<unsigned long long>(m_vsyncTick),
                 m_traceFromId,
                 toThreadId,
                 reason);
    ++m_traceCount;
    m_traceReason = kDispatchNone;
    m_traceFromId = 0;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    // Sample at most once per kPaceSamplePeriodNs: a CPU-time clock_gettime()
    // is a syscall (not vDSO), and paying it per dispatch cost ~13% of the
    // menu frame budget in the first version of this fix.  The sample interval
    // is far below a VBlank period, so pacing stays smooth.
    // The wall clock itself is vDSO but was still read at every checkpoint
    // after 2 ms of guest time; gate it on guest cycles instead.  Emulated
    // time is floored at wall time, so 2 ms of guest time is the same cadence
    // on a healthy host, and a delayed sample after a host stall still sees
    // the whole stall interval because the timestamp is read at sample time.
    const uint64_t nowWall = steadyNowNs();
    if (!m_paceInitialized || nowWall - m_paceLastWallNs >= kPaceSamplePeriodNs)
    {
        sampleHostPacing(nowWall);
    }

    uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;

    // Wall-clock floor from the pacing sample, capped to one VBlank period per
    // call so a large backlog is spread over checkpoints instead of landing as
    // one timer/IOP burst.
    const uint64_t target = m_hostAnchorCycle +
        hostDurationToEeCycles(std::chrono::nanoseconds(m_paceAdvanceNs));
    uint64_t catchUp = 0u;
    if (target > m_eeCycle)
    {
        catchUp = std::min<uint64_t>(target - m_eeCycle, kVBlankPeriodCycles);
        m_eeCycle += catchUp;
        elapsed += catchUp;
    }

    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    m_runtime.advanceIopEeCycles(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }

    if (eeTimeTraceEnabled())
    {
        static auto nextTrace = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextTrace)
        {
            nextTrace = now + std::chrono::seconds(1);
            const GuestThread *running = currentThread();
            std::cerr << "[ee-time] vsync=" << m_vsyncTick
                      << " eeCycle=" << m_eeCycle
                      << " paceMs=" << (m_paceAdvanceNs / 1000000ull)
                      << " debtMs=" << (m_paceDebtNs / 1000000ull)
                      << " guestMs=" << (m_paceGuestCpuNs / 1000000ull)
                      << " waitMs=" << (m_paceWaitNs / 1000000ull)
                      << " stallMs=" << (m_paceStalledNs / 1000000ull)
                      << " deschedMs=" << (m_paceDescheduledNs / 1000000ull)
                      << " shortMs=" << (m_paceShortDeschedNs / 1000000ull)
                      << " movieMs=" << (m_paceMovieNs / 1000000ull)
                      << " pc=0x" << std::hex
                      << (running != nullptr ? running->activeContext().pc : 0u)
                      << std::dec << std::endl;
        }
    }
    if (eeSwitchStatsEnabled())
    {
        static auto nextSwitchTrace = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextSwitchTrace)
        {
            nextSwitchTrace = now + std::chrono::seconds(1);
            const uint64_t guestDelta = m_paceGuestCpuNs - m_switchLastGuestNs;
            m_switchLastGuestNs = m_paceGuestCpuNs;
            uint64_t totalCount = 0u;
            uint64_t totalNs = 0u;
            std::cerr << "[ee-switch] vsync=" << m_vsyncTick;
            for (uint32_t reason = 0u; reason < 4u; ++reason)
            {
                const uint64_t count = m_switchCounts[reason] - m_switchLastCounts[reason];
                const uint64_t ns = m_switchNs[reason] - m_switchLastNs[reason];
                m_switchLastCounts[reason] = m_switchCounts[reason];
                m_switchLastNs[reason] = m_switchNs[reason];
                totalCount += count;
                totalNs += ns;
                std::cerr << ' ' << kTransferNames[reason] << '=' << count
                          << " " << kTransferNames[reason] << "Us="
                          << (count != 0u ? ns / count / 1000u : 0u);
            }
            std::cerr << " n=" << totalCount
                      << " guardUs=" << (totalCount != 0u ? totalNs / totalCount / 1000u : 0u)
                      << " savedMs=" << (totalNs / 1000000u)
                      << " guestMs=" << (guestDelta / 1000000u)
                      << " pct=" << (guestDelta != 0u ? (totalNs * 100u / guestDelta) : 0u);
            static const bool fiberStats = std::getenv("PS2X_EE_FIBER_STATS") != nullptr;
            if (fiberStats)
            {
                std::size_t peak = 0u;
                for (const auto &[id, fiber] : m_fibers)
                {
                    (void)id;
                    if (fiber)
                    {
                        peak = std::max(peak, fiber->peakStackBytes());
                    }
                }
                std::cerr << " fiberPeakKb=" << (peak / 1024u);
            }
            std::cerr << std::endl;
        }
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < 1 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    releaseFiber(id);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    m_traceFromId = id;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
        // The fiber currently holds this host stack; the dispatcher frees it
        // after the switch, never from inside the fiber.
        if (m_currentFiber != nullptr)
        {
            m_pendingFiberFree = id;
        }
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    switchToDispatcher(kTransferExit);
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (schedTraceEnabled())
    {
        std::cerr << "[ee-sched] wakeup caller=" << m_currentThreadId << " target=" << id << std::endl;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (schedTraceEnabled())
    {
        std::cerr << "[ee-sched] setprio tid=" << id << " " << oldPriority << " -> " << priority << std::endl;
    }
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        m_traceFromId = self->id;
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    switchToDispatcher(kTransferYield);
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    if (schedTraceEnabled())
    {
        std::cerr << "[ee-sched] createSema id=" << id << " init=" << initCount
                  << " max=" << maxCount << std::endl;
    }
    if (semaTraceEnabled())
    {
        std::cerr << "[ee-sema] create id=" << id << " init=" << initCount
                  << " max=" << maxCount << std::endl;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    if (semaTraceEnabled())
    {
        std::cerr << "[ee-sema] delete id=" << id << " waiters=" << waiters.size() << std::endl;
    }
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        if (semaTraceEnabled())
        {
            std::cerr << "[ee-sema] signal id=" << id << " wake=" << waiterId << std::endl;
        }
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    if (schedTraceEnabled())
    {
        std::cerr << "[ee-sched] signalSema id=" << id << " count=" << object->count + 1 << std::endl;
    }
    if (semaTraceEnabled())
    {
        std::cerr << "[ee-sema] signal id=" << id << " count=" << object->count + 1 << std::endl;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    if (schedTraceEnabled())
    {
        std::cerr << "[ee-sched] pollSema id=" << id << " count=" << object->count - 1 << std::endl;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        if (schedTraceEnabled())
        {
            std::cerr << "[ee-sched] waitSema-ok id=" << id << " count=" << object->count << std::endl;
        }
        if (semaTraceEnabled())
        {
            std::cerr << "[ee-sema] wait-ok id=" << id << " count=" << object->count << std::endl;
        }
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (semaTraceEnabled())
    {
        std::cerr << "[ee-sema] wait-block id=" << id << " tid=" << self->id << std::endl;
    }
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    if (schedTraceEnabled())
    {
        std::cerr << "[ee-sched] inv-queue kind=" << static_cast<int>(invocation.kind)
                  << " pc=0x" << std::hex << invocation.context.pc
                  << " a0=0x" << getRegU32(&invocation.context, 4) << std::dec << std::endl;
    }
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    if (schedTraceEnabled())
    {
        std::cerr << "[ee-sched] inv-push tid=" << owner->id
                  << " depth=" << owner->invocations.size()
                  << " kind=" << static_cast<int>(invocation.kind)
                  << " pc=0x" << std::hex << invocation.context.pc
                  << " sp=0x" << getRegU32(&invocation.context, 29)
                  << " ra=0x" << getRegU32(&invocation.context, 31)
                  << " a0=0x" << getRegU32(&invocation.context, 4)
                  << std::dec << std::endl;
    }
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    m_traceFromId = owner->id;
    switchToDispatcher(kTransferInvocation);
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    m_traceFromId = owner->id;
    switchToDispatcher(kTransferInvocation);
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    if (schedTraceEnabled())
    {
        std::cerr << "[ee-sched] addIrq dmac=" << (dmac ? 1 : 0) << " cause=" << cause
                  << " handler=0x" << std::hex << handler << std::dec << " id=" << id << std::endl;
    }
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        if (dmac && cause < kDmacChannelCount)
        {
            // PS2X(dmac-dstat-mask): the channel masks live in the hardware
            // D_STAT register (CIM, bits 16..25), which is also what games
            // read and toggle directly. Enable/DisableDmac flip the bit only
            // when it differs from the request (D_STAT mask bits toggle on
            // write-one), and DisableDmac returns the previous state. Keeping
            // a private mask instead let the two views drift apart: Suikoden
            // Tactics re-enables VIF1/GIF only when it *reads* the D_STAT
            // mask as off (0x11bce8), so after its movie player's
            // EnableDmac/DisableDmac pair the private mask stayed off while
            // D_STAT said on, VIF1 completions were never delivered, and the
            // game's DMA queue never drained (no title screen).
            PS2Memory &memory = m_runtime.memory();
            const uint32_t maskBit = 1u << (kDmacMaskShift + cause);
            const bool wasOn = (memory.readIORegister(kDStatAddress) & maskBit) != 0u;
            if (enabled != wasOn)
            {
                memory.writeIORegister(kDStatAddress, maskBit);
            }
            return (!enabled && wasOn) ? 1 : 0;
        }
        if (dmac)
        {
            // The Sony kernel guards EnableDmac/DisableDmac with a software
            // cache of the channel-enable state and only writes the D_STAT
            // mask bit when the requested state differs from the cached one
            // (same pattern as the dmac_mask guard in Linux' PS2 irq driver).
            // A game that enables DMAC IRQs via direct D_STAT MMIO writes and
            // then calls DisableDmac for a channel the kernel never enabled
            // therefore keeps its IRQs on real hardware; model that here.
            const uint32_t bit = 1u << cause;
            if (enabled)
            {
                if ((m_dmacEnableCache & bit) == 0u)
                {
                    m_dmacEnableCache |= bit;
                    m_enabledDmacMask |= bit;
                }
            }
            else if ((m_dmacEnableCache & bit) != 0u)
            {
                m_dmacEnableCache &= ~bit;
                m_enabledDmacMask &= ~bit;
            }
        }
        else
        {
            uint32_t &mask = m_enabledIntcMask;
            if (enabled)
            {
                mask |= 1u << cause;
            }
            else
            {
                mask &= ~(1u << cause);
            }
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (dmac && cause < kDmacChannelCount)
    {
        // PS2X(dmac-dstat-mask): D_STAT CIM is authoritative once a guest (or
        // EnableDmac/DisableDmac) has programmed a channel mask bit.  A fresh
        // memory image has CIM zero, which on hardware means every channel is
        // masked, but the scheduler has always started with all channels
        // enabled; keep that default until the masks are actually programmed.
        // Without the fallback, a DMAC dispatch after reset was dropped: the
        // handler never ran, the waiting semaphore was never signalled, and a
        // synchronous scheduler run had no stop request to end it.
        PS2Memory &memory = m_runtime.memory();
        if (memory.dmacChannelMasksProgrammed())
        {
            const uint32_t cim =
                (memory.readIORegister(kDStatAddress) >> kDmacMaskShift) & kDmacChannelMask;
            mask = (mask & ~kDmacChannelMask) | cim;
        }
    }
    if (schedTraceEnabled())
    {
        std::cerr << "[ee-sched] irq-fire dmac=" << (dmac ? 1 : 0) << " cause=" << cause
                  << " mask=0x" << std::hex << mask << std::dec << std::endl;
    }
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    if (schedTraceEnabled() && !matching.empty())
    {
        std::cerr << "[ee-sched] dispatchIrq dmac=" << (dmac ? 1 : 0) << " cause=" << cause
                  << " handlers=" << matching.size() << std::endl;
    }
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

[[noreturn]] void EeScheduler::delayCurrentThread(uint64_t eeCycles, std::function<void(R5900Context &)> completion)
{
    assertExecutor();
    // Internal one-shot waits use a dedicated external wait type and a unique
    // token so concurrent delays never wake each other.
    constexpr uint32_t kInternalDelayWaitType = 0x44656C00u;
    const uint64_t cycles = std::max<uint64_t>(1u, eeCycles);
    const uint64_t token = ++m_internalDelayToken;
    scheduleEvent(m_eeCycle + cycles,
                  std::chrono::steady_clock::now() + eeCyclesToHostDuration(cycles),
                  EeEvent{EeEventType::ExternalWake, kInternalDelayWaitType, token});
    blockCurrent(EeWaitState{EeWaitReason::External,
                             EeExternalWait{kInternalDelayWaitType, token},
                             std::move(completion)});
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::shared_ptr<const EeKernelSnapshot> held;
    {
        std::lock_guard lock(m_snapshotMutex);
        held = m_snapshot;
    }
    return held ? *held : EeKernelSnapshot{};
}

void EeScheduler::publishSnapshot()
{
    // The debugger samples at frame rate.  Rebuilding the whole thread table
    // on every semaphore signal/poll (thousands per second on the title
    // renderer) cost ~8% of the title CPU on the Legion; one publish per 32
    // operations is still far fresher than the panel can show.
    if (m_snapshotPublishCountdown != 0u)
    {
        --m_snapshotPublishCountdown;
        return;
    }
    m_snapshotPublishCountdown = 31u;
    publishSnapshotNow();
}

void EeScheduler::publishSnapshotNow()
{
    auto next = std::make_shared<EeKernelSnapshot>();
    next->sequence = ++m_snapshotSequence;
    next->eeCycle = m_eeCycle;
    next->sliceEndCycle = m_sliceEndCycle;
    next->nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next->runningThreadId = m_currentThreadId;
    next->threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next->threads.push_back(snapshot);
    }
    std::sort(next->threads.begin(), next->threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next->semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next->semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next->semaphores.begin(), next->semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next->eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next->eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next->eventFlags.begin(), next->eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
    traceSwitch(item.id);
}

void EeScheduler::makeDormant(GuestThread &item)
{
    if (schedTraceEnabled())
    {
        std::cerr << "[ee-sched] dormant tid=" << item.id
                  << " pc=0x" << std::hex << item.activeContext().pc
                  << " ra=0x" << getRegU32(&item.activeContext(), 31)
                  << std::dec << std::endl;
    }
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (schedTraceEnabled())
    {
        const char *reason = "?";
        switch (wait.reason)
        {
        case EeWaitReason::Sleep: reason = "Sleep"; break;
        case EeWaitReason::Semaphore: reason = "Semaphore"; break;
        case EeWaitReason::EventFlag: reason = "EventFlag"; break;
        case EeWaitReason::VSync: reason = "VSync"; break;
        case EeWaitReason::External: reason = "External"; break;
        case EeWaitReason::Mpeg: reason = "Mpeg"; break;
        default: break;
        }
        int detail = -1;
        if (auto *sema = std::get_if<EeSemaphoreWait>(&wait.payload))
        {
            detail = sema->id;
        }
        std::cerr << "[ee-sched] block tid=" << self->id << " reason=" << reason
                  << " id=" << detail << std::endl;
    }
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_traceFromId = self->id;
    m_currentThreadId = 0;
    publishSnapshot();
    switchToDispatcher(kTransferBlock);
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const bool cycleEventDue = deadlineProbeDue();
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

bool EeScheduler::deadlineProbeDue() noexcept
{
    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle == 0u || m_eeCycle < nextEventCycle)
        return false;
    if (m_deadlineProbeCountdown != 0u)
    {
        --m_deadlineProbeCountdown;
        return false;
    }
    m_deadlineProbeCountdown = kDeadlineProbeInterval;
    if (steadyNowNs() < m_nextDeadlineHostNs.load(std::memory_order_acquire))
        return false;
    m_deadlineDueObserved = true;
    return true;
}

void EeScheduler::processDueDeadlines()
{
    // Most dispatches reach this with no deadline even close: avoid the mutex,
    // vector allocation and wall-clock read.  Only a checkpoint probe (or a
    // wait timeout) can have observed the host deadline pass; consume that
    // observation rather than re-reading the clock here.
    if (!m_deadlineDueObserved)
        return;
    m_deadlineDueObserved = false;

    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline)
            {
                // EE time has reached this event but its host deadline has not
                // passed yet.  Do not block the executor on it: guest threads
                // still have work to do, and the host-aware checkpoint trigger
                // re-enters here once the host deadline passes.  When no thread
                // is runnable the equivalent wait happens in waitForEvent.
                updateNextDeadline();
                return;
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        // The vsync chain is a singleton.  If EE time ran past several
        // periods at once (a host-side stall), the due batch holds every
        // missed vsync; fire only the latest start and end so the game sees
        // one catch-up frame instead of a burst that would run it faster
        // than real time.
        const auto keepLatestVSync = [&due](EeEventType type)
        {
            size_t keep = due.size();
            for (size_t i = 0; i < due.size(); ++i)
            {
                if (due[i].event.type == type)
                {
                    keep = i;
                }
            }
            if (keep == due.size())
            {
                return;
            }
            size_t write = 0;
            for (size_t i = 0; i < due.size(); ++i)
            {
                if (i != keep && due[i].event.type == type)
                {
                    continue;
                }
                if (write != i)
                {
                    due[write] = std::move(due[i]);
                }
                ++write;
            }
            due.resize(write);
        };
        keepLatestVSync(EeEventType::VBlankStart);
        keepLatestVSync(EeEventType::VBlankEnd);

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                // Keep the vsync chain anchored to the host clock.  If the
                // next start would already be due (the chain fell a full
                // period behind after a stall), skip the missed vsyncs and
                // re-anchor the chain to now instead of replaying the
                // backlog as a burst.  The end event follows its start: if
                // the start was noticed late, fire the end as soon as
                // possible rather than on the stale grid.
                const auto now = std::chrono::steady_clock::now();
                auto endHostDeadline = scheduled.hostDeadline + kVBlankDuration;
                uint64_t endCycle = scheduled.deadlineCycle + kVBlankDurationCycles;
                if (endHostDeadline <= now)
                {
                    endHostDeadline = now;
                    endCycle = m_eeCycle;
                }
                scheduleEvent(endCycle, endHostDeadline,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                auto startHostDeadline = scheduled.hostDeadline + kVBlankPeriod;
                uint64_t startCycle = scheduled.deadlineCycle + kVBlankPeriodCycles;
                if (startHostDeadline <= now)
                {
                    startHostDeadline = now + kVBlankPeriod;
                    startCycle = m_eeCycle + kVBlankPeriodCycles;
                }
                scheduleEvent(startCycle, startHostDeadline,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
                // PS2X(iop-vblank-phase): the IOP's WaitVblank* wakes at these
                // same EE cycles, so both processors see one VBlank.
                m_runtime.announceIopVblank(startCycle > m_eeCycle ? startCycle - m_eeCycle : 0u,
                                            endCycle > m_eeCycle ? endCycle - m_eeCycle : 0u,
                                            kVBlankPeriodCycles);
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
    {
        const VBlankTraceConfig &vblankTrace = vblankTraceConfig();
        const uint64_t traceTick = m_vsyncTick + 1u;
        const bool traceThisVblank = vblankTrace.enabled && traceTick >= vblankTrace.start &&
                                     traceTick < vblankTrace.start + vblankTrace.count;
        const uint64_t traceWallStart = traceThisVblank ? steadyNowNs() : 0u;
        const uint64_t traceCpuStart = traceThisVblank ? threadCpuNowNs() : 0u;
        const uint64_t tracePaceStart = traceThisVblank ? m_paceAdvanceNs : 0u;
        const uint64_t traceBeforeStart = traceWallStart;
        // The frame presented below must contain every VIF1/VU1 kick queued
        // before this VBlank (docs/vu1-thread.md).  With a register-carrying
        // vsync hook the present is queued behind that work instead of
        // waiting for it; the game cannot observe the difference.
        ps2x::Vu1Thread *vu1Thread = m_runtime.memory().vu1Thread();
        const auto &vblankHooks = Ps2GsHostHooks::hooks();
        if (vu1Thread)
        {
            if (vblankHooks.vsyncRegs)
                vu1Thread->beforeVblank();
            else
                vu1Thread->sync(ps2x::Vu1SyncReason::VBlank);
        }
        const uint64_t traceBeforeUs =
            traceThisVblank ? (steadyNowNs() - traceBeforeStart) / 1000u : 0u;
        ++m_vsyncTick;
        {
            // PS2X_EE_BURN=<us>@<vsync>: from that vsync on, spend <us> of extra
            // game-thread CPU at every VBlank, a stand-in for a slower host.
            // The largest burn that still holds 60 vsync/s is the frame-time
            // headroom (dev measurement only).
            static const std::pair<uint64_t, uint64_t> s_burn = []
            {
                const char *value = std::getenv("PS2X_EE_BURN");
                if (value == nullptr)
                    return std::pair<uint64_t, uint64_t>{0u, 0u};
                char *end = nullptr;
                const uint64_t us = std::strtoull(value, &end, 10);
                const uint64_t from = (end != nullptr && *end == '@') ? std::strtoull(end + 1, nullptr, 10) : 0u;
                return std::pair<uint64_t, uint64_t>{us, from};
            }();
            if (s_burn.first != 0u && m_vsyncTick >= s_burn.second)
            {
                const uint64_t until = threadCpuNowNs() + s_burn.first * 1000u;
                volatile uint64_t sink = 0u;
                while (threadCpuNowNs() < until)
                    sink = sink + 1u;
            }
        }
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        ps2CoverageSetVSync(m_vsyncTick);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        // PS2X(gs-phase4): tick the optional external GS backend (no-op unless
        // the host registered hooks; see runtime/ps2_gs_host_hooks.h).
        const uint64_t tracePresentStart = traceThisVblank ? steadyNowNs() : 0u;
        if (vblankHooks.vsyncRegs)
        {
            const auto regs = ps2x::Vu1Thread::captureVsyncRegs(m_runtime.memory().gs());
            if (vu1Thread)
                vu1Thread->vblankPresent(m_vsyncTick, regs);
            else
                vblankHooks.vsyncRegs(m_vsyncTick, regs);
        }
        else if (vblankHooks.vsync)
        {
            vblankHooks.vsync(m_vsyncTick);
        }
        const uint64_t tracePresentUs =
            traceThisVblank ? (steadyNowNs() - tracePresentStart) / 1000u : 0u;
        if (ps2x::diag::gsSyncEnabled())
            ps2x::diag::gsEvent(m_vsyncTick, "present");
        if (ps2x::Vu1Thread *vu1Thread = m_runtime.memory().vu1Thread())
            vu1Thread->onVsync(m_vsyncTick);
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        if (traceThisVblank)
        {
            static uint64_t lastWallNs = 0u;
            static uint64_t lastCpuNs = 0u;
            static uint64_t lastCycle = 0u;
            static uint64_t lastPaceNs = 0u;
            const uint64_t endWallNs = steadyNowNs();
            const uint64_t endCpuNs = threadCpuNowNs();
            const uint64_t wallUs = lastWallNs != 0u ? (traceWallStart - lastWallNs) / 1000u : 0u;
            const uint64_t cpuUs = lastWallNs != 0u ? (traceCpuStart - lastCpuNs) / 1000u : 0u;
            const uint64_t offCpuUs = wallUs > cpuUs ? wallUs - cpuUs : 0u;
            std::fprintf(stderr,
                         "[vblank-trace] vsync=%llu wall_us=%llu cpu_us=%llu offcpu_us=%llu"
                         " ee=%llu dcycle=%llu before_us=%llu present_us=%llu total_us=%llu"
                         " pace_us=%llu debt_us=%llu",
                         static_cast<unsigned long long>(traceTick),
                         static_cast<unsigned long long>(wallUs),
                         static_cast<unsigned long long>(cpuUs),
                         static_cast<unsigned long long>(offCpuUs),
                         static_cast<unsigned long long>(m_eeCycle),
                         static_cast<unsigned long long>(lastWallNs != 0u ? m_eeCycle - lastCycle : 0u),
                         static_cast<unsigned long long>(traceBeforeUs),
                         static_cast<unsigned long long>(tracePresentUs),
                         static_cast<unsigned long long>((endWallNs - traceWallStart) / 1000u),
                         static_cast<unsigned long long>(
                             lastWallNs != 0u ? (m_paceAdvanceNs - lastPaceNs) / 1000u : 0u),
                         static_cast<unsigned long long>(m_paceDebtNs / 1000u));
            const GuestThread *vblankRunning = currentThread();
            std::fprintf(stderr, " pc=0x%x",
                         vblankRunning != nullptr ? vblankRunning->activeContext().pc : 0u);
            for (const uint32_t address : vblankTrace.watch)
            {
                uint32_t value = 0u;
                if (m_rdram != nullptr && address + sizeof(value) <= PS2_RAM_SIZE)
                    std::memcpy(&value, m_rdram + address, sizeof(value));
                std::fprintf(stderr, " w%06x=%u", address, value);
            }
            std::fprintf(stderr, "\n");
            lastWallNs = traceWallStart;
            lastCpuNs = traceCpuStart;
            lastCycle = m_eeCycle;
            lastPaceNs = tracePaceStart;
        }
        break;
    }
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        const auto waitStart = std::chrono::steady_clock::now();
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        t_guestWaitNs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - waitStart)
                .count());
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    const auto waitStart = std::chrono::steady_clock::now();
    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    t_guestWaitNs += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - waitStart)
            .count());
    if (!signaled)
    {
        // Re-arm the checkpoint probe: the host clock has advanced past a
        // deadline that was still ahead at the last probe.
        m_deadlineProbeCountdown = 0u;
        m_deadlineDueObserved = true;
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        m_nextDeadlineHostNs.store(0u, std::memory_order_release);
        return;
    }
    // An event fires when both its EE deadline and its host deadline have
    // passed.  The checkpoint trigger only needs the earliest EE deadline;
    // the host deadline stays a separate lower bound, because EE time is paced
    // by wall time minus reported host stalls and is not a linear function of
    // host wall time (a stalled guest runs in slow motion).
    const ScheduledEvent *best = nullptr;
    for (const ScheduledEvent &item : m_deadlines)
    {
        if (best == nullptr || item.deadlineCycle < best->deadlineCycle ||
            (item.deadlineCycle == best->deadlineCycle &&
             (item.hostDeadline < best->hostDeadline ||
              (item.hostDeadline == best->hostDeadline && item.sequence < best->sequence))))
        {
            best = &item;
        }
    }
    m_nextDeadlineCycle.store(best->deadlineCycle, std::memory_order_release);
    m_nextDeadlineHostNs.store(static_cast<uint64_t>(
                                   std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       best->hostDeadline.time_since_epoch())
                                       .count()),
                               std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}
