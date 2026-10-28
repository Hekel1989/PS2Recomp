#pragma once

#include "ps2_runtime.h"
#include "runtime/ee_fiber.h"

#include <array>
#include <atomic>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

// This exception is the EE equivalent of a longjmp to the dispatcher.  It is
// not an error and must only be caught at EeScheduler::run().
struct EeDispatcherTransfer final
{
};

enum class EeThreadStatus : uint8_t
{
    Running,
    Ready,
    Waiting,
    WaitingSuspended,
    Suspended,
    Dormant,
};

enum class EeWaitReason : uint8_t
{
    None,
    Sleep,
    Semaphore,
    EventFlag,
    VSync,
    External,
    Mpeg,
};

struct EeSemaphoreWait
{
    int id = 0;
};

struct EeEventFlagWait
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t mode = 0;
    uint32_t resultAddress = 0;
};

struct EeVSyncWait
{
    uint64_t afterTick = 0;
    int fixedResult = -1;
};

struct EeExternalWait
{
    uint32_t type = 0;
    uint64_t token = 0;
};

using EeWaitPayload = std::variant<std::monostate,
                                   EeSemaphoreWait,
                                   EeEventFlagWait,
                                   EeVSyncWait,
                                   EeExternalWait>;

struct EeWaitState
{
    EeWaitReason reason = EeWaitReason::None;
    EeWaitPayload payload{};
    std::function<void(R5900Context &)> completion;
};

enum class GuestInvocationKind : uint8_t
{
    Interrupt,
    Alarm,
    GsCallback,
    RpcCallback,
    SyscallOverride,
    ExitHandler,
    HleCall,
    SifCommand,
};

struct GuestInvocation
{
    GuestInvocationKind kind = GuestInvocationKind::Interrupt;
    uint64_t sequence = 0;
    uint64_t tag = 0;
    R5900Context context{};
    std::function<void(const R5900Context &, R5900Context &)> onComplete;
};

struct GuestThread
{
    int id = 0;
    R5900Context context{};
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t arg = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    bool ownsStack = false;
    uint32_t tlsBase = 0;
    EeWaitState wait{};
    std::function<void(R5900Context &)> resumeCompletion;
    std::vector<GuestInvocation> invocations;

    [[nodiscard]] R5900Context &activeContext()
    {
        return invocations.empty() ? context : invocations.back().context;
    }

    [[nodiscard]] const R5900Context &activeContext() const
    {
        return invocations.empty() ? context : invocations.back().context;
    }
};

struct EeSemaphore
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    int initCount = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    std::deque<int> waiters;
};

struct EeEventFlag
{
    int id = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t initBits = 0;
    uint32_t bits = 0;
    std::deque<int> waiters;
};

struct EeAlarm
{
    int id = 0;
    uint16_t ticks = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
};

struct EeIrqHandler
{
    int id = 0;
    uint32_t cause = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
    bool enabled = true;
    int order = 0;
};

struct EeThreadSnapshot
{
    int id = 0;
    uint32_t pc = 0;
    uint32_t ra = 0;
    uint32_t sp = 0;
    uint32_t contextGp = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    EeWaitReason waitReason = EeWaitReason::None;
    int waitId = 0;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    uint32_t invocationDepth = 0;
};

struct EeSemaphoreSnapshot
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    uint32_t waiters = 0;
};

struct EeEventFlagSnapshot
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t initBits = 0;
    uint32_t attr = 0;
    uint32_t waiters = 0;
};

struct EeKernelSnapshot
{
    uint64_t sequence = 0;
    uint64_t eeCycle = 0;
    uint64_t sliceEndCycle = 0;
    uint64_t nextEventCycle = 0;
    int runningThreadId = 0;
    std::vector<EeThreadSnapshot> threads;
    std::vector<EeSemaphoreSnapshot> semaphores;
    std::vector<EeEventFlagSnapshot> eventFlags;
};

