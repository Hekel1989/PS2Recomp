# PS2 static recomp playbook

See also `docs/upstream-candidates.md`: fixes from this project that could go back to PS2Recomp.

Lessons from recompiling Suikoden Tactics (SLUS_212.45) with
[PS2Recomp](https://github.com/ran-j/PS2Recomp) that carry over to other PS2 games. Game-specific
findings live in `docs/bringup.md`, `docs/overlays.md` and `docs/gs-port-plan.md`; this file keeps only
what you would reuse. Kept up to date as the project goes on.

Status tags: **[confirmed]** seen working in this project. **[expect]** likely for most games, but
check. **[guess]** not verified yet.

---

## 1. Before you start: triage the game

- **ISO layout.** `7z l game.iso` shows the boot ELF (`SYSTEM.CNF` → `BOOT2`), the `IOP/*.IRX` modules,
  an `IOPRP*.IMG` replacement image and the big data archives. **[confirmed]**
- **ELF.** `readelf -h -l -S`. A stripped ELF that still has section headers is the normal case for
  retail games. Look for `.vutext`/`.vudata` and `.DVP.overlay*`, which hold VU1 microcode, so the game
  needs VU1 emulation. **[confirmed]**
- **Big `.bss`.** A `.bss` of several MB is a hint that code gets loaded into RAM at runtime
  (overlays). Static recompilation of the ELF alone won't be enough. See §6. **[confirmed]**
- **VU1 microcode can be compiled at build time.** Inventory the executed images first
  (`PS2X_VU1_INVENTORY`), then parse the ELF's `.vudata` MPG chains structurally and key each
  program by an FNV-1a hash of its uploaded extent; `.vutext` may need the inventory hashes because
  the game builds the MPG commands in EE RAM. Compile every program found, keep a weak registry so
  builds without generated code link, and run the existing interpreter differential from identical
  state as the correctness gate. See `tools/vu1_aot.py` and `docs/vu1-aot.md`. **[confirmed]**
- **Don't stop at the executed programs.** The DVP overlay table (`.DVP.ovlytab` +
  `.DVP.ovlystrtab`) enumerates every overlay chunk and points straight at its raw microcode in
  `.vudata`; group entries by consecutive slot field (`region_off == slot * 0x800`) to rebuild the
  programs that never ran in your captures. Compiling them costs a few KB and makes later
  `MSCAL` paths work. **[confirmed]**
- **AOT layout: cap your basic blocks.** Specialised code per pair is ~1.8 KB and indirect calls
  per pair thrash L1I. Emitting one straight-line function per basic block, capped at ~6 pairs
  (≈11 KB, inside L1I), was worth 10-15 % over an uncapped block split and ~25 % over the v1 table
  loop. Keep a per-pair fallback for data-driven `JR`/`JALR` targets so correctness never depends
  on the block graph. **[confirmed]**
- **Self-test the generated code without the game.** Emit the program bytes into the descriptor
  and compare interpreter vs compiled from PRNG-seeded VF/VI/ACC/flags/data memory; this is how the
  never-executed overlays and the rare exact-flag paths get coverage. **[confirmed]**
- **PCSX2 game database.** Look up the serial in PCSX2's `bin/resources/GameIndex.yaml`. The
  compatibility rating and `gsHWFixes` tell you up front how hard the graphics will be. **[confirmed]**

## 2. Project layout that works

- **Pin PS2Recomp as a git submodule** and keep every local fix as a patch file in
  `patches/ps2recomp/NNNN-name.patch`:
  - `tools/apply_patches.sh` resets the submodule to the pinned commit and applies the queue as one
    local commit.
  - `tools/make_patch.sh <name>` saves the current edits as the next patch.
  - Set `ignore = all` on the submodule in `.gitmodules` so the queue commit doesn't make the
    superproject look dirty.

  **[confirmed]**
- **Never put generated code inside the submodule.** Upstream expects it in
  `ps2xRuntime/src/runner/`. Instead, keep your own top-level CMake that
  `add_subdirectory(third_party/PS2Recomp EXCLUDE_FROM_ALL)`, links `ps2_runtime`, and globs
  `generated/<game>/*.cpp`. **[confirmed]**
- **Gitignore** the ISO, `data/` (extracted files, dumps), `generated/`, `build/` and Ghidra
  projects. Commit only code, configs and patches. **[confirmed]**
- **Scripts for every step:** `extract_iso.sh` → `recompile.sh` → build → `run.sh`. Recompiling only
  takes seconds, so make regeneration cheap and routine. **[confirmed]**
- **Test on target hardware from day one.** The build machine is not the target: hidden local
  dependencies (window system, GPU driver, core count, files the game silently read from the
  checkout) only show up there. `tools/run.sh` copies the fresh build to a test box, runs it there
  and fetches the log/snapshots/WAV back, so the same checks work remotely; calibrate speed gates
  on the target and use an exclusive lock for measurements. Here the target is a 6-core Ryzen Z1
  Extreme handheld, and its vsync trace replaced frame-mtime timing. See
  `docs/remote-testing.md`. **[confirmed]**
- **Header-only host-hook seams keep the submodule free of host deps.** PS2Recomp's raylib-coupled
  presentation and input are replaced by two tiny seams: a header in `ps2xRuntime/include/runtime/`
  exposing `std::function` hooks (`ps2_present_hooks.h`: mainTick/presentationDisabled;
  `ps2_pad_host_hooks.h`: a `provider(port) -> DualShock report`), applied via `tools/make_patch.sh`.
  The app injects SDL3-backed implementations at startup; the submodule never names SDL/raylib, the
  patch stays small, and env-driven paths (e.g. `PS2X_PAD_SCRIPT`) keep priority so automated runs
  stay deterministic. Test such seams hardware-free: an env-gated thread pushes synthetic SDL events
  (`SDL_PushEvent`) and logs the reports the provider hands the game (`SUIKO_INPUT_SELFTEST=1`).
  **[confirmed]**
- **A fake peripheral hides a working guest driver.** If the IOP already executes the game's sound
  IRX modules, "no audio" usually means the SPU2 behind them is a register store, not that the game
  never sends sound. Hook the whole device block (`0x1F900000-0x1F9007FF`, DMA channels 4/7 with
  real direction/size and AutoDMA) and drive the model's clock from the IOP cycle count: 768 IOP
  cycles = one 48 kHz sample at 36.864 MHz. Keep the old store as the no-host fallback so other
  games and `SUIKO_ENABLE_* = OFF` builds are unchanged. When porting PCSX2's SPU2, replace its
  `AudioStream`/`EmuConfig` front end with a callback, not with a stub config object; and remember
  SPU2's 32-bit address registers (`TSA`, `IRQA`, voice `SSA`) are **high word at the low address**,
  unlike normal little-endian, so route 16- and 32-bit MMIO exactly as the core's RegTable expects.
  **[confirmed]**
- **Measure the stream, not just the device.** A real SPU2 can be perfectly correct while the game
  stops feeding it, and a stall the guest waits on can be a symptom of a bad transfer. Here the
  title BGM stopped after six IOP-side SDSTR3 voice transfers: the state machine's buffer-reuse
  check compared `sceSdGetAddr` (voice NAX via `0x1F9006B8/6BA`) against the ring and the voice had
  already walked past it. The voice only left the ring because our DMA glue moved half the
  requested bytes: SPU2 DMA `BCR` is in 32-bit words, PCSX2's `SPU2writeDMA*Mem` take 16-bit units
  (`size * 2`), so only the first 8 KiB of each 16 KiB stream half-buffer was written and the
  ADPCM loop-end block never arrived. Dump the SPU2 RAM ring and compare its 16-byte block flag
  bytes (bit0 loop-end, bit1 repeat, bit2 loop-start) and voice `SSA/LSA/NAX` against a reference
  savestate before interpreting a guest poll. Track DMA starts (`MADR/BCR/CHCR/TSA`), key on/off
  with SSA/pitch/ADSR, CD reads and
  the SDSTR3 state machine's branch/import calls together before blaming the audio core. Use
  `PS2X_IOP_IMPORT_TRACE` + `PS2X_IOP_INTR_TRACE` (patch 0077) to identify the guest function and
  the interrupt handler actually running, and `PS2X_SPU2_DUMP_AT` for the ring dump. **[confirmed]**
- **A stateful EE driver client must run for real, not as an HLE stub.** If the IOP already runs the
  real driver, a stub on the EE side can accept every command and return success while the device
  never sees it. Here the movie's PCM was decoded, SIF-DMA'd into the IOP ring and waiting, but
  `sceSdRemote`/`sceSdRemoteInit` were HLE stubs: the `0x8130`/`0x8070` commands never reached
  SDRDRV, so no ADMA transfer started and the SPU2 stayed silent. Un-stub the EE client
  (`general.unstub`) and let it bind the real IOP RPC server; the same guest calls then start the
  device. Tells: the device/MMIO trace is empty, the transport/DMA trace shows data arriving at the
  IOP, and the generated wrapper in the call path is `ps2_stubs::sceSd*`. **[confirmed]**

## 3. Building PS2Recomp and the generated code

- **Linux/clang.** PS2Recomp builds cleanly on Linux with clang and Ninja, even though upstream mostly
  tests with MSVC. 444 of 445 upstream tests pass. The failing one reads `instructions.h` relative to
  the working directory, so it's harmless. **[confirmed]**
- **Generated code needs `-msse4.1`** with clang/GCC. The VU macros expand to
  `_mm_blendv_ps`/`_mm_shuffle_epi8`. Upstream only handles this for MSVC (`/arch:AVX2`). **[confirmed]**
- **Generated-code flags.** Compile it with `-O2 -g0 -w` regardless of build type, and use a unity
  build (batch 64). CMake refuses to unity-batch sources that have per-source `COMPILE_OPTIONS`, so put
  those flags on the target, and exempt your own sources with `SKIP_UNITY_BUILD_INCLUSION` plus their
  own flags. **[confirmed]**
- **Scale.** About 12.8k functions came to 207 MB of C++ and a 2.5 min full build on 16–20 cores,
  giving a 127 MB binary. **[confirmed]**
- **CMake version.** The submodule needs CMake ≥ 3.21. **[confirmed]**

## 4. Function discovery

- **`ps2_analyzer`.** It works well on a stripped retail ELF: it matched 270 Sony SDK functions by
  signature (sceGs*, sceCd*, sceSd*, sceMpeg*, …) and found 296 jump tables. Its `name@0xADDR` stub
  list doubles as your symbol map. **[confirmed]**
