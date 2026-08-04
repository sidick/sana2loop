# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

`sana2loop` builds **`loopback.device`**, a hardware-free SANA-II network
device for AmigaOS: echo, crossover, and pcap replay/capture modes, plus
companion Shell tools (`SanaInfo`, `SanaDump`, `SanaSend`). It gives any
network stack, shim, or driver-consuming software a deterministic test
partner -- with zero hardware and zero emulator network support required.
(The docs originally claimed no null/dummy SANA-II device had ever
existed; **nullsana** (Aminet `comm/tcp/nullsana`, 2000, binary-only,
based on Pekka Pessi's agnet.device) turned out to be prior art, found
only after the first release -- the shipped docs now say so instead of
claiming primacy. sana2loop still differentiates on being open source,
KS1.3-capable, and on pcap replay/record, fault injection, and
conformance tooling -- see GitHub issues #24-#30 for nullsana-inspired
feature ideas.)

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

Working through `docs/PLAN.md`'s milestones (M0-M13: spec-correctness
backfill, then Phases 2-4). **M0-M13 done**, each validated on-target under
Copperline against both bundled AROS (CI) and a real Kickstart 1.3 ROM
(`make copperline-smoke` / `KICK=... make copperline-smoke`,
see `tests/copperline/`) before being considered complete. M9 initially
shipped with bundled AROS hanging specifically once a unit used
`replaymode=scheduled` or `record=`, while real Kickstart 1.3 passed
cleanly -- root-caused (not the AllocMem-in-interrupt theory first
suspected) to a bundled-AROS `CheckIO()`/`WaitIO()` bug that can falsely
report an `IOSana2Req` as already complete immediately after `SendIO()`,
before the device has actually replied it, for requests destined to
complete via a genuine hardware-interrupt-driven `ReplyMsg()` (M9's
`vblank_server()`) rather than task-context completion. Fixed entirely in
the test harness (`reap_interrupt_io()` in `tests/copperline/sana2test.c`,
waits on a real side-effect of delivery instead of trusting
CheckIO()/WaitIO(), then unconditionally reaps via AbortIO()+WaitIO()) --
no device-code change was needed for the actual fix. See docs/PLAN.md's M9
risk note for the full investigation, including how it was root-caused via
on-target instrumentation.

