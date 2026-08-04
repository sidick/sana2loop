# version.mk -- the one place the sana2loop version lives.
#
# Unlike narrator.wyoming (which had to exceed stock narrator/translator
# versions to be unambiguous), loopback.device has no prior version to beat,
# so this started at 1.0 (released 2026-07-31). VERSION is the Amiga major
# (what OpenDevice-style compatibility checks would read); REVISION is the
# minor -- bumped here (1.0 -> 1.1) for M15-M19: five milestones of
# additive, backward-compatible config keys/tools functionality (wire=/
# bps=/mintu=, addr=/errors=/loss=, delay=/deviation=, multi-open fan-out +
# real S2_PacketFilter dispatch + SANA2OPF_MINE, and DEVICE=/CONFIG/ONLINE
# on all four companion tools) -- nothing that changed or removed existing
# behavior, so a REVISION bump, not VERSION.
VERSION  := 1
REVISION := 1
