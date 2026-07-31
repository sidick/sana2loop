# Makefile for sana2loop
#
# Targets:
#   make device  - build build/loopback.device  (freestanding, -nostdlib)
#   make tools   - build SanaInfo/SanaDump/SanaSend (normal clib2 programs)
#   make amiga   - both of the above
#   make docker  - run 'make amiga' inside the cross-compiler container
#   make dist    - package build/dist/sana2loop.{lha,readme} for Aminet
#                  (needs prebuilt m68k binaries: make amiga / make docker)
#   make clean
#
# Amiga toolchain: Bebbo m68k-amigaos GCC, same pinned image sana2loop's
# sibling project (narrator.wyoming) uses. Use 'make docker' if you don't
# have the cross-compiler installed locally.
#
# Target floor: 68000 / Kickstart 1.3 for the device itself (see
# docs/sana2-notes.md for why). The companion Shell tools may require more
# (V36+ Exec calls) -- see src/sanainfo.c, src/tools/*.c.

SRCDIR := src
BUILD  := build

include version.mk

CC := m68k-amigaos-gcc

# --- Device build: hand-rolled Resident/auto-init, no libnix devinit.o ---
# (see the comment at the top of src/loopback_device.c for why). -m68000:
# the actual target floor, not just a default -- this device must run on
# plain 68000/KS1.3. No FPU on that target either way, so -msoft-float.
DEV_CFLAGS := -nostdlib -nostartfiles -O2 -Wall -Wextra -Werror \
              -Wno-unused-parameter -fomit-frame-pointer -m68000 -msoft-float \
              -DDEVICE_VERSION=$(VERSION) -DDEVICE_REVISION=$(REVISION)
DEV_SRC    := $(SRCDIR)/loopback_device.c
DEV_BIN    := $(BUILD)/loopback.device

# --- SanaInfo/SanaDump/SanaSend/SanaConform: normal CLI programs (clib2) ---
TOOL_CFLAGS := -O2 -Wall -Wextra -Werror -m68000 -msoft-float -mcrt=clib2
TOOL_BIN       := $(BUILD)/SanaInfo
SANADUMP_BIN   := $(BUILD)/SanaDump
SANASEND_BIN   := $(BUILD)/SanaSend
SANACONFORM_BIN := $(BUILD)/SanaConform
# pcapio.c/sana2open.c are shared building blocks (src/tools/*.h), not
# separate binaries -- SanaDump/SanaSend each compile+link them directly
# alongside their own .c file in one gcc invocation (no intermediate .o
# management needed for a project this size).
TOOLS_SRCDIR := $(SRCDIR)/tools
TOOLS_COMMON := $(TOOLS_SRCDIR)/pcapio.c $(TOOLS_SRCDIR)/sana2open.c

# --- Copperline on-target smoke test harness (tests/copperline/) ---
# Freestanding like the device itself (see sana2test.c's header for why: a
# normal linked -noixemul program either failed to launch or crashed under
# real Kickstart 1.3 in testing, so this sidesteps the crt entirely).
TEST_CFLAGS := -nostdlib -nostartfiles -O2 -Wall -Wextra -m68000 -msoft-float
TEST_BIN    := $(BUILD)/sana2test

# --- Docker (same pinned image as narrator.wyoming's Bebbo toolchain) ---
IMAGE      ?= ghcr.io/reinauer/container-amiga-gcc@sha256:75eee7a55bcffdde76e0a4f42ca025b89796053807e8f1b05bab9294b640f4e8
# --user matches the container process's UID/GID to the host caller's, so
# every file `make amiga` creates under build/ (bind-mounted, not a
# container-private volume) is host-owned, not root-owned. Without this,
# a Linux Docker host (unlike Docker Desktop on macOS, which already maps
# bind-mount ownership to the host user regardless of --user) runs the
# container as root by default, leaving build/ root-owned on the host --
# release.yml's later `make dist` step, run natively as the `runner`
# user, then can't write into that same build/ directory (mkdir/cp under
# it fails with EACCES). Verified locally (macOS/arm64, under QEMU
# emulation for this amd64 image) that --user doesn't break the build
# itself; the actual root-vs-host-user distinction this fixes is a
# Linux-specific Docker behavior this platform can't fully reproduce, so
# the real proof is release.yml's own CI run succeeding end to end.
DOCKER_RUN := docker run --rm --user "$$(id -u):$$(id -g)" -v "$(CURDIR)":/work -w /work $(IMAGE)

