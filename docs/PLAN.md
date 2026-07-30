# sana2loop — Implementation Plan (Phases 2–4 + spec-correctness backfill)

## Context

Phase 1 of the [proposal](https://github.com/sidick/project-ideas/blob/main/sana2loop-proposal.md) is committed and validated on-target (echo, `S2_DEVICEQUERY`, `S2_ONLINE/OFFLINE`, `S2_ONEVENT`, real KS1.3 under Copperline). This plan covers everything remaining: fixing SANA-II spec-correctness gaps in the Phase 1 device (without which real stacks — AmiTCP, Roadshow, TheWire13 — cannot use it at all), then crossover pairs + fault injection (Phase 2), pcap replay/record with deterministic scheduling (Phase 3), and companion tools + release (Phase 4).

The device stays on its hard constraints: 68000/KS1.3 baseline, no utility.library (hand-rolled tag walk), no dos.library off the opener's process context, deterministic by construction, single heavily-commented source file (the teaching artifact — do **not** split it into multiple TUs; the string-literal-pooling/link-order hazards documented in CLAUDE.md are a second reason).

**Decision made with user:** strict SANA-II receive semantics by default — a packet with no matching queued read is **dropped** (bumping `UnknownTypesReceived`), `rxqueue=0`. A config knob `rxqueue=N` opts into a bounded convenience queue. The on-target harness converts to pre-queuing reads (`SendIO` read → write → `WaitIO`), which is itself the correct consumer pattern to demonstrate.

## Key design decisions (from Plan-agent analysis, verified against `devices/sana2.h`)

1. **Per-open cookie** (`struct Sana2LoopOpen`): allocated in `open()`, its pointer **replaces** `ios2_BufferManagement` per SANA-II convention — consumers contractually copy it into every subsequent IOSana2Req, so every queued read carries its opener's hooks. Freed in `close()` (CloseDevice uses the same IORequest). Fields: MinNode (on a `su_Opens` list), unit back-pointer, magic (`'S2LO'`), `CopyToBuff`/`CopyFromBuff` function pointers, recorded `S2_PacketFilter` hook, open flags (`SANA2OPF_MINE/PROM`). When the opener passes no copy tags, install internal `CopyMem`-based defaults — one uniform copy path everywhere.
2. **Tag walk** must handle `TAG_MORE`/`TAG_SKIP`/`TAG_IGNORE`/`TAG_END` (the trap `FindTagItem` normally hides). Tags: `S2_CopyToBuff`(+1)/`S2_CopyFromBuff`(+2)/`S2_PacketFilter`(+3) off `S2_Dummy = TAG_USER+0xB0000`; record-and-ignore the Rev 7 16/32/DMA variants.
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
| M6 | ⏳ Next | Crossover: config-driven `mode`/`peer` (defaults: units 0–1 echo, 2↔3 crossover), pair validation at first-open, unswapped addressing, receive filtering (station/bcast/mcast/PROM) | `src/loopback_device.c`, `run.sh`, harness (SendIO read on 3 + write on 2; wrong-dst filtered unless PROM) |
| M7 | | Fault injection: xorshift32 + full pipeline + forced offline/online events + PMTUD black hole | `src/loopback_device.c`, harness (droppct=100 never completes; duppct=100 → 2 reads; fixed-seed reorder → assert exact permutation) |
| M8 | | pcap core (`src/pcap.h`, both endians) + consume-mode replay; commit fixture bytes | `src/pcap.h`, `src/loopback_device.c`, `fixtures/`, harness |
| M9 | | VBlank tick server, tick-scheduled replay, RAM record + close-flush, interrupt-context-hook knob, 68000 per-packet overhead measurement (VBlank-count bracketing, tracked not gated). Record test: `run.sh` extracts written pcap off the ADF post-run via xdftool and byte-compares — **verify early that Copperline persists floppy writes to the image** | `src/loopback_device.c`, `run.sh` |
| M10 | | Docs pass: teaching-reference comments through the source, README, sana2-notes refresh | all sources, `docs/` |
| M11 | | Tools: `src/tools/pcapio.c`, `sana2open.c`, `SanaDump`, `SanaSend`; `host/make_fixtures.py` + `host/lint_pcap.py` (scapy); canonical corpus (ARP, DHCP, TCP handshake) | new `src/tools/*`, `host/*`, `fixtures/`, `Makefile` |
| M12 | | Aminet packaging + release flow (adopt narrator.wyoming's version.mk/release-tag pattern), `.readme`, final full-suite + real-1.3 gate | `Makefile`, release docs |

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
- `-lgcc` linked into the freestanding device -- **still deferred, not
  decided**: M5's own parsing needed only multiply-and-add, never
  division, so this stays open until M7's percentage-threshold math
  actually forces the question.
- ReplyMsg + consumer hooks from a VBlank server on real 1.3 (M9).
- Copperline floppy write-back persistence (M9's record assertion depends on it).

## Verification

Every milestone: `make docker && make docker-test-harness`, then `sh tests/copperline/run.sh` (bundled AROS — what CI runs) **and** `KICK=/Users/simond/Downloads/kickstart-1.3.rom sh tests/copperline/run.sh` (real 1.3 — the claim that matters) — all `SUB=` lines and `RESULT=PASS` asserted over serial. CI (`copperline-smoke-aros` job) already runs the AROS lane on every push/PR. Phase-1-era tests must stay green through every refactor (M0 locks them in first).
