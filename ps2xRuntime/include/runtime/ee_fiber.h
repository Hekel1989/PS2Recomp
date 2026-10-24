#pragma once

#include <cstddef>

// Host stacks for the EE scheduler.
//
// The exception-based dispatcher ran every guest dispatch on the executor
// thread's stack and transferred by throwing EeDispatcherTransfer, which
// unwound the generated call chain (a C++ throw plus the libgcc unwinder on
// every block/yield).  A Fiber is a private host stack for guest execution:
// the dispatcher switches to it before calling a recompiled function and the
// guest switches back when it blocks, yields, exits or invokes.
//
// The fiber is not resumed after a transfer.  Guest state lives in
// R5900Context, exactly as it did across the throw, and the dispatcher
// re-enters at context.pc (or runs the queued invocation) on a fresh stack.
// That keeps the guest-visible semantics and native object lifetimes of the
// old path while removing the unwinder from the hot path.
//
// Nothing with a non-trivial destructor may be live on the host stack at a
// transfer point: the stack is abandoned rather than unwound.  Every transfer
// site is a scheduler method called as a leaf from a syscall wrapper, and
// completions/vectors are moved into scheduler state before the switch.
//
// The switch and the stack allocation live in this one header/implementation
// pair so a Windows backend can replace them with ConvertThreadToFiber /
// CreateFiber / SwitchToFiber behind the same interface.
namespace ps2x::ee
{
    class Fiber
    {
    public:
        using Entry = void (*)(void *);

        Fiber(const char *name, std::size_t stackSize);
        ~Fiber();

        Fiber(const Fiber &) = delete;
        Fiber &operator=(const Fiber &) = delete;

        // Arm the fiber for a new dispatch.  The previous contents of the
        // stack are dead; only the top is reset.
        void prepare(Entry entry, void *argument) noexcept;

        // Dispatcher -> fiber.  Returns when the fiber switches back.
        void switchIn();

        // Fiber -> dispatcher.  The fiber is never resumed, so this does not
        // return into the guest stack.
        [[noreturn]] void switchOut();

        // Deepest stack use since construction, when PS2X_EE_FIBER_STATS=1 was
        // set at construction (0 otherwise).  The stack is painted once.
        [[nodiscard]] std::size_t peakStackBytes() const noexcept;
        [[nodiscard]] std::size_t stackSize() const noexcept { return m_stackSize; }
        [[nodiscard]] const char *name() const noexcept { return m_name; }

    private:
        static void trampoline();
        [[noreturn]] void run();

        const char *m_name = nullptr;
        void *m_mapping = nullptr;
        std::size_t m_mappingSize = 0;
        void *m_stack = nullptr;
        std::size_t m_stackSize = 0;
        void *m_sp = nullptr;
        Entry m_entry = nullptr;
        void *m_argument = nullptr;
        bool m_painted = false;
    };

    // A fast x86-64 SysV context switch.  Saves the callee-saved registers and
    // MXCSR on the current stack, stores the stack pointer through saveSp,
    // then loads newSp and returns on the new stack.  The first return on a
    // prepared fiber lands in Fiber::trampoline().
    extern "C" void ps2xEeSwitchContext(void **saveSp, void *newSp);
} // namespace ps2x::ee