.PHONY: all device tools amiga test-harness docker docker-test-harness copperline-smoke copperline-smoke-tools guide dist clean version

all: amiga

device: $(DEV_BIN)
tools:  $(TOOL_BIN) $(SANADUMP_BIN) $(SANASEND_BIN) $(SANACONFORM_BIN)
amiga:  $(DEV_BIN) $(TOOL_BIN) $(SANADUMP_BIN) $(SANASEND_BIN) $(SANACONFORM_BIN)
test-harness: $(TEST_BIN)

# version.mk is a prerequisite of every binary below (not just `include`d
# above): VERSION/REVISION feed each one's $VER string via -D, so a local
# version bump without this would let an incremental `make dist` package
# binaries still carrying the OLD $VER (release CI always builds from a
# clean checkout, so this is a local-only hazard -- release.yml's own
# $VER grep would still catch it there).
$(DEV_BIN): $(DEV_SRC) src/pcap.h version.mk
	@mkdir -p $(BUILD)
	$(CC) $(DEV_CFLAGS) -o $@ $(DEV_SRC)

$(TOOL_BIN): $(SRCDIR)/sanainfo.c version.mk
	@mkdir -p $(BUILD)
	$(CC) $(TOOL_CFLAGS) -DSANAINFO_VERSION=$(VERSION) -DSANAINFO_REVISION=$(REVISION) \
	      -o $@ $(SRCDIR)/sanainfo.c

$(SANADUMP_BIN): $(TOOLS_SRCDIR)/sanadump.c $(TOOLS_COMMON) $(TOOLS_SRCDIR)/pcapio.h $(TOOLS_SRCDIR)/sana2open.h version.mk
	@mkdir -p $(BUILD)
	$(CC) $(TOOL_CFLAGS) -DSANADUMP_VERSION=$(VERSION) -DSANADUMP_REVISION=$(REVISION) \
	      -o $@ $(TOOLS_SRCDIR)/sanadump.c $(TOOLS_COMMON)

$(SANASEND_BIN): $(TOOLS_SRCDIR)/sanasend.c $(TOOLS_COMMON) $(TOOLS_SRCDIR)/pcapio.h $(TOOLS_SRCDIR)/sana2open.h version.mk
	@mkdir -p $(BUILD)
	$(CC) $(TOOL_CFLAGS) -DSANASEND_VERSION=$(VERSION) -DSANASEND_REVISION=$(REVISION) \
	      -o $@ $(TOOLS_SRCDIR)/sanasend.c $(TOOLS_COMMON)

# SanaConform doesn't need TOOLS_COMMON (pcapio.c/sana2open.c) -- it does
# its own manual OpenDevice() with a richer Rev 2/3/7 tag list than
# sana2open.c's fixed 3-tag helper carries (see sanaconform.c's own
# header comment), and never touches pcap files at all.
$(SANACONFORM_BIN): $(TOOLS_SRCDIR)/sanaconform.c version.mk
	@mkdir -p $(BUILD)
	$(CC) $(TOOL_CFLAGS) -DSANACONFORM_VERSION=$(VERSION) -DSANACONFORM_REVISION=$(REVISION) \
	      -o $@ $(TOOLS_SRCDIR)/sanaconform.c

$(TEST_BIN): tests/copperline/sana2test.c
	@mkdir -p $(BUILD)
	$(CC) $(TEST_CFLAGS) -o $@ tests/copperline/sana2test.c

docker:
	$(DOCKER_RUN) make amiga

docker-test-harness:
	$(DOCKER_RUN) make test-harness

