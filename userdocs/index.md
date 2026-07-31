# sana2loop

**A hardware-free SANA-II network device for AmigaOS.**

`loopback.device` gives any network stack, shim, or driver-consuming
software a deterministic test partner with **zero hardware and zero
emulator network support required**. No null/dummy SANA-II device has
existed before this, despite being the obvious first thing to reach for
when testing a TCP/IP stack, a shim, or anything else that talks to a
SANA-II driver.

It provides three modes on the same device: **echo** (whatever you write
comes straight back to you), **crossover** (two opens of the device talk to
each other, like a virtual null-modem cable), and **pcap replay/record**
(play back a captured trace on real, deterministic timing, or capture
whatever a unit actually sees). A configurable **fault-injection**
pipeline — drop, duplicate, truncate, reorder, silently-oversized frames,
forced online/offline flaps — lets you test how a stack copes with a bad
link, not just a perfect one.

It targets **plain 68000 and Kickstart 1.3** — a test tool's value is
universality, so there's no reason to demand anything newer. It runs
identically on a stock A500 or an accelerated/emulated machine.

## Where to start

- **New user?** Read [Installation](Installation.md), then
  [Getting Started](Getting-Started.md) for a first echo test.
- **Configuring a unit?** The
  [Configuration Reference](Configuration-Reference.md) covers every
  `ENV:sana2loop/unitN.config` key.
- **Testing against a flaky link?** See
  [Fault Injection](Fault-Injection.md).
- **Replaying or capturing traffic?** See
  [Replay and Record](Replay-and-Record.md).

## Documentation

| Page | What it covers |
|------|-----------------|
| [Installation](Installation.md) | Requirements, copying the device and tools into place |
| [Getting Started](Getting-Started.md) | First `OpenDevice()`, going online, an echo round-trip |
| [Configuration Reference](Configuration-Reference.md) | Every `unitN.config` key, defaults, and fail-fast rules |
| [Echo and Crossover Modes](Echo-and-Crossover.md) | The two live-traffic modes and how units pair up |
| [Replay and Record](Replay-and-Record.md) | Playing back a pcap capture, and recording real traffic to one |
| [Fault Injection](Fault-Injection.md) | Drop/duplicate/truncate/reorder/oversilent/online-flap knobs |
| [SanaInfo Reference](SanaInfo-Reference.md) | The bundled CLI query/online-toggle tool |
| [SanaDump and SanaSend](SanaDump-and-SanaSend.md) | Capturing and sending traffic through a unit from the Shell |
| [SANA-II Conformance](SANA-II-Conformance.md) | Rev 2/3/4/7 support, and the `SanaConform` probe tool |
| [Troubleshooting and FAQ](Troubleshooting-and-FAQ.md) | Common problems and questions |
| [Building and Testing](Building-and-Testing.md) | Cross-building the device and running the on-target test suite |
| [Changelog](Changelog.md) | What landed in each milestone |

Developer-facing design documents (the full milestone plan, protocol notes,
and the reasoning behind specific implementation choices) live in the
repository under
[`docs/`](https://github.com/sidick/sana2loop/tree/main/docs).

## AI-assisted development

Be aware: **sana2loop was written largely by an AI coding agent**
(Anthropic's Claude, via Claude Code), working under human direction,
review, and on-hardware/on-emulator testing. Every milestone is validated
on-target under the Copperline emulator against both a real Kickstart 1.3
ROM and a bundled, redistributable AROS Kickstart replacement before being
considered done — see [Building and Testing](Building-and-Testing.md). The
entire source is BSD-licensed and open for review.

## License

BSD 2-Clause. Copyright © 2026 Simon Dick.