enum class EeEventType : uint8_t
{
    Stop,
    VBlankStart,
    VBlankEnd,
    Dmac,
    ExternalWake,
    Alarm,
};

struct EeEvent
{
    EeEventType type = EeEventType::ExternalWake;
    uint32_t id = 0;
    uint64_t value = 0;
};

struct EeThreadCreateParams
{
    uint32_t attr = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int priority = 0;
    uint32_t option = 0;
};

class EeScheduler
{
public:
    static constexpr int kMainThreadId = 1;
    static constexpr int kFirstThreadId = 2;
    static constexpr int kLastThreadId = 255;
    static constexpr int kPriorityCount = 128;
    static constexpr uint64_t kEeClockHz = 294912000ull;
    static constexpr uint32_t kGeneratedCheckpointCycles = 32u;
    static constexpr uint32_t kGuestDispatchCycles = 8u;
    static constexpr uint64_t kDefaultTimeSliceCycles = 65536ull;

    explicit EeScheduler(PS2Runtime &runtime);
    ~EeScheduler();

    EeScheduler(const EeScheduler &) = delete;
    EeScheduler &operator=(const EeScheduler &) = delete;

    void reset(uint8_t *rdram, const R5900Context &mainContext);
    void run();
    void requestStop();
    void postEvent(EeEvent event);
    [[nodiscard]] bool checkpointDue(uint32_t cycles = kGeneratedCheckpointCycles) noexcept;
    void accountCycles(uint32_t cycles) noexcept;
    [[nodiscard]] bool isExecutingGuest() const noexcept;

