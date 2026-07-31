#!/bin/sh
# run.sh -- headless Copperline on-target smoke test for loopback.device.
#
# Builds a minimal bootable ADF (xdftool) with just DEVS:loopback.device,
# C:sana2test, and a one-line Startup-Sequence, then boots it. sana2test
# opens DEVS:loopback.device, exercises S2_DEVICEQUERY/S2_ONLINE/echo, and
# emits PASS/FAIL over serial via RawPutChar. Copperline forwards serial to
# its stdout (`--serial stdout`); we assert the result.
#
# Two modes:
#   KICK=path/to/real-1.3.rom  -- validates actual Kickstart 1.3 fidelity,
#                                  the point of this project. Use this
#                                  locally before releases; the ROM is
#                                  licensed and can't live in public CI
#                                  (see CLAUDE.md's "Testing under
#                                  Copperline" section).
#   KICK unset                 -- boots Copperline's bundled, redistributable
#                                  AROS Kickstart replacement instead. Weaker
#                                  (can't validate 1.3-specific behavior) but
#                                  needs no licensed asset, so this is what
#                                  public CI runs. AROS boots slower, hence
#                                  the bigger default BENCH below (real 1.3's
#                                  own dos.library file I/O -- M5's config
#                                  read -- is measurably SLOWER than AROS's,
#                                  which is why the KICK= default needed
#                                  raising too once that landed).
#
# A plain released `copperline` (e.g. `brew install copperline`, or the
# AppImage in CI) is sufficient for both modes -- this test only ever boots
# from a floppy image, never a [[filesys]] host-directory mount, so it does
# not hit the real 68000+[[filesys]] doorbell bug documented in CLAUDE.md
# (confirmed empirically: released 0.13.0 passes both modes here).
#
# Prereqs:
#   - xdftool on PATH (amitools: `pip install amitools`, or
#     XDFTOOL=/path/to/xdftool)
#   - copperline on PATH, or COPPERLINE=/path/to/copperline
#   - the cross-built device + harness: SANA2LOOP_DEVICE= / SANA2TEST_M68K=
#     (defaults: ../../build/loopback.device, ../../build/sana2test)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)

COPPERLINE=${COPPERLINE:-copperline}
KICK=${KICK:-}
XDFTOOL=${XDFTOOL:-xdftool}
DEVICE=${SANA2LOOP_DEVICE:-$ROOT/build/loopback.device}
BIN=${SANA2TEST_M68K:-$ROOT/build/sana2test}
BENCH=${BENCH:-$([ -n "$KICK" ] && echo 30 || echo 40)}
ADF="$HERE/minimal-boot.adf"

[ -e "$DEVICE" ] || { echo "FAIL: missing $DEVICE (run make device)" >&2; exit 2; }
[ -e "$BIN" ] || { echo "FAIL: missing $BIN (run make test-harness)" >&2; exit 2; }
[ -z "$KICK" ] || [ -e "$KICK" ] || { echo "FAIL: KICK set but missing: $KICK" >&2; exit 2; }
command -v "$COPPERLINE" >/dev/null || { echo "FAIL: $COPPERLINE not found" >&2; exit 2; }
command -v "$XDFTOOL" >/dev/null || { echo "FAIL: $XDFTOOL not found (pip install amitools)" >&2; exit 2; }
[ -n "$KICK" ] && echo "ROM: $KICK" || echo "ROM: bundled AROS"

rm -f "$ADF"
"$XDFTOOL" "$ADF" format 'S2LTest' ofs + \
    boot install boot1x + \
    makedir c + makedir s + makedir devs + makedir s/sana2loop + \
    write "$DEVICE" devs/loopback.device + \
    write "$BIN" c/sana2test + \
    write "$HERE/adf-startup-sequence.txt" s/startup-sequence \
    > /dev/null

