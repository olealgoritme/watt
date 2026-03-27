KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
CC   ?= gcc

POWMON_H := kernel/include/powmon.h
FLUX_H   := lib/flux.h/flux.h

.PHONY: all watt module tools clean install load unload reload submodules

all: watt module tools
	@echo ""
	@echo "  Build complete:"
	@echo "    watt          → watt"
	@echo "    kernel module → kernel/powmon.ko"
	@echo "    tools         → tools/powmon-cli, tools/powmon-top"
	@echo ""
	@echo "  Quick start:"
	@echo "    sudo insmod kernel/powmon.ko track_all=1"
	@echo "    sudo watt"

# ── dependencies ─────────────────────────────────────────────
submodules: $(FLUX_H)
$(FLUX_H):
	git submodule update --init --recursive

# ── watt (main app) ──────────────────────────────────────────
watt: src/watt.c $(FLUX_H) $(POWMON_H)
	$(CC) -Wall -O2 -std=c99 -I lib/flux.h -I kernel/include -o $@ $< -lpthread

# ── kernel module ────────────────────────────────────────────
module:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel modules

install:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel modules_install
	depmod -a

# ── tools ────────────────────────────────────────────────────
tools: tools/powmon-cli tools/powmon-top

tools/powmon-cli: tools/powmon-cli.c $(POWMON_H)
	$(CC) -Wall -O2 -I kernel/include -o $@ $<

tools/powmon-top: tools/powmon-top.c $(POWMON_H)
	$(CC) -Wall -O2 -I kernel/include -o $@ $< -lncurses

# ── clean ────────────────────────────────────────────────────
clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel clean
	rm -f watt tools/powmon-cli tools/powmon-top

# ── load/unload helpers ─────────────────────────────────────
load: module
	sudo insmod kernel/powmon.ko $(MODPARAMS)

unload:
	sudo rmmod powmon || true

reload: unload load
