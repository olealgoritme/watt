KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
CC   ?= gcc

VERSION  := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
POWMON_H := kernel/include/powmon.h
FLUX_H   := lib/flux.h/flux.h
BUILD    := build

.PHONY: all watt module tools clean install uninstall load unload reload submodules \
        package-deb package-tgz

all: watt module tools
	@echo ""
	@echo "  Build complete (v$(VERSION)):"
	@echo "    watt          → $(BUILD)/watt"
	@echo "    kernel module → kernel/powmon.ko"
	@echo "    tools         → $(BUILD)/powmon-cli, $(BUILD)/powmon-top"
	@echo ""
	@echo "  Quick start:"
	@echo "    sudo insmod kernel/powmon.ko track_all=1"
	@echo "    sudo $(BUILD)/watt"
	@echo ""
	@echo "  Install:"
	@echo "    sudo make install           # install to /usr/local/bin"
	@echo "    make package-deb            # build .deb with DKMS"
	@echo "    make package-tgz            # build source tarball"

# ── dependencies ─────────────────────────────────────────────
submodules: $(FLUX_H)
$(FLUX_H):
	git submodule update --init --recursive

# ── watt (main app) ──────────────────────────────────────────
watt: src/watt.c $(FLUX_H) $(POWMON_H)
	@mkdir -p $(BUILD)
	$(CC) -Wall -O2 -std=c99 -DWATT_VERSION=\"$(VERSION)\" \
	    -I lib/flux.h -I kernel/include -o $(BUILD)/watt $< -lpthread

# ── kernel module ────────────────────────────────────────────
module:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel modules

install: all
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel modules_install
	depmod -a
	install -Dm755 $(BUILD)/watt $(DESTDIR)/usr/local/bin/watt
	@echo ""
	@echo "  Installed:"
	@echo "    watt          → $(DESTDIR)/usr/local/bin/watt"
	@echo "    powmon.ko     → /lib/modules/$(shell uname -r)/"
	@echo ""

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/watt

# ── tools ────────────────────────────────────────────────────
tools: $(BUILD)/powmon-cli $(BUILD)/powmon-top

$(BUILD)/powmon-cli: tools/powmon-cli.c $(POWMON_H)
	@mkdir -p $(BUILD)
	$(CC) -Wall -O2 -DWATT_VERSION=\"$(VERSION)\" -I kernel/include -o $@ $<

$(BUILD)/powmon-top: tools/powmon-top.c $(POWMON_H)
	@mkdir -p $(BUILD)
	$(CC) -Wall -O2 -I kernel/include -o $@ $< -lncurses

# ── clean ────────────────────────────────────────────────────
clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/kernel clean
	rm -rf $(BUILD)/

# ── load/unload helpers ─────────────────────────────────────
load: module
	sudo insmod kernel/powmon.ko $(MODPARAMS)

unload:
	sudo rmmod powmon || true

reload: unload load

# ── packaging: .tar.gz ───────────────────────────────────────
package-tgz:
	@mkdir -p $(BUILD)
	git archive --prefix=watt-$(VERSION)/ -o $(BUILD)/watt-$(VERSION).tar HEAD
	cd lib/flux.h && git archive --prefix=watt-$(VERSION)/lib/flux.h/ -o /tmp/_flux.tar HEAD
	tar --concatenate --file=$(BUILD)/watt-$(VERSION).tar /tmp/_flux.tar
	rm -f /tmp/_flux.tar
	gzip -f $(BUILD)/watt-$(VERSION).tar
	@echo ""
	@echo "  Created: $(BUILD)/watt-$(VERSION).tar.gz"