- **Ghidra with the stock MIPS language (`MIPS:LE:64:64-32addr`) can't decode R5900-only
  instructions** (MMI, 128-bit `lq`/`sq`, …). The constructor errors make it over-split functions:
  56.8k functions, 9.3k of them ≤ 8 bytes, against the analyzer's 12.8k. Don't trust that output
  without an R5900 processor extension. Run Ghidra headless (`analyzeHeadless -import … -postScript
  ExportPS2Functions.java <toml> <csv>`). **[confirmed]**
- **Pitfall: SDK functions reached only through tail-`j` thunks are never discovered**, so their stub
  bindings are never generated and the call lands in an empty function-table slot. The symptom is the
  EE parked at that SDK address (here `sceGsSyncVCallback`). The fix is to synthesize a stub entry for
  every configured `handler@0xADDR` binding (patch 0001). **[confirmed]**
- **You can un-stub an SDK library and let the real guest code run.** `general.unstub` entries in
  `config/recomp_base.toml` (applied by `tools/recompile.sh`) remove stub entries so the library is
  recompiled normally. The emulator must then answer it like hardware: un-stubbing libfileio required
  an HLE IOP FILEIO RPC server (SID `0x80000001`, patch 0051) that reproduces the client protocol
  exactly — init version handshake, fd = echo of the client's own slot index, non-zero recv
  completion marker, and signaling the client's per-call completion sema (send+0x00) from the
  transport. Keep the SIF RPC primitives (`sceSifCallRpc`/`sceSifBindRpc`) stubbed; the HLE
  transport routes them to the IOP services. Per-library verdicts: `docs/stub-inventory.md`.
  **[confirmed]**

## 5. Runtime bring-up

- **Give the runtime the disc image.** Set `PS2Runtime::IoPaths::cdImage` before `loadELF`, because
  retail games read the big archives by LBA through `sceCdRead`. The upstream runner never sets it.
  Without it the game silently waits forever. **[confirmed]**
- **Progress signals.** The `[run:tick]` lines show `pc`/`ra` and the `dma`/`gif`/`gsw`/`vif`
  counters. If `pc` stays put across ticks, the game is stuck. `gsw`/`vif` above zero means it's
  drawing. `[guest-branch:missing-target]` names a jump into code that was never recompiled.
  **[confirmed]**
- **A wait-loop pc in a sample is not proof of a hang.** A busy-wait (WaitSema/scan/SignalSema)
  keeps the sampled `pc` on the loop even while the wait is progressing slowly. Before diagnosing
  "stuck at X", prove the loop never exits: `PS2X_EE_JMPWATCH` on the wait's call site and on a pc
  only reachable after it, and count those dispatches over the run (here the "parked" main thread
  actually passed its drain wait ~288x in 60 s - the game just paced itself to ~5 fps).
  **[confirmed]**
- **Run the game under `stdbuf -oL -eL`.** Stdout into a pipe is block-buffered, so a run killed by
  `timeout` loses its last few KB. Logs then end at a suspiciously constant line count and hide the
  real state. **[confirmed]**
- **Function coverage + static call graph beats watchpoints for "why is this code dead".** A
  recompiler flag (`general.coverage`) that emits a first-hit marker at each function entry and
  resumable loop head (zero cost when off), plus a runtime that flushes the hit set to
  `$PS2X_COVERAGE_OUT` periodically and at exit, gives the executed-function set for free.
  `tools/callgraph.py` parses the generated-code `// jal func_…` comments into a call graph and
  reports, for dead targets, their ancestors and the executed-caller→dead-callee boundary edges.
  If the boundary set is empty at every depth, the entry is an indirect call - find the dispatch
  table by scanning reference RAM for pointers to the dead functions (here: a key→handler table
  walked by a message pump, gated by one flag bit). **[confirmed]**
- **CD HLE has to model the drive state.** Games poll `sceCdStatus` each vsync until it matches a
  table of acceptable states. A constant return value hangs the boot. Track
  stop/spin/pause/read across sceCdInit/Stop/Pause/Read (patch 0008). **[confirmed]**
- **CD status stubs must block like the SIF RPC they replace.** On real hardware `sceCdStatus`,
  `sceCdDiskReady`, `sceCdGetError`, `sceCdStop` etc. are synchronous SIF RPCs: the calling thread
  waits on the RPC completion semaphore for ~1 ms. Games rely on that yield - a high-priority CD
  thread busy-polling these calls starves lower-priority threads under strict priority-preemptive
  scheduling if the HLE returns instantly (here the main thread, preempted right after
  `StartThread` of the CD thread, never resumed and never queued its first file request). Make the
  stubs block the caller for ~1 ms of EE time on a host-paced deadline (so the wait also completes
  when no other thread is runnable). **[confirmed]**
- **IOP-side CD HLE `sceCdSearchFile` must search the real ISO9660 tree.** IOP middleware (CRI ADX)
  resolves game archives by name through `sceCdSearchFile` on the IOP. If the HLE only searches an
  extracted-file directory, a file that exists only inside the ISO image (e.g. `SD.BIN`) misses, the
  middleware continues with an uninitialized LSN (`0xedededed`), its reads fail forever, and the
  EE-side loader - which polls an RPC for that server's readiness - never queues the next read. Parse
  the PVD (LSN 16) and directory records from the disc image, fall back to the extracted tree
  (patch 0030). **[confirmed]**
- **Read wait loops from the generated C++.** Each generated function keeps the MIPS as comments.
  `grep` just the branches and `jal`s, then map the `jal` targets through the analyzer symbols. That
  usually explains a wait loop in a couple of minutes, faster than tracing. **[confirmed]**
- **Check whether an SDK call reaches the HLE stub or the recompiled real function.** The analyzer
  only stubs functions it recognises; others are translated as guest code even when they belong to a
  library the runtime HLEs elsewhere. Here `sceMpegIsEnd` (12 bytes) stayed a real guest function at
  `0x3722E8` and read the end flag inside the MPEG work area (`[mpeg+0x40][0]`), while the HLE only
  maintained its own C++ playback state. The guest flag never changed, so the movie-end loop never
  exited. If a game polls a library function that is not in the stub list, trace its address with
  `PS2X_EE_JMPWATCH`; if it is real code, the HLE must keep the guest struct fields it reads
  up to date, not just return values from its own stub. **[confirmed]**
- **Get a screenshot of the built-in renderer early.** Add an env-gated raylib `TakeScreenshot`
  after `EndDrawing()` (patch 0026) and build with `-DPS2X_ENABLE_DEBUG_UI=OFF`, since the debug
  overlay covers the frame. A real image ("Loading" screen) proved the game was progressing while
  the counters (`gsw=0`) and a not-yet-complete PCSX2 path both looked like "nothing drawn".
  **[confirmed]**
- **2D usually has more than one renderer.** Here the full-screen pre-rendered backgrounds, title labels and HUD sprites go through one sprite-list API (`sub_001540A8`-`sub_001552B8`), while the UI panels and the dialogue box are built by the script/UI layer and reach the GS by another path. A single projection or sprite hook cannot fix "all 2D": catalogue each element by tracing the sprite entry *and* the UI draw path, and prove one element before promising a global 2D fix.  **[confirmed]**
- **Mapping an address to a symbol.** Take the nearest `name@0xADDR` below the address in the
  analyzer TOML. **[confirmed]**
- **`bindAddressHandler` and the pc convention.** Generated stub wrappers set `ctx->pc = $ra` before
  calling a handler. Handlers bound straight into the function table didn't, so they were
  re-dispatched forever. Bind through a thunk (patch 0003). **[confirmed]**
- **IOP HLE `sysmem`.** Upstream used a fixed 832 KB bump heap with no reuse. Middleware like CRI's
  `IOPMAIN.IRX` asks for big blocks (0x5d000) and has no error path, so a failed allocation turns into
  a wild jump much later (`execution outside RAM`). Use a free-list allocator over all IOP RAM not
  taken by modules, with FIRST/LAST/ADDR modes and correct `QueryMax/TotalFreeMemSize` (patch 0004).
  **[confirmed]**
- **Async callback stacks must not overlap the main thread's stack.** PS2Recomp ran RPC and CD
  completion callbacks as guest "invocations" on stacks taken from the top of EE RAM, which is also
  where the main thread's stack starts. A callback preempting the main thread overwrote its frames,
  and the symptom was a much later jump through a corrupted pointer into the heap. Reserve the main
  stack (64 KB) and start the async stacks below it (patch 0007). **[confirmed]**
- **`EnableDmac`/`DisableDmac` are cache-guarded D_STAT toggles, not direct mask writes.** The Sony
  kernel tracks the per-channel enable state in software and only flips the D_STAT mask bit when
  the requested state differs from its cache (same guard as `dmac_mask` in Linux' PS2 `irq.c`). A
  game that enables DMAC IRQs with direct D_STAT MMIO writes (bits 16-25, "write 1 to reverse") and
  then calls `DisableDmac` for those channels gets a no-op on hardware - the kernel never enabled
  them - but an HLE that applies the syscall unconditionally masks the channel and silently drops
  the completion IRQ, hanging a stream-drain wait. Model the guard with a kernel-side enable cache
  (patch 0022). **[confirmed]**
- **Emulated time must be driven by guest work, not host wall time.** If the runtime charges only a
  few cycles per dispatch (PS2Recomp: 8 per inter-function dispatch, 32 per generated loop
  checkpoint), guest time runs far slower than real time while threads busy-poll, and every event
  gated on "EE deadline AND host deadline" (VBlank above all) never fires on its EE side - a 60 s run
  got ~1 vsync. The first fix floored the cycle counter at `anchor + host_elapsed × clock`, which
  restores real-time pacing but makes *all* host time part of the guest clock: a PNG snapshot, debug
  dump or shader compile advances VBlank/timers while the guest executes almost nothing (boot stayed
  in `.bss` clear; the post-skip movie/scene worker froze). Keep the cheap `steady_clock` floor, but
  report host-only work (`ps2x::time::noteHostStall`, e.g. snapshot encoding/readback, VRAM dumps)
  and exclude it from the emulated clock. Use the executor's CPU time to separate guest waits (they
  count, so sleeping threads reach their timer deadlines) from OS descheduling outside a wait (host
  contention - exclude it too). Excluded time must not simply be dropped: a normal run has to stay
  at real time or audio underruns. Repay it later, bounded by surplus guest CPU time
  (2x guest CPU - wall), so a stalled guest runs in slow motion while a healthy one catches up
  within a few frames. Sample `CLOCK_THREAD_CPUTIME_ID` every ~2 ms, not per dispatch: it is a
  syscall on Linux and cost ~13% of the menu frame budget when called per dispatch. GS raster/present
  time is *not* host-only here - it runs on the executor thread and dominates the title's frames;
  counting it keeps IOP/SPU2 audio real-time (excluding it ran the title at a fraction of wall speed).
  Keep the both-deadlines rule so nothing fires early, never block the scheduler thread waiting for
  a not-yet-due host deadline (keep dispatching guests; wait only when all threads are idle), and
  compute the next checkpoint from the earliest EE deadline directly - converting host deadlines to
  cycles assumes the old 1:1 wall-clock mapping. Re-anchor/skip the vsync chain when it falls a full
  period behind, or the missed-frame backlog replays as a burst faster than real time (patch 0024,
  superseded by 0078). Verify with png-every-vsync boot, `tools/repeat_title.sh`, and a 90 s
  menu/title run through `tools/speed_check.py` (menu vsync/s and WAV coverage). **[confirmed]**
