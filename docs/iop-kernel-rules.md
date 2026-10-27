> Copied from the Suikoden Tactics recomp, where these rules were worked out against its sound and
> disc modules. Paths such as `third_party/PS2Recomp/` and `bringup.md` refer to that repository.

# IOP kernel rules: where our kernel differs from the real one

The game's sound and disc work runs as real IOP modules (`data/iso/IOP/*.IRX`) on our
interpreter, on top of a kernel we wrote in C++ (`third_party/PS2Recomp/ps2xIOP/src/emulator/`).
Those modules were written for Sony's kernel, so every rule our kernel gets wrong is a race or
a hang waiting to happen. The opening freezes (the book, the black screen after Razril) and
probably the battle hang are such races. This file lists the rules the game's modules rely
on, how the real kernel behaves, and what ours does. It is the spec for the fix; the plan
and the reasons for it are in "Opening freezes" in `bringup.md`.

## References

Agreed order (the user, 2026-10-26): when Play! and OpenPS2BIOS agree, follow them; when they
differ, follow Play! first (the more mature project), then OpenPS2BIOS; design our own only when
neither works.

- **Play!** (BSD-2, runs games without a BIOS by reimplementing the IOP kernel):
  `Source/iop/Iop_SifCmd.cpp` (the RPC loop it assembles into IOP memory, `ProcessInvocation`,
  `FinishExecRequest`), `Source/iop/IopBios.cpp` (threads, semaphores, scheduling):
  <https://github.com/jpd002/Play->.
- **OpenPS2BIOS** (MIT, no Sony code): `src/common/iopk.c` (threads, semaphores, event
  flags), `src/iop/kernel.c` (interrupts, preemption, VBlank, alarms), `src/iop/sifrpc.c`
  (the RPC server loop). Its kernel boots retail games in PCSX2. It is our working reference
  implementation: <https://github.com/TamaribaEmu/OpenPS2BIOS>.
- **PS2SDK** headers (`thbase.h`, `thsemap.h`, `thevent.h`, `sifrpc.h`): the API and error codes.
- **PCSX2**: timing (EE:IOP clock 8:1, SIF transfer cost).

## Which kernel calls the game uses

