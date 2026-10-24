#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
        }

        void reset()
        {
            memory.reset();
            if (const char *watch = std::getenv("PS2X_IOP_WATCH"); watch != nullptr)
            {
                uint32_t watchLo = 0u;
                uint32_t watchHi = 0u;
                if (std::sscanf(watch, "%x:%x", &watchLo, &watchHi) == 2)
                {
                    unsigned watchBudget = 80u;
                    if (const char *wb = std::getenv("PS2X_IOP_WATCH_BUDGET"); wb != nullptr)
                        watchBudget = static_cast<unsigned>(std::atol(wb));
                    memory.setWriteWatch(
                        watchLo, watchHi,
                        [this, watchBudget](uint32_t addr, uint32_t value, uint32_t size) mutable
                            {
                                if (const char *all = std::getenv("PS2X_IOP_WATCH_ALL"); all == nullptr)
                                {
                                    if (size <= 8u && value != 0u)
                                        return;
                                }
                                if (watchBudget == 0u)
                                    return;
                            --watchBudget;
                            std::ostringstream out;
                            out << "[IOP] watch write 0x" << std::hex
                                << std::setw(8) << std::setfill('0') << addr
                                << " = 0x" << value << " size=0x" << size;
                            if (activeCpu != nullptr)
                                out << " pc=0x" << activeCpu->pc << " ra=0x" << activeCpu->gpr[31];
                            log(LogLevel::Info, out.str());
                        });
                }
            }
            if (const char *hw = std::getenv("PS2X_IOP_HWWRITE_TRACE"); hw != nullptr)
            {
                uint32_t hwLo = 0u;
                uint32_t hwHi = 0u;
                if (std::sscanf(hw, "%x-%x", &hwLo, &hwHi) == 2)
                {
                    memory.setHardwareWriteWatch(
                        hwLo, hwHi,
                        [this](uint32_t addr, uint32_t value)
                        {
                            static const long budget = []
                            {
                                if (const char *b = std::getenv("PS2X_IOP_TRACEBUDGET"); b != nullptr)
                                    return std::atol(b);
                                return 4000L;
                            }();
                            static const uint64_t after = []
                            {
                                if (const char *a = std::getenv("PS2X_IOP_TRACEAFTER"); a != nullptr)
                                    return std::strtoull(a, nullptr, 0);
                                return 0ULL;
                            }();
                            static long budget2 = budget;
                            if (totalCycles < after || budget2 == 0)
                                return;
                            if (budget2 > 0)
                                --budget2;
                            std::ostringstream out;
                            out << "[IOP] hwwrite 0x" << std::hex << addr << " val=0x" << value;
                            if (activeCpu != nullptr)
                                out << " pc=0x" << activeCpu->pc << " ra=0x" << activeCpu->gpr[31];
                            if (const IopThread *thread = kernel.currentThread(); thread != nullptr)
                                out << " tid=" << std::dec << thread->id;
                            log(LogLevel::Info, out.str());
                        });
                }
            }
            if (const char *hw = std::getenv("PS2X_IOP_HWREAD_TRACE"); hw != nullptr)
            {
                uint32_t hwLo = 0u;
                uint32_t hwHi = 0u;
                if (std::sscanf(hw, "%x-%x", &hwLo, &hwHi) == 2)
                {
                    memory.setReadWatch(
                        hwLo, hwHi,
                        [this](uint32_t addr, uint32_t value)
                        {
                            static const long budget = []
                            {
                                if (const char *b = std::getenv("PS2X_IOP_TRACEBUDGET"); b != nullptr)
                                    return std::atol(b);
                                return 4000L;
                            }();
                            static const uint64_t after = []
                            {
                                if (const char *a = std::getenv("PS2X_IOP_TRACEAFTER"); a != nullptr)
                                    return std::strtoull(a, nullptr, 0);
                                return 0ULL;
                            }();
                            static long budget2 = budget;
                            if (totalCycles < after || budget2 == 0)
                                return;
                            if (budget2 > 0)
                                --budget2;
                            std::ostringstream out;
                            out << "[IOP] hwread 0x" << std::hex << addr << " val=0x" << value;
                            if (const IopThread *thread = kernel.currentThread(); thread != nullptr)
                                out << " tpc=0x" << thread->cpu.pc << " tra=0x" << thread->cpu.gpr[31]
                                    << " tid=" << std::dec << thread->id;
                            if (activeCpu != nullptr)
                                out << " apc=0x" << activeCpu->pc;
                            log(LogLevel::Info, out.str());
                        });
                }
            }
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            totalCycles = 0;
            totalInstructions = 0;
            branchTraceNext = 0u;
            branchTraceFilled = 0u;
            firstRunLogged.clear();
            eeCycleCarry = 0;
            lastIdleDumpCycle = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            nextSpu2Cycle = 0;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            if (spu2Host != nullptr)
                spu2Host->syncCycles(totalCycles);
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            if (spu2Host != nullptr)
                spu2Host->syncCycles(totalCycles);
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            if (spu2Host != nullptr)
                spu2Host->syncCycles(totalCycles);
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (const auto dma = memory.takeDmaStart())
            {
                if (std::getenv("PS2X_IOP_DMAIRQ_TRACE") != nullptr)
                {
                    std::ostringstream out;
                    out << "[IOP] dma-start irq=" << dma->irq << " delay=" << std::dec << dma->delayCycles;
                    log(LogLevel::Info, out.str());
                }
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
            }
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                const uint32_t descriptor = cpu.gpr[4];
                const bool handled =
                    kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles);
                if (handled && (call.ordinal == 4u || call.ordinal == 6u || call.ordinal == 7u))
                {
                    std::ostringstream out;
                    if (call.ordinal == 4u)
                    {
                        out << "[IOP] CreateThread id=" << std::dec << cpu.gpr[2]
                            << " entry=0x" << std::hex << read32(descriptor + 8u)
                            << " stack=0x" << read32(descriptor + 12u)
                            << " prio=0x" << read32(descriptor + 16u);
                    }
                    else
                    {
                        out << "[IOP] StartThread id=" << std::dec << descriptor;
                    }
                    log(LogLevel::Info, out.str());
                }
                return handled ? ImportDisposition::Handled
                               : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                if (std::getenv("PS2X_IOP_IMPORT_TRACE") != nullptr)
                {
                    std::ostringstream out;
                    out << "[IOP] import " << call.library << ':' << call.ordinal
                        << " -> 0x" << std::hex << target
                        << " ra=0x" << cpu.gpr[31]
                        << " a0=0x" << cpu.gpr[4] << " a1=0x" << cpu.gpr[5];
                    if (iequals(call.library, "libsd") && call.ordinal == 10u)
                    {
                        const uint32_t frame = cpu.gpr[30];
                        const uint32_t stream = read32(frame + 0x28u);
                        out << " fp=0x" << frame << " stream=0x" << stream;
                        if (stream != 0u && stream < IopMemory::RamSize)
                        {
                            out << " s0=0x" << read32(stream)
                                << " s10=0x" << read32(stream + 0x10u)
                                << " s48=0x" << read32(stream + 0x48u)
                                << " s4b=0x" << static_cast<unsigned>(read8(stream + 0x4Bu))
                                << " s6c=0x" << read32(stream + 0x6Cu);
                        }
                    }
                    log(LogLevel::Info, out.str());
                }
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        void recordBranch(uint32_t from, uint32_t to)
        {
            static const char *s_range = std::getenv("PS2X_IOP_TRACERANGE");
            if (s_range != nullptr)
            {
                static uint32_t lo = 0u, hi = 0u;
                static long budget = -1;
                static uint64_t after = 0u;
                if (hi == 0u)
                {
                    unsigned h = 0u;
                    if (std::sscanf(s_range, "%x-%x", &lo, &h) == 2)
                        hi = h;
                    else
                    {
                        lo = 0u;
                        hi = 0u;
                    }
                    if (const char *b = std::getenv("PS2X_IOP_TRACEBUDGET"); b != nullptr)
                        budget = std::atol(b);
                    if (const char *a = std::getenv("PS2X_IOP_TRACEAFTER"); a != nullptr)
                        after = std::strtoull(a, nullptr, 0);
                }
                if (hi != 0u && totalCycles >= after && to >= lo && to < hi && budget != 0)
                {
                    if (budget > 0)
                        --budget;
                    const IopThread *thread = kernel.currentThread();
                    std::ostringstream out;
                    out << "[IOP] branch tid=" << std::dec
                        << (thread != nullptr ? thread->id : -1)
                        << " cycle=" << totalCycles
                        << " 0x" << std::hex << from << " -> 0x" << to;
                    log(LogLevel::Info, out.str());
                }
            }
            branchTrace[branchTraceNext] = {from, to};
            branchTraceNext = (branchTraceNext + 1u) % kBranchTraceCapacity;
            if (branchTraceFilled < kBranchTraceCapacity)
                ++branchTraceFilled;
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc
                    << " ra=0x" << cpu.gpr[31]
                    << " sp=0x" << cpu.gpr[29]
                    << " gp=0x" << cpu.gpr[28];
                const uint32_t raPhys = physicalAddress(cpu.gpr[31]);
                for (const auto &[moduleId, module] : modules)
                {
                    if (raPhys >= module.base && raPhys < module.base + module.size)
                    {
                        out << " caller=" << module.name
                            << "+0x" << (raPhys - module.base);
                        break;
                    }
                }
                if (IopThread *thread = kernel.currentThread();
                    thread != nullptr && &thread->cpu == &cpu)
                {
                    out << " thread=" << std::dec << thread->id
                        << " entry=0x" << std::hex << thread->entry
                        << " stack=0x" << thread->stackBase
                        << "+0x" << thread->stackSize;
                }
                log(LogLevel::Error, out.str());
                const auto dumpRange = [&](uint32_t start, uint32_t end)
                {
                    const uint32_t from = physicalAddress(start) & ~0xFu;
                    const uint32_t to = physicalAddress(end);
                    for (uint32_t line = from; line + 16u <= to; line += 16u)
                    {
                        std::ostringstream row;
                        row << "[IOP]   mem 0x" << std::hex << std::setw(8)
                            << std::setfill('0') << line << ": ";
                        for (uint32_t i = 0u; i < 16u; ++i)
                        {
                            row << std::hex << std::setw(2) << std::setfill('0')
                                << static_cast<uint32_t>(read8(line + i)) << ' ';
                        }
                        log(LogLevel::Error, row.str());
                    }
                };
                dumpRange(cpu.gpr[29] - 64u, cpu.gpr[29] + 256u);
                if (IopThread *thread = kernel.currentThread();
                    thread != nullptr && &thread->cpu == &cpu)
                {
                    dumpRange(thread->stackBase, thread->stackBase + thread->stackSize);
                }
                for (size_t i = 0; i < branchTraceFilled; ++i)
                {
                    const BranchTraceEntry &entry =
                        branchTrace[(branchTraceNext + i) % kBranchTraceCapacity];
                    std::ostringstream line;
                    line << "[IOP]   branch 0x" << std::hex << entry.from
                         << " -> 0x" << entry.to;
                    log(LogLevel::Error, line.str());
                }
                if (const char *dumpPrefix = std::getenv("PS2X_IOP_RAM_DUMP");
                    dumpPrefix != nullptr)
                {
                    static unsigned dumpCount = 0u;
                    const std::string file = std::string(dumpPrefix) +
                                             std::to_string(dumpCount++) + ".bin";
                    std::ofstream out(file, std::ios::binary);
                    const auto ram = memory.ram();
                    out.write(reinterpret_cast<const char *>(ram.data()),
                              static_cast<std::streamsize>(ram.size()));
                    std::ostringstream note;
                    note << "[IOP]   ram dump -> " << file;
                    log(LogLevel::Error, note.str());
                }
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            if (const auto import = imports.decode(cpu.pc))
            {
                const uint32_t importPc = cpu.pc;
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                {
                    recordBranch(importPc, cpu.pc);
                    return true;
                }
                // A wake-up import may have readied a higher-priority thread;
                // real hardware switches before the caller executes another
                // instruction. (Interrupt-context calls use a temporary CPU
                // and are not the current thread, so they keep running until
                // the interrupt returns, like the i* variants.)
                (void)kernel.preemptIfHigherPriority(cpu);
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const uint32_t previousPc = cpu.pc;
            const bool running = cpuCore.executeInstruction(cpu);
            if (cpu.pc != previousPc + 4u)
                recordBranch(previousPc, cpu.pc);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                advanceSpu2();
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            {
                static uint32_t s_callFunctionLogCount = 0u;
                if (s_callFunctionLogCount < 256u)
                {
                    ++s_callFunctionLogCount;
                    std::ostringstream out;
                    out << "[IOP] callFunction addr=0x" << std::hex << address
                        << " a0=0x" << a0 << " a1=0x" << a1
                        << " gp=0x" << gp << " depth=" << std::dec << depth;
                    log(LogLevel::Info, out.str());
                }
            }
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            return cpu.gpr[2];
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                {
                    if (std::getenv("PS2X_IOP_DMAIRQ_TRACE") != nullptr)
                    {
                        std::ostringstream out;
                        out << "[IOP] dma-irq fire cause=" << std::dec << irq << " cycles=" << totalCycles;
                        log(LogLevel::Info, out.str());
                    }
                    (void)intrman.dispatchInterrupt(irq, *this);
                }
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        if (std::getenv("PS2X_IOP_GUESTCB_TRACE") != nullptr)
                        {
                            std::ostringstream cbOut;
                            cbOut << "[IOP] guest-callback fn=0x" << std::hex << callback.function
                                  << " arg=0x" << callback.argument << " cycles=" << std::dec << totalCycles;
                            log(LogLevel::Info, cbOut.str());
                        }
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void maybeDumpIdleThreads()
        {
            static const bool s_dump = std::getenv("PS2X_IOP_IDLE_DUMP") != nullptr;
            if (!s_dump)
                return;
            if (totalCycles - lastIdleDumpCycle < 33000000ull)
                return;
            lastIdleDumpCycle = totalCycles;
            std::ostringstream out;
            out << "[IOP] idle thread-dump cycles=" << std::dec << totalCycles;
            kernel.forEachThread([&](const IopThread &thread)
                                 {
                                     static constexpr const char *kStates[] = {
                                         "Dormant", "Ready", "Running", "Sleep",
                                         "Delay", "Semaphore", "EventFlag", "Suspended", "Dead",
                                     };
                                     const auto stateIndex = static_cast<size_t>(thread.state);
                                     out << "\n  tid=" << thread.id
                                         << " state=" << (stateIndex < 9u ? kStates[stateIndex] : "?")
                                         << " prio=0x" << std::hex << thread.priority
                                         << " pc=0x" << thread.cpu.pc
                                         << " ra=0x" << thread.cpu.gpr[31]
                                         << " sp=0x" << thread.cpu.gpr[29]
                                         << " waitId=" << std::dec << thread.waitId
                                         << " waitBits=0x" << std::hex << thread.waitBits
                                         << " wake=0x" << thread.wakeCycle;
                                     for (const auto &[moduleId, module] : modules)
                                     {
                                         if (thread.cpu.pc >= module.base && thread.cpu.pc < module.base + module.size)
                                         {
                                             out << " in=" << module.name << "+0x" << (thread.cpu.pc - module.base);
                                             break;
                                         }
                                     } });
            std::ostringstream intrOut;
            intrOut << "[IOP] idle intc-handlers:";
            intrman.forEachHandler([&](int irq, uint32_t function)
                                   { intrOut << " cause=" << irq << "@0x" << std::hex << function; });
            log(LogLevel::Info, intrOut.str());
            log(LogLevel::Info, out.str());
            if (const char *pokes = std::getenv("PS2X_IOP_POKE"); pokes != nullptr)
            {
                std::istringstream pin(pokes);
                std::string pitem;
                while (std::getline(pin, pitem, ','))
                {
                    uint32_t paddr = 0u;
                    uint32_t pval = 0u;
                    if (std::sscanf(pitem.c_str(), "%x:%x", &paddr, &pval) == 2)
                        memory.write32(paddr, pval);
                }
            }
            if (const char *ranges = std::getenv("PS2X_IOP_DUMP_RANGES"); ranges != nullptr)
            {
                std::istringstream in(ranges);
                std::string item;
                while (std::getline(in, item, ','))
                {
                    uint32_t start = 0u;
                    unsigned len = 0u;
                    if (std::sscanf(item.c_str(), "%x:%x", &start, &len) != 2 || len == 0u)
                        continue;
                    std::ostringstream memOut;
                    memOut << "[IOP] idle mem 0x" << std::hex << start << " len=0x" << std::hex << len << ":";
                    for (unsigned off = 0u; off < len; off += 4u)
                        memOut << " " << std::setw(8) << std::setfill('0') << std::hex << memory.read32(start + off);
                    log(LogLevel::Info, memOut.str());
                }
            }
        }

        void advanceSpu2() noexcept
        {
            if (spu2Host == nullptr || totalCycles < nextSpu2Cycle)
                return;
            // Drive the SPU2 at sub-sample granularity (768 IOP cycles per
            // 48 kHz sample at 36.864 MHz) so IRQs land close to their cycle.
            nextSpu2Cycle = totalCycles + 768u;
            try
            {
                const uint64_t irqs = spu2Host->advance(totalCycles);
                for (int irq = 0; irq < 64; ++irq)
                {
                    if ((irqs & (uint64_t{1} << irq)) != 0u)
                        pendingDmaInterrupts[irq] = totalCycles;
                }
            }
            catch (...)
            {
                // Host audio must never take the emulator down.
            }
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    advanceSpu2();
                    servicePendingDmaInterrupts();
                    servicePendingGuestCallbacks();
                    timrman.serviceDue(totalCycles, *this);
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (next != nullptr &&
                        firstRunLogged.insert(next->id).second)
                    {
                        std::ostringstream out;
                        out << "[IOP] thread " << std::dec << next->id << " first-run pc=0x"
                            << std::hex << next->cpu.pc << " sp=0x" << next->cpu.gpr[29]
                            << " gp=0x" << next->cpu.gpr[28]
                            << " ra=0x" << next->cpu.gpr[31];
                        log(LogLevel::Info, out.str());
                    }
                    if (!next)
                    {
                        maybeDumpIdleThreads();
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory);
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            const uint32_t startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            memory.freeAllocation(it->second.base);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        IopSpu2Host *spu2Host = nullptr;
        uint64_t nextSpu2Cycle = 0;

        // Ring buffer of taken branches (delay-slot pc -> target), dumped when
        // execution leaves RAM so the path into the bad jump is visible.
        struct BranchTraceEntry
        {
            uint32_t from = 0;
            uint32_t to = 0;
        };
        static constexpr size_t kBranchTraceCapacity = 64u;
        std::array<BranchTraceEntry, kBranchTraceCapacity> branchTrace{};
        size_t branchTraceNext = 0u;
        size_t branchTraceFilled = 0u;
        std::set<int> firstRunLogged;
        uint32_t nextModuleId = 1;
        uint64_t totalCycles = 0;
        uint64_t lastIdleDumpCycle = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
    }

    void IopEmulator::setSpu2Host(IopSpu2Host *host) noexcept
    {
        m_impl->spu2Host = host;
        m_impl->nextSpu2Cycle = 0u;
        m_impl->memory.setSpu2Host(host);
        if (host != nullptr)
            host->setIopRam(m_impl->memory.ramData(), IopMemory::RamSize);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