- **Debugging IOP crashes.** Use the env-gated branch-trace and RAM-dump diagnostics (patch 0002,
  `PS2X_IOP_WATCH`, `PS2X_IOP_RAM_DUMP`). Find the import stub address and its export ordinal, then
  check the HLE implementation for that ordinal. **[confirmed]**
- **An over-declared GIFtag is not proof of DMA truncation - trace the chain first.** Before
  blaming the chain walker for a GIF packet whose NLOOP exceeds the data present, log the full chain
  (per-tag id/QWC/ADDR/IRQ, TTE/TIE, stop reason) and the GIFtag walk of the assembled buffer
  (`PS2X_DMA_CHAIN_TRACE`, patch 0032), and watch the source RAM (`PS2X_EE_WATCH`). Here every
  over-declared chain stopped at a normal `REFE` with the full declared QWC fetched, and the
  "missing" qwords were zeros the game itself had written - the game was submitting an incomplete
  display list because it was stalled on unimplemented HLE, not losing data in the DMAC. A chain
  that ends at `REFE`/`END` with `qwc` fully transferred means the data never entered the chain.
  **[confirmed]**
- **The EE VFS `cdrom0:` must serve from the disc image, not just the extracted folder.** Only the
  ELF/SYSTEM.CNF/IOP files get extracted, but the game also opens the big archives through
  `cdrom0:\FILEDATA.BIN;1`-style paths (`sceOpen`/`fioOpen`); if `cdrom0:` maps only onto the
  extracted folder those opens fail. Mount the ISO itself: look up the disc's own directory records
  (PVD at LSN 16; `;version`-suffix and case-insensitive matching - share the parser with the IOP
  `sceCdSearchFile` HLE), ISO first with fallback to the extracted folder so overrides keep working,
  and keep open/read/lseek offsets 64-bit for the 800 MB+ archives. Gotcha: the PVD's root directory
  record starts at byte 156 of the sector (extent LBA LE at 158, size LE at 166), not at 158.
  **[confirmed]**

- **Pad HLE must advertise the modes the game asks for, not just "a pad".** Games commonly run a
  libpad init state machine: scan `scePadInfoMode(PAD_MODETABLE, i)` for the mode they want (here
  id 7, DualShock/analog) and call `scePadSetMainMode` only with that index; until then they never
  parse reports. An HLE that advertises only digital (id 4 / `data[1]=0x41`) leaves the machine
  looping forever even though `scePadRead` returns the correct button bytes. Advertise both digital
  (4) and DualShock (7), treat SetMainMode's mode argument as the table index, and follow the bits
  from `scePadRead` into the game's derived held/pressed words. `PS2X_EE_JMPWATCH` on the reader and
  the report parser, plus `PS2X_EE_WATCH` on the pad struct, shows whether the parser ever runs;
  trap `scePadInfoMode`/`scePadSetMainMode` in the reference to see the table the real game expects.
  **[confirmed]**
- **Never mix HLE and real functions of one library that share a state struct.** PS2Recomp's
  HLE `sceVif1Pk*`/`sceGifPk*` stubs keep the open DMA tag's QWC up to date on every write; Sony's
  real library and the game's own code add the length once, at close. With some functions stubbed
  and others recompiled, every packet length was counted twice. The over-long DMA tags swallowed
  inline texture uploads, and no text was drawn. Pure memory helpers (packet builders, math, string)
  should run as real code: put them on the `unstub` list. Keep HLE for things that touch hardware or
  the IOP. When a guest-memory value changes but `PS2X_EE_WATCH` sees no guest store, a C++ HLE stub
  wrote it. **[confirmed]**
- **Debug missing GS data from the sink backwards.** Log at the GS itself first: which transfers
  start (`SUIKO_GS_TRX_TRACE=1`), with which DBP and size. Then the DMA kick and chain
  (`PS2X_VIF1_KICK_TRACE=1`). Then the memory that built the chain, diffed against a reference
  capture. Weeks of call-graph and scheduling theories about the missing font ended within an hour
  once the GS-side log showed exactly which two transfers were absent. **[confirmed]**

- **Model hardware registers as the single source of truth.** Our runtime gated DMAC interrupts
  with a private mask, while games read and toggle the real D_STAT mask bits. One game read D_STAT
  as "enabled" while the private mask said "off", and the title screen never appeared. Wherever a
  kernel call and direct MMIO touch the same hardware state, keep that state in the register and
  have both paths go through it. **[confirmed]**
- **A game's own DMA queue is a great probe.** Many engines queue DMA per channel and drain the queue
  from the completion interrupt. If the main loop stalls, dump the queue: pending entries on one
  channel point straight at a missing completion interrupt for that channel. **[confirmed]**
- **`missing-target` on an indirect `jalr` into a function's interior** usually means a leaf
  routine reached only through a pointer table, folded into its neighbour by the analyzer. Add it
  to `entry_points` in the recomp config. **[confirmed]**
- **A code pointer can name a control-transfer delay slot.** If the scanner promotes that address
  to a function start, it ends the real owner *before* its delay slot, dropping e.g. the epilogue's
  `addiu sp,sp,N`. The function still returns, but with a leaked stack, and the game dies much later
  through `ra=0`. When a stack pointer drifts, check the generated function range against the
  `jr $ra` before it; our fix decodes the real delay slot past the owner's end
  (`0103-recomp-trailing-delay-slot`, which replaced an earlier boundary-moving `0112`). **[confirmed]**
- **Give every post-return routine an entry, not one `entry_points` line per crash.** Code after
  `jr $ra` + delay slot is unreachable by fall-through, so it is a separate routine; a scan found 852
  such prologues folded into neighbours, and the game builds some of their pointers at run time
  (no static table to find). `0109-recomp-resume-after-return` makes each a resume entry of its
  containing function (no split, one switch case). Check speed afterwards: 1445 generated files
  gained a label. **[confirmed]**
- **Before debugging a rendering bug from scratch, check the open upstream PRs for the same
  subsystem.** The squashed heads were two one-line upstream fixes (SQRT.S operand, VU0 matrix
  multiply order) waiting in open PRs; the CVT.W.S freeze was another. Copy them as local patches
  with the PR number in the name/comment so they can be dropped when upstream merges. **[confirmed]**
- **An in-memory movie that decodes no frames is not a pad or scene bug.** Trace the MPEG HLE state
  (`AddBs` bytes, `sawSequenceEnd`, decoded/served frames) before blaming the loader. A whole short
  MPEG ES can arrive in one `sceMpegAddBs`; it must be searched for a sequence header before any
  resync-buffer cap is applied. **[confirmed]**
- **Test with the player's path first.** The user pointed out that Cross skips the intro. The skip
  path reproduced the same blocker in 45 s instead of 3.5 min, and matched what a player does.
  **[confirmed]**
- **A visual detector must key on content that does not change, and be calibrated on a labelled
  corpus.** The title screen picks one of several random 3D backgrounds per boot; a menu-region
  colour heuristic tuned on one scene false-negatived the campfire (outline fraction 0.054-0.20
  against a 0.05 ceiling), and the warm fortress stone merged background rows into the menu. Detect
  the UI itself: connected components of the menu glyphs (dim olive/bright gold with a dark
  outline, three right-aligned lines, equal pitch, one highlighted) plus a scene-independent
  background-detail gate (edge/texture energy outside the menu box, not correlation with one
  scene). Keep a gitignored labelled corpus covering every random background and every negative
  state (TV menu, logo, intro movie, prompt, options, opening book, attract demo) and a self-test
  that prints a confusion table and every mismatch - it caught the Options screen passing as a
  title menu. **[confirmed]**

## 6. Overlays (code loaded at runtime)

- **A `missing-target` jump into `.bss`/heap is NOT proof of overlays.** Dump RAM at the jump first.
  Here `0x60c560` held heap data and no CD read had happened, so it was a bad function pointer caused
  by incomplete initialization. Only call it an overlay once the bytes there are code that came from
  the disc. **[confirmed]**
- **`.DVP.ovlytab` / `.DVP.overlay*` sections are VU microcode overlays** (Sony's DVP toolchain), not
  EE code overlays. **[confirmed]**
- **PS2Recomp has no overlay support** as of `eb8688b`. It only registers the ELF's executable
  segments as code regions. **[confirmed]**
- **A syscall override pointing at runtime-copied code fails with `-1` until the blob is recompiled.**
  Games may do syscall-table surgery at boot (raw `SetSyscall`/`Copy`/`GetEntryAddress`, number in
  `$v1`) and `Copy` a small kernel-mode service blob (here: raw COP0 TLB/scratchpad ops at
  `0x80075000`) into low RAM, then `SetSyscall` hooks into it. The HLE stores the override, but
  `dispatchSyscallOverride` requires `hasFunction(handler)`, so every call into the blob returns
  `KE_ERROR` and the game records `-1` where the real kernel ran the blob. Diagnose by diffing what
  the game stores from `GetEntryAddress`/hooked syscalls against reference RAM and checking whether
  the handler address lies outside the recompiled ELF's code. Do not fake the recorded values.
  **[confirmed]**
- **Declared code blobs (`[[code_blobs]]`) are the fix for static-data service blobs.** When the blob
  bytes are static ISO/ELF data (here `.data` `0x42A838`, 0x330 bytes, copied verbatim to
  `0x80075000`), declare the blob in the recompile config with its source VA, size, exec address and
  the entry points the game calls. The recompiler translates it like any function and emits it to a
  separate TU (`ps2_recompiled_code_blobs.cpp`); the entries are registered through
  `PS2Runtime::registerSparseFunction` (a small mutex-guarded map consulted by `hasFunction`/
  `lookupFunction` after the dense table misses). The sparse map normalizes KSEG0/KSEG1 to physical,
  so `0x80075000`/`0xA0075000`/`0x00075000` all resolve. Keep blob entries out of the dense table
  (the dense emitter must filter resume-entry owners to dense functions, or the table stretches to
  the blob's address). Syscall numbers the blob hooks may collide with built-in HLE cases (here
  `0x55..0x59` are also event-flag syscalls) — the dispatcher must try the guest override before the
  built-in switch. Verify by checking the game's stored `GetEntryAddress` result against reference
  RAM, and by a first-dispatch log line in `dispatchSyscallOverride`. **[confirmed]**
- **Finding an overlay's source.** Dump EE RAM on the first missing-target event, then search the
  disc's data files for those bytes and confirm against a `sceCdRead` trace. **[expect]** Details of
  the format and the recompile and dispatch design to follow once done.

## 7. Graphics: PCSX2 GS instead of the built-in path

- **Why.** Reusing PCSX2's GS gives upscaling, FXAA/CAS, anisotropic filtering, texture dump and
  replace, and widescreen hooks for free. The price is GPL-3 for the whole project. See
  `docs/gs-port-plan.md` for the full file-level design. **[confirmed]**
- **The seam is small.** PCSX2's GS has a flat API (`GSopen`, `GSgifTransfer*`, `GSvsync`,
  `GSclose`). PS2Recomp already has the producer side (GIF arbiter, VIF1, VU1 interpreter, vsync
  events), and its GS privileged-register offsets match PCSX2's `GSPrivRegSet`. MTGS isn't needed; run
  the GS inline. **[confirmed]**