    // Kernel object API. All calls except postEvent/requestStop execute on the
    // EE executor and therefore need no host synchronization.
    void setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp);
    int createThread(const EeThreadCreateParams &params);
    int deleteThread(int id, uint32_t &ownedStack);
    int startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe);
    [[noreturn]] void exitCurrent(bool deleteThread);
    int terminateThread(int id, uint32_t &ownedStack, bool interruptSafe);
    int suspendThread(int id, bool interruptSafe);
    int resumeThread(int id, bool interruptSafe);
    void sleepCurrent();
    int wakeupThread(int id, bool interruptSafe);
    int cancelWakeup(int id);
    int changePriority(int id, int priority, bool interruptSafe, int &oldPriority);
    int rotateReadyQueue(int priority, bool interruptSafe);
    int releaseWait(int id, bool interruptSafe);
    void transferIfRequested(bool interruptSafe);

    int createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option);
    int deleteSemaphore(int id, bool interruptSafe);
    int signalSemaphore(int id, bool interruptSafe);
    int pollSemaphore(int id);
    void waitSemaphore(int id);

    int createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option);
    int deleteEventFlag(int id, bool interruptSafe);
    int setEventFlag(int id, uint32_t bits, bool interruptSafe);
    int clearEventFlag(int id, uint32_t mask);
    int pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits);
    void waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress);

    int setAlarm(uint16_t ticks, uint32_t handler, uint32_t argument, uint32_t gp, uint32_t sp);
    int cancelAlarm(int id);
    void queueInvocation(GuestInvocation invocation);
    [[noreturn]] void invokeCurrent(GuestInvocation invocation);
    [[noreturn]] void invokeCurrentSequence(std::vector<GuestInvocation> invocations);
    [[nodiscard]] bool hasInvocation(GuestInvocationKind kind, uint64_t tag) const;
    [[nodiscard]] uint32_t invocationStackTop();

    int addIrqHandler(bool dmac,
                      uint32_t cause,
                      uint32_t handler,
                      bool append,
                      uint32_t argument,
                      uint32_t gp,
                      uint32_t sp);
    int removeIrqHandler(bool dmac, uint32_t cause, int id);
    int setIrqHandlerEnabled(bool dmac, int id, bool enabled);
    int setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled);
    void dispatchIrq(bool dmac, uint32_t cause);
    void setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress);
    [[nodiscard]] uint64_t currentVSyncTick() const noexcept;
    uint32_t setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp);

    [[noreturn]] void waitVSync(uint64_t afterTick, int fixedResult = -1, std::function<void(R5900Context &)> completion = {});
    void completeVSync(uint64_t tick);
    void completeExternalWait(uint32_t type, uint64_t token, int result);
    [[noreturn]] void waitExternal(EeWaitReason reason, uint32_t type, uint64_t token, std::function<void(R5900Context &)> completion = {});

    // Blocks the current thread for a short I/O latency (a round trip of a
    // synchronous SIF RPC, a drive seek, ...) and resumes it when the
    // deadline passes, paced by the host clock so the wait completes even
    // when no other guest thread is runnable. The completion runs on resume
    // and should set the guest return value.
    [[noreturn]] void delayCurrentThread(uint64_t eeCycles, std::function<void(R5900Context &)> completion = {});

    // PS2X(ee-idle-poll): replace "park until the next event" for a known
    // guest busy-wait with a short real sleep that keeps EE time (and so the
    // IOP) moving with wall time.  Deterministic mode returns at once.  When
    // EE time is behind the wall-clock floor this charges the catch-up and
    // returns without sleeping; otherwise it sleeps for at most the poll
    // period (PS2X_EE_IDLE_POLL_NS, default 100 us, 0 = never sleep), ending
    // early on a posted event or a stop request.  The thread stays Running.
    // Returns the nanoseconds actually slept (0 when it only charged cycles).
    uint64_t idlePoll();

    // PS2X(rpc-wait-pace): keep a blocking EE->IOP RPC on the wall-clock floor.
    // On hardware the calling EE thread sleeps while the IOP serves the call in
    // real time; here the IOP runs the server thread synchronously and the EE
    // is charged its cycles afterwards (RPC.cpp), so without this the quiet
    // skip lets milliseconds of IOP time pass in microseconds of host time.
    // `eeCyclesInCall` is the charge RPC.cpp will apply, so candidate =
    // m_eeCycle + eeCyclesInCall is where EE time will land.  Sleeps (at most
    // 1 ms per step) until candidate is within one VBlank of the pacing floor -
    // the granularity of PCSX2's frame limiter, which throttles the CPU thread
    // at each VSync when emulation is ahead of wall time.  Deterministic mode
    // and PS2X_RPC_WAIT_PACE=0 return at once.  The sleep is guest wait, not
    // descheduling, and is reported as rpcPaceMs by PS2X_EE_TIME_TRACE.
    void paceRpcWait(uint64_t eeCyclesInCall);

    // PS2X(ee-idle-poll): cumulative pacing components, for tests and
    // diagnostics.  Executor-thread state; read it from a guest callback.
    struct EePacingCounters
    {
        uint64_t guestWaitNs = 0u;
        uint64_t descheduledNs = 0u;
        uint64_t advanceNs = 0u;
    };
    [[nodiscard]] EePacingCounters pacingCounters() const noexcept;

    [[nodiscard]] GuestThread *thread(int id);
    [[nodiscard]] const GuestThread *thread(int id) const;
    [[nodiscard]] EeSemaphore *semaphore(int id);
    [[nodiscard]] const EeSemaphore *semaphore(int id) const;
    [[nodiscard]] EeEventFlag *eventFlag(int id);
    [[nodiscard]] const EeEventFlag *eventFlag(int id) const;
    [[nodiscard]] GuestThread *currentThread();
    [[nodiscard]] const GuestThread *currentThread() const;
    [[nodiscard]] int currentThreadId() const noexcept;
    [[nodiscard]] R5900Context *currentContext();
    [[nodiscard]] uint8_t *rdram() const noexcept;

    // Direct syscall tests use the same main-thread record without starting a
    // second executor. Production execution calls reset() before run().
    void bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram);

    [[nodiscard]] EeKernelSnapshot snapshot() const;
    // Rebuilds the debug view of the kernel.  This is a presentation copy, not
    // guest state: the semaphore/thread calls that used to publish on every
    // operation spent ~8% of the title CPU on vector builds and sorts.  The
    // wrapper skips most calls; publishSnapshotNow() is the unconditional one.
    void publishSnapshot();
    void publishSnapshotNow();

