# VU1 backends

`VU1Interpreter` (and the AOT-compiled programs built on it) emulates VU1 exactly: PS2
float results and a cycle-by-cycle pipeline scoreboard.  That is the reference, but it is
slow: at 60 fps a 3D-heavy game can saturate a core with it, even on its own thread
(`docs/vu1-thread.md`).  `runtime/ps2_vu1_backend.h` lets the host replace it with a
recompiler such as PCSX2's microVU1, which works out stalls and flag instances at compile
time and uses SSE maths with PCSX2's clamping (not bit-exact, but what PCSX2 ships).

## The hook

The host fills a `ps2x::Vu1Backend` and calls `ps2x::setVu1Backend()` before
`PS2Runtime::initialize()`:

| field | meaning |
|---|---|
| `micro`, `data` | 16 KiB each.  The runtime keeps VU1 memory in them (`PS2Memory::useVu1Storage`), because a JIT encodes their addresses into its code. |
| `reset()` | drop compiled programs, zero VU1 registers |
| `run(args)` | run from `startPc` (MSCAL/MSCALF) or continue at TPC (`resume`, MSCNT) with VIF1 `TOP`/`ITOP`, VU0 `FBRST`, a cycle budget and the micro-memory generation (`PS2Memory::getVU1CodeGeneration()`; a new value means micro memory changed).  Each XGKICK packet goes whole to `args.sink`.  Returns E-bit / D-bit / T-bit stops. |
| `readRegisters`, `writeRegisters` | VF/VI/ACC/Q/P/I/R and the three flags, for the diff mode |

Plain types only: the backend's own (e.g. GPL PCSX2) types never reach the runtime.  The
runtime calls `run()` from the thread that runs VIF1 (the VU1 thread when it is on), never
concurrently, and submits the packets as PATH1 through the normal GIF path, so the VU1
thread's ordered log and fences are unchanged.

## Switches

| variable | effect |
|---|---|
| `PS2X_VU1_JIT=0` | ignore the backend (interpreter/AOT) |
| `PS2X_VU1_JIT=diff` | run every kick on the backend and on the interpreter from the same registers and VU memory; the interpreter stays authoritative; `[vu1-jit-diff]` lines count identical kicks and packets, packet count/size mismatches, differing words by ulp, VU-memory and register differences |
| `PS2X_VU1_JIT_DIFF_DUMP=<file>` | with `diff`: write the inputs of the first structural mismatch and of the kick before it, for a local replay |
| `PS2X_VU1_THREAD=verify` | keeps the interpreter (its shadow compares bit for bit) |

Reading a diff: a recompiler with SSE maths differs by 1-15 ulp in many floats (vertex
positions, STQ, colours) and in VI registers that hold float bits; that is expected.  Any
difference in packet count or size is a real bug on one side.

## Interpreter fix found this way

A second XGKICK must stall until PATH1 has sent the first packet.  The interpreter only
stalled one cycle and then restarted the transfer, so the first packet of a back-to-back
pair (typically a small A+D header such as `TEX0`, then the strip) was lost.
`startXgkick()` now advances cycles while a transfer is active; the existing test "a second
XGKICK stalls until the active PATH1 transfer completes" passes with it.

## Tests

`ps2xTest/src/ps2_vu1_backend_tests.cpp` (suite `PS2VU1Backend`): VU1 memory moves into the
host's buffers and stays there across `initialize()`; kicks reach the backend with the VIF1
state; its packets reach the GIF as PATH1; stop bits come back.

A microVU1 backend for x86-64 (vendored PCSX2 v2.8.2 microVU and its emitter, plus a
dozen stand-in headers) is in suikoden-tactics-recomp `third_party/pcsx2_vu`; see its `UPSTREAM.md`.
