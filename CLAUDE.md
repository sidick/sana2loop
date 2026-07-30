# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`sana2loop` builds **`loopback.device`**, a hardware-free SANA-II network
device for AmigaOS: echo, crossover, and pcap replay/capture modes, plus
companion Shell tools (`SanaInfo`, later `SanaDump`/`SanaSend`). It exists
because no null/dummy SANA-II device has ever existed, despite being the
obvious first tool for testing any network stack, shim, or driver-consuming
software -- deterministically, with zero hardware and zero emulator network
support required.

**Full design rationale and the phased plan live in the original proposal**:
https://github.com/sidick/project-ideas/blob/main/sana2loop-proposal.md --
read that first for *why* each design choice was made. This file tracks
*current state*; `docs/sana2-notes.md` is the distilled SANA-II protocol
reference (field names, command values, conventions the header alone
doesn't make obvious).

### The 68000 / Kickstart 1.3 baseline (non-negotiable for the device)

`loopback.device` itself targets plain 68000 + Kickstart 1.3: no
utility.library, no `ReadArgs`, no dos.library calls from the unit's
perspective. This is a deliberate, load-bearing design choice (a test
tool's value is universality) -- see the proposal for the full reasoning,
including why it matters for testing against **TheWire13** (a KS1.3-capable
TCP/IP stack on Aminet). **Companion Shell tools are not bound by this** --
they may require Kickstart 2.0+ where convenient (documented per-tool).

## Current state

Working through `docs/PLAN.md`'s milestones (M0-M12: spec-correctness
backfill, then Phases 2-4). **M0-M8 done** (validated on-target under
Copperline against both bundled AROS (CI) and a real Kickstart 1.3 ROM,
`make copperline-smoke` / `KICK=... make copperline-smoke`,
see `tests/copperline/`, before being considered complete). **M9 is
implemented and fully verified on real Kickstart 1.3** (`make
copperline-smoke KICK=...` passes every assertion, including the new
`scheduled`/`record` sub-tests and the post-run `RECORD_VERIFY=PASS`
check) **but currently hangs under bundled AROS** specifically once a
unit uses `replaymode=scheduled` or `record=` -- a real, not-yet-root-caused
AROS/Copperline compatibility issue (confirmed NOT simply "AROS doesn't
dispatch custom VBlank interrupt servers" -- see docs/PLAN.md's M9 risk
note for the full investigation). The public `copperline-smoke-aros` CI
job is expected to fail/hang on this until resolved.

- `src/loopback_device.c` -- the device. Hand-rolled Resident/auto-init
  (see "Architecture" below for why, and for the on-target Guru
  narrator.wyoming hit that this design avoids). Implements:
  - **Echo** (unit 0 default): `CMD_WRITE` delivers through `deliver_packet()`
    with real SANA-II precedence -- a queued **typed** `CMD_READ` (matched
    by `ios2_PacketType`) first, else a queued `S2_READORPHAN`, else
    (`rxqueue` config, defaults to **0** -- strict drop, no buffering) a
    bounded queue, else dropped with `Sana2DeviceStats.UnknownTypesReceived`
    counted. Src/dst addresses swap per the proposal's echo semantics.
  - **Real SANA-II buffer-management hooks** (M1): `OpenDevice()` parses an
    incoming `S2_CopyToBuff`/`S2_CopyFromBuff`/`S2_PacketFilter` tag list
    with a hand-rolled walk (no utility.library), replacing
    `ios2_BufferManagement` with a per-open cookie every subsequent request
    must carry -- every packet-payload copy goes through the completing
    request's own hooks (`CopyMem`-based defaults if none supplied).
  - `S2_DEVICEQUERY`, `S2_GETSTATIONADDRESS` (current in `ios2_SrcAddr`,
    factory/default in `ios2_DstAddr` -- M3), `S2_ONLINE`/`S2_OFFLINE`,
    `S2_ONEVENT` (completes **immediately** if the requested state already
    holds, else waits for the next transition -- corrected from an earlier,
    wrong "always edge-triggered" assumption after cross-checking a real
    driver, plipbox), `S2_GETGLOBALSTATS`, `S2_GETSPECIALSTATS` (zero-record
    stub), `CMD_FLUSH`, `AbortIO` (covers pending reads, orphan reads, and
    event waits).
  - **M3**: `S2_CONFIGINTERFACE` (one-shot per unit lifetime -- adopts
    `ios2_SrcAddr` as the current station address; a second attempt fails
    `S2ERR_BAD_STATE`/`S2WERR_IS_CONFIGURED`; deliberately *not* cross-gated
    with `S2_ONLINE` either direction, since the factory address already
    makes the unit usable with zero setup) and `S2_TRACKTYPE`/
    `S2_UNTRACKTYPE`/`S2_GETTYPESTATS` (fixed 8-slot per-unit table, real
    counts fed from `CMD_WRITE`/`deliver_packet()` traffic, not a stub).
  - **M4**: `S2_BROADCAST` (forces `ios2_DstAddr` to all-1s, confirmed
    against plipbox) and `S2_MULTICAST` (validates the caller's address has
    the Ethernet group bit set) both share `CMD_WRITE`'s body via a new
    `do_write()` helper; unlike a plain write, their delivered destination
    is the real address, not this unit's station address, so a reader's
    `SANA2IOF_BCAST`/`SANA2IOF_MCAST` (recomputed every read in
    `fill_read_from_packet()`) mean something. `S2_ADD/DELMULTICASTADDRESS`
    add a refcounted fixed 8-slot per-unit group-membership table
    (bookkeeping only -- no per-open receive filtering exists to gate on
    it). `SANA2IOF_RAW` relaxes the MTU-exceeded check by 14 bytes
    (deliberately not a full raw-frame reinterpretation -- see
    `docs/sana2-notes.md`). Surfaced a real, confirmed-on-both-AROS-and-1.3
    AmigaOS gotcha along the way: `SendIO()`/`DoIO()` both clear `io_Flags`
    before dispatch (per their own autodocs), silently discarding
    `SANA2IOF_RAW` -- only `amiga.lib`'s `BeginIO()` preserves it, hence
    `sana2test.c`'s hand-rolled `flagged_begin_io()` (this build links no
    amiga.lib). See `docs/sana2-notes.md`'s M4 section for the full story,
    including a real real-1.3-only hang that turned out to be a missing
    `a1` clobber in that inline asm, not an emulator quirk.
  - **M5**: config-file infrastructure -- `ENV:sana2loop/unit<N>.config`,
    falling back to `S:sana2loop/unit<N>.config`, read once at first-open
    (same 0->1 trigger as `reset_unit()`) and only from a real Process
    opener (a bare Task gets compiled-in defaults, no dos.library touched
    at all). Only `mtu`/`rxqueue` are recognized so far -- the only two
    per-unit knobs that already exist as real device state; the rest of
    the proposal's key vocabulary lands with the milestones that actually
    consume it (M6-M9). Fail-fast: an unrecognized key or malformed line
    in a file that DOES exist fails the whole `OpenDevice()` call with
    `S2ERR_BAD_ARGUMENT` -- a missing file entirely is not an error.
    Surfaced two real on-target findings while proving docs/PLAN.md's
    "DOS I/O under Forbid in open()" risk: `Open("ENV:...")` hangs
    indefinitely (not a clean failure) when `ENV:` isn't assigned --
    exactly this project's own minimal boot ADF, and any real deployment
    where `ENV:` happens to be unassigned -- fixed via the process's
    `pr_WindowPtr = (APTR)-1` (the standard AmigaOS "suppress DOS
    requesters, fail cleanly" mechanism), not by special-casing the test
    ADF; and real Kickstart 1.3's dos.library file I/O is measurably
    slower than AROS's (`tests/copperline/run.sh`'s `KICK=` benchmark
    default needed raising, 10s -> 30s). See `docs/sana2-notes.md`'s M5
    section for the full story.
  - **M6**: crossover mode. A unit is either standalone echo (unchanged)
    or crossover-paired with exactly one other unit -- config-only
    concept (`mode=echo|crossover`, `peer=<unit>`), compiled-in default of
    units 2<->3 paired, everything else echo. `do_write()` now resolves a
    delivery TARGET (`unit` for echo, `&sld->sld_Units[unit->su_Peer]` for
    crossover) and uses real, unswapped addressing for any crossover write
    (src = writer's own station, dst = exactly what was specified) --
    fixing a latent M4 bug where echo's own broadcast/multicast reused the
    swap machinery's `src=ios2_DstAddr` line unconditionally, so a
    broadcast frame's "source" was wrongly the broadcast address itself
    (never caught since M4's tests only checked delivered destination).
    `deliver_packet()` gained real destination-address filtering for
    crossover-mode targets (`dest_matches_unit()`: unicast/broadcast/
    joined-multicast, or the specific candidate reader's own
    `SANA2OPF_PROM` -- finally given real meaning, previously ignored
    entirely). Echo mode stays fully unfiltered by design. `SANA2LOOP_NUM_UNITS`
    is now 5 (was 4): a spare unit 4 exists so M5's malformed-config test
    and M6's crossover test (needing units 2 and 3 to both open cleanly)
    don't collide over the same fixture. See `docs/sana2-notes.md`'s M6
    section for the full story.
  - **M7**: fault injection pipeline, all off by default. Fixed order in
    `do_write()` after `CopyFromBuff`: MTU-vs-peer (`oversilent=1` -- a
    real PMTUD black hole with mismatched crossover MTUs: the write
    succeeds, `io_Error` stays 0, but the packet silently never arrives)
    -> `droppct` -> `truncate` -> `duppct` (a genuine second
    `deliver_or_reorder()` call with a `CopyMem`-duplicated packet, not
    the same buffer referenced twice) -> `reorder` (fixed 8-slot
    PRNG-permuted buffer, flushed in real arrival order on `S2_OFFLINE`/
    `CMD_FLUSH`/last-close). `offlineafter`/`onlineafter` auto-flap
    online/offline off write-attempt counters, simulating a flaky link.
    PRNG is xorshift32, seeded from config (`seed=N`, 0 coerced to 1),
    re-seeded fresh every 0->1 open transition. Percent thresholds and the
    reorder buffer's index draw both avoid needing 32-bit division (no
    libgcc in this build) -- so despite docs/PLAN.md flagging M7 as the
    milestone likely to finally force the deferred `-lgcc` decision, it
    still didn't. `SANA2LOOP_NUM_UNITS` is now 9 (was 5): five more
    fixture-dedicated units for droppct/duppct/reorder/offlineafter-
    onlineafter, plus two new config keys added to the existing M6
    crossover pair (2/3) for the oversilent/PMTUD test -- harmless to M6's
    own assertions there, which only ever write tiny 20-byte payloads.
    Also fixed a latent M4 bug (real, unswapped addressing was already
    needed for M6, and turned up that M4's broadcast/multicast had reused
    echo's swap-mode source-address line unconditionally) and found a
    real on-target harness bug of its OWN (a third test `IOSana2Req` used
    for `S2_READORPHAN` without ever calling `OpenDevice()` on it first --
    hung identically on both AROS and real 1.3, so a genuine bug, not a
    quirk). See `docs/sana2-notes.md`'s M7 section for the full story.
  - **M8**: `src/pcap.h` -- hand-rolled classic pcap format, both endians
    (magic-based per-file byte-order detection, since a big-endian 68000
    reading a little-endian host capture needs to byte-swap every OTHER
    numeric field despite nothing about the 68000 changing), no I/O in the
    header itself. `mode=replay` + `replay=<path>` (a dos.library-openable
    pcap file, loaded once at open time by `load_replay_file()`) plays the
    capture back in order, one record per `CMD_READ`/`S2_READORPHAN`
    ("consume mode" -- no timestamp/VBlank scheduling at all; M9 owns that
    variant) -- both commands behave identically on a replay unit, ignoring
    `ios2_PacketType` entirely. `CMD_WRITE`/`S2_BROADCAST`/`S2_MULTICAST`
    on a replay unit fail `S2ERR_NOT_SUPPORTED`. Unlike a missing CONFIG
    file (defaults apply), a missing/malformed REPLAY file fails the whole
    `OpenDevice()` -- there's no sensible default capture to fall back to.
    Found two build-breaking, libgcc-adjacent miscompiles while building
    this: a hand-rolled `dst[k]=src[k]` copy loop for the `replay=` config
    value became an unavailable `memcpy` call (fixed via `CopyMem`, the
    established pattern), and `load_replay_file()` getting inlined into
    `open()` made the compiler rematerialize the `unit` pointer via a
    runtime multiply-by-`sizeof(struct Sana2LoopUnit)` (`__mulsi3`, also
    unavailable) instead of keeping it live in a register -- fixed with
    `__attribute__((noinline))`, since every other array-of-unit-structs
    index site in this file needs the identical multiply and has so far
    stayed under this backend's shift/add synthesis budget; this one
    didn't, once inlined. Also found, empirically, that
    `SANA2LOOP_MAX_REPLAY_PACKETS` (the fixed cap on loaded records)
    couldn't be as generous as first guessed (32, ~49 KB): that reliably
    failed its own `AllocMem()` under bundled AROS specifically, since
    AROS has to load its own Kickstart replacement into the same
    512K+512K test profile a real ROM never touches, leaving measurably
    less free RAM. Settled on 16 (~25 KB) at M8 -- **M9 had to shrink this
    again, to 4** (~6 KB), after its own per-unit field growth (plus a
    second replay-mode unit having to coexist with the first one's
    still-held buffer, since this device only frees replay memory at the
    NEXT open, never at close) ate further into that headroom. See
    `docs/sana2-notes.md`'s M8/M9 sections for the full story.
  - **M9**: one VBlank interrupt server (`vblank_server()`,
    `AddIntServer(INTB_VERTB, ...)`, added once at device init) drives two
    new features via the same tick: `replaymode=scheduled` (an alternative
    to M8's on-demand `consume` default) delivers each loaded record
    through the real `deliver_packet()` precedence at its own captured
    timestamp's due tick (whole-second granularity, deliberately not
    microsecond-exact -- see `docs/sana2-notes.md`), and `record=<path>`
    (+ `recordmax`, default 64 KB) taps that same `deliver_packet()` to
    build a RAM-buffered pcap capture of whatever a unit actually
    receives, flushed to file at last close. Verified on real Kickstart
    1.3: all assertions pass, including a real S2_CopyToBuff hook
    correctly invoked from actual VBlank interrupt context (the
    project's `docs/PLAN.md`-flagged risk for this milestone), and a
    post-run host-side byte-compare of the flushed pcap file
    (`tests/copperline/verify_record.py`) against the 3 known writes that
    produced it. Found two real bugs along the way: a latent, since-M7
    off-by-assumption in `build_config_path()` (its one-line `'0' +
    unit_index` digit substitution silently mis-encoded unit numbers past
    9, never triggered before M9 since every unit number through M8 was
    still single-digit); and confirmed empirically (via a standalone
    write-then-read-back test, before building the real feature on the
    assumption) that Copperline persists floppy writes to the host ADF
    file, but only once `write_protected = false` is set in
    `[floppy.dfN]` (defaults to `true`). **Known, unresolved on bundled
    AROS specifically**: the guest hangs once a unit actually uses
    `replaymode=scheduled` or `record=` -- confirmed via an isolated
    standalone test that this is NOT simply "AROS doesn't dispatch custom
    VBlank interrupt servers" (it does, verified). See `docs/PLAN.md`'s M9
    risk note and `docs/sana2-notes.md`'s M9 section for the full
    investigation so far.
  - 12 units pre-allocated in the device base (unit 0 wired for echo by
    default; units 2-3 default-paired for crossover; units 4-11
    spare/echo, each claimed by exactly one on-target test's own config
    fixture so no two tests' expectations ever collide on the same unit
    -- see `tests/copperline/run.sh`'s fixture layout for the current
    allocation). A unit fully resets (`reset_unit()`: offline, factory
    address, stats zeroed, `rxqueue` back to 0, mode/peer/fault-injection
    knobs all to their compiled-in defaults) the moment its open count
    goes 0->1, then (M5) has its config file read -- "config re-read on
    next open" now means exactly this. No separate unit task -- echo
    completes synchronously inside `BeginIO` on the calling task; that
    path itself still never touches dos.library (only `open()`'s M5
    config read does), so the echo/delivery path's own 1.3 compliance is
    unchanged.
- `src/sanainfo.c` -- `SanaInfo`, the Phase 1 validation tool: opens a unit,
  optionally issues `S2_ONLINE`/`S2_OFFLINE`, prints `S2_DEVICEQUERY` +
  station address. Normal clib2 CLI program (V36+ Exec calls -- see the
  file header for why that's fine for a tool but wouldn't be for the
  device).

- `tests/copperline/sana2test.c` -- the on-target smoke test (see "Testing
  under Copperline" below). Freestanding like the device itself, for a
  reason specific to itself (not the device): see the comment block at its
  top for a real on-target bug this tripped over and how it was found.

Not yet done: `SanaDump`/`SanaSend` (M11), Aminet packaging (M12). M9
(VBlank tick-scheduled replay + RAM record) is implemented and real-1.3-verified
but has an open AROS-under-Copperline compatibility issue -- see "Current
state" above.

## Architecture

### Why hand-rolled Resident/auto-init, not libnix's `devinit.o`

The sibling project **narrator.wyoming** (`/Users/simond/src/narrator.wyoming`)
built its device via libnix's `devinit.o` framework and hit a real on-target
crash (Guru `#8100 0005`, `AN_MemCorrupt`) from a stray C global in the
device -- because `devinit.o`'s `-fbaserel` instance-data model doesn't
reliably point A4 at the device's data segment inside every hook. That
project's workaround was "no globals, ever, in the device" -- workable
because `narrator.device` has no state shared *across* independent opens
(each open owns its own private connection).

`loopback.device` can't take that shortcut: echo/crossover mode's entire
point is that two independent opens of the same unit see each other's
packets, which requires real shared state. `devinit.o` also hands back a
**fixed-size** opaque device base (`sizeof(struct libBase)`, baked into the
precompiled `devinit.o` -- there is no way to extend it from our code), so
there's nowhere to put that shared state even if the A4 hazard weren't a
concern.

The fix: skip `devinit.o` entirely and hand-roll the Resident/auto-init
table ourselves (the pattern bebbo's own toolchain examples use --
see `docs/sana2-notes.md` and the git history of `src/loopback_device.c`
for the reference material this was built from). That lets us size the
device base to `sizeof(struct Sana2LoopDevice)` (device header + a
fixed array of per-unit state), reached explicitly via the `dev` (a6)
parameter every hook (`open`/`close`/`begin_io`/`abort_io`/`expunge`)
already receives as a real C parameter -- no global, no A4 tricks, no
`-fbaserel` at all in this build (so the entire hazard class narrator.wyoming
had to work around doesn't apply here).

### Echo mode without a unit task

A "real" SANA-II driver typically runs a background unit task that owns
`unit_MsgPort` and processes queued I/O asynchronously (needed when the
work involves waiting on hardware or, per the 1.3 rules, dos.library).
`loopback.device`'s echo logic needs neither: a `CMD_WRITE` either finds a
already-queued `CMD_READ` and completes it immediately (`ReplyMsg` from
inside the writer's own `BeginIO` call -- perfectly legal; `ReplyMsg` just
posts to whatever port the reader's IORequest names, regardless of which
task calls it), or queues the packet for the next `CMD_READ` to pick up.
Both the pending-reads list and the pending-echoed-data list are wrapped in
`Disable()`/`Enable()` critical sections since multiple tasks can call
`BeginIO`/`AbortIO` on the same unit concurrently.

## Build / test

The Amiga build uses the **Bebbo m68k-amigaos GCC** cross-toolchain, same
pinned `ghcr.io/reinauer/container-amiga-gcc` image narrator.wyoming uses
(see that project if the pinned digest ever needs rolling forward).

- `make docker` -- cross-compiles `build/loopback.device` and
  `build/SanaInfo` inside the container; no local toolchain needed.
- `make amiga` -- same, using a local `m68k-amigaos-gcc` on PATH.
- `make device` / `make tools` -- build just one or the other.

`make docker` has been run successfully in this environment (both binaries
build clean), and `make copperline-smoke` has confirmed every sub-test
through M8 on-target against both bundled AROS and real Kickstart 1.3, and
every M9 sub-test on real Kickstart 1.3 (bundled AROS currently hangs on
M9's `scheduled` sub-test specifically -- see "Current state" above) (see
"Testing under Copperline" below and `tests/copperline/sana2test.c`'s
`SUB=` list).

No release/versioning/Aminet packaging infrastructure yet -- that's Phase 4
per the proposal; `version.mk` exists now only so the device's `$VER`
string has a single source of truth from day one.

## Testing under Copperline (project preference -- not Amiberry)

`tests/copperline/` holds the on-target smoke test: `sana2test.c` (harness),
`machine.toml` (68000/1MB profile), `adf-startup-sequence.txt` (one line:
`sana2test`), `run.sh` (builds a bootable ADF via `xdftool` and asserts the
serial output). Run it with `make copperline-smoke KICK=/path/to/kick13.rom`
(local, real Kickstart 1.3) or with `KICK` unset (bundled AROS, weaker but
ROM-free -- what public CI runs; see the `copperline-smoke-aros` CI job).
Any released `copperline` works for both -- see the `[[filesys]]` note
below for why no special build is needed here.

- **Prefer a real Kickstart 1.3 ROM (`KICK=`) locally.** AROS can't validate
  this project's actual 1.3-fidelity claims -- it's a fine ROM-free stand-in
  for catching gross regressions in CI, but confirm against real 1.3 before
  trusting a Phase 1 claim or cutting a release.
- **A real 68000+`[[filesys]]` doorbell bug exists in Copperline 0.12.0/
  0.13.0** (any host-directory mount hangs forever on a 68000/68010 guest,
  bundled AROS or a real ROM alike -- root cause: the bug's fix is on the
  unmerged `fix/filesys-68000-doorbell-split` branch of the local checkout
  at `/Users/simond/src/external/Copperline`). **This project's own test
  doesn't hit it** -- it boots from a plain floppy image, never
  `[[filesys]]`, confirmed empirically with released 0.13.0 against both a
  real 1.3 ROM and AROS. Only relevant if a *future* test here wants a
  host-directory mount instead of a floppy -- that would need the fix
  branch built in an isolated worktree (`git -C
  /Users/simond/src/external/Copperline worktree add <dir>
  fix/filesys-68000-doorbell-split && cd <dir> && cargo build --release`;
  check `worktree list` first, one may already exist from prior work).
- **Kickstart 1.3 does not autoboot a `[[filesys]]` host-directory mount** --
  confirmed empirically: a real 1.3 ROM shows the "insert Workbench disk"
  hand icon and waits for a floppy in DF0, unlike 2.0+'s boot-node scanning
  across Zorro-autoconfig filesystem devices. Boot from an actual ADF
  (`[floppy.df0] path = ...`) instead.
- **Use a minimal from-scratch bootable ADF, not a full Workbench disk.** A
  real Workbench 1.3 image takes ~90 emulated seconds to reach a usable
  state under `warp_speed = "max"` (disk-validator/font/icon overhead this
  test doesn't need) -- a from-scratch disk with just
  `DEVS:loopback.device` + `C:sana2test` + a one-line `Startup-Sequence`
  (built via `xdftool ... format ... + boot install boot1x + ...`, see
  `run.sh`) reaches `RESULT=PASS`/`FAIL` in under 10. `[floppy] speed = 0`
  (turbo DMA, no realistic seek/rotation delay) cuts wall-clock further on
  top of that -- there was no reason to accept realistic floppy timing for
  a test that doesn't care about it.
- **The on-target test harness (`sana2test.c`) must itself be freestanding
  (`-nostdlib -nostartfiles`), like the device -- for a different reason
  than the device's own.** A normal linked `-noixemul` program either
  failed to launch under real 1.3 (`utility.library failed to load` -- its
  crt forces a V37 auto-open regardless of `__oslibversion`) or crashed
  outright. Freestanding sidesteps the crt entirely.
- **A freestanding AmigaDOS command's real entry point must be textually
  first in the file, AND contain no data of its own -- not just marked
  `__attribute__((no_reorder))`.** This bit twice while chasing an
  on-target "task held" Guru that reproduced on every single run,
  regardless of Kickstart version or floppy image:
  1. An early version defined helper functions before `_start` in the
     source. `no_reorder` only stops the compiler moving `_start` under
     optimization -- it does not override source order, so a helper's code
     landed at hunk offset 0 instead. AmigaDOS (which always jumps to
     offset 0 of the first hunk, with no named-entry-symbol concept) jumped
     straight into that helper with uninitialized arguments and globals.
  2. Moving `_start` first fixed that, but its body called `raw_str("...")`
     with inline string literals -- this freestanding build's backend (no
     linker script, and evidently no distinct `.rodata` section: `objdump
     -h` on the `.o` shows only `.text`/`.bss`) pools **every** string
     literal used anywhere in the translation unit into a block at the
     very front of `.text`, regardless of which function references them
     or their source order. That pooled string data, not code, ended up at
     offset 0. Fix: give `_start` a trivial body that only calls a
     `test_main()` holding all the real logic (reached by an ordinary call,
     so its position doesn't matter), AND convert every message from an
     inline literal to a named **non-const** `char[]` (plain `const` still
     got pooled into `.text` the same way -- only ordinary initialized data
     lands in the separate `.data` hunk).
  - **Found by static disassembly (`objdump -D`) in seconds, not by
    attaching gdb to a live session** -- gdb-attached execution was
    dramatically slower (breakpoint/single-step overhead) than a plain run,
    to the point of being impractical to iterate on for this. Prefer a
    static look at the actual built binary before reaching for a live
    debugger, especially for "what code is actually at this address"
    questions.
- **Copperline does support breakpoints and gdb** (`--gdb ADDR`,
  `monitor loadseg-break` to catch a program's load without knowing its
  address up front, `--control ADDR` JSON-RPC for programmatic
  breakpoints/state) -- useful and worth reaching for when a bug needs
  *runtime* state (register/memory contents, call history) that a static
  disassembly can't show. Recipe that worked here: `target remote
  localhost:PORT`, `monitor loadseg-break`, `continue` (stops the instant
  the program's seglist installs and reports its base address), then
  `add-symbol-file <binary> <that address>` in the *same* gdb session
  (reconnecting fresh left the remote target in a running state gdb
  wouldn't `continue` from again). Use `/opt/amiga/bin/m68k-amigaos-gdb`
  (local), not a copy inside the cross-compiler Docker container -- the
  container's `localhost` isn't the host's, so `target remote
  localhost:PORT` silently never connects there (a documented dead end in
  the sibling amiauth project's own Copperline notes).

### Why the real-1.3 ROM stays out of CI

A Kickstart ROM is Cloanto/Hyperion-licensed software, not something this
project can redistribute. GitHub Actions secrets also cap out at 48 KB --
moot anyway here, since a 1.3 ROM dump is 256-512 KB. Encrypting the ROM
and committing the ciphertext to the repo (storing only a decryption
passphrase as a secret) is a known pattern for oversized secrets in
general, but doesn't change the redistribution question: the bytes would
still ship inside this **public** repo's git history. So: the real-1.3 run
stays a local/manual step (`make copperline-smoke KICK=...`, before trusting
a Phase 1 claim or cutting a release); public CI runs the same test against
the bundled, redistributable AROS replacement instead (`copperline-smoke-aros`
in `.github/workflows/ci.yml`) -- weaker, but catches gross regressions
without needing a licensed asset. A self-hosted runner (ROM lives only on
hardware under this project's control, never uploaded anywhere) would be
the option if full automated 1.3 coverage is ever wanted; not set up.
