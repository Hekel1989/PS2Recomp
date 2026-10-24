#include "runtime/ee_fiber.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include <sys/mman.h>
#include <unistd.h>

#if !defined(__x86_64__) || defined(_WIN32)
#error "The EE fiber backend is x86-64 SysV only; add the Win32 Fiber backend here."
#endif

// void ps2xEeSwitchContext(void **saveSp, void *newSp)
//
//   push rbp; push rbx; push r12; push r13; push r14; push r15
//   sub 8, rsp; stmxcsr (rsp)
//   *saveSp = rsp
//   rsp = newSp
//   ldmxcsr (rsp); add 8, rsp
//   pop r15; pop r14; pop r13; pop r12; pop rbx; pop rbp
//   ret
//
// The saved frame is: mxcsr, r15, r14, r13, r12, rbx, rbp, return address.
// System V AMD64 requires the callee-saved GPRs to survive the call; MXCSR is
// saved as well so a guest path that changes the host FP control state cannot
// leak it into another guest thread.
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl ps2xEeSwitchContext\n"
    ".type ps2xEeSwitchContext,@function\n"
    "ps2xEeSwitchContext:\n"
    "    pushq %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    subq  $8, %rsp\n"
    "    stmxcsr (%rsp)\n"
    "    movq  %rsp, (%rdi)\n"
    "    movq  %rsi, %rsp\n"
    "    ldmxcsr (%rsp)\n"
    "    addq  $8, %rsp\n"
    "    popq  %r15\n"
    "    popq  %r14\n"
    "    popq  %r13\n"
    "    popq  %r12\n"
    "    popq  %rbx\n"
    "    popq  %rbp\n"
    "    retq\n"
    ".size ps2xEeSwitchContext, .-ps2xEeSwitchContext\n");

namespace ps2x::ee
{
    namespace
    {
        constexpr uint8_t kStackPaint = 0xA5u;

        // The dispatcher's stack pointer, saved by switchIn().  Only used on
        // the executor thread.
        thread_local void *t_schedulerSp = nullptr;

        // The fiber that the next trampoline entry belongs to.
        thread_local Fiber *t_startingFiber = nullptr;

        bool fiberStatsEnabled()
        {
            static const bool enabled = std::getenv("PS2X_EE_FIBER_STATS") != nullptr;
            return enabled;
        }

        std::size_t pageSize()
        {
            static const std::size_t size = []
            {
                const long value = ::sysconf(_SC_PAGESIZE);
                return value > 0 ? static_cast<std::size_t>(value) : std::size_t{4096};
            }();
            return size;
        }

        std::size_t roundUp(std::size_t value, std::size_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }
    } // namespace

    Fiber::Fiber(const char *name, std::size_t stackSize)
        : m_name(name)
    {
        const std::size_t page = pageSize();
        m_stackSize = roundUp(stackSize == 0u ? (2u * 1024u * 1024u) : stackSize, page);
        m_mappingSize = m_stackSize + page;
        m_mapping = ::mmap(nullptr,
                           m_mappingSize,
                           PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS,
                           -1,
                           0);
        if (m_mapping == MAP_FAILED)
        {
            m_mapping = nullptr;
            throw std::bad_alloc();
        }
        // Lowest page is a guard: a stack overflow faults here instead of
        // silently overwriting another thread's stack.
        ::mprotect(m_mapping, page, PROT_NONE);
        m_stack = static_cast<uint8_t *>(m_mapping) + page;
        if (fiberStatsEnabled())
        {
            std::memset(m_stack, kStackPaint, m_stackSize);
            m_painted = true;
        }
        prepare(nullptr, nullptr);
    }

    Fiber::~Fiber()
    {
        if (m_mapping != nullptr)
        {
            ::munmap(m_mapping, m_mappingSize);
        }
    }

    void Fiber::prepare(Entry entry, void *argument) noexcept
    {
        m_entry = entry;
        m_argument = argument;

        uintptr_t top = reinterpret_cast<uintptr_t>(m_stack) + m_stackSize;
        top &= ~static_cast<uintptr_t>(15);

        auto push = [&top](uintptr_t value)
        {
            top -= sizeof(uintptr_t);
            *reinterpret_cast<uintptr_t *>(top) = value;
        };

        // High to low.  The top slot is the fake return address that the
        // trampoline sees as its caller; after switchContext's ret, rsp is
        // top (8 mod 16), exactly as at a normal function entry.
        push(0u);
        push(reinterpret_cast<uintptr_t>(&Fiber::trampoline));
        push(0u); // rbp
        push(0u); // rbx
        push(0u); // r12
        push(0u); // r13
        push(0u); // r14
        push(0u); // r15
        uint32_t mxcsr = 0x1F80u;
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        push(static_cast<uintptr_t>(mxcsr));
        m_sp = reinterpret_cast<void *>(top);
    }

    void Fiber::switchIn()
    {
        t_startingFiber = this;
        ps2xEeSwitchContext(&t_schedulerSp, m_sp);
    }

    [[noreturn]] void Fiber::switchOut()
    {
        ps2xEeSwitchContext(&m_sp, t_schedulerSp);
        // The dispatcher never switches back into an abandoned fiber.
        std::abort();
    }

    void Fiber::trampoline()
    {
        Fiber *self = t_startingFiber;
        t_startingFiber = nullptr;
        self->run();
    }

    [[noreturn]] void Fiber::run()
    {
        m_entry(m_argument);
        switchOut();
    }

    std::size_t Fiber::peakStackBytes() const noexcept
    {
        if (!m_painted)
        {
            return 0u;
        }
        const auto *begin = static_cast<const uint8_t *>(m_stack);
        const auto *end = begin + m_stackSize;
        const auto *first = begin;
        while (first < end && *first == kStackPaint)
        {
            ++first;
        }
        return static_cast<std::size_t>(end - first);
    }
} // namespace ps2x::ee
