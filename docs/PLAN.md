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
| M10 | ✅ Done | Docs pass: teaching-reference comments through the source (the top-of-file overview was still describing "Phase 1" only -- rewritten to cover every mode/feature through M9; a real, previously-missing `$VER:` cookie added to both the device and `SanaInfo` along the way, since accurately documenting "check your version" needs one to actually exist), README, sana2-notes refresh (one stale `S2_GETSPECIALSTATS` table row fixed). Stood up `userdocs/` (11 pages, MkDocs Material, adopting sibling project amiauth's pattern in preference to narrator.wyoming's) alongside `tools/docs2guide.py` (adapted from amiauth's own, algorithm unchanged) producing an on-Amiga AmigaGuide manual (`make guide`) from the same source -- one docs source feeding both. Published later (2026-07-31, see the pre-release-verification note below) at https://sidick.github.io/sana2loop/ via `.github/workflows/docs.yml`; `mkdocs build --strict` + `make guide` still run as a CI build-only check (`docs-build` job) on every push so the site can't silently rot between real publishes | `src/loopback_device.c`, `src/sanainfo.c`, `src/pcap.h`, `README.md`, `docs/sana2-notes.md`, new `userdocs/`, `mkdocs.yml`, `tools/docs2guide.py`, `tools/docs-requirements.txt`, `Makefile`, `.github/workflows/ci.yml` |
| M11 | ✅ Done | Tools: `src/tools/pcapio.c` (dos.library-based, not stdio -- see its own comment for why: clib2's `fopen()` reliably failed under Copperline's minimal Startup-Sequence boot where plain dos.library `Open()` didn't), `sana2open.c` (model-citizen real buffer-management hooks), `SanaDump`, `SanaSend`; `host/make_fixtures.py` + `host/lint_pcap.py` (scapy); canonical corpus (ARP, DHCP, TCP handshake) in top-level `fixtures/`. On-target round-trip test (`tests/copperline/run-tools.sh`, a *separate* Copperline boot session from the device's own `run.sh` -- see that file's own comment: the device's "never free replay buffers at close" design leaves too little memory for a second clib2 program in the same session) -- see risk note below for the two real, non-obvious findings this surfaced | new `src/tools/*`, `host/*`, `fixtures/`, `Makefile`, `tests/copperline/run-tools.sh` + fixtures |
| M12 | ✅ Done | Aminet packaging + release flow, adopting sibling project amiauth's pattern (in preference to narrator.wyoming's simpler one, since amiauth's is the more tried-and-tested of the two): tag-driven `release.yml` that verifies the pushed tag matches `version.mk` and `sana2loop.readme`'s `Version:` field before doing anything else, cross-builds via `make docker`, greps all four built binaries for their `$VER` strings, packages via `make dist` (new `Makefile` `lha`/`dist` targets building a real, archive-capable `lha` from the same pinned `jca02266/lha` source commit amiauth uses -- Ubuntu's/Homebrew's `lha` is Lhasa, extract-only), validates the readme with `aminet-release-action` in `validate-only` mode, creates the GitHub release (curated notes sourced from `sana2loop.readme`'s own intro + Features section for the first release, `--generate-notes` once there's real tag-to-tag history to diff), and only then hands off to a *separate* job gated on the `aminet` GitHub Environment for the actual (irreversible-ish) Aminet FTP upload -- human-in-the-loop by construction, not an afterthought. See risk note below for two real gaps this surfaced (private-repo Environment protection limits, and the pre-existing V36-vs-V37 compatibility wording this milestone's own readme draft got wrong before a user catch fixed it) | `Makefile`, `sana2loop.readme` (new), `.github/workflows/release.yml` (new) |
| M13 | ✅ Done | SANA-II Rev 2/3/4/7 conformance coverage: buffer-hook negotiation genuinely INVOKED not just accepted (`S2_CopyTo/FromBuff16/32`, new `buffhooks=8\|16\|32` config knob), Rev 4 `S2_GETPEERADDRESS`/`S2_GETDNSADDRESS` + `Sana2DeviceQuery.RawMTU` (all missing from this toolchain's own `<devices/sana2.h>` -- local `#define`s sourced from wiki.amigaos.net, cross-checked against the header's own existing `0xc000`/`0xc001` sequence), Rev 7 DMA hooks accepted+counted (`su_DMAHooksOffered`, new `S2_GETSPECIALSTATS` record type) but never invoked (spec-legal graceful decline -- this device has no real DMA to offer). New `SanaConform` probe tool (`src/tools/sanaconform.c`) exercising both directions (driver advertising vs. stack actually invoking) with graceful-fallback checks against a real running unit. `SANA2LOOP_NUM_UNITS` now 14 (was 13): unit 13 is a new `buffhooks=32` fixture. See risk note below for a real, non-device Copperline/AROS quirk this surfaced (and fixed via test reordering, not device code) | `src/loopback_device.c`, `src/tools/sanaconform.c` (new), `Makefile`, `tests/copperline/sana2test.c`, `tests/copperline/run.sh`, `tests/copperline/run-tools.sh`, `tests/copperline/tools-startup-sequence.txt`, `tests/copperline/fixtures/unit13.config` (new), `docs/sana2-notes.md`, `userdocs/Configuration-Reference.md`, `userdocs/SANA-II-Conformance.md` (new) |
| M14 | ✅ Done | `replaycap=` config knob: makes `mode=replay`'s previously-fixed 4-record ceiling (`SANA2LOOP_MAX_REPLAY_PACKETS`) a per-unit override, 1-256 (`SANA2LOOP_CONFIG_MAX_REPLAYCAP`), validated at config-parse time like every other numeric key. Prompted by a user exploratory question (config-based override: yes; RAM-auto-sizing: no, conflicts with the project's determinism-by-construction goal). `su_ReplayCap` threads through `open()`/`reset_unit()` (free-before-reset ordering matches `su_RecordMax`'s established pattern) and `load_replay_file()` (13 call sites switched from the old compile-time constant to the per-unit runtime value -- verified via `nm`/`objdump` that the resulting `cap * sizeof(struct EchoPacket)` runtime multiply still needs no `__mulsi3`, this `-nostdlib` build's perennial libgcc hazard). `SANA2LOOP_NUM_UNITS` now 15 (was 14): unit 14 is a new `replaycap=0` (out-of-range) fixture, config-validation-only by design -- see risk note below for why the positive case (a raised cap actually loading more records) is deliberately NOT covered on-target, only via real Kickstart 1.3/3.1 locally | `src/loopback_device.c`, `tests/copperline/sana2test.c`, `tests/copperline/run.sh`, `tests/copperline/fixtures/unit14.config` (new), `userdocs/Configuration-Reference.md`, `userdocs/Replay-and-Record.md` |
| M15 | ✅ Done | `wire=`/`bps=`/`mintu=` config keys (GitHub issues #26/#28/#30, all prompted by reviewing nullsana, Aminet's 2000-era virtual SANA-II device found as prior art -- see the repo's 2026-08-04 docs commit retracting the "no prior art" claim). `bps=` makes `S2_DEVICEQUERY`'s `BPS` a per-unit value (was hardcoded 10000000 at the query site; reported only, deliberately never a throttle). `mintu=` adds a minimum-write-size floor: shorter writes fail `S2ERR_BAD_ARGUMENT` (rejected, never padded -- padding would mutate delivered bytes; and deliberately not `S2ERR_MTU_EXCEEDED`, which names the too-LARGE failure). `wire=ethernet\|slip\|cslip\|ppp` makes a unit report a point-to-point serial wire: `HardwareType`/`AddrFieldSize=32` via DEVICEQUERY, 4-byte IP-style station address (`0A:53:32:<unit>`, nullsana's own "IP number as hardware address" convention), per-wire mtu/bps defaults (1006/19200, ppp 115200 -- explicit `mtu=`/`bps=` always wins via `_explicit` flags, key order irrelevant), `S2_BROADCAST`/`S2_MULTICAST`/`S2_ADD/DELMULTICASTADDRESS`/`SANA2IOF_RAW` all `S2ERR_NOT_SUPPORTED`, `RawMTU=0`, `SANA2IOF_BCAST/MCAST` never set on reads, `dest_matches_unit()` compares `su_AddrSize` bytes with no broadcast/group concept, crossover across mismatched wire types fails `S2ERR_BAD_STATE` (loud, not a silent address-width blackhole), and replay/record are rejected at config time (the pcap side is LINKTYPE_ETHERNET-framed). `SANA2LOOP_NUM_UNITS` now 16 (was 15): unit 15 is the `wire=slip` fixture -- the first unit growth since the 2 MB memory-floor bump made it affordable (verified: bundled AROS's `scheduled` canary still green); `bps=57600`/`mintu=16` ride unit 1's existing fixture additively. New `mintu`/`wire` sub-tests placed before `record` (the "record is the last DOS I/O" M13 invariant). Verified: `make copperline-smoke` on bundled AROS + real Kickstart 1.3 + real 3.1 (all 36 sub-tests), `run-tools.sh` on bundled AROS | `src/loopback_device.c`, `tests/copperline/sana2test.c`, `tests/copperline/run.sh`, `tests/copperline/fixtures/unit15.config` (new), `tests/copperline/fixtures/unit1.config`, `userdocs/Configuration-Reference.md`, `userdocs/Changelog.md`, `userdocs/Installation.md`, `userdocs/SanaInfo-Reference.md`, `docs/sana2-notes.md` |
| M16 | ✅ Done | `addr=`/`errors=`/`loss=` config keys (GitHub issues #27/#29, second nullsana-inspired batch). `addr=` pins the FACTORY station address (current starts equal; M3's `S2_CONFIGINTERFACE` one-shot override unchanged on top) -- colon-separated hex, byte count validated against the wire's address width after `wire=` resolution, Ethernet group-bit rejected. `errors=N`: 1-in-N writes fail VISIBLY (`S2ERR_TX_FAILURE`/`S2WERR_TOO_MANY_RETRIES` + `S2EVENT_TX\|ERROR` via the existing `fire_event()` -- first real exercise of the momentary event bits); checked pre-allocation, PacketsSent/flap counters untouched. `loss=N`: 1-in-N silent loss right after `droppct`, identical observable semantics, composes. Both use `prng_hits_one_in()` (hit iff 16-bit roll * n < 65536; 0/1 exact draw-free boundaries so enabling one knob never shifts another's seeded sequence). **Milestone side-quest: the device now provides its own `__mulsi3`** -- M16's two-ULONG unit-struct growth pushed `sizeof(struct Sana2LoopUnit)` past the backend's shift/add budget at PRE-EXISTING index sites (3 undefined-`__mulsi3` link failures with zero new multiplies in the new code), proving the M8/M14 avoidance strategy no longer scales; a 3-mulu.w schoolbook implementation (inline asm, placed AFTER `_start()`/romtag -- it landed at text offset 0 otherwise, displacing the safety return, observed not theoretical) retires the hazard class permanently. `SANA2LOOP_NUM_UNITS` now 18 (was 16): units 16/17 are the `errors=1`/`loss=1` fixtures (each knob changes what a plain write observably does, so neither can ride another unit's fixture); `addr=` rides unit 1 additively. Verified: `make copperline-smoke` on bundled AROS + real Kickstart 1.3 + real 3.1 (all 39 sub-tests) + `run-tools.sh` | `src/loopback_device.c`, `tests/copperline/sana2test.c`, `tests/copperline/run.sh`, `tests/copperline/fixtures/unit16.config` (new), `tests/copperline/fixtures/unit17.config` (new), `tests/copperline/fixtures/unit1.config`, `userdocs/Configuration-Reference.md`, `userdocs/Fault-Injection.md`, `userdocs/Changelog.md`, `userdocs/Installation.md`, `userdocs/SanaInfo-Reference.md`, `docs/sana2-notes.md` |
| M17 | ✅ Done | `delay=`/`deviation=` config keys (GitHub issue #24, third and final nullsana-inspired batch item). A written packet's delivery can be deferred `delay` VBlank ticks (0 = disabled, synchronous, byte-for-byte pre-M17 behavior) with optional symmetric `deviation` jitter (`compute_delay_ticks()`: uniform draw in `[delay-deviation, delay+deviation]`, clamped at 0, via the same 16x16 mulu.w top-bits technique `prng_hits_pct()`/`prng_hits_one_in()` use -- not `%`, since `__umodsi3` still isn't provided even though M16 added `__mulsi3`). New per-unit `su_DelayQueue` (a plain `struct List` of `EchoPacket`s, reusing `ep_Node` -- no wrapper allocation, and genuinely unbounded rather than fixed-array-sized like `su_ReorderBuf`, since an arbitrary number of packets can be in flight at once; bounded only by the same `AllocMem()` backpressure every other allocation here already has). Runs as the pipeline's final stage (`deliver_with_delay()`, replacing `deliver_or_reorder()`'s direct `deliver_packet()` call and reorder's own natural-eviction path) -- deliberately AFTER reorder, so a packet's simulated transit latency is whatever's left once every other fault decision already ran. `vblank_server()`'s per-unit tick gate widened to a third reason (config-derived, not queue-depth-derived, so it can never miss a drain): `delay_queue_tick()` scans the WHOLE queue every tick (not just a sorted prefix -- jitter means a later write can come due sooner than an earlier one) and releases due packets via the same real-VBlank-interrupt `deliver_packet()` invocation path M9 already proved legal. `CMD_FLUSH`/`S2_OFFLINE`/last-close gained a parallel `delay_queue_flush()` (mirroring `reorder_flush()`'s own placement and reasoning exactly: `su_Tick` only advances while online, so anything left queued when a unit goes offline would otherwise wait for a tick that never comes) -- and, critically, flushed/reorder-evicted-by-flush packets bypass `deliver_with_delay()` entirely (call `deliver_packet()` directly), since a flush's whole point is "push everything out now," not "re-queue into a fresh wait." `SANA2LOOP_NUM_UNITS` now 19 (was 18): unit 18 is the `delay=10` fixture (deviation=0, kept PRNG-free so the on-target assertion is exact) -- its own on-target sub-test is the first to need a NEW proof technique: right after the write's own (always-synchronous) `do_io()` returns, a plain read of the existing `g_HookToCount` side-effect global (not `CheckIO()`/`WaitIO()` on the read's own request, which the M9 risk note already flagged as unreliable on bundled AROS) proves delivery has NOT happened synchronously, before `reap_interrupt_io()` proves it eventually does, correctly, via the genuine interrupt path. Verified: `make copperline-smoke` on bundled AROS (passed clean first try) + real Kickstart 1.3 + real 3.1 (all 40 sub-tests) + `run-tools.sh` | `src/loopback_device.c`, `tests/copperline/sana2test.c`, `tests/copperline/run.sh`, `tests/copperline/fixtures/unit18.config` (new), `userdocs/Configuration-Reference.md`, `userdocs/Fault-Injection.md`, `userdocs/Changelog.md`, `userdocs/Installation.md`, `userdocs/SanaInfo-Reference.md`, `docs/sana2-notes.md` |
| M18 | ✅ Done | Multi-open fan-out delivery + real `S2_PacketFilter` dispatch + `SANA2OPF_MINE` (GitHub #25, nullsana-inspired -- scoped via two prior investigation comments on the issue before any code moved). Typed `CMD_READ` fan-out: every DISTINCT open with a matching queued read gets one fill (at most one per open per packet -- `open_already_collected()` dedup during the collect walk), rewriting `deliver_packet()` around the existing `abort_requests_for_open()`/`fire_event()` collect-under-one-`Disable()`-then-act-outside pattern. `S2_READORPHAN` deliberately stays GLOBAL and single-delivery (the spec's own "unclaimed-type catch-all" definition, not a sniffing tool) -- this design choice is exactly what let the pre-existing `delivery` sub-test's own typed-beats-orphan assertion pass UNCHANGED, confirming the fork resolved correctly rather than needing a test rewrite. `S2_PacketFilter` (recorded since M1, "not yet dispatched") now genuinely runs per candidate, before the copy hook: new `Sana2FilterFunc` typedef (this file's THIRD distinct hook register convention -- `a0`=hook/`a2`=ios2/`a1`=data/`d0`=result, sourced from wiki.amigaos.net's Autodocs, objdump-verified to confirm the compiler honored the non-sequential register assignment, same M1 discipline as the original copy-hook ABI proof), `call_packet_filter()`, and a `fill_read_from_packet()` split into `prefill_read_metadata()` (everything the filter is entitled to see) + the copy step. A rejected candidate requeues (`AddHead`) rather than failing; investigated the AbortIO-during-filter-transit race in depth and deliberately added NO new tracking machinery for it (an io_Flags bit is unsafe -- SANA2IOF_* is spec-owned and SendIO()/DoIO() already clear it wholesale per M4's own finding -- and a 4th searched list would add exactly the list-walk surface the pre-launch review's own fixes were reducing) -- `abort_io()`'s existing, pre-M18 "-1: let WaitIO see it naturally" contract already covers the widened window correctly, and this device's synchronous task-context delivery model makes the race unreachable from any test following the file's own write-then-check sequencing; documented, not just asserted. `SANA2OPF_MINE`: new `su_Exclusive` BOOL, checked in `open()` before `dev->lib_OpenCnt++` (busy-rejected opens never became real opens), `IOERR_UNITBUSY` either direction. Zero new units (fanout/packetfilter run on unit 0's config-free default; mine reuses unit 13). New `fanout`/`packetfilter`/`mine` sub-tests (`fanout`'s (b) half manually shares one open's cookie across two IORequests -- the standard SANA-II pipelining pattern -- to prove one-candidate-per-open without a second real `OpenDevice()` call). Real-1.3 `run.sh`'s own `BENCH` default raised 30->45 (found via this milestone's own extra OpenDevice/CloseDevice/config-read cycles pushing total session time past the old budget on real 1.3's slower dos.library -- confirmed via a working guest-side `END`/`RESULT=PASS` but a 0-byte `recorded.pcap` extraction, i.e. the LAST file's own write-back not yet flushed when `--benchmark-until` tore the session down; same class of fix as M9's own KICK= default raise). Verified: `make copperline-smoke` on bundled AROS (43 sub-tests, first try) + real Kickstart 1.3 + real 3.1 + `run-tools.sh` | `src/loopback_device.c`, `tests/copperline/sana2test.c`, `tests/copperline/run.sh`, `userdocs/Echo-and-Crossover.md`, `userdocs/SANA-II-Conformance.md`, `userdocs/Changelog.md`, `docs/sana2-notes.md` |
| M19 | ✅ Done | Companion tools become general-purpose SANA-II utilities (GitHub #33-#37, sanamon-inspired -- `comm/net/sanamon`, Michael van Elst, 1995, a general SANA-II packet monitor whose whole design assumes it targets any driver). `DEVICE=` added to all four tools (`SanaInfo`/`SanaDump`/`SanaSend`/`SanaConform`), each previously hardcoding `"loopback.device"` as a literal -- default unchanged so every existing invocation keeps working. Opt-in `CONFIG` (all four): `S2_GETSTATIONADDRESS` then `S2_CONFIGINTERFACE` using the driver's own reported factory address (matching sanamon's own documented "configure interface with its default address"), tolerating `S2WERR_IS_CONFIGURED` gracefully -- sana2loop's own units never need this, a real driver with no stack running might. `ONLINE`'s default FLIPPED on `SanaDump`/`SanaSend`/`SanaConform`: pre-M19 always forced `S2_ONLINE` unconditionally at startup with no offline at exit -- harmless against sana2loop's own device, a real footgun once `DEVICE=` can target a live driver a stack manages (matching sanamon's own opt-in `ONLINE` flag design, which "brings driver online... and offline when sanamon is exiting" only when explicitly asked). `SanaConform` gained a genuine `S2_PacketFilter` invocation probe (`hook_filter()`, the same `a0`/`a2`/`a1` convention M18 established, added to its existing tag array) -- the exact gap sanamon's own readme calls out ("very few SANA-2 drivers implement the packet filter option"), now also validating M18's own dispatch work end-to-end. `SanaDump` gained `DUMP`/`LEN` -- a live sanamon-styled console line (`dump_packet()`/`print_addr_compact()`) per captured packet, alongside its existing `.pcap` file output, with zero new device-side work (reuses `SANA2IOF_BCAST`/`MCAST` M18's `prefill_read_metadata()` already computes). Real, unplanned finding while wiring the round-trip test's own `tools-startup-sequence.txt` for the new `ONLINE` default: giving BOTH `SanaDump` and `SanaSend` their own `ONLINE` in that shared-unit scenario is an actual correctness bug (`SanaSend`'s own exit would take unit 12 offline while `SanaDump`, still running, still expects it online) -- resolved by giving `ONLINE` only to `SanaDump` (first user, longest-open). Second, more involved finding chasing on-target coverage for `DUMP`/`LEN`: a backgrounded `Run`'s stdout redirected to a REAL FILE (`>S:sana2loop/dump.out`) reliably captures only AmigaDOS's own `"[CLI N]"` process-creation banner, never a child clib2 program's actual stdout content -- confirmed on both bundled AROS and real Kickstart 3.1, a genuine environment/toolchain limitation of this specific combination, not a device/tool bug; a red herring chased first (bundled AROS separately, transiently hung the WHOLE session when that same real-file redirect was combined with a 3rd `/K/N` template field specifically -- real Kickstart 3.1 never reproduced it, and it stopped mattering once the redirect itself proved unable to capture content anyway). Net: `DUMP LEN=16` stays exercised in the shared round-trip session (proving the code path runs without error) via the working `>NIL:` redirect; its own printed TEXT is verified by code review instead, operating on `io` fields `verify_sanadump.py` already proves correct via `captured.pcap`. Verified: `make copperline-smoke` (device/harness, unaffected by this milestone) on bundled AROS + real Kickstart 1.3 + real 3.1; `make copperline-smoke-tools` on bundled AROS + real Kickstart 3.1 (real 1.3 not applicable -- these are V36+ tools, per `run-tools.sh`'s own long-standing documented constraint) | `src/sanainfo.c`, `src/tools/sanadump.c`, `src/tools/sanasend.c`, `src/tools/sanaconform.c`, `tests/copperline/run-tools.sh`, `tests/copperline/tools-startup-sequence.txt`, `userdocs/SanaDump-and-SanaSend.md`, `userdocs/SanaInfo-Reference.md`, `userdocs/SANA-II-Conformance.md`, `userdocs/Changelog.md`, `docs/sana2-notes.md` |

## SANA-II conformance coverage: Rev 2/3/4/7 additions

**Status: done as of M13** (see the milestone table above and
`docs/sana2-notes.md`'s own "SANA-II Rev 2/3/4/7 additions (M13)" section
for what actually shipped). One deliberate scope note against the
original plan below: Rev 7's DMA hooks are recorded and counted but
never INVOKED at all (a hardware-less RAM loopback has nothing to hand a
DMA-capable buffer address to), so there is no "rejection" call whose
`ios2_WireError` needs setting -- declining to call a hook in the first
place isn't a failure path, it's the spec-legal default every driver is
allowed to take ("shall fall back to standard CPU callbacks if
necessary"). Kept as originally planned: this section's own
S2ERR_MTU_EXCEEDED-for-raw-writes idea was already covered by M4's
existing MTU+14 raw-write allowance (RawMTU just gives that number a
name), so no separate new fault-injection knob was needed for it.

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
- **M11's on-target round-trip test surfaced two real, non-obvious
  findings, neither a device bug.** (1) clib2's `fopen()` reliably FAILED
  to open a file (`ENOENT`-shaped failure) that plain dos.library
  `Open()` opened without issue, for the identical path, under
  Copperline's minimal Startup-Sequence-launched boot environment -- root
  cause not pinned down further once the fix was this simple: switched
  `src/tools/pcapio.c` to dos.library `Open()`/`Read()`/`Write()`/
  `Close()` entirely, matching `loopback_device.c`'s own
  `load_replay_file()`/`read_unit_config()` pattern (different original
  reason -- freestanding, no libc at all -- but the identical mechanism,
  proven reliable everywhere this project has tested it). (2) Running
  SanaDump/SanaSend in the SAME Copperline boot session immediately after
  `sana2test.c`'s own full run reliably failed `OpenDevice()` with
  `IOERR_OPENFAIL` -- confirmed via `AvailMem(MEMF_CHIP)` checked right at
  the failing call: only ~9 KB chip free, because `loopback.device`'s own
  documented "never free replay-mode buffers at close, only at the next
  open" design (see M8/M9 above) leaves M8/M9's replay-mode units'
  buffers permanently allocated for as long as the device stays loaded --
  fine for `sana2test.c` itself (nothing needed to run afterward) but
  starves a second, separate clib2 program trying to load in the same
  session. Fixed by giving the tools their own **separate** Copperline
  boot session (`run-tools.sh`/`machine-tools.toml`, fresh boot, no
  `sana2test.c` run first) with a more generous memory profile than the
  device's own bare-512K target floor (documented in
  `machine-tools.toml`'s own comment: SanaDump/SanaSend are V36+/2.0+
  convenience tools, never claimed to run on the device's own
  hardware-floor budget in the first place). A **third**, genuinely
  test-*design* gap (not an environment quirk) surfaced once memory
  wasn't the blocker: SanaSend closing the device (dropping unit 12's
  open count to 0) before SanaDump opened fresh triggered
  `reset_unit()`'s own documented "reset at first-open" behavior,
  silently discarding whatever SanaSend had just buffered via `rxqueue` --
  fixed by having SanaDump run FIRST, backgrounded via `Run`, so the open
  count never drops back to 0 between the two (see
  `tests/copperline/run-tools.sh`'s own comment). Verified: 5/5 clean
  `SANADUMP_VERIFY=PASS` runs on bundled AROS.
- **Post-M11, found while documenting the device's "never free replay/
  record buffers at close" design: `expunge()` never freed those buffers
  either.** `reset_unit()` frees `su_ReplayPackets`/`su_ReplayDueTicks`/
  `su_ReplayDeliveryCopies`/`su_RecordBuf` only at a unit's own next
  0->1 open transition -- by design, so far so documented. But
  `expunge()` (which Exec calls to unload the whole device once its
  device-wide `lib_OpenCnt` reaches 0 -- e.g. via the Shell's
  `Avail FLUSH`, whose documented job is expunging currently-unused
  libraries/devices/fonts) only ever freed the device base struct itself,
  never looping over `sld_Units[]` first. Any unit that had used
  `mode=replay`/`record=` and was closed but never reopened before the
  whole device got expunged would leak those buffers permanently -- once
  the device base is freed, nothing points at them again, so this was
  strictly worse than the documented "held until next open" cost, not
  just an instance of it. Fixed by looping over every unit and calling
  `reset_unit()` on each before the base's own `FreeMem()` -- reuses the
  existing free logic rather than duplicating it (harmless that it also
  resets other unit fields on a struct about to be freed anyway). Found
  by code review, not on-target testing; verified the fix doesn't
  regress anything via `make copperline-smoke` (bundled AROS) and
  `run-tools.sh`, both still green.
- **M12's `sana2loop.readme` first draft stated the companion tools'
  floor as "Kickstart 2.0 (V36)"** -- technically the API-level fact
  (`CreateMsgPort()`/`CreateIORequest()` are indeed V36+ Exec calls, per
  `src/sanainfo.c`'s own header comment) but a real regression from what
  `userdocs/Installation.md` already documented correctly: V36 (bare
  Kickstart 2.0) only ever existed in unreleased pre-2.04 hardware, so
  "2.04 (V37) or later" is the only realistic, testable floor to publish
  as a compatibility claim. Caught by the user reviewing the draft, not
  self-caught -- fixed to match `userdocs/Installation.md`'s existing
  wording; no other files needed changing since that page already had it
  right.
- ~~The `aminet` GitHub Environment's required-reviewer protection rule
  could not be provisioned~~ -- **resolved 2026-07-31**: `gh api` first
  returned 422 ("Please ensure the billing plan supports the required
  reviewers protection rule") because the repo was still private at the
  time -- GitHub only allows required-reviewer environment protection on
  private repos on a paid plan (Team/Enterprise); sibling project
  amiauth's own `aminet` environment only ever worked because that repo
  is public. The repo went public the same day (independent of this
  milestone's own scope), and re-running the identical `gh api` call
  immediately succeeded with no other change needed -- the `aminet`
  environment now has `sidick` as a required reviewer, matching amiauth's
  own setup exactly. The `aminet` job will now genuinely pause for manual
  approval on any real tag push, as originally designed.
- **M13's on-target test coverage surfaced a real, non-device Copperline/
  AROS floppy write-back ordering quirk.** Adding a 5th sub-test cluster
  that performs a genuinely successful `dos.library` `Read()` of a NEW
  fixture file (`unit13.config`, for the new `buffhooks` sub-test) AFTER
  M9's `record` sub-test's own already-flushed-and-closed `recorded.pcap`,
  in the same Copperline boot session, reliably corrupted
  `recorded.pcap`'s data blocks on the host-side ADF -- `xdftool ... type`
  on it post-run failed with `FSError: Invalid FileData Block`, even
  though the guest's own serial capture showed every `SUB=` line and
  `RESULT=PASS` cleanly and the disk was nowhere near full (~7% used).
  Root-caused by elimination: NOT a device bug (the identical buffhooks
  logic, isolated via a temporary debug build, produced entirely correct
  in-guest results); NOT a raw file-count effect (a throwaway 14th
  fixture file the guest never actually opens left `recorded.pcap`
  intact); NOT a timeout-truncation effect (raising `BENCH` well past
  what the guest needs made no difference). The one variable that
  mattered: whether `read_unit_config()` performed a real, successful
  `Open()`+`Read()` of a new file LATE in the session, after
  `recorded.pcap` was already flushed and closed -- pointing at a
  Copperline/AROS floppy write-back ordering quirk (likely a cached
  track/block buffer for the earlier file getting disturbed by a later,
  unrelated file read before `--benchmark-until`'s cutoff ends the
  session), not something this project's own code controls either side
  of. **Fixed by reordering `sana2test.c`'s sub-tests, not by touching
  device code**: the whole M13 block now runs BEFORE M9's `record`
  sub-test, restoring the "the file-write-then-verify test is the last
  DOS I/O in the session" property every milestone before M13 already had
  by accident. Verified: 3/3 clean `RESULT=PASS` + `RECORD_VERIFY=PASS`
  runs on bundled AROS, 2/2 on real Kickstart 1.3, after the reorder. See
  `docs/sana2-notes.md`'s M13 section for the full investigation.
- **Pre-release verification (2026-07-31)**: prompted by the user asking
  to test against a real Kickstart 3.1 ROM at least once before launch,
  and to confirm the real SANA-II install-directory convention. `run.sh`
  against real 3.1 passed 100% clean first try. `run-tools.sh` against
  the same ROM did not -- a real, pre-existing (not M13-introduced) bug:
  `tools-startup-sequence.txt`'s `Wait 2` needs `C:Wait`, which the
  minimal test disk never actually contains; bundled AROS silently
  tolerates this, real Kickstart 3.1 hangs indefinitely at that line
  (root-caused via bisection to the same "silently waits forever for a
  human" class of hazard as the M5-documented `ENV:`-unassigned hang, not
  a device bug). Fixed by removing `Wait 2` (unneeded -- SanaSend's write
  is synchronous and echo delivery completes inside that same call, so
  SanaDump's own signal is already pending by the time the script
  continues); verified 3/3 clean on both bundled AROS and real 3.1
  afterward. Separately, confirmed against three independent real-world
  sources (a driver's own install docs, classic AmiTCP 3.0's installer
  text, and AmiTCP_NG's own troubleshooting notes verbatim -- "OpenDevice()
  does not search DEVS:Networks/, so you must name the driver by full
  path") that `DEVS:Networks/` is the real convention a network stack's
  own config expects a SANA-II driver in, separate from and not
  superseding plain `DEVS:` (what this project's own bare-name
  `OpenDevice()` calls need). Fixed via documentation only -- no
  `OpenDevice()` call site changed, given the real (if unconfirmed either
  way) risk that a full-path open might not reliably find/reuse an
  already-resident device the way echo/crossover's shared-instance design
  requires. See CLAUDE.md's matching entry for the full account of both.
  Same day: published the M10 docs site (built but never deployed until
  now) at https://sidick.github.io/sana2loop/ via a new
  `.github/workflows/docs.yml` (mike-based versioned deploy, adapted from
  amiauth's own -- see mkdocs.yml's header comment), deployed under
  version `v1.0` (matching `version.mk`'s eventual first release) ahead
  of any real release tag, then updated every "not yet published"
  reference (`README.md`, `sana2loop.readme`, `CLAUDE.md`) to the live
  URL.
- **M14's on-target test coverage for the POSITIVE `replaycap=` case
  (a raised cap actually loading more than the default 4 records)
  reliably fails under bundled AROS -- root-caused to memory pressure,
  not a device bug, and scoped out of the automated suite as a result.**
  A first attempt (`replaycap=6` on a new unit alongside the existing
  M8/M9 replay-mode units 9/10) failed with `SUB=replaycap=FAIL` on
  bundled AROS only; real Kickstart 1.3 passed cleanly first try.
  Debugged by stashing progress markers into unused `Sana2DeviceStats`
  fields (the same technique M9's investigation established) at each
  `return FALSE;` inside `load_replay_file()`: the failure was the
  `AllocMem(cap * sizeof(struct EchoPacket), MEMF_CLEAR)` call itself,
  and a follow-up `AvailMem(MEMF_LARGEST)` probe at that exact point
  showed only ~4-5 KB free against the ~9.2 KB the cap=6 allocation
  needed. Bisecting the cap value down (5, then even 4 -- the
  pre-existing, already-shipped default) reproduced the identical
  failure, proving this isn't about the raised value specifically: it's
  a THIRD simultaneously-held replay buffer (alongside units 9/10's own,
  never freed per the documented "held until next open" design -- see
  `userdocs/Replay-and-Record.md`'s "Memory usage" section) exceeding
  bundled AROS's tight 512K+512K profile at this point in the sequence,
  the same class of AROS-specific memory-budget gap M8 first hit when
  settling on 4 as the default. Real Kickstart 1.3 confirmed the feature
  itself is correct (`DBG_RC=1,1`, all 6 records read back with matching
  type/length/payload). A second attempt moved the positive case to
  `run-tools.sh`'s more generous session (1M chip + 512K slow + 1M fast)
  via `SanaConform`/`SanaDump`, but even just OPENING the extra unit
  there broke the whole bundled-AROS session (every subsequent file,
  including pre-existing ones from earlier in the same run, came back
  empty/not-found) -- confirmed via bisection that the open call alone
  reproduces it, and confirmed via real Kickstart 3.1 that the identical
  commands work perfectly there. **Resolution**: scaled the automated
  on-target test down to a single, allocation-free check (unit 14,
  `replaycap=0`, out of the valid 1-256 range, must fail at pure config
  validation before `load_replay_file()` runs at all) that passes
  reliably everywhere. The positive case is verified functionally
  correct via real Kickstart 1.3 (debug-instrumented run) and real
  Kickstart 3.1 (`SanaConform`), just not covered by the automated
  bundled-AROS-based suite -- consistent with this project's existing
  stance that AROS is "a fine ROM-free stand-in for catching gross
  regressions... not a substitute for confirming against real 1.3" (see
  CLAUDE.md's "Testing under Copperline" section), extended here to a
  genuine memory-budget gap rather than a behavioral difference.
- **Pre-launch review (2026-07-31)**: an independent multi-agent review
  (device code, tools, tests/CI/build, docs accuracy, release readiness)
  found 20 real issues (#1-#20 on GitHub), each fixed and verified
  on-target before closing. Highlights: four real device-code concurrency
  bugs (unprotected M7 reorder-buffer mutation; unprotected consume-mode
  replay reads; two list-walk functions resuming from a stale `next`
  pointer across a dropped `Disable()` window; `close()`/`reset_unit()`
  letting a drained unit's queue get silently repopulated); a
  `parse_ulong()` overflow that defeated the config parser's own
  fail-fast contract; `S2_DEVICEQUERY` writing past a caller's honestly-
  advertised `SizeAvailable`; `open()` never clearing `LIBF_DELEXP`;
  the per-open cookie magic being written but never validated; a
  recorded-pcap timestamp field actually holding raw VBlank ticks instead
  of real seconds; and a genuine gap in the DOS-I/O-under-broken-Forbid
  concurrency story (partially closed -- `lib_OpenCnt` now bumped before
  any dos.library I/O in `open()`, closing a real use-after-free via
  concurrent `expunge()`; the remaining cross-unit race is documented,
  not fixed, a deliberate scope call). Also a `SanaDump` capture-buffer
  overflow on a legal `SANA2IOF_RAW`-sized delivery, several companion-
  tool robustness fixes, a missing AmigaGuide page, inverted echo-address
  docs, a Linux-runner `release.yml` root-ownership bug, Makefile
  dependency gaps, and five new on-target sub-tests (`truncate=`,
  `SanaInfo` execution, `recordmax=` overflow, `rxqueue>0`'s actual
  bound, plus a fixed vacuous grep). See CLAUDE.md's own entry for the
  fuller account, including two real AROS-specific environmental findings
  this surfaced along the way (both resolved, neither a device bug): new
  on-target test units initially broke an unrelated, pre-existing
  sub-test on bundled AROS via memory pressure (resolved by reusing
  existing units instead of growing `SANA2LOOP_NUM_UNITS`), and the
  recorded-timestamp fix's own small per-unit struct growth did the same
  on its own -- resolved by raising `machine.toml`'s documented minimum
  RAM floor from 1 MB to 2 MB rather than re-shrinking future fixes
  indefinitely (a deliberate, user-approved decision, now documented in
  `userdocs/Installation.md`).
- **Copperline 0.14.0 upgrade (2026-08-01)**: bumped `COPPERLINE_VERSION`
  0.13.0 -> 0.14.0 in CI (`.github/workflows/ci.yml`); release notes
  reviewed for anything load-bearing to this project. Two items were:
  the real 68000+`[[filesys]]` MMIO doorbell bug (contributed upstream by
  the user, sidick) is fixed and released, and hostfs (`HOSTFS0:`) volumes
  now advertise mounting under Kickstart 1.3 (also contributed by the
  user), not just 2.0+. Explored restructuring `run.sh`/`run-tools.sh` to
  boot a static ADF plus a `[[filesys]]` hostfs mount for fixture/output
  data, replacing the `xdftool`-built-and-extracted floppy layout and
  sidestepping the M13/pre-launch-review floppy write-back-ordering quirk
  class entirely. Implemented and passed 4/4 clean against bundled AROS --
  but empirically, on both a real Kickstart 1.3 ROM AND a real Kickstart
  3.1 ROM, guest `Open()` of `HOSTFS0:` (read or write) reliably fails in
  this project's own minimal from-scratch boot ADF (no `SetPatch`, no
  Workbench boot init), despite the underlying board visibly initializing
  in Copperline's own boot log. Root cause not chased past that (plausibly
  a real-Kickstart boot-node-scan step this project's bare-bones
  Startup-Sequence never triggers, that AROS performs unconditionally).
  **Reverted** rather than shipped: building this project's own on-target
  verification around a mechanism that only works on the ROM-free stand-in
  and fails on every real ROM tested would invert this project's own
  "AROS can't validate real hardware fidelity" stance into an active trap.
  `run.sh`/`run-tools.sh` stay on their pre-0.14.0 `xdftool`-based
  approach; `[[filesys]]` remains unused by this project's own tests. See
  CLAUDE.md's "Testing under Copperline" section for the full account.

## Verification

Every milestone: `make docker && make docker-test-harness`, then `sh tests/copperline/run.sh` (bundled AROS — what CI runs) **and** `KICK=/Users/simond/Downloads/kickstart-1.3.rom sh tests/copperline/run.sh` (real 1.3 — the claim that matters) — all `SUB=` lines and `RESULT=PASS` asserted over serial. CI (`copperline-smoke-aros` job) already runs the AROS lane on every push/PR. Phase-1-era tests must stay green through every refactor (M0 locks them in first).
