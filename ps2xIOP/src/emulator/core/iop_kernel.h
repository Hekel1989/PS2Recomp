#pragma once

#include "iop_cpu.h"

#include <cstddef>
#include <cstdint>
#include <map>

namespace ps2x::iop::detail
{
    class IopMemory;

    enum class IopThreadState : uint8_t
    {
        Dormant,
        Ready,
        Running,
        Sleep,
        Delay,
        Semaphore,
        EventFlag,
        Suspended,
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
        uint64_t wakeCycle = 0;
        // Non-zero while the Delay comes from WaitVblank*: 1 = waiting for a
        // VBlank start, 2 = for a VBlank end (see retargetVblankWaiters).
        uint8_t vblankWait = 0;
        int waitId = 0;
        uint32_t waitBits = 0;
        uint32_t waitMode = 0;
        uint32_t waitResultAddress = 0;
        int wakeupCount = 0;
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

        void sleepCurrent(IopCpuState &cpu);
        void delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu);
        void markCurrentVblankWait(uint8_t kind) noexcept
        {
            if (m_currentThread != nullptr)
                m_currentThread->vblankWait = kind;
        }
        // Moves every thread waiting for this kind of VBlank edge to wakeCycle
        // (the EE re-anchored its VBlank chain).
        void retargetVblankWaiters(uint8_t kind, uint64_t wakeCycle);

        [[nodiscard]] IopThread *beginNextReady(uint64_t currentCycle);
        // PS2 threads are priority-preemptive and round-robin within a
        // priority. Returns true when a wake-up import readied a thread that
        // should run before the caller; the caller must then yield.
        [[nodiscard]] bool preemptIfHigherPriority(IopCpuState &cpu);
        [[nodiscard]] IopThread *currentThread() noexcept { return m_currentThread; }
        [[nodiscard]] uint64_t nextWakeCycle(uint64_t fallback) const;
        void endTimeslice(IopThread &thread, uint32_t returnSentinel);
        void cleanupDeadThreads();
        void terminateThreadsInRange(uint32_t base, uint32_t size);

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

        [[nodiscard]] bool referThreadStatus(int id, uint32_t outputAddress);
        void wakeOneSemaphore(int id);
        [[nodiscard]] static bool eventSatisfied(const EventFlag &event, uint32_t bits, uint32_t mode);
        void wakeEventWaiters(EventFlag &event);

        IopMemory &m_memory;
        std::map<int, IopThread> m_threads;
        std::map<int, Semaphore> m_semaphores;
        std::map<int, EventFlag> m_eventFlags;
        uint32_t m_nextThreadId = 1;
        uint32_t m_nextSemaphoreId = 1;
        uint32_t m_nextEventFlagId = 1;
        uint32_t m_lastScheduledId = 0;
        // Earliest wake cycle among Delay threads.  A lower bound that is only
        // refreshed when it comes due, so the per-slice scheduler does not
        // rescan every thread to find that no delay expired.
        uint64_t m_nextDelayWakeCycle = UINT64_MAX;
        // Number of threads in the Dead state; cleanupDeadThreads() is a full
        // scan and runs after every time slice, so skip it when nothing died.
        uint32_t m_deadThreadCount = 0;
        IopThread *m_currentThread = nullptr;
    };
}