private:
    struct ScheduledEvent
    {
        uint64_t deadlineCycle = 0;
        std::chrono::steady_clock::time_point hostDeadline{};
        EeEvent event{};
        uint64_t sequence = 0;
    };

    void assertExecutor() const;
    [[nodiscard]] int allocateThreadId();
    GuestThread &acquireInvocationThread();
    void enqueueReady(GuestThread &thread, bool front = false);
    void removeReady(GuestThread &thread);
    [[nodiscard]] GuestThread *selectReady();
    void makeRunning(GuestThread &thread);
    void makeDormant(GuestThread &thread);
    void removeFromWaitObject(GuestThread &thread);
    [[noreturn]] void blockCurrent(EeWaitState wait);
    void makeReady(GuestThread &thread, int result, bool interruptSafe);
    void requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe);
    void applyPendingPreemption();
    void processPendingEvents();
    void processDueDeadlines();
    void processEvent(const EeEvent &event);
    void finishEventWaiters(EeEventFlag &flag, bool interruptSafe);
    [[nodiscard]] static bool eventCondition(uint32_t current, uint32_t requested, uint32_t mode);
    static int waitObjectId(const EeWaitState &wait);
    void writeGuestU32(uint32_t address, uint32_t value);
    void waitForEvent();
    void scheduleEvent(uint64_t deadlineCycle, std::chrono::steady_clock::time_point hostDeadline, EeEvent event);
    void updateNextDeadline();
    // PS2X(jitter-seed): read PS2X_JITTER_SEED and initialise the per-run
    // PRNG used by accountCycles().  Called from reset() so one run always
    // starts from the same sequence.
    void configureCycleJitter();
    [[nodiscard]] uint64_t jitterCycles(uint64_t cycles) noexcept;
    void sampleHostPacing(uint64_t nowWallNs) noexcept;
    // PS2X(ee-idle-poll): read PS2X_EE_IDLE_POLL_NS once per reset.
    void configureIdlePoll();
    // PS2X(rpc-wait-pace): read PS2X_RPC_WAIT_PACE once per reset.
    void configureRpcWaitPace();
    void beginTransfer(uint32_t reason) noexcept;
    void endTransfer() noexcept;
    void dispatchOnFiber(PS2Runtime::RecompiledFunction function, R5900Context &context);
    static void fiberEntryThunk(void *argument);
    [[nodiscard]] ps2x::ee::Fiber &acquireFiber(int threadId);
    void releaseFiber(int threadId) noexcept;
    [[noreturn]] void switchToDispatcher(uint32_t reason);
    void traceSwitch(int toThreadId) noexcept;
    [[nodiscard]] bool hasReadyAtOrAbovePriority(int priority) const;
    void renewTimeSlice();
    void copyMainContextToRuntime();
    void publishDebugContext(const R5900Context &context);
    void publishIdleDebugContext();

    PS2Runtime &m_runtime;
    uint8_t *m_rdram = nullptr;
    std::array<std::deque<int>, kPriorityCount> m_readyQueues{};
    std::unordered_map<int, GuestThread> m_threads;
    std::unordered_map<int, EeSemaphore> m_semaphores;
    std::unordered_map<int, EeEventFlag> m_eventFlags;
    std::unordered_map<int, EeAlarm> m_alarms;
    std::unordered_map<int, EeIrqHandler> m_intcHandlers;
    std::unordered_map<int, EeIrqHandler> m_dmacHandlers;
    int m_nextThreadId = kFirstThreadId;
    int m_nextInvocationThreadId = -1;
    int m_nextSemaphoreId = 1;
    int m_nextEventFlagId = 1;
    int m_nextAlarmId = 1;
    int m_nextIntcHandlerId = 1;
    int m_nextDmacHandlerId = 1;
    int m_intcHeadOrder = 0;
    int m_intcTailOrder = 1000;
    int m_dmacHeadOrder = 0;
    int m_dmacTailOrder = 1000;
    uint32_t m_enabledIntcMask = 0xFFFFFFFFu;
    uint32_t m_enabledDmacMask = 0xFFFFFFFFu;
    // Software mirror of the DMAC channel-enable state as tracked by the Sony
    // kernel's EnableDmac/DisableDmac syscalls.  The kernel only flips the
    // D_STAT mask bits when the requested state differs from this cache, so
    // direct D_STAT MMIO writes by a game (which the kernel cannot observe)
    // are not undone by a DisableDmac for a channel the kernel never enabled.
    uint32_t m_dmacEnableCache = 0u;
    int m_currentThreadId = 0;
    bool m_rescheduleRequested = false;
    bool m_timeSliceExpired = false;
    bool m_insideInterrupt = false;
    uint32_t m_pendingEeTimerInterrupts = 0;
    uint64_t m_eeCycle = 0;
    // PS2X(jitter-seed): PS2X_DETERMINISTIC=1 plus PS2X_JITTER_SEED=<n>
    // charges every accountCycles() call a seeded 75..125% of its guest
    // cycles.  The EE clock and the IOP both advance by the jittered amount,
    // so the IOP's progress relative to executed EE code varies per seed while
    // the same seed reproduces the same sequence.  Off (identity) by default.
    bool m_jitterEnabled = false;
    uint64_t m_jitterState = 0u;
    // Emulated-time pacing.  EE time follows host wall time while the host
    // keeps up (audio and VBlank stay real-time) but must not count host-only
    // work (PNG encoding, VRAM dumps): that work blocks the guest, so charging
    // it would advance VBlank/timer deadlines while the guest executes almost
    // nothing.  accountCycles() samples wall time, executor CPU time, explicitly
    // noted host-stall time and guest wait time every kPaceSamplePeriodNs, then
    // advances m_paceAdvanceNs by (wall - host stalls - time the executor spent
    // descheduled outside a guest wait).  Guest waits still count, so idle
    // threads reach their VBlank/timer deadlines.  Excluded time is repaid
    // later, but only out of a bounded multiple of guest CPU time, so a stalled
    // guest runs in slow motion instead of skipping ahead.
    uint64_t m_hostAnchorCycle = 0u;
    uint64_t m_paceLastWallNs = 0u;
    uint64_t m_paceLastCpuNs = 0u;
    uint64_t m_paceLastStallNs = 0u;
    uint64_t m_paceLastWaitNs = 0u;
    uint64_t m_paceAdvanceNs = 0u;
    uint64_t m_paceDebtNs = 0u;
    // Cumulative components for PS2X_EE_TIME_TRACE.
    uint64_t m_paceGuestCpuNs = 0u;
    uint64_t m_paceWaitNs = 0u;
    uint64_t m_paceStalledNs = 0u;
    uint64_t m_paceDescheduledNs = 0u;
    uint64_t m_paceShortDeschedNs = 0u;
    uint64_t m_paceMovieNs = 0u;
    bool m_paceInitialized = false;
    // PS2X(rpc-wait-pace): host time slept inside blocking RPC waits, and the
    // developer switch (0 = off) read once per reset.
    uint64_t m_rpcPaceNs = 0u;
    bool m_rpcWaitPaceEnabled = true;
    // PS2X(ee-idle-poll): maximum host sleep per idlePoll() call.  0 keeps the
    // original spin (used by SUIKO_WAIT_LOOP_IDLE=0 A/B runs and the switch).
    uint64_t m_idlePollNs = 100000u;
    // Diagnostic: PS2X_EE_SWITCH_STATS=1 counts EeDispatcherTransfer throws by
    // reason and measures the throw-to-catch host time.  Inert when unset.
    uint64_t m_transferStartNs = 0u;
    uint32_t m_transferReason = 0u;
    bool m_transferActive = false;
    uint64_t m_switchCounts[4]{};
    uint64_t m_switchNs[4]{};
    uint64_t m_switchLastCounts[4]{};
    uint64_t m_switchLastNs[4]{};
    uint64_t m_switchLastGuestNs = 0u;
    // Fiber dispatcher.  m_currentFiber is non-null only while recompiled
    // code runs; transfer sites switch back to the dispatcher instead of
    // throwing.  A fiber is never resumed: dispatcher re-entry is at the saved
    // guest context, exactly like the exception path.
    ps2x::ee::Fiber *m_currentFiber = nullptr;
    uint32_t m_dispatchReason = 0u;
    int m_pendingFiberFree = 0;
    std::exception_ptr m_fiberException;
    PS2Runtime::RecompiledFunction m_fiberFunction = nullptr;
    R5900Context *m_fiberContext = nullptr;
    std::unordered_map<int, std::unique_ptr<ps2x::ee::Fiber>> m_fibers;
    int m_traceFromId = 0;
    uint32_t m_traceReason = 0xFFu;
    uint64_t m_traceCount = 0u;
    uint64_t m_sliceEndCycle = kDefaultTimeSliceCycles;
    std::thread::id m_executorThread{};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_guestExecuting{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_checkpointPending{false};
    uint32_t m_debugPublishCountdown = 0u;

    mutable std::mutex m_eventMutex;
    std::condition_variable m_eventCv;
    std::deque<EeEvent> m_events;
    std::vector<ScheduledEvent> m_deadlines;
    std::deque<GuestInvocation> m_pendingInvocations;
    uint64_t m_eventSequence = 0;
    uint64_t m_invocationSequence = 0;
    uint64_t m_internalDelayToken = 0;
    uint64_t m_vsyncTick = 0;
    uint32_t m_vsyncFlagAddress = 0;
    uint32_t m_vsyncTickAddress = 0;
    uint32_t m_gsVSyncCallback = 0;
    uint32_t m_gsVSyncCallbackGp = 0;
    uint32_t m_gsVSyncCallbackSp = 0;
    std::unordered_map<uint64_t, uint32_t> m_invocationStackTops;
    std::atomic<uint64_t> m_nextDeadlineCycle{0};
    // Host-clock probe for the deadline above.  Reading the wall clock on
    // every checkpoint once the cycle deadline had passed cost 20% of the
    // title CPU on the Legion (vdso clock_gettime).  Only every Nth such
    // checkpoint reads it instead: a checkpoint is one backward branch, so
    // the event still fires within a few microseconds of its host deadline
    // and the vsync chain cannot drift.
    uint32_t m_deadlineProbeCountdown = 0u;
    // Set when a probe (or a wait timeout) has observed a due event's host
    // deadline pass.  The dispatcher consumes it instead of reading the wall
    // clock on every entry; the deadline could not have fired otherwise.
    bool m_deadlineDueObserved = false;
    [[nodiscard]] bool deadlineProbeDue() noexcept;
    // Host-clock time (nanoseconds since the steady_clock epoch) of the event
    // m_nextDeadlineCycle refers to.  An event fires only when its EE deadline
    // and its host deadline have both passed, so the checkpoint trigger that
    // generated code polls must consult both.
    std::atomic<uint64_t> m_nextDeadlineHostNs{0};

    mutable std::mutex m_snapshotMutex;
    // Immutable after publish: the debugger takes a shared_ptr copy under the
    // lock and clones it outside, so the executor never waits for a long
    // snapshot copy on the presentation thread.
    std::shared_ptr<const EeKernelSnapshot> m_snapshot;
    uint64_t m_snapshotSequence = 0;
    uint32_t m_snapshotPublishCountdown = 0u;
};
