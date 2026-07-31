# sana2loop — Implementation Plan (Phases 2–4 + spec-correctness backfill)

## Context

Phase 1 of the [proposal](https://github.com/sidick/project-ideas/blob/main/sana2loop-proposal.md) is committed and validated on-target (echo, `S2_DEVICEQUERY`, `S2_ONLINE/OFFLINE`, `S2_ONEVENT`, real KS1.3 under Copperline). This plan covers everything remaining: fixing SANA-II spec-correctness gaps in the Phase 1 device (without which real stacks — AmiTCP, Roadshow, TheWire13 — cannot use it at all), then crossover pairs + fault injection (Phase 2), pcap replay/record with deterministic scheduling (Phase 3), and companion tools + release (Phase 4).

The device stays on its hard constraints: 68000/KS1.3 baseline, no utility.library (hand-rolled tag walk), no dos.library off the opener's process context, deterministic by construction, single heavily-commented source file (the teaching artifact — do **not** split it into multiple TUs; the string-literal-pooling/link-order hazards documented in CLAUDE.md are a second reason).

**Decision made with user:** strict SANA-II receive semantics by default — a packet with no matching queued read is **dropped** (bumping `UnknownTypesReceived`), `rxqueue=0`. A config knob `rxqueue=N` opts into a bounded convenience queue. The on-target harness converts to pre-queuing reads (`SendIO` read → write → `WaitIO`), which is itself the correct consumer pattern to demonstrate.

## Key design decisions (from Plan-agent analysis, verified against `devices/sana2.h`)

1. **Per-open cookie** (`struct Sana2LoopOpen`): allocated in `open()`, its pointer **replaces** `ios2_BufferManagement` per SANA-II convention — consumers contractually copy it into every subsequent IOSana2Req, so every queued read carries its opener's hooks. Freed in `close()` (CloseDevice uses the same IORequest). Fields: MinNode (on a `su_Opens` list), unit back-pointer, magic (`'S2LO'`), `CopyToBuff`/`CopyFromBuff` function pointers, recorded `S2_PacketFilter` hook, open flags (`SANA2OPF_MINE/PROM`). When the opener passes no copy tags, install internal `CopyMem`-based defaults — one uniform copy path everywhere.
2. **Tag walk** must handle `TAG_MORE`/`TAG_SKIP`/`TAG_IGNORE`/`TAG_END` (the trap `FindTagItem` normally hides). Tags: `S2_CopyToBuff`(+1)/`S2_CopyFromBuff`(+2)/`S2_PacketFilter`(+3) off `S2_Dummy = TAG_USER+0xB0000`; record-and-ignore the Rev 7 16/32/DMA variants **at the time this decision was made** -- revisited by the "SANA-II conformance coverage" section below (M13), which plans to actually support them as advisory hooks instead.
3. **`deliver_packet()` refactor**: one delivery path used by echo, crossover, and replay: complete a matching **typed** read → else a queued `S2_READORPHAN` → else (rxqueue>0) queue bounded → else drop + stats. Copies always via the completing request's cookie hooks; **never call a consumer hook or ReplyMsg inside `Disable()`** (pop nodes under Disable, act outside — existing discipline).
4. **Unit reset at first-open** (open count 0→1): offline, unconfigured address, stats zeroed, PRNG reseeded, config re-read. Defines the "config re-read on next open" semantics; busy units latch their active config (never re-read mid-flight).
5. **Config**: `ENV:sana2loop/unit<N>.config`, fallback `S:sana2loop/unit<N>.config`. Line-based `key=value`, `#` comments. dos.library opened locally inside `open()` only when `FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS` (plain-task openers get defaults). Whole file (≤2 KB) into a temp buffer, parsed in place. **Fail-fast**: unknown key/malformed line → open fails `S2ERR_BAD_ARGUMENT` (silent typo-tolerance is wrong for a test tool). Keys: `mode` (echo|crossover|replay), `peer`, `mtu`, `addr`, `seed`, `droppct`, `duppct`, `reorder` (0–8 window), `truncate`, `oversilent`, `rxqueue`, `strictconfig`, `offlineafter`/`onlineafter`, `replay`/`record`/`replaymode`/`recordmax` (Phase 3).
6. **Fault pipeline** (write path of the written unit, after CopyFromBuff, fixed order): MTU check (`oversilent=1` = accept-and-vanish → PMTUD black hole with mismatched crossover MTUs) → `droppct` → `truncate` → `duppct` → `reorder` (fixed 8-slot pointer array, PRNG-permuted flush; flushed in arrival order on OFFLINE/CMD_FLUSH/last-close) → `offlineafter`/`onlineafter` (packet-count-driven forced events). PRNG: xorshift32, per-unit, seeded from config (0 coerced to 1). Percent thresholds via `pct*655` vs `prng>>16` (no 32-bit `/`/`%` — no libgcc; alternatively decide `-lgcc` at M5).
7. **Replay (Phase 3): no unit task.** VBlank interrupt server (`AddIntServer(INTB_VERTB, …)`) advances `su_Tick` and delivers due packets — ReplyMsg is interrupt-callable and SANA-II requires consumer hooks to be interrupt-safe, so this is legal *and* provides the proposal's interrupt-context-hooks consumer-discipline knob for free. Deterministic under Copperline (VBlank = emulated time). `replaymode=consume` = tick per CMD_READ/WRITE, zero interrupts. Record: append to RAM (`recordmax`, default 64 KB; overflow = stop + count via `S2_GETSPECIALSTATS`), flushed to file at last close if closer is a process.
8. **pcap**: hand-rolled classic format (magic `0xa1b2c3d4`, LINKTYPE_ETHERNET=1), **both endians** (host scapy writes little-endian). Shared `src/pcap.h` (structs + endian helpers, no I/O); device parses/appends RAM-side, tools use stdio.
9. **Tools stay 2.0+** (ReadArgs/clib2, documented) per proposal; the device is the 1.3 artifact. Shared `src/tools/pcapio.c` + `src/tools/sana2open.c` (which must itself pass real, interrupt-safe copy hooks — model citizen).
10. **Tests**: one growing `tests/copperline/sana2test.c`, sub-results as `SUB=<name>=PASS|FAIL` serial lines + aggregate `RESULT=`; `run.sh` greps per-milestone. Config files ride the ADF as `s/sana2loop/unitN.config` (the S: fallback exists precisely because the minimal ADF has no ENV: assign — exercise it). All new harness strings = named non-const `char[]` (pooling hazard per CLAUDE.md).

## Milestones (each = one commit; gate = `make copperline-smoke` green on bundled AROS **and** locally vs real KS1.3)

Status is kept current here as each milestone lands — see CLAUDE.md's
"Current state" for the fuller prose account and `docs/sana2-notes.md` for
the technical detail behind each one.

| # | Status | Work | Files |
|---|---|---|---|
| M0 | ✅ Done | Test backfill, no device change: S2_OFFLINE (write→`S2ERR_OUTOFSERVICE`), S2_ONEVENT edge-trigger (SendIO+CheckIO), AbortIO on pending read, CMD_FLUSH. Convert echo test to pre-queue-read pattern (ready for M2's strict drop). Add `SUB=` protocol to harness + `run.sh` | `tests/copperline/sana2test.c`, `run.sh` |
| M1 | ✅ Done | Per-open cookie + buffer hooks: tag walk (incl. TAG_MORE/SKIP/IGNORE), default CopyMem hooks, hook-failure path (`S2ERR_NO_RESOURCES`+`S2WERR_BUFF_ERROR`), defensive close-abort of the dying cookie's queued requests. **Verify by objdump** that bebbo gcc honors register annotations on calls through function pointers (fallback: asm thunk). Fix `docs/sana2-notes.md` hook stance | `src/loopback_device.c`, `docs/sana2-notes.md`, harness (hook-tag test via TAG_MORE chain + counters) |
| M2 | ✅ Done | `deliver_packet()` refactor: typed reads, `S2_READORPHAN`, strict-drop default + `rxqueue` bound, `S2_GETGLOBALSTATS`, zero-record `S2_GETSPECIALSTATS`, NULL-`ios2_StatData` guard, no delivery while offline, unit reset at first-open | `src/loopback_device.c`, harness (type-mismatch stays pending; orphan catches it; `UnknownTypesReceived` asserted) |
| M3 | ✅ Done | `S2_CONFIGINTERFACE` (second → `S2ERR_BAD_STATE`+`S2WERR_IS_CONFIGURED`), `S2_GETSTATIONADDRESS` current-vs-factory split, `S2_TRACKTYPE`/`UNTRACKTYPE`/`GETTYPESTATS` (8 fixed slots) | `src/loopback_device.c`, harness |
| M4 | ✅ Done | `S2_BROADCAST`/`S2_MULTICAST` writes, `S2_ADD/DELMULTICASTADDRESS` (refcounted, fixed slots), `SANA2IOF_RAW` (buffer → MTU+14), BCAST/MCAST read flags | `src/loopback_device.c`, harness |
| M5 | ✅ Done | Config infra: parser, ENV:/S: read at first-open, latch semantics; decide `-lgcc` vs local div helpers; prove DOS-under-Forbid open on real 1.3 | `src/loopback_device.c`, `Makefile`, `run.sh` (config on ADF: `mtu=1400` → DQ reports 1400; malformed → open fails) |
| M6 | ✅ Done | Crossover: config-driven `mode`/`peer` (defaults: units 0–1 echo, 2↔3 crossover), pair validation at first-open, unswapped addressing, receive filtering (station/bcast/mcast/PROM) | `src/loopback_device.c`, `run.sh`, harness (SendIO read on 3 + write on 2; wrong-dst filtered unless PROM) |
| M7 | ✅ Done | Fault injection: xorshift32 + full pipeline + forced offline/online events + PMTUD black hole | `src/loopback_device.c`, harness (droppct=100 never completes; duppct=100 → 2 reads; fixed-seed reorder → assert exact permutation) |
| M8 | ✅ Done | pcap core (`src/pcap.h`, both endians) + consume-mode replay; commit fixture bytes | `src/pcap.h`, `src/loopback_device.c`, `fixtures/`, harness |
| M9 | ✅ Done | VBlank tick server, tick-scheduled replay, RAM record + close-flush, interrupt-context-hook knob, 68000 per-packet overhead measurement (VBlank-count bracketing, tracked not gated). Record test: `run.sh` extracts written pcap off the ADF post-run via xdftool and byte-compares — **verify early that Copperline persists floppy writes to the image**. AROS CI lane's earlier hang was root-caused to a bundled-AROS `CheckIO()`/`WaitIO()` false-completion bug (harness-side fix, `reap_interrupt_io()`) — see risk note below | `src/loopback_device.c`, `run.sh` |
| M10 | | Docs pass: teaching-reference comments through the source, README, sana2-notes refresh. Also stand up a `userdocs/` end-user site (MkDocs Material, adopting sibling project amiauth's pattern in preference to narrator.wyoming's, since amiauth's is the more tried-and-tested one) alongside a `tools/docs2guide.py`-style converter producing an on-Amiga AmigaGuide manual from the same `userdocs/` source -- one docs source feeding both the web site and native Amiga reading, not two to keep in sync by hand | all sources, `docs/`, new `userdocs/`, `mkdocs.yml`, `tools/docs2guide.py` |
| M11 | | Tools: `src/tools/pcapio.c`, `sana2open.c`, `SanaDump`, `SanaSend`; `host/make_fixtures.py` + `host/lint_pcap.py` (scapy); canonical corpus (ARP, DHCP, TCP handshake) | new `src/tools/*`, `host/*`, `fixtures/`, `Makefile` |
| M12 | | Aminet packaging + release flow, adopting sibling project amiauth's pattern (in preference to narrator.wyoming's simpler one, since amiauth's is the more tried-and-tested of the two): tag-driven `release.yml` that verifies the pushed tag matches `version.mk` and the `.readme`'s `Version:` field before doing anything else, cross-builds via the pinned Docker image, greps the built binary for its `$VER` string, packages via `make dist` (building a real, archive-capable `lha` from source -- Ubuntu's/Homebrew's is extract-only), validates the readme with `aminet-release-action` in `validate-only` mode, creates the GitHub release (curated notes from the `.readme` for the first release, `--generate-notes` once there's real tag-to-tag history to diff), and only then hands off to a *separate* job gated on a GitHub Environment requiring manual approval for the actual (irreversible-ish) Aminet FTP upload -- human-in-the-loop by construction, not an afterthought | `Makefile`, `version.mk`, `.github/workflows/release.yml`, release docs |
| M13 | | SANA-II Rev 2/3/4/7 conformance coverage (see its own section below) + a conformance/quirk probe tool exercising both directions (driver advertising vs stack actually invoking) with graceful-fallback checks | `src/loopback_device.c`, new probe tool, harness |

## SANA-II conformance coverage: Rev 2/3/4/7 additions

loopback.device and the conformance/quirk probe should exercise the
post-Commodore SANA-II revisions (as documented by Olaf Barthel on the
AmigaOS docs wiki), since real-world stacks (Roadshow, AmiTCP-derived
stacks) probe for these:

- Rev 2+3 — buffer management negotiation
  - Support S2_CopyToBuff16 / S2_CopyFromBuff16 and
    S2_CopyToBuff32 / S2_CopyFromBuff32 as advisory hooks, negotiated
    via tags at OpenDevice(). Driver may ignore per-buffer at its
    discretion — probe should confirm stacks handle a driver that
    declines as well as one that accepts.

- Rev 4 — peer/DNS address + raw MTU
  - Implement S2_GETPEERADDRESS and S2_GETDNSADDRESS commands
    (meaningful for PPP-style / dynamically addressed links; loopback
    can synthesize plausible values or zero-fill per spec when N/A).
  - Populate Sana2DeviceQuery.RawMTU distinctly from the negotiated
    MTU, and support S2ERR_NOT_SUPPORTED / S2ERR_MTU_EXCEEDED paths
    for raw read/write when RawMTU is 0 or exceeded — good fit for
    the existing fault-injection knobs.

- Rev 7 — DMA-capable buffer hooks
  - Add S2_DMACopyToBuff32 / S2_DMACopyFromBuff32 and the 64-bit
    variants as optional hooks, with correct ios2_WireError reporting
    on rejection (FALSE hook return == NULL-pointer equivalent per
    spec).

Conformance probe should test both directions: driver advertising
support and stack actually invoking each hook/command, plus graceful
fallback when either side doesn't implement a given revision's
addition. Vendor-specific extensions (WiFi ioctls, batch transfer
modes, etc.) are explicitly out of scope — they're outside SANA-II
proper.

## Risks to verify at first touch

- ~~Register-annotated calls through function pointers under bebbo gcc~~ --
  **resolved at M1**: confirmed correct by objdump before trusting it.
- ~~DOS I/O breaking Forbid inside `open()` on real 1.3~~ -- **resolved at
  M5**: `open()` itself was never the hazard. `Open("ENV:...")` hangs
  indefinitely (not a clean failure) when `ENV:` isn't assigned, on both
  AROS and real 1.3 alike -- fixed via `pr_WindowPtr = (APTR)-1`. With
  that fixed, dos.library I/O from `open()` works correctly on both
  targets; real 1.3 is just measurably slower at it than AROS (see
  `docs/sana2-notes.md`'s M5 section).
- ~~`-lgcc` linked into the freestanding device~~ -- **resolved (unneeded)
  at M7**: the milestone flagged as the one likely to finally force this
  (percentage thresholds, reorder's index draw) turned out not to --
  `pct*655` avoids division for percent thresholds, and a mask-and-reject
  loop (not `% window`) avoids it for the reorder index, since a
  runtime-variable modulo compiles to a `__umodsi3` call on this
  `-m68000` target regardless of how small the operands are at runtime.
  Stays undecided until something genuinely needs real division. M8 came
  close from the multiplication side instead: indexing an array of
  non-power-of-2-sized structs by a runtime variable needs `__mulsi3` too
  (found when `load_replay_file()` got inlined into `open()` and the
  compiler rematerialized a unit pointer that way) -- avoided via
  `__attribute__((noinline))` plus pointer-walking instead of indexing,
  not by finally linking `-lgcc`. Still deferred.
- ~~ReplyMsg + consumer hooks from a VBlank server on real 1.3~~ -- **resolved
  at M9**: `make copperline-smoke KICK=...` (real 1.3) passes every M9
  assertion cleanly, including the interrupt-context hook-invocation check
  -- `S2_CopyToBuff` genuinely gets called from inside the VBlank interrupt,
  confirmed via invocation counting, and the register/Z-flag convention for
  the server itself was verified by objdump before trusting it (same
  discipline M1 established for hook calls).
- ~~Copperline floppy write-back persistence~~ -- **resolved at M9**: confirmed
  empirically via a standalone experiment (write a known file to `S:`, read
  it back post-run via `xdftool type`, byte-identical) before building the
  record-flush feature on top of it. Needed `write_protected = false` in
  `[floppy.dfN]` (defaults to `true`) -- see `machine.toml`.
- ~~bundled AROS under Copperline hangs once a unit actually uses
  `replaymode=scheduled` or `record=` (M9)~~ -- **resolved, root-caused via
  on-target instrumentation, not the AllocMem-in-interrupt theory this
  entry originally named.** That theory was tested directly (moved
  `vblank_server()`'s per-due-record allocation out to
  `load_replay_file()`, made at ordinary task-context load time instead --
  `su_ReplayDeliveryCopies[]`, kept in the source as a real hygiene
  improvement even though it wasn't the fix) and disproven: the hang
  persisted unchanged with zero allocator calls left in the interrupt
  path. The real cause, found by stashing per-stage progress markers into
  otherwise-unused `Sana2DeviceStats` fields inside `deliver_packet()`/
  `vblank_server()` and reading them back through a second, independent
  `IOSana2Req` (so they were visible even if the hung request's own task
  never resumed): **bundled AROS's `CheckIO()`/`WaitIO()` can both falsely
  report an `IOSana2Req` as already complete immediately after
  `SendIO()`, before the device has actually replied it** -- specifically
  for a request destined to complete via a genuine hardware-interrupt-
  driven `ReplyMsg()` (M9's `vblank_server()`), unlike every earlier
  sub-test's task-context completion. The stashed markers proved
  `deliver_packet()` had not even been entered at the exact moment a
  plain `WaitIO()` returned "success" with stale data; a test harness that
  then reuses the same (AROS's-eyes-completed, actually-still-enqueued)
  `IOSana2Req` for the next record double-inserts it into the device's
  own `su_OrphanReads` list, corrupting it -- that corruption, not the
  device's own interrupt handling, is what produces the full, permanent
  guest freeze (matching the observed symptom: no further `SUB=`/`END`
  lines, Copperline itself exits 0). Real Kickstart 1.3 never exhibits the
  false-completion signal, which is why this was invisible there.
  Fixed entirely in `tests/copperline/sana2test.c`
  (`reap_interrupt_io()`): wait on `g_HookToCount` reaching an expected
  value -- a side effect only the real `S2_CopyToBuff` hook invocation can
  produce, immune to the false CheckIO/WaitIO signal -- then
  unconditionally `AbortIO()` (a documented no-op if genuinely already
  complete) followed by `WaitIO()`, guaranteeing the request is cleanly
  reaped either way before any reuse. No device-code change was needed
  for the actual fix. Verified: 5/5 clean `RESULT=PASS` runs on bundled
  AROS, 3/3 on real Kickstart 1.3, after the harness change.

## Verification

Every milestone: `make docker && make docker-test-harness`, then `sh tests/copperline/run.sh` (bundled AROS — what CI runs) **and** `KICK=/Users/simond/Downloads/kickstart-1.3.rom sh tests/copperline/run.sh` (real 1.3 — the claim that matters) — all `SUB=` lines and `RESULT=PASS` asserted over serial. CI (`copperline-smoke-aros` job) already runs the AROS lane on every push/PR. Phase-1-era tests must stay green through every refactor (M0 locks them in first).