# ── packaging: .deb ──────────────────────────────────────────
package-deb: watt tools
	@mkdir -p $(BUILD)/deb/DEBIAN
	@mkdir -p $(BUILD)/deb/usr/bin
	@mkdir -p $(BUILD)/deb/usr/share/applications
	@mkdir -p $(BUILD)/deb/usr/share/icons/hicolor/scalable/apps
	@mkdir -p $(BUILD)/deb/usr/share/doc/watt
	@mkdir -p $(BUILD)/deb/usr/src/powmon-$(VERSION)

	@# control file
	@echo "Package: watt" > $(BUILD)/deb/DEBIAN/control
	@echo "Version: $(VERSION)" >> $(BUILD)/deb/DEBIAN/control
	@echo "Section: utils" >> $(BUILD)/deb/DEBIAN/control
	@echo "Priority: optional" >> $(BUILD)/deb/DEBIAN/control
	@echo "Architecture: $(shell dpkg --print-architecture)" >> $(BUILD)/deb/DEBIAN/control
	@echo "Depends: dkms, linux-headers-generic | linux-headers-$(shell uname -r)" >> $(BUILD)/deb/DEBIAN/control
	@echo "Maintainer: olealgoritme" >> $(BUILD)/deb/DEBIAN/control
	@echo "Description: Per-process power monitoring TUI for Linux" >> $(BUILD)/deb/DEBIAN/control
	@echo " Reads RAPL MSRs via a custom kernel module and shows" >> $(BUILD)/deb/DEBIAN/control
	@echo " real-time wattage per process, core, and package." >> $(BUILD)/deb/DEBIAN/control
	@echo " Supports Intel (Sandy Bridge+) and AMD (Zen+)." >> $(BUILD)/deb/DEBIAN/control

	@# postinst: register DKMS module
	@echo '#!/bin/sh' > $(BUILD)/deb/DEBIAN/postinst
	@echo 'set -e' >> $(BUILD)/deb/DEBIAN/postinst
	@echo 'dkms add powmon/$(VERSION) || true' >> $(BUILD)/deb/DEBIAN/postinst
	@echo 'dkms build powmon/$(VERSION) || true' >> $(BUILD)/deb/DEBIAN/postinst
	@echo 'dkms install powmon/$(VERSION) || true' >> $(BUILD)/deb/DEBIAN/postinst
	@chmod 755 $(BUILD)/deb/DEBIAN/postinst

	@# prerm: unregister DKMS module
	@echo '#!/bin/sh' > $(BUILD)/deb/DEBIAN/prerm
	@echo 'set -e' >> $(BUILD)/deb/DEBIAN/prerm
	@echo 'dkms remove powmon/$(VERSION) --all || true' >> $(BUILD)/deb/DEBIAN/prerm
	@chmod 755 $(BUILD)/deb/DEBIAN/prerm

	@# binaries
	install -m755 $(BUILD)/watt $(BUILD)/deb/usr/bin/watt
	install -m755 $(BUILD)/powmon-cli $(BUILD)/deb/usr/bin/powmon-cli
	install -m755 tools/powmon-diag.sh $(BUILD)/deb/usr/bin/powmon-diag

	@# desktop + icons
	install -m644 watt.desktop $(BUILD)/deb/usr/share/applications/watt.desktop
	install -m644 logo.svg $(BUILD)/deb/usr/share/icons/hicolor/scalable/apps/watt.svg
	@for size in 16 32 48 64 128 256; do \
	    mkdir -p $(BUILD)/deb/usr/share/icons/hicolor/$${size}x$${size}/apps; \
	    rsvg-convert -w $$size -h $$size logo.svg \
	        -o $(BUILD)/deb/usr/share/icons/hicolor/$${size}x$${size}/apps/watt.png; \
	done

	@# docs
	install -m644 README.md $(BUILD)/deb/usr/share/doc/watt/README.md

	@# DKMS kernel module source
	cp -r kernel/ $(BUILD)/deb/usr/src/powmon-$(VERSION)/kernel/
	cp Makefile $(BUILD)/deb/usr/src/powmon-$(VERSION)/Makefile
	cp VERSION $(BUILD)/deb/usr/src/powmon-$(VERSION)/VERSION

	@# dkms.conf
	@echo 'PACKAGE_NAME="powmon"' > $(BUILD)/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'PACKAGE_VERSION="$(VERSION)"' >> $(BUILD)/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'BUILT_MODULE_NAME[0]="powmon"' >> $(BUILD)/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'BUILT_MODULE_LOCATION[0]="kernel/"' >> $(BUILD)/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'DEST_MODULE_LOCATION[0]="/extra"' >> $(BUILD)/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'MAKE[0]="make -C $${kernel_source_dir} M=$${dkms_tree}/powmon/$(VERSION)/build/kernel modules"' >> $(BUILD)/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'CLEAN="make -C $${kernel_source_dir} M=$${dkms_tree}/powmon/$(VERSION)/build/kernel clean"' >> $(BUILD)/deb/usr/src/powmon-$(VERSION)/dkms.conf
	@echo 'AUTOINSTALL="yes"' >> $(BUILD)/deb/usr/src/powmon-$(VERSION)/dkms.conf

	dpkg-deb --build $(BUILD)/deb $(BUILD)/watt_$(VERSION)_$(shell dpkg --print-architecture).deb
	@echo ""
	@echo "  Created: $(BUILD)/watt_$(VERSION)_$(shell dpkg --print-architecture).deb"
