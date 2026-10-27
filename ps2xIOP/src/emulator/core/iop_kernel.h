#pragma once

#include "iop_cpu.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory;

    // PS2X(iop-module-start-thread): suspension is IopThread::suspended, not
    // a state, so a suspended waiter keeps its wait state (R9).
    enum class IopThreadState : uint8_t
    {
        Dormant,
        Ready,
        Running,
        Sleep,
        Delay,
        Semaphore,
        EventFlag,
        Dead,
    };

    struct IopThread
    {
        int id = 0;
        IopThreadState state = IopThreadState::Dormant;
        IopCpuState cpu;
        uint32_t entry = 0;
        uint32_t stackBase = 0;
        uint32_t stackSize = 0;
        uint32_t priority = 0x40;
        uint32_t initialPriority = 0x40;
        uint32_t option = 0;
        uint32_t attr = 0;
        // PS2X(iop-module-start-thread): suspended is kept apart from the wait
        // state, as in OpenPS2BIOS sched.c. A suspended waiter keeps waiting;
        // when the wait ends the thread becomes ready but is not scheduled
        // until ResumeThread.
        bool suspended = false;
        uint64_t wakeCycle = 0;
        // Queue position: set when the thread becomes ready or starts to wait.
        // The ready queue and FIFO wait queues are ordered by it, so a thread
        // that keeps running (or is preempted) keeps its place, as on the IOP.
        uint64_t order = 0;
        // Non-zero while the Delay comes from WaitVblank*: 1 = waiting for a
        // VBlank start, 2 = for a VBlank end (see retargetVblankWaiters).
        uint8_t vblankWait = 0;
        int waitId = 0;
        uint32_t waitBits = 0;
        uint32_t waitMode = 0;
        uint32_t waitResultAddress = 0;
        int wakeupCount = 0;
    };

    // thbase alarm (SetAlarm/iSetAlarm): the handler runs in interrupt context
    // at `target`; a non-zero return re-arms it that many clocks later.
    struct IopAlarm
    {
        uint64_t serial = 0;
        uint32_t handler = 0;
        uint32_t argument = 0;
        uint32_t gp = 0;
        uint64_t target = 0;
    };

    class IopKernel
    {
    public:
        explicit IopKernel(IopMemory &memory) noexcept;

        void reset();

        [[nodiscard]] bool dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        [[nodiscard]] bool dispatchSemaphoreImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchEventImport(uint16_t ordinal, IopCpuState &cpu);

        [[nodiscard]] int createInternalEventFlag(uint32_t attr, uint32_t option, uint32_t bits);
        [[nodiscard]] bool setInternalEventFlag(int id, uint32_t bits);

        // Both return false when the call was refused (interrupt context); the
        // import then returns KE_ILLEGAL_CONTEXT.
        [[nodiscard]] bool sleepCurrent(IopCpuState &cpu, const char *call = "SleepThread");
        [[nodiscard]] bool delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu, const char *call = "DelayThread");
        void markCurrentVblankWait(uint8_t kind) noexcept
        {
            if (m_currentThread != nullptr)
                m_currentThread->vblankWait = kind;
        }
        // Moves every thread waiting for this kind of VBlank edge to wakeCycle
        // (the EE re-anchored its VBlank chain).
        void retargetVblankWaiters(uint8_t kind, uint64_t wakeCycle);

        // WakeupThread: a sleeping thread becomes ready, any other one (not
        // dormant) gets a pending wakeup for its next SleepThread.
        void wakeupThread(int id);

        // PS2X(iop-module-start-thread): creates and starts the loader thread
        // that runs a module's start function, so waits inside it block like
        // any thread's. Priority 8 and a 0x4000 stack, as Play!'s module
        // starter thread (IopBios.h MODULE_INIT_PRIORITY, DEFAULT_STACKSIZE).
        // Returns the thread id, or 0 if its stack could not be allocated.
        [[nodiscard]] int startModuleThread(uint32_t entry, uint32_t a0, uint32_t a1, uint32_t gp);

        // Interrupt context (interrupt, timer and alarm handlers): blocking
        // calls fail with KE_ILLEGAL_CONTEXT instead of blocking the thread
        // the interrupt arrived on.
        void enterInterrupt() noexcept { ++m_interruptDepth; }
        void leaveInterrupt() noexcept { --m_interruptDepth; }
        [[nodiscard]] bool inInterrupt() const noexcept { return m_interruptDepth != 0u; }

        [[nodiscard]] IopThread *beginNextReady(uint64_t currentCycle);
        // PS2 threads are priority-preemptive and, within a priority, run
        // until they block (no time slicing). Returns true when a thread of
        // higher priority than the caller is ready; the caller then yields.
        [[nodiscard]] bool preemptIfHigherPriority(IopCpuState &cpu);
        [[nodiscard]] IopThread *currentThread() noexcept { return m_currentThread; }
        [[nodiscard]] uint64_t nextWakeCycle(uint64_t fallback) const;
        void endTimeslice(IopThread &thread, uint32_t returnSentinel);
        void cleanupDeadThreads();
        void terminateThreadsInRange(uint32_t base, uint32_t size);

        // The earliest alarm due at `now`, still registered; the caller runs
        // it in interrupt context and passes its return value to finishAlarm.
        [[nodiscard]] std::optional<IopAlarm> dueAlarm(uint64_t now) const;
        void finishAlarm(uint64_t serial, uint32_t again, uint64_t now);
        [[nodiscard]] uint64_t nextAlarmCycle(uint64_t fallback) const;

        [[nodiscard]] size_t threadCount() const noexcept { return m_threads.size(); }

        template <typename Fn>
        void forEachThread(Fn &&fn) const
        {
            for (const auto &[id, thread] : m_threads)
                fn(thread);
        }

    private:
        struct Semaphore
        {
            int id = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
            int current = 0;
            int maximum = 1;
        };

        struct EventFlag
        {
            int id = 0;
            uint32_t bits = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
        };

        // Checks where a wait that has to block was made.  Returns false in
        // interrupt context, where the call must fail with KE_ILLEGAL_CONTEXT.
        // With no current thread (an RPC handler or module start run as a
        // plain call) the wait cannot block either; that case is logged and
        // counted (docs/iop-kernel-rules.md, R2) and the caller carries on.
        [[nodiscard]] bool checkWaitContext(const IopCpuState &cpu, const char *call, int id);
        [[nodiscard]] bool referThreadStatus(int id, uint32_t outputAddress);
        void makeReady(IopThread &thread, int32_t result);
        void markWaiting(IopThread &thread, IopThreadState state, int waitId);
        [[nodiscard]] IopThread *firstWaiter(IopThreadState state, int waitId, bool byPriority);
        void releaseWaiters(IopThreadState state, int waitId, int32_t result);
        [[nodiscard]] static bool eventSatisfied(const EventFlag &event, uint32_t bits, uint32_t mode);
        void wakeEventWaiters(EventFlag &event);
        [[nodiscard]] int setAlarm(uint32_t clockAddress, uint32_t handler, uint32_t argument, uint32_t gp,
                                   uint64_t now);
        [[nodiscard]] int cancelAlarm(uint32_t handler, uint32_t argument);

        IopMemory &m_memory;
        std::map<int, IopThread> m_threads;
        std::map<int, Semaphore> m_semaphores;
        std::map<int, EventFlag> m_eventFlags;
        std::vector<IopAlarm> m_alarms;
        uint32_t m_nextThreadId = 1;
        uint32_t m_nextSemaphoreId = 1;
        uint32_t m_nextEventFlagId = 1;
        uint64_t m_order = 0;
        uint64_t m_alarmSerial = 0;
        // Earliest wake cycle among Delay threads.  A lower bound that is only
        // refreshed when it comes due, so the per-slice scheduler does not
        // rescan every thread to find that no delay expired.
        uint64_t m_nextDelayWakeCycle = UINT64_MAX;
        // Number of threads in the Dead state; cleanupDeadThreads() is a full
        // scan and runs after every time slice, so skip it when nothing died.
        uint32_t m_deadThreadCount = 0;
        uint32_t m_interruptDepth = 0;
        uint64_t m_unblockableWaits = 0;
        IopThread *m_currentThread = nullptr;
    };
}
