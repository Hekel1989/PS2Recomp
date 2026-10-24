# VU1 thread

VIF1 command processing and VU1 execution run on their own host thread
(`ps2x-vu1`).  The game (EE) thread keeps everything else, including the GS.
The design goal is **exact equivalence with the serial runtime**: the game can
never observe a difference, and the GS receives the identical packet stream in
the identical order.  `PS2X_VU1_THREAD=0` forces the old serial path (a
developer switch for A/B and diffing, never a user option), and
`PS2X_VU1_THREAD=verify` proves the equivalence in-process on every packet.

## Why VIF1 moves too

In the serial runtime a VIF1 DMA kick (`D1_CHCR.STR`) walks the DMA chain,
copies the bytes, runs the VIF1 interpreter over them (UNPACK/MPG write VU1
memory, MSCAL/MSCNT run VU1 to completion, DIRECT submits PATH2, XGKICK submits
PATH1) and completes the DMA, all inside the guest's store.  Moving only VU1
would race the next UNPACK/MPG of the same chain against the running program;
that hazard limited a bare VU1 thread to 0.2-2.1 ms/frame of overlap.  Running the whole VIF1 stream on the worker
keeps every VIF1/VU1 hazard inside one thread, so no double-buffering is
needed, and the next observation point is the next frame's vsync instead of
the next VIF1 kick.

## What the game thread still does at a kick

* Walks the DMA chain and copies the bytes, exactly as before (the chain walk
  never depended on VIF1 results).
* Completes the DMA exactly as before: `D_STAT`, `CHCR.STR`, `QWC`, the DMAC
  completion cause and its interrupt handler, all at the same guest point.
* Hands the copied bytes (chunk boundaries preserved, because the VIF1
  interpreter's behaviour at a chunk end is part of its semantics) and the
  `FBRST` value of the current context to the worker.

## The ordered GIF sink

The serial runtime's GS input is one sequence of *GIF-front operations*:
packet submissions (PATH1 from XGKICK, PATH2 from DIRECT/DIRECTHL, PATH3 from
GIF DMA and HLE), arbiter drains, PATH3 mask changes (MSKPATH3) and GS
privileged-register notifications.  The arbiter's path-priority sort and the
PATH3 mask FIFO make the final packet order depend on that whole sequence, so
the threaded runtime does not try to re-order packets; it **replays the same
operation sequence**:

* The game thread keeps a log.  An entry is either a game-thread operation or a
  marker for VIF1 work item *k*.
* The worker records the operations of work item *k* (its PATH1/PATH2
  submissions with their drains, its MSKPATH3 changes) into that item instead
  of executing them.
* A game-thread operation executes at once when the log is empty (the serial
  state is up to date) and is appended otherwise.
* The game thread replays the log from the front.  At a marker it replays the
  item's operations if the worker has finished it, and stops (or waits, at a
  fence) if not.

The arbiter, the PATH3 mask and its FIFO are therefore only ever touched by the
game thread, in the serial order.  The GS stays on the game thread.

## The present is an operation too

Nothing the game can observe at a VBlank (its RAM, the CSR FIELD bit, the
VBlank interrupt) depends on the worker; only the presented image does.  So the
VBlank does not wait for the frame's VU1 work.  It queues the present (the
host's `vsyncRegs` hook) in the log behind that work, with a snapshot of the GS
privileged registers taken at the VBlank:

* DISPFB/DISPLAY/PMODE/BGCOLOR/IMR come from the snapshot, because the game may
  already have written the next frame's values when the present runs;
* CSR and SIGLBLID are read when the present runs, because the replayed
  SIGNAL/FINISH/LABEL packets before it are what set them (and every
  game-thread CSR/SIGLBLID access fences first, so nothing else changes them).