# Needs COPPERLINE= pointing at a build with the 68000 filesys-doorbell fix
# (see tests/copperline/run.sh); KICK= optionally points at a real Kickstart.
copperline-smoke: docker docker-test-harness
	sh tests/copperline/run.sh

# SanaDump/SanaSend/SanaConform on-target round-trip, in its OWN separate
# Copperline boot session -- see tests/copperline/run-tools.sh's own
# comment for why (short version: the device's own copperline-smoke run
# above leaves too little memory for a second clib2 program in the same
# session). Only needs the built binaries (`docker`), not the freestanding
# test harness `copperline-smoke` also builds. Previously had no Makefile
# target at all, making it easy to skip in the documented local
# pre-release flow (`sh tests/copperline/run-tools.sh` by hand still
# works too). KICK= optionally points at a real Kickstart, same as above.
copperline-smoke-tools: docker
	sh tests/copperline/run-tools.sh

version:
	@echo "$(VERSION).$(REVISION)"

# --- guide: AmigaGuide user documentation, generated from userdocs/ --------
# userdocs/ is the single source of truth for user docs (built as the
# MkDocs site, see mkdocs.yml); this converts it for on-Amiga reading
# (MultiView/AmigaGuide).
guide: | $(BUILD)
	python3 tools/docs2guide.py userdocs $(BUILD)/sana2loop.guide

$(BUILD):
	@mkdir -p $(BUILD)

# --- lha: build the real LHa for UNIX (archive-capable), pinned ------------
# Homebrew's and Ubuntu's `lha` is Lhasa -- extract-only, useless for
# packaging -- and the last lha *release* tag (2021) no longer compiles with
# modern compilers, so build a pinned master commit from source into
# build/tools/. Needs git + autoconf/automake. Override with a known-good
# archiver: make dist LHA=/path/to/real/lha
# Same pinned commit as sibling project amiauth's own dist target.
LHA_REPO   := https://github.com/jca02266/lha.git
LHA_COMMIT := 86094cb56aba34de45668f39f74fcfb61e9d7fb6
LHA        ?= $(BUILD)/tools/lha

$(BUILD)/tools/lha:
	@mkdir -p $(BUILD)/tools
	rm -rf $(BUILD)/tools/lha-src
	git clone -q $(LHA_REPO) $(BUILD)/tools/lha-src
	cd $(BUILD)/tools/lha-src && \
		git -c advice.detachedHead=false checkout -q $(LHA_COMMIT) && \
		autoreconf -fi >/dev/null 2>&1 && ./configure >/dev/null && \
		$(MAKE) >/dev/null
	cp $(BUILD)/tools/lha-src/src/lha $(BUILD)/tools/lha
	rm -rf $(BUILD)/tools/lha-src

# --- dist: assemble the Aminet upload pair (archive + .readme) -------------
# Expects prebuilt m68k binaries (make amiga / make docker); the lha
# archiver is built automatically (above). Produces build/dist/sana2loop.lha
# (drawer with device+tools+guide+license) and build/dist/sana2loop.readme
# alongside it -- the two files Aminet wants.
dist: guide $(LHA)
	@test -f $(DEV_BIN) -a -f $(TOOL_BIN) -a -f $(SANADUMP_BIN) -a -f $(SANASEND_BIN) -a -f $(SANACONFORM_BIN) || \
		{ echo "dist: missing m68k binaries; run: make amiga (or make docker)"; exit 1; }
	rm -rf $(BUILD)/dist
	mkdir -p $(BUILD)/dist/sana2loop
	cp $(DEV_BIN) $(TOOL_BIN) $(SANADUMP_BIN) $(SANASEND_BIN) $(SANACONFORM_BIN) $(BUILD)/sana2loop.guide \
		LICENSE sana2loop.readme $(BUILD)/dist/sana2loop/
	cp sana2loop.readme $(BUILD)/dist/
	cd $(BUILD)/dist && $(abspath $(LHA)) aq sana2loop.lha sana2loop
	@ls -l $(BUILD)/dist/sana2loop.lha $(BUILD)/dist/sana2loop.readme

clean:
	rm -rf $(BUILD)
