KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
CC   ?= gcc

VERSION  := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
POWMON_H := kernel/include/powmon.h
FLUX_H   := lib/flux.h/flux.h

.PHONY: all watt module tools clean install uninstall load unload reload submodules \
        package-deb package-tgz

all: watt module tools
	@echo ""
	@echo "  Build complete (v$(VERSION)):"
	@echo "    watt          → watt"
	@echo "    kernel module → kernel/powmon.ko"
	@echo "    tools         → tools/powmon-cli, tools/powmon-top"
	@echo ""
	@echo "  Quick start:"
	@echo "    sudo insmod kernel/powmon.ko track_all=1"
	@echo "    sudo ./watt"
	@echo ""
	@echo "  Install globally:"
	@echo "    sudo make install"

# ── dependencies ─────────────────────────────────────────────
submodules: $(FLUX_H)
$(FLUX_H):
	git submodule update --init --recursive

# ── watt (main app) ──────────────────────────────────────────
watt: src/watt.c $(FLUX_H) $(POWMON_H)
	$(CC) -Wall -O2 -std=c99 -DWATT_VERSION=\"$(VERSION)\" \
	    -I lib/flux.h -I kernel/include -o $@ $< -lpthread

# ── kernel module ────────────────────────────────────────────
module:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel modules

install: all
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel modules_install
	depmod -a
	install -Dm755 watt $(DESTDIR)/usr/local/bin/watt
	@echo ""
	@echo "  Installed:"
	@echo "    watt          → $(DESTDIR)/usr/local/bin/watt"
	@echo "    powmon.ko     → /lib/modules/$(shell uname -r)/"
	@echo ""

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/watt

# ── tools ────────────────────────────────────────────────────
tools: tools/powmon-cli tools/powmon-top

tools/powmon-cli: tools/powmon-cli.c $(POWMON_H)
	$(CC) -Wall -O2 -DWATT_VERSION=\"$(VERSION)\" -I kernel/include -o $@ $<

tools/powmon-top: tools/powmon-top.c $(POWMON_H)
	$(CC) -Wall -O2 -I kernel/include -o $@ $< -lncurses

# ── clean ────────────────────────────────────────────────────
clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel clean
	rm -f watt tools/powmon-cli tools/powmon-top
	rm -rf build/

# ── load/unload helpers ─────────────────────────────────────
load: module
	sudo insmod kernel/powmon.ko $(MODPARAMS)

unload:
	sudo rmmod powmon || true

reload: unload load

# ── packaging: .tar.gz ───────────────────────────────────────
package-tgz: clean
	@mkdir -p build
	git archive --prefix=watt-$(VERSION)/ -o build/watt-$(VERSION).tar HEAD
	cd lib/flux.h && git archive --prefix=watt-$(VERSION)/lib/flux.h/ -o /tmp/_flux.tar HEAD
	tar --concatenate --file=build/watt-$(VERSION).tar /tmp/_flux.tar
	rm -f /tmp/_flux.tar
	gzip build/watt-$(VERSION).tar
	@echo ""
	@echo "  Created: build/watt-$(VERSION).tar.gz"