**M10** (docs pass): stood up `userdocs/` (11 pages, MkDocs Material) plus
`tools/docs2guide.py` (adapted from sibling project amiauth's own,
algorithm unchanged) producing an on-Amiga AmigaGuide manual from the same
source (`make guide`) -- one docs source feeding both. Published later
(2026-07-31, once the repo went public -- see the pre-release-verification
entry below) at https://sidick.github.io/sana2loop/ via a new
`.github/workflows/docs.yml` (mike-based versioned deploy, adapted from
amiauth's own docs.yml); PRs/pushes to main still only get a CI
build-only check (`docs-build` in `.github/workflows/ci.yml`) so the site
can't silently rot between real publishes. Also fixed
a real, previously-missing gap surfaced while writing the docs: neither
the device nor `SanaInfo` had ever carried a standard AmigaDOS `$VER:`
version cookie (so the Shell `Version` command found nothing for either) --
added to both, and `DEVICE_DATE` reformatted to the numeric `dd.mm.yyyy`
the cookie format actually requires (a textual month never parsed as one).
The device's own top-of-file overview comment was still describing "Phase
1" only (echo + a handful of commands) despite M0-M9 having long since
added crossover, replay, record, and fault injection -- rewritten to match
current reality.

**M11** (companion tools): `SanaDump`/`SanaSend` (V36+/2.0+ Shell tools,
like `SanaInfo`), `src/tools/pcapio.c` (host/tool-side pcap file I/O) and
`src/tools/sana2open.c` (shared "open with real buffer-management hooks"
helper -- a model-citizen reference, not just a bare `OpenDevice()`), plus
`host/make_fixtures.py`/`host/lint_pcap.py` and a top-level `fixtures/`
canonical corpus (ARP, DHCP discover, TCP handshake). Building the
on-target round-trip test surfaced two real, non-device-code findings:
(1) clib2's `fopen()` reliably failed under Copperline's minimal
Startup-Sequence boot where plain dos.library `Open()` didn't -- fixed by
switching `pcapio.c` to dos.library I/O entirely, matching
`loopback_device.c`'s own established pattern; (2) running the tools in
the SAME Copperline boot session right after `sana2test.c`'s own full run
reliably ran out of chip memory (confirmed via `AvailMem()`: ~9 KB free)
because the device's own "never free replay-mode buffers at close" design
(see M8/M9) leaves them permanently allocated for as long as it stays
loaded -- fixed with a **separate** Copperline boot session for the tools
(`tests/copperline/run-tools.sh`/`machine-tools.toml`) with a more
generous memory profile (SanaDump/SanaSend are documented 2.0+
convenience tools, never claimed to run on the device's own bare-512K
target floor). See docs/PLAN.md's M11 risk note for the full
investigation, including a third, genuinely test-design gap (reopening a
unit resets it, discarding buffered traffic -- fixed by keeping SanaDump's
own open alive across both tools via `Run`).

**Post-M11** (found by code review while documenting the device's memory
behavior, not on-target testing): the device never calls `AddMemHandler()`
(no participation in Exec's low-memory recovery protocol -- invisible to
system memory pressure until its own `AllocMem()` fails, handled locally
with no retry), and every `AllocMem()` it makes asks only for `MEMF_CLEAR`
(fast RAM preferred, chip only as fallback -- the M11 "chip RAM exhausted"
findings above were the test machine profile's lack of any `fast=` memory
at all, not a device preference for chip). Writing that up surfaced a real
gap: `expunge()` (Exec unloading the whole device once its device-wide
`lib_OpenCnt` hits 0 -- e.g. via the Shell's `Avail FLUSH`) froze only the
device base struct, never looping over units first to free any held
replay/record buffers -- worse than the documented "held until next open"
cost, since those buffers become permanently unreachable once the base
itself is freed. Fixed by looping over every unit and calling
`reset_unit()` on each (reusing its existing free logic) before the base's
own `FreeMem()`. Also documented a manual, no-reboot recipe for reclaiming
a unit's held memory (close it, drop `replay=`/`record=` from its config,
reopen once) and the whole-device recipe (`Avail FLUSH` once every unit is
closed) -- see `docs/sana2-notes.md`'s "Memory lifetime" section and
`userdocs/Replay-and-Record.md`. Verified the fix via `make
copperline-smoke` (bundled AROS) and `run-tools.sh`, both still green.

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
    driver, plipbox), `S2_GETGLOBALSTATS`, `S2_GETSPECIALSTATS` (zero
    records unless `record=<path>` (M9) is active and has actually dropped
    a packet to `recordmax` -- see M9's own entry below), `CMD_FLUSH`,
    `AbortIO` (covers pending reads, orphan reads, and event waits).
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
    `[floppy.dfN]` (defaults to `true`). Bundled AROS specifically first
    shipped hanging once a unit actually used `replaymode=scheduled` or
    `record=` -- confirmed via an isolated standalone test that this was
    NOT simply "AROS doesn't dispatch custom VBlank interrupt servers"
    (it does, verified), and eventually root-caused to a bundled-AROS
    `CheckIO()`/`WaitIO()` false-completion bug for interrupt-driven
    replies, fixed in the test harness (`reap_interrupt_io()`), not the
    device -- see `docs/PLAN.md`'s M9 risk note and
    `docs/sana2-notes.md`'s M9 section for the full investigation.
  - 19 units pre-allocated in the device base as of M17 (unit 0 wired for
    echo by default; units 2-3 default-paired for crossover; every other
    unit spare/echo, each claimed by exactly one on-target test's own
    config fixture so no two tests' expectations ever collide on the same
    unit -- see `tests/copperline/fixtures/` for the current allocation). A unit fully resets (`reset_unit()`: offline, factory
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

- `userdocs/` (M10) -- the user-facing documentation site source (MkDocs
  Material, `mkdocs.yml`), 13 pages covering installation, configuration,
  every mode, fault injection, `SanaInfo`, `SanaDump`/`SanaSend`,
  SANA-II Rev 2/3/4/7 conformance/`SanaConform` (M13), troubleshooting,
  and a changelog. `tools/docs2guide.py` converts the same source into an
  on-Amiga AmigaGuide manual (`make guide`) -- one source, two outputs.
  Published at https://sidick.github.io/sana2loop/ (`.github/workflows/docs.yml`,
  mike-based versioned deploy); every push to main still only gets a CI
  build-check (`docs-build` job) so the site can't rot between real
  publishes.

- `src/tools/` (M11) -- `sanadump.c`/`sanasend.c` (the CLI tools), plus
  two shared building blocks: `pcapio.c`/`.h` (host-side pcap file I/O,
  dos.library-based -- see that header's own comment for why not stdio)
  and `sana2open.c`/`.h` (shared "open with real, explicit buffer-
  management hooks" helper -- a model-citizen reference for consumers,
  not just a bare `OpenDevice()`). Each tool compiles+links directly
  against both shared files in one `gcc` invocation (see the Makefile) --
  no intermediate `.o` management needed at this project's size.
  `sanaconform.c` (M13) is the odd one out: it does its OWN manual
  `OpenDevice()` with a richer 9-tag list (base + Rev 2/3 16/32-bit +
  Rev 7 DMA hooks) rather than reusing `sana2open.c`'s fixed 3-tag
  helper, so it doesn't link against the shared building blocks at all.

- `host/make_fixtures.py`/`host/lint_pcap.py` (M11) -- a `scapy`-based
  canonical `.pcap` corpus generator (ARP, DHCP discover, TCP handshake,
  written to top-level `fixtures/`) and a general-purpose structural
  linter (magic/linktype/header-size/monotonic-timestamp checks) --
  distinct from `tests/copperline/fixtures/`, which holds on-target-
  test-specific captures with fixed byte content the harness asserts
  against exactly.

- `tests/copperline/run-tools.sh` (M11) -- the SanaDump/SanaSend
  on-target round-trip test, in its own **separate** Copperline boot
  session (`machine-tools.toml`) from the device's own `run.sh` -- see
  that file's own comment for why (short version: the device's own test
  run leaves too little chip memory for a second clib2 program to load
  afterward in the same session). `tools-startup-sequence.txt` runs
  SanaDump first (backgrounded via `Run`, keeping unit 12's open count
  above 0 across SanaSend's later write -- reopening it would trigger
  `reset_unit()` and discard whatever was buffered); verified via
  `verify_sanadump.py` extracting the produced capture post-run, the same
  pattern as `run.sh`'s own M9 record= check.

**M12** (Aminet packaging + release flow): adopted sibling project
amiauth's pattern (tag-driven `release.yml`, verify-then-build-then-gate)
in preference to narrator.wyoming's simpler one. New `sana2loop.readme`
(Aminet-format package readme, `Type: comm/net`) and `Makefile` `lha`/
`dist` targets (a pinned, source-built `jca02266/lha` -- Ubuntu's/
Homebrew's `lha` is Lhasa, extract-only -- same commit amiauth already
uses) assembling `build/dist/sana2loop.{lha,readme}` from the four m68k
binaries + `sana2loop.guide` + `LICENSE`. `.github/workflows/release.yml`
verifies a pushed `v<version>` tag against both `version.mk` and
`sana2loop.readme`'s `Version:` field before building anything, greps
all four binaries for their `$VER` strings, validates the readme via
`aminet-release-action` (`validate-only`), creates the GitHub release
(curated notes sourced from the readme's own intro/Features for the
first release, `--generate-notes` thereafter), then hands off to a
separate `aminet`-environment-gated job for the actual FTP upload.
Verified locally end-to-end (`make docker && make dist`, then `lha l` on
the resulting archive) before considering this done -- same "prove the
pipeline actually produces a working artifact" discipline as every other
milestone, not just a read of the workflow YAML.

Two real, non-obvious findings along the way (neither a code bug in the
device/tools themselves): (1) the first draft of `sana2loop.readme`
stated the companion tools' floor as "Kickstart 2.0 (V36)" -- an API-level
fact (`CreateMsgPort()`/`CreateIORequest()` really are V36+ Exec calls)
but a regression from what `userdocs/Installation.md` already got right:
V36 (bare Kickstart 2.0) never shipped outside pre-2.04 hardware, so
"2.04 (V37) or later" is the only realistic floor to publish -- caught by
the user reviewing the draft, fixed to match the existing docs page, no
other file needed touching since only this new readme had it wrong.
(2) The `aminet` GitHub Environment's required-reviewer protection rule
initially could not be provisioned via `gh api` (422: billing plan
doesn't support required reviewers on a private repo -- sibling project
amiauth's own `aminet` environment works only because that repo is
public). **Resolved 2026-07-31**, the same day: the repo went public
(independent of M12/M13's own scope), and re-running the identical
`gh api` call immediately succeeded -- `sidick` is now a required
reviewer on the `aminet` environment, matching amiauth's setup exactly.
The Aminet-upload job now genuinely pauses for manual approval on any
real tag push, as designed. See `docs/PLAN.md`'s M12 risk note for the
full detail on both findings.

**M13** (SANA-II Rev 2/3/4/7 conformance coverage): this toolchain's own
bundled `<devices/sana2.h>` (AROS-derived) turned out to already carry
Rev 2/3's `S2_CopyTo/FromBuff16/32`/`S2_DMACopyTo/FromBuff32` tag values
and the `0xc000`/`0xc001` `S2_ADD/DELMULTICASTADDRESSES` command
numbers, but nothing from Rev 4 or the rest of Rev 7 -- confirmed no
vendor NDK ever shipped those at all (they live only as informal
wiki.amigaos.net documentation), so `loopback_device.c` (and every tool/
test that needs the same values) each carry their own local `#define`s
for `S2_GETPEERADDRESS`/`S2_GETDNSADDRESS` (`0xC002`/`0xC003`, quoted
from the wiki and corroborated against the header's own existing
`0xc000`/`0xc001` sequence) and `S2_DMACopyTo/FromBuff64` (`S2_Dummy+10`/
`+11`, this project's own inferred continuation since Rev 7's doc never
published a numeric value -- harmless since these hooks are only ever
recorded, never invoked). New per-unit `buffhooks=8|16|32` config knob
(default `8`, byte-for-byte unaffected behavior for every milestone
before this one) makes a unit actually INVOKE its negotiated Rev 2/3
hook width for whichever opener supplied it, falling back to the base
hook transparently otherwise -- `pick_copy_to_buff()`/
`pick_copy_from_buff()`. Rev 4's `RawMTU` extends `Sana2DeviceQuery` via
a local `struct Sana2LoopDeviceQuery` (same layout, field appended),
written only when the caller's `SizeAvailable` covers it -- the
`SizeAvailable`/`SizeSupplied` growth-safe negotiation already existed
for exactly this. Rev 7 DMA hooks are accepted and counted
(`su_DMAHooksOffered`, new `S2_GETSPECIALSTATS` record type) but never
invoked -- a hardware-less RAM loopback has nothing to hand a DMA-capable
buffer address to, and declining is fully spec-legal. New companion tool
`SanaConform` (`src/tools/sanaconform.c`, V36+/2.0+ like SanaDump/
SanaSend) probes a real running unit against all of the above and
reports which hook width actually got invoked via instrumented counting
hooks -- the "stack actually invoking it" half of conformance, not just
"driver accepted the tag". `SANA2LOOP_NUM_UNITS` is now 14 (was 13): a
new unit 13 fixture (`buffhooks=32`) for the on-target test.

Building the on-target test coverage surfaced a real, non-device finding:
adding a 5th sub-test cluster that performs a genuinely successful
`dos.library` file `Read()` (unit 13's config) AFTER M9's `record`
sub-test's own already-flushed-and-closed `recorded.pcap`, in the same
Copperline session, reliably corrupted that earlier file's data blocks
on the host-side ADF (`FSError: Invalid FileData Block` from `xdftool`
post-run) despite the guest's own serial output showing everything
passing cleanly and the disk nowhere near full. Root-caused by
elimination (isolated debug build proved the device logic itself was
correct; a throwaway inert 14th fixture file left `recorded.pcap`
intact; raising `BENCH` made no difference) to a Copperline/AROS floppy
write-back ordering quirk: a later, unrelated successful file read can
disturb an earlier file's already-flushed cached blocks before
`--benchmark-until`'s cutoff ends the session. Fixed by reordering
`sana2test.c`'s sub-tests (M13's new block now runs before `record`, not
after) rather than touching device code -- restores the "the file-write-
then-verify test is the last DOS I/O in the session" property every
milestone before M13 had by accident. See `docs/PLAN.md`'s M13 risk note
and `docs/sana2-notes.md`'s own M13 section for the full investigation.
Verified via `make copperline-smoke` (3/3 bundled AROS, 2/2 real
Kickstart 1.3) and `run-tools.sh` (SanaConform's own output redirected
to a file and grep-checked, since its stdout -- like SanaDump/SanaSend's
-- never reaches Copperline's serial capture).

**Pre-release verification (2026-07-31, prompted by the user asking to
test against Kickstart 3.1 at least once before launch, and to confirm
the real SANA-II install-directory convention)**: `make copperline-smoke`
against a real Kickstart 3.1 ROM (`run.sh`) passed 100% clean first try --
no device-code surprises. `run-tools.sh` against the SAME 3.1 ROM did
NOT, and this was a real, pre-existing bug (not something M13 introduced):
`tools-startup-sequence.txt`'s closing `Wait 2` relies on `C:Wait`, which
this minimal test disk's `C:` never actually contains. Bundled AROS
apparently tolerates this some other way (every prior milestone's own
`run-tools.sh` run passed there without issue); real Kickstart 3.1 does
not -- the whole boot HANGS at that line, root-caused via a bisection
(strip commands from the startup-sequence one at a time, rebuild the ADF,
re-run) to AmigaDOS's own command-not-found path apparently waiting on a
volume-insert requester indefinitely on a single-floppy system with
nothing else to search -- the same class of "silently waits forever for a
human" hazard already documented for `Open("ENV:...")` with no `ENV:`
assign (see M5 above), not a device bug either time. Fixed by removing
`Wait 2` entirely rather than sourcing a real `Wait` binary: SanaSend's
own write is synchronous (`DoIO`), and echo/crossover delivery completes
synchronously inside that same call (`ReplyMsg()`d straight into
SanaDump's already-queued read from within SanaSend's own `BeginIO` --
see "Echo mode without a unit task" below), so by the time SanaSend's
command returns to the script, SanaDump's own signal is already pending
and needs only Exec's ordinary preemptive scheduling (not a wall-clock
delay) to flush its own capture to disk before SanaConform's own startup
gets far enough to matter -- verified via 3/3 clean runs on both bundled
AROS and real Kickstart 3.1 after the fix (see `tools-startup-sequence.txt`'s
own comment for the full account). Separately, confirmed the user's own
hypothesis about the SANA-II install-directory convention against three
independent real-world sources (a driver's own install docs, classic
AmiTCP 3.0's installer instructions, and AmiTCP_NG's own troubleshooting
notes verbatim: *"OpenDevice() does not search DEVS:Networks/, so you
must name the driver by full path"*): `DEVS:Networks/` is where a real
network stack's own config expects to find a SANA-II driver (the stack
constructs the full path itself), while `DEVS:` is what Exec's own
`OpenDevice()` searches for a bare device name -- the two are separate,
not one superseding the other. Given the real (if small) risk that a full
-path `OpenDevice()` call might not reliably find/reuse an ALREADY-
resident device the way echo/crossover's shared-instance design requires
(genuinely unclear from available documentation, and not worth risking
on a guess), this project's own consumers (test harness, all four
companion tools) deliberately keep their existing bare-name
`OpenDevice("loopback.device", ...)` calls unchanged -- fixed via
documentation only (`userdocs/Installation.md`, `sana2loop.readme`, a new
`userdocs/Troubleshooting-and-FAQ.md` entry): copy to `DEVS:` for direct
tool/test use (unchanged), additionally to `DEVS:Networks/` if/when
wiring the device into a real stack's own config.

**M14** (`replaycap=` config knob, prompted by a user exploratory
question about making the replay-record limit configurable): the
4-record ceiling `mode=replay` units had carried since M8
(`SANA2LOOP_MAX_REPLAY_PACKETS`) is now a per-unit override,
`replaycap=1`..`256` (`SANA2LOOP_CONFIG_MAX_REPLAYCAP`), validated at
config-parse time like every other numeric key; the compiled-in
constant now means only the DEFAULT when no `replaycap=` is given, not a
hard ceiling. A RAM-auto-sizing alternative (size the cap to available
memory instead) was deliberately not pursued -- it would conflict with
the project's own determinism-by-construction goal, unlike a config
value the operator controls explicitly. `su_ReplayCap` threads through
`open()` and `reset_unit()` (same free-before-reset ordering
`su_RecordMax` already established: free using the PREVIOUS lifetime's
value before resetting to the default for the next one) and
`load_replay_file()`, where every one of the 13 call sites that used to
reference the compile-time constant now uses the per-unit runtime
value instead -- verified via `nm`/`objdump` on the built binary that
the resulting `cap * sizeof(struct EchoPacket)` runtime multiply still
needs no `__mulsi3` (this `-nostdlib`, libgcc-less build's perennial
hazard, first flagged at M8): no undefined symbol appears, and the link
itself succeeds, confirming the backend still synthesizes it via
shift/add. `SANA2LOOP_NUM_UNITS` is now 15 (was 14): a new unit 14
fixture (`replaycap=0`, out of the valid range) covers only the
config-validation failure path.

Extensive on-target debugging (stashed-marker technique from the M9
investigation, `AvailMem(MEMF_LARGEST)` probing, and bisection) showed
the POSITIVE case -- an actually-raised cap loading more records than
the default -- reliably fails under bundled AROS specifically, root-
caused to a genuine memory-budget gap (a third simultaneously-held
replay buffer, alongside the pre-existing M8/M9 units 9/10's own, never
freed until next open per this device's documented design, exceeds
bundled AROS's tight 512K+512K profile at this point in the sequence --
reproduced even at the historical default cap of 4, proving it's about
buffer COUNT, not the raised value). Real Kickstart 1.3 confirmed the
feature itself works correctly (debug-instrumented run: all 6 records
of a `replaycap=6` capture read back with matching type/length/
payload). A second attempt relocating the positive case to
`run-tools.sh`'s more generous session also failed under bundled AROS
(merely opening the extra unit corrupted the whole session's later file
reads) while working cleanly under real Kickstart 3.1 via `SanaConform`
-- confirming this is an AROS environmental ceiling, not a device or
test-design bug. **Resolution**: the automated on-target suite covers
only the allocation-free config-validation case (unit 14); the positive
case is verified functionally correct via real Kickstart 1.3 and 3.1
locally, consistent with this project's existing AROS-is-a-weaker-
stand-in stance (see "Testing under Copperline" below). See
`docs/PLAN.md`'s M14 risk note for the full investigation.

**Pre-launch review (2026-07-31)**: an independent multi-agent review
pass (device code, companion tools, tests/CI/build, docs accuracy,
release readiness) surfaced 20 real findings, filed as GitHub issues and
fixed one at a time with on-target verification before each was closed.
Device-code fixes (all verified via `make copperline-smoke` across
bundled AROS + real Kickstart 1.3 + 3.1): four real concurrency bugs
(`reorder_push()`/`reorder_flush()` mutating the M7 reorder buffer with
no `Disable()`; consume-mode replay's take-and-advance racing the same
way; `fire_event()`/`abort_requests_for_open()` resuming a list walk from
a stale `next` pointer across a dropped critical section; `close()`
repopulating a just-drained unit via `reorder_flush()` running after
`drain_unit()`, plus `reset_unit()` never freeing `su_PendingData`
itself), `parse_ulong()` silently wrapping on overflow, `S2_DEVICEQUERY`
writing base fields regardless of the caller's `SizeAvailable`, `open()`
never clearing `LIBF_DELEXP`, the recorded pcap's `ts_sec` field being
raw VBlank ticks instead of real "virtual seconds" (`su_TickSec`, a new
per-unit field -- see below for why this one nearly got reverted),
`SANA2LOOP_OPEN_MAGIC` being written but never validated (now checked
inside `fill_read_from_packet()`/`do_write()`), and a narrower, partial
fix for the long-documented DOS-I/O-under-broken-Forbid gap (`open()` now
bumps `dev->lib_OpenCnt` before any dos.library I/O, closing the specific
concurrent-`expunge()`-during-a-slow-open use-after-free; the remaining
cross-unit DOS-I/O race is real but deliberately left as a documented,
scoped TODO rather than risking a bigger structural change under time
pressure -- see the `DOSBase` global's own comment). Also fixed:
`load_replay_file()` now genuinely distinguishes `S2ERR_NO_RESOURCES`
(a real `AllocMem()` failure) from `S2ERR_BAD_ARGUMENT` (every other
cause), matching what the docs already promised but the code didn't do.

Companion-tool fixes (all in `src/tools/`): `SanaDump`'s capture buffer
was 14 bytes too small for a legal `SANA2IOF_RAW`-sized delivery (fixed,
same class of exposure fixed in `SanaConform`'s own probe buffer);
`SanaConform` freed an IORequest via raw `FreeMem()` instead of
`DeleteIORequest()`; `SanaDump`/`SanaSend`'s `TYPE=` argument silently
accepted garbage as type 0 via a bare `strtoul()` (now a shared, strict
`parse_type()`); `SanaSend`'s `DELAY=` accepted a negative value that
became a multi-year `Delay()` call; `pcapio_close_write()` ignored a
real `Close()` failure (a filesystem's own deferred-write error), now
reported and propagated.

Also fixed: `tools/docs2guide.py`'s `PAGES` list never got M13's
SANA-II-Conformance page added (the shipped `.guide` was missing it
entirely, with two dead cross-references); the echo-mode address-swap
direction was stated backwards in two userdocs pages; several smaller
docs inaccuracies (`BPS: 0` in example output vs the real `10000000`,
stale `0-11` unit-count references, the FAQ never mentioning `replaycap=`
as the actual fix for "only 4 records", `SanaInfo UNIT=0` vs the tool's
real plain-argv usage); `release.yml`'s `make docker` step left `build/`
root-owned on a real Linux runner (fixed via `--user` on the Docker
invocation, verified locally end-to-end including a full `make dist` +
`lha l`); Makefile dependency gaps (the device didn't rebuild on a
`pcap.h` edit; nothing rebuilt on a `version.mk` bump; no target wrapped
`run-tools.sh` at all, now `copperline-smoke-tools`); a handful of
on-target test-coverage gaps (`truncate=` had zero coverage, `SanaInfo`
was never executed by any automated test, `recordmax=` overflow and
`rxqueue>0`'s actual bound-hit were both unasserted, and a vacuous
`run-tools.sh` grep that would false-PASS a real special-stats
regression) -- all closed out, two items (config-driven `mode=`/`peer=`,
and cleaning up a leaked-open-on-sibling-failure pattern in a few M7
sub-tests) deliberately scoped out as lower-value than the time they'd
cost. See each closed GitHub issue (#1-#20) for the full verification
account per fix.

**Two real, unplanned findings surfaced while adding the new test
coverage, both resolved, neither a device bug**: (1) the first attempt at
covering `truncate=`/`rxqueue=` used two brand-new dedicated units --
this reliably broke the pre-existing, unrelated `scheduled` sub-test on
bundled AROS specifically (passed cleanly on real Kickstart 1.3/3.1),
confirming even small per-unit struct growth measurably shrinks AROS's
already-tight free-memory headroom for later allocations, the same class
of regression M9/M14 already hit. Resolved by reusing existing units (1,
11) instead of growing `SANA2LOOP_NUM_UNITS` at all. (2) Separately, the
`su_TickSec`/`su_TickSub` fields added for the recorded-pcap-timestamp
fix ALSO tipped `scheduled` over on bundled AROS by themselves, with zero
new units involved -- confirming this device's per-unit memory margin on
bundled AROS was already razor-thin before this whole pass started.
Rather than re-shrinking future correctness fixes indefinitely to fit an
increasingly tight artificial ceiling, `machine.toml`'s own floor moved
from 1 MB to 2 MB (512K chip + 512K slow, capped by Copperline's own
A500-trapdoor limit, + 1M fast) -- a deliberate, user-approved decision
("most people using this will have 4mb min at a guess"), now documented
as the device's real minimum in `userdocs/Installation.md`,
`sana2loop.readme`, and `README.md`. (3) Adding `SanaInfo` to
`run-tools.sh`'s own session then corrupted multiple already-written
files on bundled AROS with `[floppy] speed=0` (turbo) -- a broader
instance of the M13-documented floppy write-back ordering quirk, resolved
empirically via `speed=100` (real-time) plus raising `run-tools.sh`'s own
`BENCH` default 40 -> 90; root cause not re-isolated beyond "turbo timing
interacts badly with 4+ DOS I/O operations sharing one session". All
three resolved via the same AROS-is-a-weaker-stand-in stance this project
has applied consistently since M9: confirm real hardware is unaffected,
then treat the AROS-specific symptom as an environment artifact to work
around, not a device bug to chase further.

**M15** (`wire=`/`bps=`/`mintu=` config keys -- GitHub issues #26/#28/#30,
all prompted by reviewing **nullsana**, Aminet's 2000-era virtual SANA-II
device discovered as prior art on 2026-08-04; the same review retracted
the docs' "no null device has ever existed" claim and filed issues
#24-#30): `bps=` makes `S2_DEVICEQUERY`'s BPS per-unit (was hardcoded
10000000; reported only, never a throttle); `mintu=` adds a
minimum-write-size floor (short writes fail `S2ERR_BAD_ARGUMENT` --
rejected, never padded, and deliberately not `S2ERR_MTU_EXCEEDED`);
`wire=ethernet|slip|cslip|ppp` makes a unit report a point-to-point
serial wire -- 4-byte IP-style station address (`0A:53:32:<unit>`,
rebuilt in `open()` at config-apply time since `reset_unit()` runs
before the wire type is known), per-wire mtu/bps defaults overridable by
explicit keys regardless of key order (`mtu_explicit`/`bps_explicit`
flags resolved in `read_unit_config()`), broadcast/multicast/RAW all
`S2ERR_NOT_SUPPORTED`, `RawMTU=0`, addr-size-aware `dest_matches_unit()`,
crossover across mismatched wire types failing `S2ERR_BAD_STATE` at
write time (peer's wire is unknowable at open -- config reads happen at
each unit's OWN first open), and replay/record rejected at config time
(the pcap side is LINKTYPE_ETHERNET-framed). `SANA2LOOP_NUM_UNITS` is
now 16 (was 15): unit 15 is the `wire=slip` fixture -- the first unit
growth since the 2 MB memory-floor bump made it affordable again
(bundled AROS's `scheduled` canary verified still green);
`bps=57600`/`mintu=16` ride unit 1's existing fixture additively. New
`mintu`/`wire` sub-tests sit before `record` (M13's "record is the last
DOS I/O in the session" invariant). Verified: `make copperline-smoke` on
bundled AROS + real Kickstart 1.3 + real 3.1, and `run-tools.sh` on
bundled AROS. See `docs/sana2-notes.md`'s M15 section for the design
notes.

**M16** (`addr=`/`errors=`/`loss=` config keys -- GitHub issues #27/#29,
the second nullsana-inspired batch): `addr=` pins the FACTORY station
address from config (colon-separated hex; byte count validated against
the wire's address width after `wire=` resolution; Ethernet group bit
rejected; M3's `S2_CONFIGINTERFACE` one-shot override works on top
unchanged). `errors=N` is the only VISIBLE fault knob -- 1-in-N writes
fail `S2ERR_TX_FAILURE`/`S2WERR_TOO_MANY_RETRIES` and fire
`S2EVENT_TX|S2EVENT_ERROR` (first real exercise of the momentary event
bits). `loss=N` is 1-in-N silent loss beside `droppct` (rates finer than
1%); both use `prng_hits_one_in()` with exact draw-free 0/1 boundaries
so enabling one knob never perturbs another's seeded PRNG sequence.
**The build now carries its own `__mulsi3`** (three mulu.w partial
products, inline asm, placed deliberately AFTER `_start()`/romtag --
emitted at text offset 0 otherwise, observed): M16's per-unit struct
growth broke the link at PRE-EXISTING `sld_Units[]` index sites, proving
the M8/M14 __mulsi3-avoidance strategy (noinline, nm checks) no longer
scales -- that whole hazard class is retired; the CopyMem-not-memcpy
discipline for copies still stands. `SANA2LOOP_NUM_UNITS` is now 18
(units 16/17: `errors=1`/`loss=1` fixtures). Verified: bundled AROS +
real Kickstart 1.3 + real 3.1 (39 sub-tests) + `run-tools.sh`. See
`docs/sana2-notes.md`'s M16 section.

A follow-up review of vlink (Aminet `comm/tcp/vlink`, Chris Brenner,
1999/2000, magPLIP-derived -- a second known virtual SANA-II device,
alongside nullsana) filed two more feature ideas: #31 (a second device
name, e.g. `loopmirror.device`, whose unit N crossover-pairs with
`loopback.device`'s unit N -- vlink's own `vlink.device`/
`vlinkmirror.device` pattern, needed for consumers like ShapeShifter/
Fusion that can only open unit 0 of a named device) and #32
(step-by-step real-stack wiring walkthroughs in userdocs, modeled on
vlink.guide's own numbered Miami/ShapeShifter/MacTCP setup instructions).

**M17** (`delay=`/`deviation=` config keys -- GitHub issue #24, the
third and final nullsana-inspired batch item): a written packet's
delivery can be deferred `delay` VBlank ticks (0 = disabled, unchanged
synchronous behavior) with optional symmetric `deviation` jitter
(uniform draw in `[delay-deviation, delay+deviation]`, clamped at 0 --
same 16x16 mulu.w top-bits technique as `prng_hits_pct()`/
`prng_hits_one_in()`, not `%`: M16 added `__mulsi3` but not
`__umodsi3`, so a general modulo still wouldn't link). New
`su_DelayQueue` (a plain `List` of `EchoPacket`s via their own
`ep_Node` -- no wrapper allocation, and genuinely unbounded unlike
`su_ReorderBuf`'s fixed 8 slots, since delay has no natural window to
bound it; backed by the same `AllocMem()` backpressure every packet
allocation here already has). Runs LAST in the fault pipeline, after
`reorder` (`deliver_with_delay()` replaces `deliver_or_reorder()`'s
direct delivery call and reorder's own natural-eviction path).
`vblank_server()`'s tick gate widened to a third, config-derived reason
(never misses a drain); `delay_queue_tick()` scans the WHOLE queue
every tick, not a sorted prefix -- jitter can make a later write due
sooner than an earlier one. `CMD_FLUSH`/`S2_OFFLINE`/last-close gained
a parallel `delay_queue_flush()` mirroring `reorder_flush()`'s own
placement exactly (same reason: `su_Tick` only advances while online,
so anything left queued at OFFLINE would otherwise wait forever) --
and, like `reorder_flush()`, calls `deliver_packet()` DIRECTLY,
bypassing `deliver_with_delay()`, since a flush's whole point is
delivering NOW, not re-queuing into a fresh wait. `SANA2LOOP_NUM_UNITS`
is now 19 (unit 18: the `delay=10` fixture, PRNG-free). The on-target
`delay` sub-test needed a technique `scheduled` never did: right after
the write's own (always-synchronous) `do_io()` returns, a plain read of
the `g_HookToCount` side-effect global (not `CheckIO()`/`WaitIO()`,
flagged unreliable on bundled AROS by the M9 risk note) proves delivery
hasn't happened yet, before `reap_interrupt_io()` proves it eventually
does correctly. Verified: bundled AROS (clean first try) + real
Kickstart 1.3 + real 3.1 (40 sub-tests) + `run-tools.sh`. See
`docs/sana2-notes.md`'s M17 section.

No further milestones remain unscheduled except Aminet packaging's own
"cut and publish a first tagged release" step (the M12 pipeline itself
is built and tested, but no tag has been pushed) -- see docs/PLAN.md.
The nullsana/vlink-inspired feature-idea queue is down to #25
(multi-open fan-out + real `S2_PacketFilter` dispatch), #31 (mirror
device name), and #32 (stack-wiring walkthrough docs).

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
through M9 on-target against both bundled AROS and real Kickstart 1.3 (see
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
Any released `copperline` 0.14.0+ works for both.

- **Prefer a real Kickstart 1.3 ROM (`KICK=`) locally.** AROS can't validate
  this project's actual 1.3-fidelity claims -- it's a fine ROM-free stand-in
  for catching gross regressions in CI, but confirm against real 1.3 before
  trusting a Phase 1 claim or cutting a release.
- **The real 68000+`[[filesys]]` doorbell bug that existed in Copperline
  0.12.0/0.13.0 is fixed and released in 0.14.0** ("68000 split writes":
  a 68000's word write split across the services board's MMIO doorbell now
  fires the doorbell correctly). This project's own test never hit it
  itself -- it boots from a plain floppy image, never `[[filesys]]`,
  confirmed empirically with 0.13.0 against both a real 1.3 ROM and AROS.
  No more local `fix/filesys-68000-doorbell-split` branch/worktree needed
  (`/Users/simond/src/external/Copperline`) if a future test here ever
  does want `[[filesys]]` -- that fix shipped upstream in 0.14.0.
- **Tried and reverted: `[[filesys]]` hostfs mounts as a replacement for
  this project's own `xdftool`-built fixture/output floppy layout.**
  Copperline 0.14.0's release notes say hostfs volumes (`HOSTFS0:`, ...)
  "now mount under Kickstart 1.3", and the underlying board genuinely does
  initialize on a real 1.3 ROM (visible in Copperline's own boot log: `filesys:
  expansion init at board 0x00EA0000, 1 mount(s)`) -- but empirically, in
  this project's own minimal from-scratch boot ADF (no `SetPatch`, no
  Workbench boot init), a guest `Open()` of `HOSTFS0:` -- for **either**
  read or write -- reliably **fails** on both a real Kickstart 1.3 ROM and
  a real Kickstart 3.1 ROM, while succeeding cleanly on Copperline's own
  bundled AROS. Root cause not chased further than that (plausibly: AROS
  auto-registers autoconfig filesystem boards into the DOS device list at
  a point real Kickstart's dos.library only does via boot-time init this
  project's own bare-bones Startup-Sequence never runs, e.g. `SetPatch` or
  the disk-validator boot-node scan) -- confirmed via direct `copperline`
  invocations isolating read from write and ruling out a `--benchmark-until`
  timing cutoff. **Net effect: building this project's own on-target
  fixture/output handling around hostfs would have made it pass only on
  AROS and silently break on every real ROM** -- exactly the kind of
  AROS-diverges-from-real-hardware trap this project's own testing
  philosophy exists to catch (see "Prefer a real Kickstart 1.3 ROM" above),
  just inverted (AROS does MORE here, not less). `run.sh`/`run-tools.sh`
  were reverted to their pre-0.14.0 `xdftool`-based approach; `[[filesys]]`
  stays unused by this project's own tests for now.
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