The presented images are the serial runtime's images in the same order; a
frame appears when its VU1 work is done instead of at the VBlank, a few ms
later at most.  `beforeVblank()` keeps the worker at most one frame behind: the
next VBlank waits until the previous present has run, the same bound as
PCSX2's MTGS.  A host without `vsyncRegs` (one that reads the live register
block in `vsync`) gets a full fence at every VBlank instead.

## Fences (where the game could observe the worker)

A fence waits for the worker to finish everything queued and replays the whole
log.  It runs before:

| observation | why |
|---|---|
| VBlank, only while the previous present is still queued | keeps the worker at most one frame behind |
| VIF1 register read/write (`0x10003C00-0x10003DFF`) | VIF1 state is worker-owned; `FBRST` also resets the PATH3 mask |
| VU1 code/data access (`0x11008000-0x1100FFFF`) | VU1 memory is worker-owned |
| GS `CSR` / `SIGLBLID` read or write | SIGNAL/FINISH/LABEL in replayed packets update them |
| GIF fast paths and HLE stubs that call the GS directly | they bypass the ordered sink |
| a kick with `FBRST.DE1/TE1` set (and every later kick) | D/T stops write `VPU-STAT`; those kicks run serially |
| reset, shutdown | the worker's state is torn down |

`PS2X_DUMP_AT` state dumps run on the main thread, which already reads guest
memory without synchronisation; their VU1 memory is a snapshot of a moving
target with the thread on, as their EE RAM always was.

`GIF_STAT` needs no fence either.  Its only worker-dependent bit, M3P, mirrors
the PATH3 mask, and the mask depends only on the VIF1 command stream, never on
VU1.  So the game thread runs a *parse predictor*: a copy of the VIF1/PATH3
state with no VU memory, no VU1 callbacks and no GIF output, through which
every kick is walked (UNPACK and MPG payloads are skipped, so it costs little).
Its mask is the serial mask at once.  At every full fence the predictor is
compared with the real VIF1 state and a drift is logged (and resynced); verify
mode also checks every predicted value against the shadow.  One game
reads GIF_STAT 1-3 times a frame for its own flag bit while MSKPATH3 streams are
queued, and an earlier fence there cost 150-230 ms/s of game-thread waiting in
the story scenes.

`VPU-STAT` needs no fence: VU1 only sets its D/T bits when `FBRST.DE1/TE1`
enabled them at the kick, so while they have never been enabled the serial
runtime's update at each kick is a no-op.  The first kick with them enabled
switches the runtime to the serial path for good.

## Fence waits and the guest clock

The guest clock advances with the game thread's CPU time; time off the CPU
outside a guest wait counts as OS descheduling, and long descheduling becomes
slow-motion debt.  A fence wait stands in for VU1 work the serial runtime ran on
this thread, so it is reported as emulated time
(`ps2x::time::noteEmulatedWait`) and shows up in `waitMs`.

## Verification (`PS2X_VU1_THREAD=verify`)

A shadow copy of the VIF1/VU1/GIF-front state (its own VU1 memory, VIF1
registers, PATH3 mask, arbiter and `VU1Interpreter`) runs the serial semantics
inline on the game thread with the same inputs.  Every packet the real arbiter
hands to the GS is compared with the shadow's next packet (path, size, bytes);
at every fence the VU1 memories, VIF1 registers, PATH3 mask/FIFO and the VU1
register state are compared, then the game thread's own mutation is copied to
the shadow.  Any difference logs the first mismatch.

## Developer switches

| variable | effect |
|---|---|
| `PS2X_VU1_THREAD=0` | serial path (the pre-thread runtime) |
| `PS2X_VU1_THREAD=verify` | threaded + in-process shadow comparison |
| `PS2X_VU1_THREAD_STATS=1` | per-second kicks, fences by reason, game-thread wait time, late presents |
| `PS2X_VU1_THREAD_FAULT=<n>` | drops the n-th replayed packet, to show that `verify` catches it |

Diagnostics that are not thread-safe (`PS2X_VU1_OVERLAP`,
`PS2X_VU1_INVENTORY`) force the serial path.
