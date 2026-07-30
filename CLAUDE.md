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
backfill, then Phases 2-4). **M0-M2 done**, each validated on-target under
Copperline against both bundled AROS (CI) and a real Kickstart 1.3 ROM
(`make copperline-smoke` / `KICK=... make copperline-smoke`,
see `tests/copperline/`) before being considered complete.

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
  - `S2_DEVICEQUERY`, `S2_GETSTATIONADDRESS`, `S2_ONLINE`/`S2_OFFLINE`,
    `S2_ONEVENT` (completes **immediately** if the requested state already
    holds, else waits for the next transition -- corrected from an earlier,
    wrong "always edge-triggered" assumption after cross-checking a real
    driver, plipbox), `S2_GETGLOBALSTATS`, `S2_GETSPECIALSTATS` (zero-record
    stub), `CMD_FLUSH`, `AbortIO` (covers pending reads, orphan reads, and
    event waits).
  - 4 units pre-allocated in the device base (unit 0 wired for echo; units
    1-3 exist for Phase 2 crossover pairs but aren't dispatched differently
    yet). A unit fully resets (`reset_unit()`: offline, factory address,
    stats zeroed, `rxqueue` back to 0) the moment its open count goes
    0->1 -- this is what "config re-read on next open" (M5) will mean.
    No separate unit task -- echo completes synchronously inside `BeginIO`
    on the calling task (no dos.library access needed at that point, so
    nothing about that violates the 1.3 rule).
- `src/sanainfo.c` -- `SanaInfo`, the Phase 1 validation tool: opens a unit,
  optionally issues `S2_ONLINE`/`S2_OFFLINE`, prints `S2_DEVICEQUERY` +
  station address. Normal clib2 CLI program (V36+ Exec calls -- see the
  file header for why that's fine for a tool but wouldn't be for the
  device).

- `tests/copperline/sana2test.c` -- the on-target smoke test (see "Testing
  under Copperline" below). Freestanding like the device itself, for a
  reason specific to itself (not the device): see the comment block at its
  top for a real on-target bug this tripped over and how it was found.

Not yet done: crossover pairs, fault injection knobs, pcap replay/record,
`SanaDump`/`SanaSend`, `ENV:sana2loop/unitN.config` (units currently use a
hardcoded 1500-byte MTU and a fixed fake station address -- see the
"Config via ENV" section of the proposal for the target design), and
on-target coverage of `S2_OFFLINE`/`S2_ONEVENT`/`AbortIO`/`CMD_FLUSH`.

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
build clean), and `make copperline-smoke` has confirmed the echo/query
logic on-target against real Kickstart 1.3 (see "Testing under Copperline"
below) -- `S2_OFFLINE`/`S2_ONEVENT`/`AbortIO`/`CMD_FLUSH` are not yet
covered by that test.

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
