# Makefile for sana2loop
#
# Targets:
#   make device  - build build/loopback.device  (freestanding, -nostdlib)
#   make tools   - build build/SanaInfo          (normal clib2 CLI program)
#   make amiga   - both of the above
#   make docker  - run 'make amiga' inside the cross-compiler container
#   make clean
#
# Amiga toolchain: Bebbo m68k-amigaos GCC, same pinned image sana2loop's
# sibling project (narrator.wyoming) uses. Use 'make docker' if you don't
# have the cross-compiler installed locally.
#
# Target floor: 68000 / Kickstart 1.3 for the device itself (see
# docs/sana2-notes.md for why). SanaInfo, a companion Shell tool, may
# require more (V36+ Exec calls) -- see src/sanainfo.c.

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

# --- SanaInfo: a normal CLI program (clib2 runtime, standard startfiles) ---
TOOL_CFLAGS := -O2 -Wall -Wextra -Werror -m68000 -msoft-float -mcrt=clib2
TOOL_BIN    := $(BUILD)/SanaInfo

# --- Copperline on-target smoke test harness (tests/copperline/) ---
# Freestanding like the device itself (see sana2test.c's header for why: a
# normal linked -noixemul program either failed to launch or crashed under
# real Kickstart 1.3 in testing, so this sidesteps the crt entirely).
TEST_CFLAGS := -nostdlib -nostartfiles -O2 -Wall -Wextra -m68000 -msoft-float
TEST_BIN    := $(BUILD)/sana2test

# --- Docker (same pinned image as narrator.wyoming's Bebbo toolchain) ---
IMAGE      ?= ghcr.io/reinauer/container-amiga-gcc@sha256:75eee7a55bcffdde76e0a4f42ca025b89796053807e8f1b05bab9294b640f4e8
DOCKER_RUN := docker run --rm -v "$(CURDIR)":/work -w /work $(IMAGE)

.PHONY: all device tools amiga test-harness docker docker-test-harness copperline-smoke clean version

all: amiga

device: $(DEV_BIN)
tools:  $(TOOL_BIN)
amiga:  $(DEV_BIN) $(TOOL_BIN)
test-harness: $(TEST_BIN)

$(DEV_BIN): $(DEV_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(DEV_CFLAGS) -o $@ $(DEV_SRC)

$(TOOL_BIN): $(SRCDIR)/sanainfo.c
	@mkdir -p $(BUILD)
	$(CC) $(TOOL_CFLAGS) -o $@ $(SRCDIR)/sanainfo.c

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

version:
	@echo "$(VERSION).$(REVISION)"

clean:
	rm -rf $(BUILD)