# ── packaging: .deb ──────────────────────────────────────────
package-deb: watt tools
	@mkdir -p build/deb/DEBIAN
	@mkdir -p build/deb/usr/bin
	@mkdir -p build/deb/usr/share/applications
	@mkdir -p build/deb/usr/share/icons/hicolor/scalable/apps
	@mkdir -p build/deb/usr/share/doc/watt
	@mkdir -p build/deb/usr/src/powmon-$(VERSION)

	@# control file
	@echo "Package: watt" > build/deb/DEBIAN/control
	@echo "Version: $(VERSION)" >> build/deb/DEBIAN/control
	@echo "Section: utils" >> build/deb/DEBIAN/control
	@echo "Priority: optional" >> build/deb/DEBIAN/control
	@echo "Architecture: $(shell dpkg --print-architecture)" >> build/deb/DEBIAN/control
	@echo "Depends: dkms, linux-headers-generic | linux-headers-$(shell uname -r)" >> build/deb/DEBIAN/control
	@echo "Maintainer: olealgoritme" >> build/deb/DEBIAN/control
	@echo "Description: Per-process power monitoring TUI for Linux" >> build/deb/DEBIAN/control
	@echo " Reads RAPL MSRs via a custom kernel module and shows" >> build/deb/DEBIAN/control
	@echo " real-time wattage per process, core, and package." >> build/deb/DEBIAN/control
	@echo " Supports Intel (Sandy Bridge+) and AMD (Zen+)." >> build/deb/DEBIAN/control

	@# postinst: register DKMS module
	@echo '#!/bin/sh' > build/deb/DEBIAN/postinst
	@echo 'set -e' >> build/deb/DEBIAN/postinst
	@echo 'dkms add powmon/$(VERSION) || true' >> build/deb/DEBIAN/postinst
	@echo 'dkms build powmon/$(VERSION) || true' >> build/deb/DEBIAN/postinst
	@echo 'dkms install powmon/$(VERSION) || true' >> build/deb/DEBIAN/postinst
	@chmod 755 build/deb/DEBIAN/postinst

	@# prerm: unregister DKMS module
	@echo '#!/bin/sh' > build/deb/DEBIAN/prerm
	@echo 'set -e' >> build/deb/DEBIAN/prerm
	@echo 'dkms remove powmon/$(VERSION) --all || true' >> build/deb/DEBIAN/prerm
	@chmod 755 build/deb/DEBIAN/prerm

	@# binaries
	install -m755 watt build/deb/usr/bin/watt
	install -m755 tools/powmon-cli build/deb/usr/bin/powmon-cli
	install -m755 tools/powmon-diag.sh build/deb/usr/bin/powmon-diag

	@# desktop + icons
	install -m644 watt.desktop build/deb/usr/share/applications/watt.desktop
	install -m644 logo.svg build/deb/usr/share/icons/hicolor/scalable/apps/watt.svg
	@for size in 16 32 48 64 128 256; do \
	    mkdir -p build/deb/usr/share/icons/hicolor/$${size}x$${size}/apps; \
	    rsvg-convert -w $$size -h $$size logo.svg \
	        -o build/deb/usr/share/icons/hicolor/$${size}x$${size}/apps/watt.png; \
	done

	@# docs
	install -m644 README.md build/deb/usr/share/doc/watt/README.md

	@# DKMS kernel module source
	cp -r kernel/ build/deb/usr/src/powmon-$(VERSION)/kernel/
	cp Makefile build/deb/usr/src/powmon-$(VERSION)/Makefile
	cp VERSION build/deb/usr/src/powmon-$(VERSION)/VERSION

	@# dkms.conf
	@echo 'PACKAGE_NAME="powmon"' > build/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'PACKAGE_VERSION="$(VERSION)"' >> build/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'BUILT_MODULE_NAME[0]="powmon"' >> build/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'BUILT_MODULE_LOCATION[0]="kernel/"' >> build/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'DEST_MODULE_LOCATION[0]="/extra"' >> build/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'MAKE[0]="make -C $${kernel_source_dir} M=$${dkms_tree}/powmon/$(VERSION)/build/kernel modules"' >> build/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'CLEAN="make -C $${kernel_source_dir} M=$${dkms_tree}/powmon/$(VERSION)/build/kernel clean"' >> build/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'AUTOINSTALL="yes"' >> build/deb/usr/src/powmon-$(VERSION)/dkms.conf

	dpkg-deb --build build/deb build/watt_$(VERSION)_$(shell dpkg --print-architecture).deb
	@echo ""
	@echo "  Created: build/watt_$(VERSION)_$(shell dpkg --print-architecture).deb"
