# Troubleshooting and FAQ

## `OpenDevice()` fails with `S2ERR_BAD_ARGUMENT`

Almost always a config file problem. `loopback.device` is deliberately
**fail-fast** about configuration: an unrecognized key or a malformed line
in a `unitN.config` file that *exists* fails the whole open, rather than
silently ignoring the typo or falling back to a default. Check:

- Is every `key=value` line spelled exactly right (no spaces around `=`)?
  See [Configuration Reference](Configuration-Reference.md) for the exact
  key list.
- Is every numeric value in range (`droppct=150` is invalid, for example)?
- For `mode=crossover`: does `peer=` point at a different, real unit
  number? A crossover unit with no peer, or a peer pointing at itself,
  fails to open.
- For `mode=replay`: does `replay=` point at a real, loadable `.pcap` file
  with **no more than the unit's `replaycap`** (default 4)? (See
  [Replay and Record](Replay-and-Record.md) for what `replaycap` is and
  why its default is so small.)

A **missing** config file is fine and not an error — only a file that
exists but has a problem fails the open.

## My network stack's config says it can't find/open `loopback.device`

Check **where you copied it** vs. **how your stack looks for it**.
`OpenDevice("loopback.device", ...)` (what `SanaInfo`/`SanaDump`/
`SanaSend`/`SanaConform`, and most hand-written test programs, use) only
ever searches plain `DEVS:` for a bare device name. A real network stack
(AmiTCP, AmiTCP_NG, Roadshow) instead expects the driver file to live in
`DEVS:Networks/`, and constructs the **full path**
(`DEVS:Networks/loopback.device`) itself once its own config points at it
— confirmed directly against AmiTCP_NG's own troubleshooting notes:
*"`OpenDevice()` does not search `DEVS:Networks/`, so you must name the
driver by full path."* These are two separate lookup rules for two
different kinds of caller, not a bug in either — see
[Installation](Installation.md) for copying it to both locations if
you're doing both kinds of testing.

## I edited a unit's config file but nothing changed

Configuration is only re-read the moment a unit's open count goes from 0 to
1 — i.e. the *first* `OpenDevice()` after every previous opener has closed.
A unit that's still open anywhere keeps its already-loaded configuration
untouched. Close every opener of that unit, then open it again.

## My `SANA2IOF_RAW` flag isn't being honored

This is a real AmigaOS gotcha, not specific to this device: both
`SendIO()` and `DoIO()` clear `io_Flags` before dispatching the
request (their own autodocs say so). If you set `SANA2IOF_RAW` on your
`IOSana2Req` and then call `SendIO()`/`DoIO()`, the flag is gone by the
time the device ever sees it. `amiga.lib`'s `BeginIO()` is documented as
"equivalent to `SendIO()`, except that `io_Flags` is not cleared" — that's
the call to use if you need `SANA2IOF_RAW` (or any other flag) to actually
reach the device.

## A crossover write isn't reaching the peer

Check both units are genuinely paired: `peer=` on each config must name
the *other* unit. Also check the peer's own MTU if the write is failing
with `S2ERR_MTU_EXCEEDED` — crossover mode checks **both** the writer's own
MTU and the peer's, since a real link has two independently-configured
ends. See [Fault Injection](Fault-Injection.md#pmtud-black-hole-oversilent-crossover-only)
if you specifically want an oversized write to vanish silently instead of
failing.

Also check destination filtering: unless the reading open used
`SANA2OPF_PROM` (promiscuous), a crossover peer only receives packets
addressed to its own station address, the broadcast address, or a joined
multicast group — see
[Echo and Crossover Modes](Echo-and-Crossover.md#delivery-precedence-both-modes).

## My packet never arrives and nothing tells me why

With the default `rxqueue=0`, a packet with **no queued read waiting for
it** (no matching typed `CMD_READ`, no pending `S2_READORPHAN`) is dropped
immediately — this matches real SANA-II drivers, which hold no internal
receive buffers of their own. `Sana2DeviceStats.UnknownTypesReceived`
(via `S2_GETGLOBALSTATS`) counts these. If you want a small buffering
window instead, set `rxqueue=` to a nonzero value — see
[Configuration Reference](Configuration-Reference.md).

## Why is the replay capture limit only 4 records?

That's only the *default* — set `replaycap=` (1–256, per-unit; see
[Configuration Reference](Configuration-Reference.md)) to raise it for a
unit that needs a longer capture. `4` isn't an arbitrary round number as
a default, though: it was set empirically, after replay buffers sized for
more records reliably failed to allocate under a memory-constrained test
profile (see the [Changelog](Changelog.md) for the story) — so raising
`replaycap` is your own responsibility to budget for: the device only
fails loudly (`S2ERR_NO_RESOURCES`) if a raised value doesn't actually fit
in whatever RAM your target has free, it doesn't know that in advance. If
your capture is too large for the memory you have to spare regardless,
split it into multiple smaller files and load each on a different unit,
or drive live traffic instead of replay.

## A unit that used `mode=replay`/`record=` seems to hold onto memory after I close it

That's expected — see
[Replay and Record: Memory usage, and freeing it manually](Replay-and-Record.md#memory-usage-and-freeing-it-manually)
for why, and for how to reclaim it (per-unit, or everything at once via
`Avail FLUSH`) without rebooting.

## Does this run on my hardware/emulator?

The device targets plain 68000 and Kickstart 1.3 — if your setup runs
those (or anything newer), it runs `loopback.device`. Every release is
validated on-target under the Copperline emulator against both a real
Kickstart 1.3 ROM and AROS's bundled, redistributable Kickstart
replacement — see [Building and Testing](Building-and-Testing.md) if
you're curious how.
