# sana2loop

`loopback.device` -- a hardware-free SANA-II device for AmigaOS providing
echo, crossover, and pcap replay/capture modes. No null/dummy SANA-II device
has existed before this; it exists to give any network stack, shim, or
driver-consuming software a deterministic test partner with zero hardware
and zero emulator network support required.

Targets plain 68000 and Kickstart 1.3 -- a test tool's value is
universality, and nothing here needs more.

See [the project proposal](docs/sana2-notes.md) and `CLAUDE.md` for full
design rationale, phased plan, and current status.

## Status

**Phase 1 in progress:** device skeleton, echo mode, `S2_DEVICEQUERY`,
`S2_ONLINE`/`S2_OFFLINE`, and `S2_ONEVENT` are implemented; not yet
validated on real hardware/emulation. Crossover pairs, fault injection, and
pcap replay/record are later phases.

## Build

```
make docker   # cross-compile build/loopback.device + build/SanaInfo
              # (needs Docker; no local toolchain required)
```

See the Makefile for building with a locally-installed
`m68k-amigaos-gcc` instead.

## License

BSD 2-Clause (see `LICENSE`) -- deliberately permissive so the device and
its companion tools are easy to vendor into other projects.
