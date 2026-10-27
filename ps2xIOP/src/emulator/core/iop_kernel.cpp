#include "iop_kernel.h"

#include "iop_memory.h"
#include "../iop_emulator_const.h"

#include <algorithm>
#include <cstdio>

// PS2X(iop-kernel-rules): thread, semaphore, event-flag and alarm semantics
// follow the real IOP kernel; the rules, their sources (OpenPS2BIOS's
// src/common/iopk.c, PS2SDK) and how ours used to differ are in
// docs/iop-kernel-rules.md of the Suikoden Tactics recomp.

namespace ps2x::iop::detail
{
    namespace
    {
        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            if (alignment <= 1u)
                return value;
            const uint32_t mask = alignment - 1u;
            return (value + mask) & ~mask;
        }

        constexpr int32_t KE_ILLEGAL_CONTEXT = -100;
        constexpr int32_t KE_FOUND_HANDLER = -104;
        constexpr int32_t KE_NOTFOUND_HANDLER = -105;
        constexpr int32_t KE_ILLEGAL_ENTRY = -402;
        constexpr int32_t KE_DORMANT = -413;
        constexpr int32_t KE_NOT_WAIT = -416;
        constexpr int32_t KE_RELEASE_WAIT = -418;
        constexpr int32_t KE_SEMA_ZERO = -419;
        constexpr int32_t KE_SEMA_OVF = -420;
        constexpr int32_t KE_EVF_COND = -421;
        constexpr int32_t KE_EVF_MULTI = -422;
        constexpr int32_t KE_EVF_ILPAT = -423;
        constexpr int32_t KE_WAIT_DELETE = -425;

        constexpr uint32_t SA_THPRI = 0x1u;
        constexpr uint32_t EA_MULTI = 0x2u;
        constexpr uint32_t WEF_OR = 0x1u;
        constexpr uint32_t WEF_CLEAR = 0x10u;

        // PS2X(iop-module-start-thread): the module starter thread's priority
        // and stack, from Play! Source/iop/IopBios.h (MODULE_INIT_PRIORITY = 8,
        // DEFAULT_STACKSIZE = 0x4000) and IopBios.cpp RequestModuleStart.
        constexpr uint32_t kModuleStartPriority = 8u;
        constexpr uint32_t kModuleStartStackSize = 0x4000u;