- **Vendoring (PCSX2 v2.8.2).** Copy the GS core plus the Vulkan and HW renderers. The HW renderer
  also hard-depends on a few SW files (`GSTextureCacheSW`, `GSVertexSW.h`, `GSScanlineEnvironment.h`),
  `GSLocalMemoryMultiISA.cpp` and the LZMA xz decoder for dumps. You can drop the OpenGL, DX11, DX12,
  Metal and SW-rasterizer code. Stub `Host::`/`VMManager::` (about 25 functions). Take Vulkan headers
  and VMA from PCSX2's `3rdparty/` if the system lacks them. `GSDeviceVK::Create` compiles its
  pipelines at `GSopen`, so the shaders must already be staged under the resources folder. Record
  every local change in `UPSTREAM.md` and mark it `// SUIKO:` in the code. **[confirmed]**
- **Build a headless regression harness before integrating.** `gs_replay` replays PCSX2-format
  `.gs` dumps through `GSgifTransfer*`/`GSvsync` without MTGS and saves frames as PNG with
  `GSSaveSnapshotToMemory`. `gs_dumpgen` writes a synthetic dump, and a stdlib-only Python PNG check
  verifies the colors at 1x and 2x. `--renderer null` can't capture frames. **[confirmed]**
- **Things that silently give a black frame when you hand-build GIF/GS data** **[confirmed]**:
  - Packed XYZ2 has Y at bits 32–47, one vertex per quadword.
  - Packed RGBAQ puts each channel in its own 32-bit lane.
  - `FRAME.FBMSK` has to be 0. All-ones masks every write, and the HW renderer then drops the draw.
  - `SMODE2.INT=1, FFMD=0` gives full 448-line frames. `INT=0` gives 224-line fields.
- **Hook PCSX2's GS in with runtime callbacks, not types.** Add a plain-integer hook interface
  (`privRegWrite(offset, value)`, `vsync(tick)`, `gifPacket(path, data, qwords)`) to PS2Recomp and
  keep all PCSX2 code in your own glue (`src/gs/`). Three things to watch:
  - PS2Recomp's `gs_types.h` and PCSX2 both define `GSVertex`, so keep the glue out of the
    runtime PCH and the unity build.
  - Some GS registers (DISPFB/DISPLAY/BGCOLOR via GIF A+D, CSR, IMR) are written outside the MMIO
    trap. Resync those into the mirror every vblank.
  - PS2Recomp's `GifPathId` is swapped vs the hardware paths PCSX2's `Transfer<n>` slots model
    (GIF DMA is `Path3`, VIF1 DIRECT is `Path2`), and VIF1 DIRECT payloads get truncated at
    DMA-chunk boundaries. Route by hardware path, and clamp a truncated tag's NLOOP to the qwords
    present before calling `GSgifTransfer*` — otherwise GSState waits for qwords that never come
    and desyncs into a wild-pointer crash on the next packet. **[confirmed]**
- **Present through the GS's own swapchain instead of readback.** Open an SDL3
  window on the main thread, read its platform surface from the window
  properties (Wayland: `wl_display`/`wl_surface` pointers; X11: `Display*` plus
  the numeric window id cast to `void*`), fill PCSX2's `WindowInfo` and hand it
  to the GS through the `Host::AcquireRenderWindow` stub before `GSopen`; then
  `GSvsync` presents by itself. Rules: SDL events are pumped on the thread that
  created the window; keep every GS call on the game thread (hand a resize over
  atomically and call `GSResizeDisplayWindow` at the next vsync); on close, stop
  and join the game thread before `GSclose` + window destroy. Give the runtime a
  small `presentationDisabled`/`mainTick` hook seam so it can skip its own window
  and present loop instead of restructuring threads. Use Mailbox (or Disabled)
  vsync, never FIFO, when an external scheduler is the frame clock. Two traps:
  the Vulkan platform-surface code is behind `VK_USE_PLATFORM_*` build defines —
  without them a windowed `GSopen` fails silently; and PCSX2's `Console` sink is
  off by default, so enable it or GS errors vanish. **[confirmed]**
- **Emulated time has to keep up with guest work.** PS2Recomp charges a fixed 8 EE cycles per
  dispatch, and events need both the EE-cycle and the host deadline. Busy guest threads therefore
  slowed vsync to about one per minute. Watch the vsync rate (count frames per minute) as soon as the
  game starts running, but keep host-only time (snapshots, dumps) out of the guest clock and repay it
  only from guest CPU surplus - see the wall-clock bullet above and Fixed 29 in `docs/bringup.md`.
  **[confirmed]**
- **Main risk.** Both sides write CSR (SIGNAL/FINISH/VSYNC). Define one writer per bit, or the game's
  GIF-sync loops hang. **[expect]**
- **Display flicker from fake register aliases.** Upstream PS2Recomp's GIF frontend treated the
  undefined A+D addresses 0x59–0x5f as aliases for DISPFB/DISPLAY/BGCOLOR. A stray game write of 0x5
  to 0x5a blanked every frame it landed in, which showed as flashing in the window. On the real GS
  (and in PCSX2) those writes are ignored. When the host GS mirrors privileged registers from the
  runtime's block, check every runtime writer of that block, not just the MMIO path. Snapshots taken
  every N vsyncs can't show flicker; use `SUIKO_GS_SNAPSHOT_START=<v> SUIKO_GS_SNAPSHOT_EVERY=1` and
  `md5sum frames/* | sort | uniq -c`. `SUIKO_GS_PRIVREG_TRACE=1` logs every privileged-register
  write. **[confirmed]**
- **No MSAA.** PCSX2 2.8 doesn't have it. Anti-aliasing comes from FXAA/CAS plus upscaling, which
  effectively supersamples. **[confirmed]**
- **Audit the settings, not just the options struct.** A `GSOptions` field that is set in the glue can
  still be ignored (deinterlace/integer scaling were never copied, and a skipped duplicate frame can
  hide a toggle on a static screen). Snapshot the internal GS target and the presented window
  separately, measure with `SUIKO_GS_SET_AT` on the same scene, and use
  `tests/gs/check_video_settings.py`; see `docs/graphics.md` for the Legion cost table and the
  recommended defaults. **[confirmed]**
- **Widescreen** needs per-game work: a pnach for the 3D FOV and the 2D layout. Check `pcsx2_patches`
  for an existing patch first. `video.aspect` only changes the output geometry; the port has no pnach
  loader yet. **[expect]**

## 7b. VU1 on its own thread, exactly

Patch `0112-vu1-thread`, design in `docs/vu1-thread.md`. The lessons apply to any game.

- **Move VIF1 with VU1, not VU1 alone.** A bare VU1 thread races the next UNPACK/MPG of the same
  DMA chain against the running microprogram, so it can only overlap until the next VIF1 kick
  (0.2 ms/frame measured in gameplay). With the whole VIF1 stream on the worker, every VIF1/VU1
  hazard stays inside one thread and no double-buffering is needed. **[confirmed]**
- **The game thread still walks and completes the DMA.** The chain walk never depends on VIF1
  results, so D_STAT, CHCR, the DMAC interrupt and its handler fire at the same guest point as
  before; only the copied bytes go to the worker. Keep chunk boundaries: the VIF1 interpreter's
  behaviour at a chunk end is part of its semantics. **[confirmed]**
- **Replay operations, don't re-order packets.** The GS input depends on the whole sequence of
  GIF-front operations (submits, arbiter drains with their path-priority sort, MSKPATH3 and its
  PATH3 FIFO, privileged-register notifications). Record the worker's operations instead of
  executing them and replay them on the game thread in serial order; game-thread operations run at
  once while nothing is outstanding and queue behind outstanding work otherwise. The arbiter and
  the GS then never see a second thread. **[confirmed]**
- **Make the present an operation.** Nothing the game observes at a VBlank depends on VU1; only
  the image does. Queue the present behind the frame's work with a snapshot of the display
  registers (CSR/SIGLBLID read at replay, because replayed SIGNAL/FINISH/LABEL set them), and bound
  the lag to one frame. A fence at every VBlank wasted ~2 ms/frame in battle. **[confirmed]**
- **List observation points from the overlap probe.** The `shareVu1Observe` sites of the
  measurement pass were exactly the fences needed (VU1 memory, VIF1 registers, GS CSR/SIGLBLID,
  GIF_STAT, GIF fast paths, HLE stubs that call the GS directly). **[confirmed]**
- **Avoid a fence by predicting what depends on the stream alone.** GIF_STAT's only
  worker-dependent bit is M3P, and the PATH3 mask depends on VIF1 commands, never on VU1. A
  parse-only copy of the VIF1 state (no VU memory, no VU1, no GIF output) walks each kick on the
  game thread and answers at once; check it against the real state at every full fence. Suikoden
  reads GIF_STAT 1-3 times a frame while MSKPATH3 streams are queued: a fence there cost
  150-230 ms/s, and a conservative opcode scan still fenced in the story scenes. VPU-STAT's VU1
  D/T bits only change when FBRST.DE1/TE1 are set; fall back to serial for good the first time a
  game sets them. **[confirmed]**
- **Count fence waits as emulated time.** The guest clock follows the game thread's CPU time; a
  fence wait replaces VU1 work the serial runtime ran on that thread, so report it as such or a
  long fence turns into slow-motion debt. **[confirmed]**
- **Prove equivalence in-process, not run-to-run.** The runtime is not deterministic between runs,
  so a serial shadow (its own VU1 memory, VIF1 state, arbiter and interpreter) runs inline with
  the same inputs and every GS-bound packet, present and fence state is compared
  (`PS2X_VU1_THREAD=verify`). Show it can fail: `PS2X_VU1_THREAD_FAULT=<n>` drops one replayed
  packet and the next comparison reports it. A random-stream unit test plus ThreadSanitizer covers
  interleavings the game never produces. **[confirmed]**