From the import tables of the ten modules the game loads (a scan of the IRX files' import stubs):

| library | ordinals used | by |
|---|---|---|
| thbase | 4 Create, 5 Delete, 6 Start, 8 Exit, 10 Terminate, 14 ChangePriority, 20 GetThreadId, 22/23 ReferStatus, 24 Sleep, 26 iWakeup, 28 iCancelWakeup, 33 Delay, 35/36 SetAlarm/iSetAlarm, 37/38 CancelAlarm/iCancelAlarm, 39 USec2SysClock | all; alarms: CDVDSTM, MCMAN |
| thsemap | 4 Create, 5 Delete, 6 Signal, 7 iSignal, 8 Wait, 11 ReferStatus | SDSTR3, IOPMAIN, CDVDSTM, MCMAN, PADMAN, SIO2MAN |
| thevent | 4–10 (Create, Delete, Set, iSet, Clear, iClear, Wait) | LIBSD, CDVDSTM, PADMAN, SIO2MAN |
| sifcmd | 14–25, 28 (RPC: Init, Register, SetQueue, RpcLoop, GetNext, Exec, Call…) | SDSTR3, IOPMAIN, SDRDRV, MCSERV, PADMAN |
| vblank | 4 WaitVblankStart, 8/9 Register/ReleaseVblankHandler | SDSTR3, IOPMAIN (4); PADMAN (8, 9) |
| intrman | 4–7, 17, 18, 23 | LIBSD, PADMAN, SIO2MAN, … |
| timrman | 4, 6, 7, 9, 11, 16, 20, 22, 23 | LIBSMF2, PADMAN |

## The rules

Each row: the rule, then what ours does (all paths under `ps2xIOP/src/emulator/`).

| # | rule (real kernel) | ours | impact |
|---|---|---|---|
| R1 | **RPC handlers run on the module's server thread.** `sceSifRpcLoop` loops: take the next request from the queue, call the handler on this thread, reply, `SleepThread` when the queue is empty. The handler may block like any thread code. The EE's call completes only when the reply is sent. | `RpcLoop` puts the server thread to sleep for good (`services/iop_rpc.cpp:252`). The handler runs at once inside the EE's call, outside any thread (`iop_rpc.cpp:335`), and the EE sees it complete in zero IOP time (`ps2xRuntime/.../Syscalls/RPC.cpp:565`). | **High.** The handler can't block, and the server thread's priority is ignored. |
| R2 | **A wait that cannot be satisfied blocks the calling thread.** Blocking is only legal in thread context; interrupt context uses the `i*` calls, which never block. | With no current thread, `WaitSema` and `SleepThread` return as if satisfied, without taking the semaphore (`core/iop_kernel.cpp` WaitSema, Sleep). `WaitEventFlag` returns -418. | **High.** Mutual exclusion silently breaks: IOPMAIN's command block was overwritten (the black screen); SDSTR3's CD lock loses an unlock (the book). |
| R3 | **No time slicing.** A thread runs until it blocks, exits, rotates the ready queue, or a higher-priority thread becomes ready. Threads of equal priority never interrupt each other. | Every 256 instructions (`kDefaultSlice`) the running thread goes back to Ready, and the next thread of the same priority runs, in thread-id order (`iop_emulator.cpp` runCycles, `core/iop_kernel.cpp` beginNextReady). | **High.** Code that relies on equal-priority threads not interleaving (unguarded shared words, such as SDSTR3's command word at 0x5ca90) races. |
| R4 | **The ready queue is FIFO within a priority.** A thread goes to the back when it becomes ready, and keeps its place when preempted by a higher priority. | Order is by thread id after the last thread scheduled, so a preempted thread can lose its turn. | Medium. Changes which thread runs first after a VBlank. |
| R5 | **Preemption is immediate.** When an interrupt, a delay expiry or a call readies a higher-priority thread, it runs before the current thread executes another instruction (after the interrupt handler returns). | Wake calls check this (`iop_emulator.cpp` step), but interrupt handlers and delay expiry only take effect at the next slice boundary (up to 256 instructions). | Low to medium, and R3's fix will lengthen this unless it is handled. |
| R6 | **Semaphore wait queues follow their attribute**: FIFO by default, priority order with `SA_THPRI` (0x1). `SignalSema` hands the count straight to the first waiter. With no waiter it increments the count, or returns `KE_SEMA_OVF` (-420) at the maximum. | Always the highest priority, ties by thread id. At the maximum the count is dropped silently. | Medium. |
| R7 | **Deleting a semaphore or event flag releases its waiters** with `KE_WAIT_DELETE` (-425). | Semaphore waiters stay blocked forever; event-flag waiters get -1. | Low (the game rarely deletes them while in use). |
| R8 | **Event flags wake satisfied waiters in queue order**, and a waiter with the clear mode stops the rest. A flag without `EA_MULTI` refuses a second waiter (`KE_EVF_MULTI`, -422). | Thread-id order; no single-waiter check. | Low to medium (LIBSD and CDVDSTM wait on event flags). |
| R9 | **Suspending a waiting thread keeps its wait** (WAITSUSPEND); it stays suspended after the wait ends. | A `suspended` flag is kept beside the wait state, following OpenPS2BIOS `sched.c` (Play! has no IOP suspend): a waiter keeps waiting, ends its wait ready but is not scheduled until resumed; `ReferThreadStatus` reports 8/12. | Low (not imported by the game's modules). |
| R10 | **Alarms fire.** `SetAlarm`/`iSetAlarm` call the handler in interrupt context after the delay; a non-zero return re-arms it. | Ordinals 35–38 return 0 and the alarm never fires (`core/iop_kernel.cpp`). | **Unknown, possibly high.** CDVDSTM (disc streaming) uses `iSetAlarm`/`CancelAlarm`; MCMAN uses `SetAlarm`. |
| R11 | **VBlank handlers run.** `RegisterVblankHandler` calls the handler at every VBlank start or end. | A no-op (`imports/iop_vblank.cpp`). | Low if PADMAN's work is fully replaced on the EE side; to check. |
| R12 | **`ReleaseWaitThread` wakes a waiting thread with `KE_RELEASE_WAIT` (-418)**, and returns `KE_NOT_WAIT` if it isn't waiting. `ChangeThreadPriority` moves a ready thread to the back of its new priority and can preempt; priority 0 means the initial priority. | The woken thread sees 0; priority changes don't reorder or preempt. | Low. |

| R13 | **A module's start function runs in a thread** (the loader's), so it can block. | `loadImage` runs it on a loader thread (Play!'s module starter: priority 8, 0x4000 stack) and runs the scheduler until it returns, with a bounded wait; `startResult` is its return value. In the diagnostic run, LIBSD's start skipped a `WaitEventFlag` this way (`[iop-wait] no-thread WaitEventFlag`, ra 0x4a1a4). | Unknown; LIBSD's start waits for the SPU2. |
| R14 | **Thread start is one-shot and initialises the stack.** `StartThread`/`StartThreadArgs` on a thread that is not dormant fail with `KE_NOT_DORMANT` (-414) and change nothing. A successful start resets the current priority to the initial priority. `CreateThread` fills the new stack with 0xFF unless the attribute has `TH_NO_FILLSTACK` (0x00100000), leaving the top 0x30 bytes unfilled (OpenPS2BIOS); `StartThread` then zeroes the top 0xB8 bytes (both references). The initial SP is Play!'s stack top minus `STACK_FRAME_RESERVE_SIZE` (0x10), kept 16-byte aligned. `StartThreadArgs` copies its arguments to the stack top and passes the copy's address in `a1`. | `core/iop_kernel.cpp` `dispatchThreadImport` cases 4, 6 and 7, plus `startModuleThread`, implement this in patch `0131-iop-startthread-rules`; the module starter thread gets the same fill and start-frame clear. | Medium to high: restarting a live thread lost its stack and registers (SDSTR3's stream thread), and modules rely on the zeroed start frame. |

## Status

| rule | state |
|---|---|
| R3, R4, R5, R6, R7, R8, R10, R12, interrupt context (blocking calls fail with -100, `QueryIntrContext`) | done in patch `0125-iop-kernel-rules`, unit tests in `tests/iop/iop_kernel_rules_test.cpp` |
| R1, R2 (RPCs) | done in patch `0126-iop-rpc-server-thread`, following Play! and OpenPS2BIOS (they agree): queue, wake the server thread, handler on that thread, reply on return. The EE call returns when the reply is sent and is charged the IOP time (×8 EE cycles). Not yet faithful: the EE doesn't run other threads while it waits, and a no-wait call doesn't overlap with EE work. |
| R9, R13 | done in patch `0127-iop-module-start-thread`, unit tests in `tests/iop/iop_kernel_rules_test.cpp`. R13 follows Play! `Source/iop/IopBios.cpp` (`RequestModuleStart`, `TriggerCallback`, `AssembleModuleStarterProc`) and `IopBios.h` (`MODULE_INIT_PRIORITY` = 8, `DEFAULT_STACKSIZE` = 0x4000): a dedicated starter thread runs the entry. OpenPS2BIOS `src/iop/loadcore.c` differs: it runs the entry on the calling thread (the SIF service thread for an EE load) with the priority raised to 8. Play!'s dedicated thread works here, so it is used. Play! also delays 0x4000 ticks after the entry before replying; we return as soon as the entry returns. The wait is bounded (0.5 s of IOP time) and logs if exceeded. R9 follows OpenPS2BIOS `src/common/sched.c` (`ob_thread_suspend`, `ob_thread_resume`, `ob_thread_status`, `ob_sched_finish`), because Play! has no IOP suspend implementation: a `suspended` flag separate from the wait state, with status 8/12. |
| R14 | done in patch `0131-iop-startthread-rules`, unit tests in `tests/iop/iop_kernel_rules_test.cpp`. Follows Play! `Source/iop/IopBios.cpp` (`CreateThread`, `StartThread`, `StartThreadArgs`) and OpenPS2BIOS `src/iop/kernel.c` (`CreateThread`, `clear_start_frame`, `StartThread`) / `src/common/iopk.c` (`obk_thread_start`). Where they differ, Play! wins: the refusal returns -1 with a log line (OpenPS2BIOS returns -414); CreateThread fills the whole stack with 0xFF and ignores `TH_NO_FILLSTACK` (OpenPS2BIOS honours it and leaves the top 0x30 bytes); the initial SP is Play!'s stack top minus `STACK_FRAME_RESERVE_SIZE` (0x10) and 16-byte aligned (OpenPS2BIOS uses the 8-byte aligned stack top without the reserve); `StartThreadArgs` follows Play! for the argument copy and its SP (copy address minus 0x10) and does not clear the 0xB8 start frame (OpenPS2BIOS aligns SP down to 8 and clears 0xB8 bytes below the copied arguments). The priority reset and `StartThread`'s top 0xB8 clear are the same in both references. `startModuleThread` gets the same stack treatment. |
| R11 | open |

## Order of work

1. **R3 + R4 + R5 together** (the scheduler): pick by (priority, ready order). The running thread
   keeps its place, so equal priorities no longer interleave. Re-pick after every interrupt
   or wake, and when a delay expires.
2. **R1 + R2** (the RPC server thread): the request queue and loop as in OpenPS2BIOS
   `sifrpc.c`; the EE call completes when the handler returns; drop patch 0123's
   `drainReadyThreads`. A blocking wait with no thread becomes a logged error, not a success.
3. **R10, R6, R8, R7, R12, R11.** R9 and R13 are done.

Each rule gets a unit test in the runtime's test suite whose expected result comes from the
reference. The diagnostic `[iop-wait]` log lines (patch 0124) count the R2 cases in a run:
they must drop to zero after step 2.