        bool isWaiting(IopThreadState state)
        {
            return state == IopThreadState::Sleep || state == IopThreadState::Delay ||
                   state == IopThreadState::Semaphore || state == IopThreadState::EventFlag;
        }
    }

    IopKernel::IopKernel(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopKernel::reset()
    {
        m_threads.clear();
        m_semaphores.clear();
        m_eventFlags.clear();
        m_alarms.clear();
        m_nextThreadId = 1;
        m_nextSemaphoreId = 1;
        m_nextEventFlagId = 1;
        m_order = 0;
        m_alarmSerial = 0;
        m_nextDelayWakeCycle = UINT64_MAX;
        m_deadThreadCount = 0;
        m_interruptDepth = 0;
        m_currentThread = nullptr;
    }

    void IopKernel::makeReady(IopThread &thread, int32_t result)
    {
        thread.state = IopThreadState::Ready;
        thread.order = ++m_order;
        thread.waitId = 0;
        thread.waitBits = 0;
        thread.waitMode = 0;
        thread.waitResultAddress = 0;
        thread.vblankWait = 0;
        thread.cpu.gpr[2] = static_cast<uint32_t>(result);
    }

    void IopKernel::markWaiting(IopThread &thread, IopThreadState state, int waitId)
    {
        thread.state = state;
        thread.waitId = waitId;
        thread.order = ++m_order;
    }

    IopThread *IopKernel::firstWaiter(IopThreadState state, int waitId, bool byPriority)
    {
        IopThread *best = nullptr;
        for (auto &[threadId, thread] : m_threads)
        {
            if (thread.state != state || thread.waitId != waitId)
                continue;
            if (best == nullptr ||
                (byPriority && thread.priority < best->priority) ||
                ((!byPriority || thread.priority == best->priority) && thread.order < best->order))
            {
                best = &thread;
            }
        }
        return best;
    }

    void IopKernel::releaseWaiters(IopThreadState state, int waitId, int32_t result)
    {
        while (IopThread *waiter = firstWaiter(state, waitId, false))
            makeReady(*waiter, result);
    }

    bool IopKernel::dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };

        switch (ordinal)
        {
        case 4: // CreateThread
        {
            const uint32_t descriptor = cpu.gpr[4];
            IopThread thread;
            thread.id = static_cast<int>(m_nextThreadId++);
            thread.attr = m_memory.read32(descriptor + 0u);
            thread.option = m_memory.read32(descriptor + 4u);
            thread.entry = m_memory.read32(descriptor + 8u);
            thread.stackSize = std::max<uint32_t>(m_memory.read32(descriptor + 12u), 0x100u);
            thread.priority = std::clamp<uint32_t>(m_memory.read32(descriptor + 16u), 1u, 126u);
            thread.initialPriority = thread.priority;
            thread.stackBase = m_memory.allocate(thread.stackSize + kStackGuardBytes, 16u);
            if (thread.stackBase == 0u)
            {
                setV0(-400);
                return true;
            }
            const int id = thread.id;
            m_threads.emplace(id, std::move(thread));
            setV0(id);
            return true;
        }
        case 5: // DeleteThread
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.stackBase != 0u)
                (void)m_memory.freeAllocation(it->second.stackBase);
            m_threads.erase(it);
            setV0(0);
            return true;
        }
        case 6: // StartThread
        case 7: // StartThreadArgs
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            IopThread &thread = it->second;
            thread.cpu = {};
            thread.cpu.pc = thread.entry;
            thread.cpu.gpr[4] = cpu.gpr[5];
            thread.cpu.gpr[5] = ordinal == 7 ? cpu.gpr[6] : 0u;
            thread.cpu.gpr[28] = cpu.gpr[28];
            thread.cpu.gpr[29] = alignUp(thread.stackBase + thread.stackSize, 16u) - 16u;
            thread.cpu.gpr[31] = kThreadReturnSentinel;
            makeReady(thread, 0);
            setV0(0);
            return true;
        }
        case 8: // ExitThread
        case 9: // ExitDeleteThread
            if (m_currentThread != nullptr)
            {
                m_currentThread->state = ordinal == 9 ? IopThreadState::Dead : IopThreadState::Dormant;
                if (ordinal == 9)
                    ++m_deadThreadCount;
                cpu.stopped = true;
                cpu.yielded = true;
            }
            setV0(0);
            return true;
        case 10:
        case 11: // TerminateThread
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            it->second.state = IopThreadState::Dormant;
            setV0(0);
            return true;
        }
        case 12:
        case 13:
            setV0(0);
            return true;
        case 14: // ChangeThreadPriority
        case 15: // iChangeThreadPriority
        {
            int id = static_cast<int>(cpu.gpr[4]);
            if (id == 0 && m_currentThread != nullptr)
                id = m_currentThread->id;
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            IopThread &thread = it->second;
            thread.priority = cpu.gpr[5] == 0u ? thread.initialPriority
                                               : std::clamp<uint32_t>(cpu.gpr[5], 1u, 126u);
            // A ready thread goes to the back of its new priority; a running
            // caller that lowered itself is preempted after the import.
            if (thread.state == IopThreadState::Ready)
                thread.order = ++m_order;
            setV0(0);
            return true;
        }
        case 16: // RotateThreadReadyQueue
        case 17: // iRotateThreadReadyQueue
        {
            uint32_t priority = cpu.gpr[4];
            if (priority == 0u && m_currentThread != nullptr)
                priority = m_currentThread->priority;
            if (m_currentThread != nullptr && m_currentThread->priority == priority)
            {
                // The running thread goes to the back of its priority.
                m_currentThread->order = ++m_order;
                if (&m_currentThread->cpu == &cpu)
                    cpu.yielded = true;
            }
            else
            {
                IopThread *first = nullptr;
                for (auto &[threadId, thread] : m_threads)
                {
                    if (thread.state == IopThreadState::Ready && thread.priority == priority &&
                        (first == nullptr || thread.order < first->order))
                        first = &thread;
                }
                if (first != nullptr)
                    first->order = ++m_order;
            }
            setV0(0);
            return true;
        }
        case 18: // ReleaseWaitThread
        case 19: // iReleaseWaitThread
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            if (!isWaiting(it->second.state))
            {
                setV0(KE_NOT_WAIT);
                return true;
            }
            makeReady(it->second, KE_RELEASE_WAIT);
            setV0(0);
            return true;
        }
        case 20:
            setV0(m_currentThread != nullptr ? m_currentThread->id : 0);
            return true;
        case 21:
            setV0(0x1000);
            return true;
        case 22:
        case 23:
            setV0(referThreadStatus(static_cast<int>(cpu.gpr[4]), cpu.gpr[5]) ? 0 : -1);
            return true;
        case 24: // SleepThread
            if (m_currentThread != nullptr && m_currentThread->wakeupCount > 0 && !inInterrupt())
            {
                --m_currentThread->wakeupCount;
                setV0(0);
            }
            else
                setV0(sleepCurrent(cpu) ? 0 : KE_ILLEGAL_CONTEXT);
            return true;
        case 25: // WakeupThread
        case 26: // iWakeupThread
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.state == IopThreadState::Dormant)
            {
                setV0(KE_DORMANT);
                return true;
            }
            wakeupThread(id);
            setV0(0);
            return true;
        }
        case 27: // CancelWakeupThread
        case 28: // iCancelWakeupThread
        {
            int id = static_cast<int>(cpu.gpr[4]);
            if (id == 0 && m_currentThread != nullptr)
                id = m_currentThread->id;
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            const int old = it->second.wakeupCount;
            it->second.wakeupCount = 0;
            setV0(old);
            return true;
        }
        case 29:
        case 30: // SuspendThread / iSuspendThread
        {
            // PS2X(iop-module-start-thread): OpenPS2BIOS src/common/sched.c
            // ob_thread_suspend keeps a suspended flag beside the wait state
            // and does not suspend the calling thread. Both calls return the
            // thread id.
            int id = static_cast<int>(cpu.gpr[4]);
            if (id == 0 && m_currentThread != nullptr)
                id = m_currentThread->id;
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.state == IopThreadState::Dormant)
            {
                setV0(KE_DORMANT);
                return true;
            }
            if (m_currentThread != &it->second)
                it->second.suspended = true;
            setV0(id);
            return true;
        }
        case 31:
        case 32: // ResumeThread / iResumeThread
        {
            // OpenPS2BIOS ob_thread_resume clears the flag and, for a ready
            // thread, sends it to the back of its priority.
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.suspended)
            {
                it->second.suspended = false;
                if (it->second.state == IopThreadState::Ready)
                    it->second.order = ++m_order;
            }
            setV0(id);
            return true;
        }
        case 33: // DelayThread
        {
            const uint64_t delayCycles = (static_cast<uint64_t>(cpu.gpr[4]) * kIopClockHz + 999'999ull) / 1'000'000ull;
            const bool waited = delayCurrentUntil(currentCycle + std::max<uint64_t>(delayCycles, 1u), cpu);
            setV0(waited ? 0 : KE_ILLEGAL_CONTEXT);
            return true;
        }
        case 34: // GetSystemTime
            m_memory.write32(cpu.gpr[4], static_cast<uint32_t>(currentCycle));
            m_memory.write32(cpu.gpr[4] + 4u, static_cast<uint32_t>(currentCycle >> 32u));
            setV0(0);
            return true;
        case 35: // SetAlarm
        case 36: // iSetAlarm
            setV0(ordinal == 35 && inInterrupt()
                      ? KE_ILLEGAL_CONTEXT
                      : setAlarm(cpu.gpr[4], cpu.gpr[5], cpu.gpr[6], cpu.gpr[28], currentCycle));
            return true;
        case 37: // CancelAlarm
        case 38: // iCancelAlarm
            setV0(ordinal == 37 && inInterrupt() ? KE_ILLEGAL_CONTEXT : cancelAlarm(cpu.gpr[4], cpu.gpr[5]));
            return true;
        case 39: // USec2SysClock
        {
            const uint64_t cycles = (static_cast<uint64_t>(cpu.gpr[4]) * kIopClockHz) / 1'000'000ull;
            m_memory.write32(cpu.gpr[5], static_cast<uint32_t>(cycles));
            m_memory.write32(cpu.gpr[5] + 4u, static_cast<uint32_t>(cycles >> 32u));
            setV0(0);
            return true;
        }
        case 40:
        {
            const uint64_t cycles = static_cast<uint64_t>(m_memory.read32(cpu.gpr[4])) | (static_cast<uint64_t>(m_memory.read32(cpu.gpr[4] + 4u)) << 32u);
            const uint64_t usec = (cycles * 1'000'000ull) / kIopClockHz;
            if (cpu.gpr[5] != 0u)
                m_memory.write32(cpu.gpr[5], static_cast<uint32_t>(usec / 1'000'000ull));
            if (cpu.gpr[6] != 0u)
                m_memory.write32(cpu.gpr[6], static_cast<uint32_t>(usec % 1'000'000ull));
            setV0(0);
            return true;
        }
        case 41:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    void IopKernel::wakeupThread(int id)
    {
        const auto it = m_threads.find(id);
        if (it == m_threads.end() || it->second.state == IopThreadState::Dormant)
            return;
        if (it->second.state == IopThreadState::Sleep)
            makeReady(it->second, 0);
        else
            ++it->second.wakeupCount;
    }

    int IopKernel::startModuleThread(uint32_t entry, uint32_t a0, uint32_t a1, uint32_t gp)
    {
        // PS2X(iop-module-start-thread): Play!'s module starter is an
        // ordinary thread (IopBios.cpp
        // RequestModuleStart / TriggerCallback); ProcessModuleStart patches
        // that thread's context to run the module entry, so its waits block.
        IopThread thread;
        thread.id = static_cast<int>(m_nextThreadId++);
        thread.entry = entry;
        thread.stackSize = kModuleStartStackSize;
        thread.priority = kModuleStartPriority;
        thread.initialPriority = kModuleStartPriority;
        thread.stackBase = m_memory.allocate(thread.stackSize + kStackGuardBytes, 16u);
        if (thread.stackBase == 0u)
            return 0;
        thread.cpu.pc = entry;
        thread.cpu.gpr[4] = a0;
        thread.cpu.gpr[5] = a1;
        thread.cpu.gpr[28] = gp;
        thread.cpu.gpr[29] = alignUp(thread.stackBase + thread.stackSize, 16u) - 16u;
        thread.cpu.gpr[31] = kThreadReturnSentinel;
        const int id = thread.id;
        const auto inserted = m_threads.emplace(id, std::move(thread));
        makeReady(inserted.first->second, 0);
        return id;
    }

    bool IopKernel::referThreadStatus(int id, uint32_t outputAddress)
    {
        if (id == 0 && m_currentThread != nullptr)
            id = m_currentThread->id;
        const auto it = m_threads.find(id);
        if (it == m_threads.end() || outputAddress == 0u)
            return false;

        const IopThread &thread = it->second;
        uint32_t status = 0x10u;
        switch (thread.state)
        {
        case IopThreadState::Running:
            status = 0x01u;
            break;
        case IopThreadState::Ready:
            // PS2X(iop-module-start-thread): OpenPS2BIOS ob_thread_status:
            // suspended ready = 8, suspended waiting = 12.
            status = thread.suspended ? 0x08u : 0x02u;
            break;
        case IopThreadState::Sleep:
        case IopThreadState::Delay:
        case IopThreadState::Semaphore:
        case IopThreadState::EventFlag:
            status = thread.suspended ? 0x0Cu : 0x04u;
            break;
        default:
            status = 0x10u;
            break;
        }

        m_memory.write32(outputAddress + 0u, thread.attr);
        m_memory.write32(outputAddress + 4u, thread.option);
        m_memory.write32(outputAddress + 8u, status);
        m_memory.write32(outputAddress + 12u, thread.entry);
        m_memory.write32(outputAddress + 16u, thread.stackBase);
        m_memory.write32(outputAddress + 20u, thread.stackSize);
        m_memory.write32(outputAddress + 24u, thread.cpu.gpr[28]);
        m_memory.write32(outputAddress + 28u, thread.initialPriority);
        m_memory.write32(outputAddress + 32u, thread.priority);
        m_memory.write32(outputAddress + 36u, thread.state == IopThreadState::Sleep ? 1u : thread.state == IopThreadState::Delay   ? 2u
                                                                                       : thread.state == IopThreadState::Semaphore ? 3u
                                                                                       : thread.state == IopThreadState::EventFlag ? 4u
                                                                                                                                   : 0u);
        m_memory.write32(outputAddress + 40u, static_cast<uint32_t>(thread.waitId));
        m_memory.write32(outputAddress + 44u, static_cast<uint32_t>(thread.wakeupCount));
        return true;
    }

    bool IopKernel::dispatchSemaphoreImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };

        switch (ordinal)
        {
        case 4:
        {
            const uint32_t descriptor = cpu.gpr[4];
            Semaphore semaphore;
            semaphore.id = static_cast<int>(m_nextSemaphoreId++);
            semaphore.attr = m_memory.read32(descriptor + 0u);
            semaphore.option = m_memory.read32(descriptor + 4u);
            semaphore.current = static_cast<int>(m_memory.read32(descriptor + 8u));
            semaphore.maximum = std::max(1, static_cast<int>(m_memory.read32(descriptor + 12u)));
            m_semaphores.emplace(semaphore.id, semaphore);
            setV0(semaphore.id);
            return true;
        }
        case 5: // DeleteSema
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            if (m_semaphores.erase(id) == 0u)
            {
                setV0(-1);
                return true;
            }
            releaseWaiters(IopThreadState::Semaphore, id, KE_WAIT_DELETE);
            setV0(0);
            return true;
        }
        case 6: // SignalSema
        case 7: // iSignalSema
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_semaphores.find(id);
            if (it == m_semaphores.end())
            {
                setV0(-1);
                return true;
            }
            // The count goes straight to the first waiter: FIFO, or priority
            // order with SA_THPRI.
            if (IopThread *waiter = firstWaiter(IopThreadState::Semaphore, id, (it->second.attr & SA_THPRI) != 0u))
                makeReady(*waiter, 0);
            else if (it->second.current < it->second.maximum)
                ++it->second.current;
            else
            {
                setV0(KE_SEMA_OVF);
                return true;
            }
            setV0(0);
            return true;
        }
        case 8: // WaitSema
        case 9: // PollSema
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_semaphores.find(id);
            if (it == m_semaphores.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.current > 0)
            {
                --it->second.current;
                setV0(0);
            }
            else if (ordinal == 9)
                setV0(KE_SEMA_ZERO);
            else if (!checkWaitContext(cpu, "WaitSema", id))
                setV0(KE_ILLEGAL_CONTEXT);
            else if (m_currentThread != nullptr)
            {
                markWaiting(*m_currentThread, IopThreadState::Semaphore, id);
                cpu.yielded = true;
                setV0(0);
            }
            return true;
        }
        case 11:
        case 12:
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_semaphores.find(id);
            if (it == m_semaphores.end())
            {
                setV0(-1);
                return true;
            }
            const uint32_t outputAddress = cpu.gpr[5];
            if (outputAddress != 0u)
            {
                m_memory.write32(outputAddress + 0u, it->second.attr);
                m_memory.write32(outputAddress + 4u, it->second.option);
                m_memory.write32(outputAddress + 8u, 0u);
                m_memory.write32(outputAddress + 12u, static_cast<uint32_t>(it->second.maximum));
                m_memory.write32(outputAddress + 16u, static_cast<uint32_t>(it->second.current));
                uint32_t waiters = 0u;
                for (const auto &[threadId, thread] : m_threads)
                {
                    if (thread.state == IopThreadState::Semaphore && thread.waitId == id)
                        ++waiters;
                }
                m_memory.write32(outputAddress + 20u, waiters);
            }
            setV0(0);
            return true;
        }
        default:
            return false;
        }
    }

    bool IopKernel::eventSatisfied(const EventFlag &event, uint32_t bits, uint32_t mode)
    {
        if (bits == 0u)
            return false;
        return (mode & WEF_OR) != 0u ? (event.bits & bits) != 0u : (event.bits & bits) == bits;
    }

    void IopKernel::wakeEventWaiters(EventFlag &event)
    {
        // Satisfied waiters wake in queue order; a clearing waiter can leave
        // nothing for the rest.
        for (;;)
        {
            IopThread *best = nullptr;
            for (auto &[threadId, thread] : m_threads)
            {
                if (thread.state != IopThreadState::EventFlag || thread.waitId != event.id ||
                    !eventSatisfied(event, thread.waitBits, thread.waitMode))
                    continue;
                if (best == nullptr || thread.order < best->order)
                    best = &thread;
            }
            if (best == nullptr)
                break;
            if (best->waitResultAddress != 0u)
                m_memory.write32(best->waitResultAddress, event.bits);
            if ((best->waitMode & WEF_CLEAR) != 0u)
                event.bits = 0u;
            makeReady(*best, 0);
        }
    }

    int IopKernel::createInternalEventFlag(uint32_t attr, uint32_t option, uint32_t bits)
    {
        EventFlag event;
        event.id = static_cast<int>(m_nextEventFlagId++);
        event.attr = attr;
        event.option = option;
        event.bits = bits;
        const int id = event.id;
        m_eventFlags.emplace(id, event);
        return id;
    }

    bool IopKernel::setInternalEventFlag(int id, uint32_t bits)
    {
        const auto event = m_eventFlags.find(id);
        if (event == m_eventFlags.end())
            return false;
        event->second.bits |= bits;
        wakeEventWaiters(event->second);
        return true;
    }

    bool IopKernel::dispatchEventImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };

        switch (ordinal)
        {
        case 4:
        {
            const uint32_t descriptor = cpu.gpr[4];
            setV0(createInternalEventFlag(m_memory.read32(descriptor + 0u),
                                          m_memory.read32(descriptor + 4u),
                                          m_memory.read32(descriptor + 8u)));
            return true;
        }
        case 5: // DeleteEventFlag
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            if (m_eventFlags.erase(id) == 0u)
            {
                setV0(-1);
                return true;
            }
            releaseWaiters(IopThreadState::EventFlag, id, KE_WAIT_DELETE);
            setV0(0);
            return true;
        }
        case 6:
        case 7:
        {
            if (!setInternalEventFlag(static_cast<int>(cpu.gpr[4]), cpu.gpr[5]))
            {
                setV0(-1);
                return true;
            }
            setV0(0);
            return true;
        }
        case 8:
        case 9:
        {
            const auto event = m_eventFlags.find(static_cast<int>(cpu.gpr[4]));
            if (event == m_eventFlags.end())
            {
                setV0(-1);
                return true;
            }
            // IOP ClearEventFlag applies a mask: callers pass ~bitsToClear.
            event->second.bits &= cpu.gpr[5];
            setV0(0);
            return true;
        }
        case 10: // WaitEventFlag
        case 11: // PollEventFlag
        {
            const auto event = m_eventFlags.find(static_cast<int>(cpu.gpr[4]));
            if (event == m_eventFlags.end())
            {
                setV0(-1);
                return true;
            }
            const uint32_t bits = cpu.gpr[5];
            const uint32_t mode = cpu.gpr[6];
            if (bits == 0u)
            {
                setV0(KE_EVF_ILPAT);
                return true;
            }
            if ((event->second.attr & EA_MULTI) == 0u &&
                firstWaiter(IopThreadState::EventFlag, event->second.id, false) != nullptr)
            {
                setV0(KE_EVF_MULTI);
                return true;
            }
            if (eventSatisfied(event->second, bits, mode))
            {
                if (cpu.gpr[7] != 0u)
                    m_memory.write32(cpu.gpr[7], event->second.bits);
                if ((mode & WEF_CLEAR) != 0u)
                    event->second.bits = 0u;
                setV0(0);
            }
            else if (ordinal == 11)
                setV0(KE_EVF_COND);
            else if (!checkWaitContext(cpu, "WaitEventFlag", event->second.id))
                setV0(KE_ILLEGAL_CONTEXT);
            else if (m_currentThread != nullptr)
            {
                markWaiting(*m_currentThread, IopThreadState::EventFlag, event->second.id);
                m_currentThread->waitBits = bits;
                m_currentThread->waitMode = mode;
                m_currentThread->waitResultAddress = cpu.gpr[7];
                setV0(0);
                cpu.yielded = true;
            }
            else
                setV0(KE_RELEASE_WAIT);
            return true;
        }
        case 13:
        case 14:
        {
            const auto event = m_eventFlags.find(static_cast<int>(cpu.gpr[4]));
            if (event == m_eventFlags.end())
            {
                setV0(-1);
                return true;
            }
            if (cpu.gpr[5] != 0u)
            {
                uint32_t waiters = 0u;
                for (const auto &[threadId, thread] : m_threads)
                {
                    if (thread.state == IopThreadState::EventFlag && thread.waitId == event->second.id)
                        ++waiters;
                }
                m_memory.write32(cpu.gpr[5] + 0u, event->second.attr);
                m_memory.write32(cpu.gpr[5] + 4u, event->second.option);
                m_memory.write32(cpu.gpr[5] + 8u, event->second.bits);
                m_memory.write32(cpu.gpr[5] + 12u, event->second.bits);
                m_memory.write32(cpu.gpr[5] + 16u, waiters);
            }
            setV0(0);
            return true;
        }
        default:
            return false;
        }
    }

    bool IopKernel::checkWaitContext(const IopCpuState &cpu, const char *call, int id)
    {
        const char *context = inInterrupt()                                         ? "interrupt"
                              : m_currentThread == nullptr                          ? "no-thread"
                              : &m_currentThread->cpu != &cpu                       ? "callback"
                                                                                    : nullptr;
        if (context == nullptr)
            return true;
        ++m_unblockableWaits;
        if (m_unblockableWaits <= 64u || m_unblockableWaits % 1000u == 0u)
        {
            std::fprintf(stderr, "[iop-wait] %s %s id=%d pc=0x%x ra=0x%x thread=%d total=%llu\n",
                         context, call, id, cpu.pc, cpu.gpr[31],
                         m_currentThread != nullptr ? m_currentThread->id : -1,
                         static_cast<unsigned long long>(m_unblockableWaits));
        }
        return m_currentThread == nullptr && !inInterrupt();
    }

    bool IopKernel::sleepCurrent(IopCpuState &cpu, const char *call)
    {
        if (!checkWaitContext(cpu, call, 0))
            return false;
        if (m_currentThread == nullptr)
            return true;
        markWaiting(*m_currentThread, IopThreadState::Sleep, 0);
        cpu.yielded = true;
        return true;
    }

    void IopKernel::retargetVblankWaiters(uint8_t kind, uint64_t wakeCycle)
    {
        for (auto &[id, thread] : m_threads)
        {
            if (thread.state != IopThreadState::Delay || thread.vblankWait != kind)
                continue;
            thread.wakeCycle = wakeCycle;
            // m_nextDelayWakeCycle is a lower bound: an earlier wake must lower
            // it, a later one is picked up by the next scan.
            m_nextDelayWakeCycle = std::min(m_nextDelayWakeCycle, wakeCycle);
        }
    }

    bool IopKernel::delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu, const char *call)
    {
        if (!checkWaitContext(cpu, call, 0))
            return false;
        if (m_currentThread == nullptr)
            return true;
        m_currentThread->vblankWait = 0;
        m_currentThread->wakeCycle = wakeCycle;
        markWaiting(*m_currentThread, IopThreadState::Delay, 0);
        m_nextDelayWakeCycle = std::min(m_nextDelayWakeCycle, wakeCycle);
        cpu.yielded = true;
        return true;
    }

    IopThread *IopKernel::beginNextReady(uint64_t currentCycle)
    {
        // Promote expired delays and pick the next thread in one pass: the
        // highest priority, and within it the one that has been ready longest
        // (order).  A thread whose time slice ended without blocking kept its
        // order, so it runs on: the IOP has no time slicing, and threads of
        // equal priority never interrupt each other.
        const bool checkDelays = currentCycle >= m_nextDelayWakeCycle;
        uint64_t nextDelay = UINT64_MAX;
        IopThread *best = nullptr;
        for (auto &[id, thread] : m_threads)
        {
            if (thread.state == IopThreadState::Delay)
            {
                if (checkDelays && thread.wakeCycle <= currentCycle)
                    makeReady(thread, 0);
                else
                    nextDelay = std::min(nextDelay, thread.wakeCycle);
            }
            // PS2X(iop-module-start-thread): a suspended ready thread is not
            // scheduled (OpenPS2BIOS ob_sched_finish skips t->suspended).
            if (thread.state != IopThreadState::Ready || thread.suspended)
                continue;
            if (best == nullptr || thread.priority < best->priority ||
                (thread.priority == best->priority && thread.order < best->order))
                best = &thread;
        }
        if (checkDelays)
            m_nextDelayWakeCycle = nextDelay;

        if (best == nullptr)
            return nullptr;

        m_currentThread = best;
        best->state = IopThreadState::Running;
        best->cpu.stopped = false;
        best->cpu.yielded = false;
        return best;
    }

    bool IopKernel::preemptIfHigherPriority(IopCpuState &cpu)
    {
        if (m_currentThread == nullptr || &m_currentThread->cpu != &cpu)
            return false;
        for (const auto &[id, thread] : m_threads)
        {
            if (thread.state == IopThreadState::Ready && !thread.suspended &&
                thread.priority < m_currentThread->priority)
            {
                cpu.yielded = true;
                return true;
            }
        }
        return false;
    }

    uint64_t IopKernel::nextWakeCycle(uint64_t fallback) const
    {
        return std::min(fallback, m_nextDelayWakeCycle);
    }

    void IopKernel::endTimeslice(IopThread &thread, uint32_t returnSentinel)
    {
        if (thread.cpu.pc == returnSentinel || thread.cpu.stopped)
            thread.state = IopThreadState::Dormant;
        else if (thread.state == IopThreadState::Running)
            thread.state = IopThreadState::Ready; // keeps its order: front of its priority
        m_currentThread = nullptr;
        cleanupDeadThreads();
    }

    void IopKernel::cleanupDeadThreads()
    {
        if (m_deadThreadCount == 0u)
            return;
        uint32_t removed = 0u;
        for (auto thread = m_threads.begin(); thread != m_threads.end();)
        {
            if (thread->second.state != IopThreadState::Dead)
            {
                ++thread;
                continue;
            }
            if (thread->second.stackBase != 0u)
                (void)m_memory.freeAllocation(thread->second.stackBase);
            thread = m_threads.erase(thread);
            ++removed;
        }
        m_deadThreadCount -= removed;
    }

    void IopKernel::terminateThreadsInRange(uint32_t base, uint32_t size)
    {
        for (auto &[id, thread] : m_threads)
        {
            const uint32_t pc = IopMemory::physicalAddress(thread.cpu.pc);
            if (pc >= base && pc < base + size && thread.state != IopThreadState::Dead)
            {
                thread.state = IopThreadState::Dead;
                ++m_deadThreadCount;
            }
        }
    }

    int IopKernel::setAlarm(uint32_t clockAddress, uint32_t handler, uint32_t argument, uint32_t gp, uint64_t now)
    {
        if (clockAddress == 0u || handler == 0u)
            return KE_ILLEGAL_ENTRY;
        for (const IopAlarm &alarm : m_alarms)
        {
            if (alarm.handler == handler && alarm.argument == argument)
                return KE_FOUND_HANDLER;
        }
        const uint64_t delay = static_cast<uint64_t>(m_memory.read32(clockAddress)) |
                               (static_cast<uint64_t>(m_memory.read32(clockAddress + 4u)) << 32u);
        m_alarms.push_back(IopAlarm{++m_alarmSerial, handler, argument, gp, now + std::max<uint64_t>(delay, 1u)});
        return 0;
    }

    int IopKernel::cancelAlarm(uint32_t handler, uint32_t argument)
    {
        const auto it = std::find_if(m_alarms.begin(), m_alarms.end(), [&](const IopAlarm &alarm)
                                     { return alarm.handler == handler && alarm.argument == argument; });
        if (it == m_alarms.end())
            return KE_NOTFOUND_HANDLER;
        m_alarms.erase(it);
        return 0;
    }

    std::optional<IopAlarm> IopKernel::dueAlarm(uint64_t now) const
    {
        const IopAlarm *due = nullptr;
        for (const IopAlarm &alarm : m_alarms)
        {
            if (alarm.target <= now && (due == nullptr || alarm.target < due->target))
                due = &alarm;
        }
        if (due == nullptr)
            return std::nullopt;
        return *due;
    }

    void IopKernel::finishAlarm(uint64_t serial, uint32_t again, uint64_t now)
    {
        // A handler that cancelled its own alarm left nothing to re-arm.
        const auto it = std::find_if(m_alarms.begin(), m_alarms.end(), [&](const IopAlarm &alarm)
                                     { return alarm.serial == serial; });
        if (it == m_alarms.end())
            return;
        if (again == 0u)
        {
            m_alarms.erase(it);
            return;
        }
        it->target += again;
        if (it->target <= now)
            it->target = now + again;
    }

    uint64_t IopKernel::nextAlarmCycle(uint64_t fallback) const
    {
        for (const IopAlarm &alarm : m_alarms)
            fallback = std::min(fallback, alarm.target);
        return fallback;
    }
}