- **Globals in the VU1 core become shared state.** The compiled-program registry, AOT hit/miss sets
  and diff counters needed `call_once`/a mutex; the per-thread FP environment needed copying to
  the worker. **[confirmed]**

## 7c. VU1 on PCSX2's microVU recompiler

The AOT-compiled VU1 programs (section 7b's worker runs them) still emulate the pipeline
cycle by cycle with exact PS2 float maths, and at 60 fps that saturates a core.  PCSX2's
microVU1 works out stalls and flag instances at compile time and uses SSE maths; vendoring
it is a few days, not a rewrite (`docs/vu1-jit.md`, `third_party/pcsx2_vu/UPSTREAM.md`).

- **Check the game's PCSX2 GameIndex entry first.** No clamp/round overrides and a playable
  rating mean PCSX2's default VU settings (round-to-zero, DAZ/FTZ, "Normal" clamping) are
  known to be fine for it. **[confirmed]**
- **microVU lifts out cleanly.** It needs the x86 emitter, `common/` (already there with the
  GS port), `Config.h`/`EmuConfig`, `g_cpu`, and stand-ins for `VU.h` (keep upstream's,
  replace `Vif.h` with TOP/ITOP), `MTVU.h` (inert), the EE allocator state its COP2 path
  names, and `Gif_Unit` (PATH1 output).  Leave out `microVU_Macro.inl` (EE COP2).
  **[confirmed]**
- **Put the code cache in `.bss`.** The emitter encodes globals as `[rip+disp32]`; a static
  cache made executable with `mprotect` is within range of every global, including VU1
  memory if that is static too.  Make the runtime keep VU1 memory in those buffers rather
  than copying per kick. **[confirmed]**
- **Set `x86Emitter::use_avx` from `g_cpu` before generating code.** PCSX2's VMManager does it;
  microVU picks AVX2 paths from `g_cpu`, and without the flag the emitter writes them as
  128-bit SSE: the block-state compare never matches and compilation recurses until the
  stack overflows. **[confirmed]**
- **Attach it through a plain-types backend hook** (`runtime/ps2_vu1_backend.h`): VU1 memory
  buffers, `reset`, `run(start/resume, TOP, ITOP, FBRST, budget, micro generation, packet
  sink)`.  Packets go to the same ordered GIF path as the interpreter's, so the VU1 thread
  needs no change.  Use the micro-memory generation counter to call microVU's `Clear()`.
  **[confirmed]**
- **Diff it against the interpreter in-process.** Run every kick on both from the same
  registers and VU memory (interpreter authoritative), count packet-structure mismatches
  separately from ulp differences, and dump the inputs of the first mismatch for a local
  replay.  Do not reset microVU's saved pipeline state (`lpState`) when copying registers
  in: MSCNT continues from it.  Expect 1-15 ulp float differences and identical packet
  structure; anything structural is a real bug on one side.  Here it was ours: the
  interpreter dropped the first of two back-to-back XGKICKs, a regression from our own VU1
  patches (plain upstream was fine). **[confirmed]**

## 7d. Function overrides must return like the original

- **Set `ctx->pc` to `$ra` in every replacement that does not call the original** (and on every
  early-return path of one that sometimes does).  Generated code checks `ctx->pc` against the
  fall-through after each call and bails out to the dispatcher when it differs; a replacement
  that leaves `ctx->pc` at its own entry address is re-dispatched forever.  It looks like a
  hang whose sampled pc is the overridden function, and it may only bite on some call sites.
  **[confirmed]**
- **One generated function can own several entry points** (`register_functions.cpp` maps more
  than one address to it, and the body switches on `ctx->pc`).  `replaceFunction()` replaces one
  table slot only, so check the other entries before assuming what a replacement covers.
  **[confirmed]**

## 7e. Emulated kernels: copy a reference, then prove it

- **Compare the emulated kernel with a working implementation, rule by rule.** Our IOP kernel had six
  silent deviations (time slicing between equal priorities, no FIFO queues, alarms that never fired,
  RPC handlers outside any thread, blocking calls inside interrupts, `StartThread` restarting running
  threads). Each looked harmless and each could explain a rare freeze. Reading Play! and OpenPS2BIOS
  side by side found them in hours, where timing patches had only moved the races around for days.
  Where the references disagree, follow the more mature one (Play!). **[confirmed]**
- **Make wrong behaviour loud before fixing it.** Counting every wait that could not block
  (`[iop-wait]`) showed the bug in a normal run (6 skipped locks) when freezes were 1 in 24. **[confirmed]**
- **A run that did not play is missing data, not a pass.** Three "clean" runs of an audio bisect had
  crashed at startup (no desktop session); the bisect named the wrong patch. Every run summary needs a
  validity check (modules loaded, audio lines, end vsync). **[confirmed]**
- **HLE that does a whole batch of work in one guest call stalls the host.** The sceMpeg stub decoded
  each ~2.4 s bitstream chunk inside `sceMpegAddBs`; the game thread stalled 30-130 ms every couple of
  seconds and the audio device ran dry. Time each step (`PS2X_MPEG_COST`) instead of reading
  time-averaged profiles, which hide periodic spikes; spread the work the way the hardware does
  (decode per picture). **[confirmed]**
- **Emulated time must track wall time on average, or audio drifts.** The sound device drains at a
  fixed 48 kHz. Any pacing rule that drops time for good (here: OS descheduling during movies) runs
  the SPU2 slow by that fraction; a drift trim clamped to ±0.5% hides up to that and no more, so the
  queue drains into silences at a regular period. Regular hiccups = rate mismatch: look at the trim
  ratio (pinned at its clamp) and at which excluded-time counter grows, before blaming stalls. Drop
  only long one-off stalls; repay ordinary jitter. **[confirmed]**

## 8. Tooling and environment

- **Ghidra:** `sudo pacman -S ghidra`, then `/usr/bin/ghidra-analyzeHeadless`. **[confirmed]**
- **PCSX2** isn't in the Arch repos (`target not found`), so use `flatpak install --user flathub
  net.pcsx2.PCSX2`, which needs no sudo. It needs a BIOS dumped from your own console before it can
  serve as a reference. **[confirmed]**
- **Missing pieces.** `vulkan-headers` and `vulkan-validation-layers` may be missing. The headers can
  be vendored. The validation layers are worth installing before swapchain work. **[confirmed]**

- **Never `pkill -f <script name>`.** A background shell that launched a task with a heredoc spec
  contains the whole spec text in its command line, so the pattern can match it and kill that shell.
  Its child keeps running, but completion notifications are lost. Kill by PID. **[confirmed]**

- **Check DISPLAY against the library's own formula.** A picture that is slightly small, shifted
  up and cut at the top looked like an emulator quirk for weeks; it was an HLE stub writing
  `DISPLAYn` with the wrong start and magnification. Retail games link libgraph statically, so the
  real `sceGsSetDefDispEnv` is in the ELF next to the stub: disassemble it and compare the register
  values (NTSC: DX 636, DY 50/25, MAGH 2560/w - 1). Also: the EE passes arguments 5-8 in `t0`-`t3`;
  never guess the stack because a register argument happens to be zero. **[confirmed]**

- **A hang that logging hides: dump on a signal.** The opening freeze vanished whenever a periodic
  IOP dump was on (0/8) and came back without it (4/10). Arm a dump that costs nothing until
  `SIGUSR1` (`PS2X_IOP_SIGNAL_DUMP=1`), detect the stuck state from the streamed log, then signal
  the process. Disassemble the IRX code at the dumped PCs (`cstool mips`), and read the module's
  globals from the RAM image. **[confirmed]**

- **An HLE RPC handler that cannot block breaks the game's locks.** Running IOP RPC server
  functions as plain calls means `WaitSema` inside them returns at once; a mutex the game uses
  between the RPC handler and its worker thread then protects nothing, and a command block is
  overwritten while it is being read. Before each RPC, let ready IOP threads run to a block
  point (what a concurrent IOP would have done), and trace frozen against healthy runs with a
  narrow per-call trace rather than a flood. **[confirmed]**

- **Two processors, one VBlank.** IOP modules poll EE-written command words once per VBlank
  (SDSTR3's CD lock); if the IOP's VBlank is a separate clock, an EE frame without an IOP VBlank
  merges two commands into one. Check any HLE that gives the IOP its own VBlank grid against the
  EE's VBlank: same rate, and at least one IOP VBlank per EE frame. Pushing IOP time forward in
  bursts to get there broke other modules, so the fix has to keep both clocks in step instead.
  **[confirmed problem; fix open]**

- **A scheduler change that is pick-for-pick exact can still change behaviour.** The IOP
  next-thread speed-up matched the old picks over 10^9 calls, yet made the opening freeze several
  times more often, because it changed how fast the IOP ran against the EE. Gate such changes on
  freeze/scene runs (many exclusive New Game runs), not only on the differential verifier.
  **[confirmed]**

- **One game process at a time for measurements.** Overlapping runs make the host-pacing model
  exclude descheduling and invalidate both runs' numbers; an afternoon of this task's title
  conclusions came from overlapping runs and had to be thrown away. Use
  `SUIKO_RUN_EXCLUSIVE=1`, run in the foreground, and check `pgrep -x suikoden` on the target
  before every measurement. The only deliberate exception is the documented two-instance freeze
  repro. **[confirmed]**

- **Label the random title scene before comparing builds.** Suikoden Tactics picks one of several
  3D flythroughs per boot and the same scene changes load along its camera path (fortress bridge
  32 pictures/s, fortress overhead 57 pictures/s). A "60 -> 40 regression" that was really two
  different scenes cost a false bisect; save `frame_<vsync>.png` with every run and compare per
  scene/camera. **[confirmed]**

- **Check the scripted New Game phase before trusting a battle window.** The opening book's
  page-turn inputs shift every later scene by hundreds of vsyncs run to run, so the documented
  `16501..18180` window is only the command menu when `frame_16800.png` shows Move/Attack/... .
  Otherwise the same numbers are a 30/60 fps story scene. Reject the run or rerun it. **[confirmed]**

## 8b. Reference runs with real PCSX2

- **Get a reference as early as you can.** With a BIOS dumped from the user's own console, run the
  real game in PCSX2 and compare. One screenshot showed that our "stuck dialog" was a TV-mode menu
  correctly waiting for input. Three executor tasks had chased it as a hang. **[confirmed]**
- **PINE IPC** (`EnablePINE = true` in `PCSX2.ini`): read/write guest memory and save states from
  a script (`tools/pine.py`). With the Flatpak, the socket is at
  `$XDG_RUNTIME_DIR/.flatpak/net.pcsx2.PCSX2/xdg-run/pcsx2.sock`. A save state (`.p2s`) is a zip with
  `eeMemory.bin` (32 MB), `iopMemory.bin`, `GS.bin`, VU memory and `Screenshot.png`, which gives full
  reference RAM for byte-level comparisons. **[confirmed]**
- **Flatpak sandbox:** it can't see the repo. Hard-link the ISO into
  `~/.var/app/net.pcsx2.PCSX2/data/` (same filesystem, no extra space) instead of widening the
  sandbox's permissions. PCSX2 doesn't scan BIOS subfolders, so symlink the dump's
  `ROM0`/`ROM1`/`NVM`/`MEC` as `<name>.bin/.rom1/.nvm/.mec` in the `bios/` root. Headless run:
  `-nogui -batch -fastboot -- <iso>`. **[confirmed]**
- **Scripted input with timings is fragile.** Boot timing varies between runs, so trigger presses on
  game state (a watched variable) rather than a fixed vsync, and make sure text renders first so
  you can see where you are. **[confirmed]**
- **Diff static sections first when comparing reference and runtime RAM dumps.** Dump both at the same
  screen and diff per ELF section: if `.text`/`.rodata` match byte-for-byte, recompiler mistranslation
  is ruled out and the bug is in runtime data flow - this focused the font-missing hunt on the absent
  GIF IMAGE upload in one run instead of code archaeology. GS.bin's VRAM blob sits at
  `filesize - 4 MiB - 84` (tail per `GSState::Freeze`; last 4 bytes are `m_q` = 1.0f). **[confirmed]**
- **"Does function X actually run in the reference?" — patch counter traps into guest RAM
  (`tools/ref_trap.py`).** PINE has no breakpoints, but `Write32` (msg 6) can replace a function's
  first instruction with `j <cave>` (original I0 moves to the delay slot) where a small stub in a
  free RAM page increments a pollable counter and replays I1 before jumping back. Two gotchas make
  or break the trick: external RAM writes do **not** invalidate PCSX2's EE JIT, so after patching
  do a `SaveState`+`LoadState` (msg 9/10) round trip to force a JIT rebuild (the loaded state
  carries the patched RAM); and a game that re-loads its own code during boot will undo traps
  installed too early - wait for a "boot finished" game marker first. This killed three wrong
  theories in one run (each candidate "gate" function provably never executed in the reference
  either), which is worth more than any amount of static reading. **[confirmed]**

- **Pressing buttons in the reference PCSX2.** PINE can't send input. Launch the Flatpak with
  per-run `--socket=x11 --nosocket=wayland --env=QT_QPA_PLATFORM=xcb`, so it runs under XWayland
  without changing the installed permissions. Then `xdotool keydown/keyup --window <id>` sends keys
  to the PCSX2 windows only, never to the focused window. PCSX2's default pad-1 binding for Cross is
  K. Wrapped as `tools/ref_input.py`. Only one PCSX2 can own the PINE socket, so don't run two
  reference tools at once. **[confirmed]**

## 9. Working with an orchestrator and executor (Opus plans, an executor model executes)

- **Executor history.** Until 2026-10-20 the executor was a local Qwen (262k context, halogen server).
  After that it's DeepSeek V4.1 Flash in the cloud (1M context, `deepseek-orchestrate`, `.deepseek/`,
  `deepseek/*` branches). The specs and the review process carry over unchanged. Qwen's weak spots
  were context pressure on long investigations (4 compactions, then the server connection dropped
  with no report) and slow turnaround. **[confirmed]**
- **Never let a stale recompiler survive a patch-queue change.** A fresh worktree's submodule
  starts at the bare upstream pin. An executor that ran `build_tools.sh` before
  `apply_patches.sh` got a vanilla `ps2_recomp`. `build_tools.sh` skipped rebuilding when
  `PS2RECOMP_BUILD` was set, and `recompile.sh` only built missing tools, so every later run reused
  that vanilla binary. It silently dropped the kernel code blob and the thunk stubs, the game died
  early in boot, and every frame was black for 30 minutes of "investigation". Now the tools always
  rebuild incrementally, and `build_tools.sh` refuses to run unless the submodule HEAD is "Local
  patch queue (N patches)" with N equal to the number of patch files. **[confirmed]**
- **Executors must write findings as they go.** A task that dies mid-run loses everything that's
  only in its context. Specs now require each confirmed measurement to be appended to the notebook
  immediately. **[confirmed]**
- **Worktrees only have committed files.** Gitignored inputs (the ISO, `data/`, `build/`) and
  uninitialized submodules are missing there. Every spec's verification has to run
  `git submodule update --init --recursive` and point at inputs with absolute paths. **[confirmed]**
- **Make tools fail loudly on missing inputs.** `run.sh` once ran "successfully" without the ISO in
  every worktree, which hid the real behaviour for a whole round. **[confirmed]**
- **Keep bring-up tasks narrow.** "Fix the next blocker and record what happens next" works. "Get to
  drawing" ran into the timeout after 1h50m. Partial work is still committed on the branch, so review
  and merge what's good, then split the rest. **[confirmed]**
- **Give the executor a shared notebook** (`docs/bringup.md`: symptom → cause → fix → patch, plus
  "Open"), and seed the next spec from it. **[confirmed]**
- **Research tasks work well on the executor,** for example "read PCSX2 and write a file-level plan
  with cited paths". They save a lot of orchestrator tokens. **[confirmed]**
- **Use a liveness watchdog, not a fixed timeout, and don't stop on context.** A fixed 110-minute cap
  killed executor tasks that were still making progress. `delegate.py` stops a task only when it is
  **stalled** (no log growth and no CPU in its process group for 20 min) or hits a 6-hour hard cap.
  An early stop at 90% context turned out harmful: opencode compacts/prunes its own context at almost
  exactly that point (one 3h21m run peaked at 235.9k, dropped to 134k and carried on), so the guard
  killed a task just as it was about to compact. Only a real overflow, where a single huge step jumps
  past the limit and the server rejects the prompt, needs handling. Then `delegate.py` restarts the
  executor in a fresh session in the same worktree, with a handoff note: last messages, last tool calls,
  and git/submodule status. Results report peak context and compactions/resets. **[confirmed]**
- **Parallel tasks that both add patches collide on numbers.** Two branches each created a `0016-*`.
  Give one task a distinct patch-name prefix (e.g. `gs-`), then renumber at merge so it runs after
  the other, and re-run `apply_patches.sh` to prove the whole queue still applies. **[confirmed]**
- **Each git worktree gets its own submodule git dir** (`.git/worktrees/<wt>/modules`), so parallel
  executor tasks can't clobber each other's submodule edits through git. **[confirmed]**
- **Never put a destructive reset in verification.** `apply_patches.sh --force` as a verify step
  wiped an executor's unsaved submodule fix after it ran out of context. Verification should use the
  non-forcing variant, which fails on unsaved edits instead of discarding them, and specs should say
  to save patches early and often. **[confirmed]**
- **The executor's context is finite (262k).** Debugging tasks blow it by reading whole logs and
  generated files. Specs should require `grep`/`head`/`tail` with limits, never `cat` of large files.
  **[confirmed]**
- **Lost executor edits can be recovered.** The opencode JSON log records every `edit` tool call with
  its `filePath`, `oldString` and `newString`, so replaying the completed ones restores the work.
  **[confirmed]**
- **When a run ends without a report** (`context_full`/`stalled`), recover its findings cheaply:
  print the last ~8 `text` events from the JSON log (a few hundred tokens). Save the unsaved
  submodule edits in its worktree with `make_patch.sh`, then commit and merge. One such run showed
  the dialog wasn't input-gated and pointed at the real stall (the IOP stream). **[confirmed]**
- **The executor sometimes skips its report.** Review the diff and outputs yourself, and never merge
  on the verify result alone for judgement calls (the Ghidra over-splitting passed verification).
  **[confirmed]**
- **When regenerating an existing patch, back up every file it touches first.** `make_patch.sh`
  snapshots the whole submodule diff, but the usual "save edited files to /tmp, `apply_patches.sh
  --force`, restore, re-snapshot" cycle silently drops files an older version of the patch touched
  but the fresh session never edited (here: `forEachThread`/`forEachHandler` helpers in
  `iop_kernel.h`/`iop_intrman.h`), producing a regenerated patch that no longer compiles. Diff the
  old patch's `+++` file list against your backup set before deleting it. **[confirmed]**
- **VIF1 IMAGE continuation = next DIRECT payload, not raw bytes.** When a GIF IMAGE tag inside a
  VIF1 `DIRECTHL` declares more qwords than the DIRECT carries (tag-only packet, `nloop` over-declaring),
  the remaining image data arrives as the *payload* of the following `DIRECT`/`DIRECTHL` on that path —
  the VIF command word itself never reaches the GIF. A continuation that consumes raw chunk bytes instead
  eats the next command word and shifts every upload payload by one qword (here: +16 B, the `-0x3F0`
  CLUT offset and garbled menu text). Skip NOP padding between tag and data; if a non-NOP, non-DIRECT
  command (e.g. `STROW`/`MSCAL`) intervenes, the data comes from a path you don't model (VU FIFO) —
  abandon the continuation rather than eating the next packet's registers. **[confirmed]**
- **Test the producer hypothesis against the reference before building the fix.** The missing font
  atlas was blamed on unexecuted VU1 microcode (no XGKICK ⇒ no PATH1 upload). Dumping the
  *reference's* VU1 micro/data memory at the upload window showed a BIOS NOP slide and zero data:
  the game never MPG's code and XGKICK never fires on real hardware either, so our zero XGKICKs
  were already correct and the "fix" would have been faking. A reference-state dump of the suspect
  subsystem costs minutes and can retire a whole lead; make it the first step of any "X should
  produce Y and doesn't" investigation. **[confirmed]**
- **A routine dead in our build may be dead in the reference too — check before "fixing" it.** The
  libgraph `GsSetLoadImage`-style entry looked like the missing font-upload path, but its trigger
  gp-global and its output packet buffer are all-zero in reference dumps as well: the game never
  calls it on real hardware. Verify a candidate path actually ran in the reference (its input
  flag/global non-zero, its output buffer populated at the capture right after the window) before
  investing in why our build doesn't run it. While mapping callers: generated-code call targets
  appear as *uppercase* `0xADDRu` inside `dispatchGuestBranch(...)` — a case-sensitive-lowercase
  grep silently misses every caller — and the trailing `ctx->pc = 0x…u` of a recompiled function is
  an unreachable fall-through artifact, not a real call edge; adjacent `sub_` boundaries are often
  one giant function. **[confirmed]**
- **First divergence over time beats static guessing: `tools/statediff.py`.** Capture a fine
  reference timeline (`tools/ref_timeline.py --interval 1` around the window where the builds part
  ways) plus matching `PS2X_DUMP_AT` dumps of ours, then diff *word-wise over time*: statediff
  reports every `.data/.sdata/.sbss/.bss` word the reference changes that ours never takes, ordered
  by reference change time — the earliest entries are the root-cause candidates, everything later is
  downstream noise. Two design details matter: pointer fields must be compared shift-tolerant (the
  HLE malloc moves our heap ~0x1200; without `--shift` the report drowns in false hits), and each
  earliest divergence must be *classified before it is "fixed"*: here the whole t=3 s cluster turned
  out to be HLE artifacts (libsd `FdTable`, libcdvd SIF client structs, RPC client fields the HLE
  writes host-side — invisible to guest-store watchpoints, which is itself the tell). The first
  non-artifact divergence was a real HLE gap (OSD config2 defaults: `sceScfGetDateNotation`/
  `GetTimeNotation` returned 0 where the real US BIOS returns 1). **[confirmed]**
- **Check the IOP scheduler before blaming an IRX for a wake-up race.** Our kernel only
  rescheduled at the end of a 256-instruction slice, and `beginNextReady` always picked the
  lowest thread id among equal priorities. Both differ from the PS2 (immediate preemption on a
  higher-priority wake-up, round-robin within a priority) and can strand middleware that uses a
  mutex/worker handshake. Measure the thread priorities from the `CreateThread`/idle-dump logs
  first; if the woken thread is not actually higher priority, preemption is not the explanation.
  **[confirmed]**
- **Test the task's premise in the reference before fixing it.** A spec claimed our build "never
  reaches the font file load" and named the game's file loaders as the route; reference traps showed
  those loaders never execute in the reference either (its only `sceOpen` was `rom0:ROMVER`), and the
  real font route (a raw `sceCdRead` LBA read from the CRI reader) already worked in ours - read at
  the same call site, byte-identical data, correct timing. The actual gap was downstream (the VRAM
  upload trigger). Trap the premise, not just the hypothesis. Related trap: a missing trace line is
  not evidence of absence - `sceCdRead`'s per-read log is gated on a different env flag than the
  status-call trace, so a calltrace-only run made a working read look dead; check which flag actually
  guards the log line before concluding the call never happened. **[confirmed]**
- **A trace line that prints an "instruction" may be printing a sentinel, not fetched memory.** The
  title log showed `[VU1 reserved lower] ... instruction=0xfffffff8`; dumping the instruction pair at
  the reported PC from the same interpreter showed valid microcode (`0x01e92802 ...`). Grep the
  report site: `0xfffffff8`/`0xfffffffb` were synthetic constants passed by the XGKICK packet walker
  for `FLG=3` (`IMAGE2`, wrongly treated as reserved) and for an oversized packet. Before blaming
  "garbage microcode", print the actual bytes at PC (or the register that fed the report) and check
  the constant against the code that emits it. Reference dumps then showed the microcode and
  `MSCAL` starts were already byte-identical, retiring the upload theory in one measurement.
  **[confirmed]**
- **Matching VU1 microcode, start addresses and input data do not clear the VU1 interpreter: trace
  the instructions.** On the title, the micro memory, `MSCAL` starts and UNPACKed data all matched
  the reference, yet every `XGKICK` read floats instead of a GIFtag. A per-instruction trace of one
  run (`PS2X_VU1_ITRACE=<startpc>`: pc, lower/upper words, VI registers) showed it at once:
  `IADDIU vi1, vi0, 0x4fb` then `JALR vi15, vi1` jumped through the *old* `vi1`. The interpreter
  applied the conditional-branch VI quirk to `JR`/`JALR`; PCSX2 applies it only to `IB*`. When
  the inputs match and the outputs don't, the bug is in the execution between them. **[confirmed]**
- **Check every diagnostic you rely on for mislabels before trusting its conclusion.** An executor's
  store trace was placed in the `LQ` case and printed "ISW", which suggested a store that never
  existed and sent the analysis down the wrong path. Read the trace code, not just its output.
  **[confirmed]**
- **A scripted press can land on the screen you're testing.** A backup `cross` press for the intro
  skip arrived after the title was up and started New Game, which looked like a title "freeze".
  Script exactly the presses the path needs and give them generous holds (`@15`); confirm in the
  log (`[padread-press]`) that each one landed. **[confirmed]**
- **Titles can pick a random scene each boot; don't gate on one reference image.** Suikoden
  Tactics shows a night campfire, a night town, a foggy town or a daytime fortress at random. Gate
  on "the menu is up and the background has scene detail", and compare with the reference only on
  a boot that rolled the same scene. **[confirmed]**
- **Profile self time before choosing a fix, and re-profile after each change.** `perf record -e
  cycles:u -F 999 -p <pid> -- sleep 20` + `perf report --no-children --stdio` works under
  `perf_event_paranoid=2`; call-graph sampling may be denied, but self time is enough to rank
  hotspots. In this task it moved the target from "VU1 getenv" (0.8 %) to the duplicate CPU
  rasterizer (~55 % of the title) and then to the VU1 dependency scan (~20 %). **[confirmed]**
- **A cached "not yet" must die when the waiter blocks.** The EE deadline probe cached a negative
  host-clock result for a window of guest cycles. While the guest ran that was fine, but once all
  threads blocked the only progress was one emulated cycle per dispatcher pass; the probe window
  took a long time to expire and the dispatcher spun rebuilding debug snapshots (99 % of one core,
  no VBlank). Re-arm the probe in `waitForEvent`, and let the dispatcher consume a *fresh*
  observation instead of trusting a memo from the checkpoint path. **[confirmed]**
- **When the target box has no `perf`, sample the game itself.** `PS2X_PROF=<file>` starts a
  per-thread `CLOCK_THREAD_CPUTIME_ID` timer and records the interrupted RIP into an mmap'd
  buffer; `tools/prof_report.py --after N` symbolises the title phase locally. One 90 s exclusive
  run ranked the whole title: vdso `clock_gettime` 20.7 %, IOP scheduler 6 %, VU1 commits 4 %,
  snapshot rebuilds 2 %, and the diagnostic store hook 1.5 %. Crop by phase with `--after`; the
  full capture is 85 MB. **[confirmed]**
- **Attribute shares with rules, and measure overlap separately from execution.**
  `tools/prof_report.py --buckets` folds a capture into VU1/GS/IOP/SPU2/MPEG/clock buckets and
  prints every symbol that matches no rule, so the table can be completed instead of eyeballing
  top symbols. For threading work, patch `0101-vu1-gs-shares` adds `PS2X_VU1_OVERLAP` and
  `PS2X_GS_SYNC` per-frame counters (zero cost when unset); `tools/share_report.py` aggregates
  them by scene. The number that matters is `min(VU1 time, EE work before the next
  observation)`, not the VU1 share: on the title the GIF-path ordering leaves only 9 % of VU1
  time overlappable until PATH1/PATH2/PATH3 are queued. **[confirmed]**
- **Re-run the audio content gate after *any* scheduler change, even one that is "only a cache".**
  A thread-order cache in the IOP kernel kept the documented order but still changed movie audio:
  the PSS correlation fell from 0.991 to 0.42 and the device ring overran ~1 000 times in 25 s.
  Speed and ctest both passed. Keep `movie_audio_check` + `audio_glitch_check` in the loop for
  scheduler/ordering work, and bisect patches against a clean 94-patch tree (the gate is a much
  sharper instrument than the vsync rate). **[confirmed]**
- **Check for diagnostics that are compiled into the normal build.** `PS2X_ENABLE_AGRESSIVE_LOGS`
  was ON: `PS_LOG_ENTRY`/`exit` wrapped every recompiled function and flushed `ps2_log.txt` on each
  call, writing 292 MB per 120 s. Release runs should compile that out; if it must stay, buffer it.
  The same rule applies to `std::getenv` in per-instruction interpreter code - cache it in a
  function-local static. **[confirmed]**
- **A second renderer is not free: suppress the one nobody displays.** PS2Recomp rasterized every
  GS primitive into its CPU VRAM while the PCSX2 GS also rendered it. When the external GS owns
  presentation, add a host seam to skip CPU pixel generation and copy the external local memory
  back before any guest readback (local-to-host IMAGE transfer or `ReadVram`); keep the CPU path
  as the fallback. That was the single largest title win (32 -> 44 vsync/s). **[confirmed]**
- **Count scheduler calls before rewriting a scheduler.** The IOP kernel's three full thread-map
  scans per `beginNextReady` looked harmless until a counter showed ~40 M calls and ~39 M
  `nextWakeCycle` calls in 70 s (one per ~64 IOP cycles, because `runEeCycles` feeds ~64 cycles at
  a time). One-pass selection plus a cached earliest Delay wake cycle removed the tree-increment
  hotspot from the menu and movie profiles. **[confirmed]**
- **Keep the semantic container, index the hot query.** `beginNextReady` still walked the
  id-ordered `std::map` on every one of those calls even when every thread was blocked: 27 % of
  the battle-menu game thread (`std::_Rb_tree_increment` 19 %, `beginNextReady` 7.9 %). The map's
  id order is part of the scheduling semantics and other scans depend on it, so keep it as storage
  and add a ready index ordered by (priority, id), a delay index ordered by (wakeCycle, id) and a
  dense id-to-thread table; selection is then a begin/lower_bound over ready threads and expired
  delays are the ones that leave the delay index. Give the old algorithm a read-only per-call
  verifier (`PS2X_IOP_SCHED_VERIFY=1`) that compares the chosen id, the promoted set and the cached
  delay wake before the new path mutates anything: a ready-index change is only safe when the old
  answer is proven equal in-process, not by a speed run. **[confirmed]**
- **Measure the cost of the context-switch mechanism itself, by reason.** The EE scheduler used a
  C++ throw per guest block/yield; counters + a timestamp at the throw and the catch showed
  185 500 yields/s × ~2.3 µs on the intro movie, i.e. **43 % of the game thread** (the libgcc_s
  share in the profile), while the menu/title/book paid only 0.6-1.9 %. Replacing the throw with a
  per-thread host stack and a hand-written x86-64 switch (`0102-ee-fibers`) cut the movie cost to
  ~0.8 %; `PS2X_EE_FIBERS=0` keeps the old path on the same binary for A/B. Keep the guest-visible
  resume rule ("re-dispatch at `ctx->pc`") and *abandon* the switched-out stack: resuming it would
  hold native locks across a guest wait and would double-execute the MPEG/CD completion callbacks
  that re-enter the syscall. Prove it with a switch trace (the old path is not deterministic
  run-to-run; compare reason/tid distributions), ctest under both mechanisms, and the movie-audio
  A/B. **[confirmed]**
- **A faster thread switch changes timing, and timing exposes old recompiler bugs.** After the fiber
  change New Game failed 2/4 vs 4/4 on the old path. The cause was a function whose last branch's
  delay slot was the next function's first instruction: the emitter substituted a NOP for it (11
  functions; lost `jr $ra` stack restores, a lost `beql` load). Before blaming the new mechanism,
  regenerate and scan the output for functions whose fall-through `pc` is not a registered entry,
  and audit what the new mechanism skips: `PS2X_EE_FIBER_AUDIT=1` walks the abandoned stack with
  `_Unwind_Backtrace` and lists frames that have exception tables (destructors/catches), the only
  thing an abandoned stack skips. Never "fix" such a stall by loosening the test or by
  re-executing an instruction at runtime. **[confirmed]**
- **Judge flaky gates by counts, not single runs.** 2 failures in 16 vs 0 in 8 is not evidence
  either way (p ≈ 0.4); run alternating pairs on the same binary with an env switch until the
  difference is clear, and keep full dumps (`PS2X_EE_THREAD_DUMP_AT`, `PS2X_EE_THREAD_DUMP_REGS`,
  `PS2X_EE_RAM_DUMP`) of every failing run. **[confirmed]**
- **When an interpreter optimization is not a pure deletion, keep a reference path and prove
  equivalence.** `PS2X_VU1_DIFF=1` runs each VU1 microprogram from identical copies of state on
  the old and new schedulers and compares VU1 data memory, VI/VF/ACC/Q/P/flags and every XGKICK
  packet. It caught two real timing bugs in the fast path (an over-aggressive cycle jump at the
  end of every instruction, and a missing commit at the cycle budget) before they reached a game
  run; the final title run reported 135 168 runs and 0 mismatches. **[confirmed]**
- **Synchronous guest-callback work must consume the derived clock budget, not add to it.** The
  IOP clock was `sum(eeCycles)/8` *plus* every instruction executed in RPC callbacks dispatched
  synchronously from the EE. The SPU2 is clocked from the IOP counter, so a 60 s movie run
  produced 62.35 s of audio - audible dropouts and A/V drift when played. Track the EE cycles
  seen and run only the difference to the current IOP cycle; instrument `totalCycles` vs
  `eeCyclesSeen/8` vs callback cycles to see it. A `--max-wav-fraction` gate on the captured WAV
  catches the regression. **[confirmed]**
- **Verify audio content against the source, not just loudness.** A WAV with the right RMS and
  duration can still be fragments of the source at random offsets. Build the expected PCM from
  the container (here `pss_wav.py` for the PSS `0xBD` track), align it, and require per-window
  waveform correlation (≥0.9 over 100 ms) before calling movie/BGM audio fixed. Loudness gates
  hid the movie problem for a full task. **[confirmed]**
- **Bound a media HLE's demux step; don't meter it to host time.** Instant CD/MPEG reads let a
  game demux a whole read (about a second of PSS audio) in one call, and a small guest audio
  ring then drops packets. A token bucket refilled from host/IOP clocks looked plausible but
  made the result depend on host speed, and a "Cross means skip" bypass could latch the same
  press that starts the movie. Cap the container bytes per demux call instead (here 8 KiB, about
  two audio packets, below the 25.6 KiB callback ring) and let the game's own loop provide
  backpressure. Compare stage captures (callback -> SIF -> ADMA) against container PCM with
  `tools/movie_audio_trace.py`. **[confirmed]**
- **A media callback may consume the container header itself.** The PSS first audio payload is
  `ff a0 00 00` + `SShd`/`SSbd` + PCM. The game's callback reads the 40-byte header from
  `dataAddr+4` and starts PCM at `dataAddr+44`; if the HLE also strips it, the game drops the
  first 40 PCM bytes. That is a 20-sample shift, and every 512-byte L/R block then contains 20
  samples of the other channel - audible as buzz/metallic roughness while correlation stays
  around 0.95. Capture the callback copies, not just the final WAV, before "fixing" framing.
  **[confirmed]**
- **Check the ADMA byte phase, not just per-block correlation.** A 512-byte-block correlator
  accepts a constant byte phase shift because each shifted block still matches a 512-byte window
  of the source. Print `expected_offset - block_index*512` (or `offset % 512`) and fail on a
  change; the first-packet bug was invisible to a correlation-only gate. **[confirmed]**
- **Host-stall debt replay must not advance the audio hardware.** Snapshot/VRAM work is excluded
  from guest time and repaid later, but repaying it into the IOP/SPU2 while the EE audio producer
  is stalled drains a small (24 KiB) ring; the AutoDMA then laps and repeats 128 ms of stale
  data. While a movie is active, discard that debt (pause the movie) instead of fast-forwarding
  its audio. A no-snapshot run passing while snapshot runs fail at the snapshot timestamps is the
  signature. **[confirmed]**
- **A port has two clocks: the guest's and the sound device's.** The SPU2/IOP
  clock follows the emulated CPU; the SDL device consumes 48 kHz of wall time.
  Do not let the device queue sawtooth at the block size: prefill a cushion,
  pause and refill after a starve, and trim `SDL_SetAudioStreamFrequencyRatio`
  with a slow PI loop (here +/-0.5%, 1.5 s smoothing) so pitch changes are
  inaudible. A wall-clock floor that keeps the audio hardware at real time can
  outrun a slow movie producer and lap a small IOP ring; feed the HLE at the
  container mux rate, charge long host-only decode work to `noteHostStall` so
  the hardware pauses with the producer, and let the output cushion absorb the
  remaining difference. Gate it with a 150 s underrun/overrun/ratio check, not
  a single WAV snapshot. **[confirmed]**
- **Measure the harness before trusting the number.** With `SUIKO_GS_SNAPSHOT_EVERY=60`, PCSX2's
  libpng snapshot path cost ~295 ms per frame on a 3D title scene (~5 ms per frame amortized)
  while the emulator used 1.6 ms/s - the every-60 title rate was capped at ~43 vsync/s although
  the game alone ran at 58.5. `PS2X_GS_TIME_TRACE=1` prints the snapshot read/write split; a
  faster PNG writer (stb level 1, same pixels) or a sparser snapshot interval separates game cost
  from measurement cost. **[confirmed]**
- **Do not shave one stage of the VU1 pipeline in isolation.** Resolving the
  per-cycle `commitReadyPipelines()` scan at compile time looked like a 17 %
  win, but every way of keeping a block's writes out of that scan (direct
  writes plus readiness notes, a lazy per-lane write table, or a bucketed
  commit scheduler) added more per-pair bookkeeping than it removed: 44.2-58.6
  ns/pair against the 42.9 ns/pair v2 baseline.  The honest unit of work is a
  whole region with registers in locals and one block version per incoming
  in-flight state (microVU-style), not a per-block patch.  Measure with
  `PS2X_VU1_BENCH=1 ... --path 1` before and after, and keep the differential
  harness as the correctness gate.  **[negative result]**
- **Distinguish OS descheduling from a host stall, and check the "movie active" flag's lifetime.**
  A wall-clock floor that excludes descheduled time runs ~0.5-0.6 % slow unless that time is repaid;
  but a freeze must stay in the slow-motion debt or it becomes a burst of due VBlanks/timers. Split
  the two: pay short off-CPU time (<= one VBlank) back immediately (the existing one-VBlank
  catch-up cap bounds the lurch), keep reported host work and long stalls in the 2x-surplus debt,
  and keep the movie rule that discards debt while audio is being produced. In this game the debt
  was not repaid on the title because `movieAudioActive()` stayed set after the intro: the game
  skips the movie without calling `sceMpegDelete`, and `sceMpegReset` did not clear the flag, so the
  movie rule zeroed the debt for the whole title. `PS2X_EE_TIME_TRACE` with a `movieMs` column and
  a developer A/B switch (`PS2X_EE_DESCHED_REPAY=0`) made the cause and the fix measurable.
  **[confirmed]**
- **At 60 fps, measure the host present separately from the VU1 worker.** The GS/present still
  runs on the EE thread (the VU1 thread only defers the present behind VU1 work). A per-VBlank
  trace (`SUIKO_VSYNC_TRACE`, `PS2X_EE_VBLANK_TRACE`) on the Legion showed `GSvsync` taking
  4-6 ms on 26 % of VBlanks under gamescope, while `beforeVblank()` stayed <= 20 us, no VU1 slow
  kick was logged, and the game was not descheduled: the thread was inside the Vulkan present.
  The game's vsync callback then ran late and missed its target, showing as 48-56 pictures/s
  seconds even though the VU1 worker had headroom. Matching the VBlank to the display refresh
  (59.855 Hz) and switching the swapchain to immediate present did not remove the stalls;
  lowering the internal resolution (`video.upscale=1`) did (max 1.6 ms) because the 3-deep
  Vulkan command-buffer queue was waiting on GPU work. Diagnose with the per-VBlank split before
  blaming the guest clock or VU1; the structural fix is a GS thread or a deeper queue, not a
  different VBlank period. **[confirmed]**
- **A DISPFB value change is not a picture count.** Suikoden Tactics reuses one framebuffer in
  the 3D scenes, so `flips` (old value != new value) is zero even at 60 pictures/s, and the
  per-vsync "drawn" count is fooled by drawing that straddles a VBlank. Count the game's display
  submissions instead (a DISPFB1 write is one picture): `SUIKO_FRAME_STATS=1` now prints
  `presents`, and `tools/frame_pace.py` turns the 60-vsync windows into per-wall-second rates
  (important under gamescope, whose 59.855 Hz mode makes a 60-vsync window 1.0024 s). Validate
  the plateau against `gifQw / qwords-per-picture`; do not gate on the raw drawn count.
  **[confirmed]**
- **Move the GS off the game thread when its present stalls become missed vsyncs.** With `GSvsync`
  on the EE thread, a GPU wait inside the present is guest-visible: the VBlank callback runs late and
  the game's limiter loses the next picture. A deeper Vulkan command-buffer queue (3 -> 5) can hide
  the dip in a lucky steady window (59.76 pictures/s, worst second 58, vs 59.62/57 for 3 buffers on
  the same 71 s window) but it does not remove the serialization, and it adds GPU-ahead latency.
  The structural fix is PCSX2 MTGS: queue every GS operation (register writes, copied GIF packets,
  the VBlank with its register snapshot) in order, run them on one GS thread, bound the queue to two
  VBlanks, and fence (`waitGsIdle`) at every game-thread readback. Measure with
  `SUIKO_FRAME_STATS=1` + `tools/frame_pace.py` under gamescope; diagnose with `SUIKO_VSYNC_TRACE`.
  **[confirmed]**