# Per-unit config fixtures: one xdftool invocation per file, not batched
# into the command above, since the fixture set has grown milestone over
# milestone (M5/M6/M7) and a plain glob keeps this from needing an edit
# every time a new one is added.
for f in "$HERE"/fixtures/unit*.config; do
    n=$(basename "$f" .config)
    "$XDFTOOL" "$ADF" write "$f" "s/sana2loop/$n.config" > /dev/null
done

# M8/M9: unit 9/10's mode=replay configs name these pcap captures via
# S:sana2loop/*.pcap -- not unit*.config-shaped names, so they don't
# match the glob above and need their own writes. (M14's own
# replaycap.pcap -- units 14/15's config -- is NOT written here: those
# units are never opened by run.sh's own harness at all, only by
# run-tools.sh's; see verify_replaycap.py's own header comment for why.)
"$XDFTOOL" "$ADF" write "$HERE/fixtures/replay.pcap" "s/sana2loop/replay.pcap" > /dev/null
"$XDFTOOL" "$ADF" write "$HERE/fixtures/scheduled.pcap" "s/sana2loop/scheduled.pcap" > /dev/null

OUT=$(mktemp)
cleanup() { rm -f "$OUT"; }
trap cleanup EXIT INT TERM

set -- --config machine.toml --noaudio --serial stdout --benchmark-until "$BENCH"
[ -n "$KICK" ] && set -- "$@" "$KICK"
( cd "$HERE" && "$COPPERLINE" "$@" ) >"$OUT" 2>/dev/null \
    || { echo "FAIL: $COPPERLINE exited non-zero" >&2; cat "$OUT" >&2; exit 3; }

tr -d '\r' <"$OUT" >"$OUT.n" && mv "$OUT.n" "$OUT"
echo "----- serial capture -----"; cat "$OUT"; echo "--------------------------"
grep -q '^END' "$OUT" 2>/dev/null || { echo "FAIL: no END marker (raise BENCH?)" >&2; exit 1; }

# Every named sub-test from sana2test.c must show PASS individually -- not
# just the aggregate RESULT= -- so a regression in one milestone's check
# names itself instead of being buried in one pass/fail bit. Keep this list
# in sync with the NAME_* constants in sana2test.c as milestones add more.
SUBTESTS="online devicequery stationaddress echo offline onevent oneventnow abortio cmdflush bufferhooks delivery configinterface tracktype broadcast multicast multicastaddress raw config crossover faultdrop faultdup faultreorder faultoversilent faultflap replay scheduled record rawmtu peeraddress dnsaddress buffhooks dmahooks replaycap"
fails=0
for s in $SUBTESTS; do
    grep -q "^SUB=${s}=PASS$" "$OUT" 2>/dev/null || {
        echo "FAIL: SUB=${s} did not report PASS" >&2
        fails=$((fails + 1))
    }
done
grep -q '^RESULT=PASS' "$OUT" 2>/dev/null || { echo "FAIL: RESULT=PASS not found" >&2; fails=$((fails + 1)); }

# M9: the "record" sub-test above only proves unit 11's 3 writes
# succeeded from the guest's own point of view -- what actually ended up
# written to the host-side ADF is inherently a host-side question, so
# extract it via xdftool and byte-compare with verify_record.py. Depends
# on Copperline actually persisting floppy writes back to the image file
# (confirmed empirically -- see docs/PLAN.md's M9 risk note) and on
# machine.toml's write_protected=false (floppies default write-protected).
RECORDED=$(mktemp)
if "$XDFTOOL" "$ADF" type s/sana2loop/recorded.pcap > "$RECORDED" 2>/dev/null; then
    python3 "$HERE/verify_record.py" "$RECORDED" || fails=$((fails + 1))
else
    echo "FAIL: s/sana2loop/recorded.pcap not found on the ADF post-run" >&2
    fails=$((fails + 1))
fi
rm -f "$RECORDED"

[ "$fails" -eq 0 ] || exit 1

echo "PASS: loopback.device on-target smoke test (via serial)"
