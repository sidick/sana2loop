# Building and Testing

This page is for building sana2loop from source and running its on-target
test suite — not needed if you're just using a released `loopback.device`
binary.

## Cross-building

The Amiga build uses the Bebbo `m68k-amigaos-gcc` cross-toolchain.

```
make docker   # cross-compiles build/loopback.device + build/SanaInfo
              # inside a pinned Docker image -- no local toolchain needed
```

Or, with a local `m68k-amigaos-gcc` on `PATH`:

```
make amiga    # same, using your local toolchain
```

`make device` / `make tools` build just one binary or the other.

## On-target testing under Copperline

sana2loop's test suite runs the actual compiled `loopback.device` under
the [Copperline](https://github.com/CopperlineHQ/Copperline) Amiga
emulator (not Amiberry — Copperline's headless serial-output mode and
breakpoint/gdb support are a better fit for this project's automated
testing) against a minimal, purpose-built bootable floppy image:

```
make copperline-smoke                          # bundled AROS Kickstart
KICK=/path/to/kick13.rom make copperline-smoke  # real Kickstart 1.3
```

Needs `xdftool` (from `amitools`: `pip install amitools`) and a released
`copperline` binary on `PATH` (or `COPPERLINE=/path/to/copperline`).

**Prefer the real Kickstart 1.3 run locally before trusting a release.**
The bundled AROS Kickstart replacement is what public CI runs (a real 1.3
ROM is Cloanto/Hyperion-licensed and can't live in a public repository or
its CI) — it's a fine ROM-free stand-in for catching gross regressions, but
it cannot validate this project's actual Kickstart-1.3-fidelity claims, and
has at least one real completion-detection quirk of its own that real
Kickstart 1.3 doesn't share (see the [Changelog](Changelog.md)'s
tick-scheduled-replay entry for a specific, resolved example). Every
milestone in this project's history was validated against both targets
before being considered done.

## Building the docs

The user documentation you're reading is built with
[MkDocs Material](https://squidfunk.github.io/mkdocs-material/) from
`userdocs/`:

```
pip install -r tools/docs-requirements.txt
mkdocs serve           # live-reloading local preview
mkdocs build --strict  # what CI runs
```

An on-Amiga AmigaGuide version of the same content is generated from the
same source:

```
make guide   # writes build/sana2loop.guide
```

so there's exactly one place to edit this content, not two copies to keep
in sync by hand.
