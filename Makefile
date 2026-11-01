CC ?= cc
CFLAGS ?= -O2 -pipe
CPPFLAGS ?=
LDFLAGS ?=
WARN = -Wall -Wextra -Wformat=2 -Wformat-security -Wshadow -Wpointer-arith -Wvla
GTK_CFLAGS = $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS = $(shell pkg-config --libs gtk+-3.0)
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib/edpkg-gui
DATADIR ?= $(PREFIX)/share

all: edpkg-gui

edpkg-gui: src/edpkg-gui.c src/flatpak-page.c src/debug-log.c src/appimage-page.c src/appimage-page.h src/appimage-data.c src/appimage-data.h src/appimage-i18n.c src/appimage-i18n.h src/flatpak-page.h src/debug-log.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(GTK_CFLAGS) $(LDFLAGS) -o $@ src/edpkg-gui.c src/flatpak-page.c src/appimage-page.c src/appimage-data.c src/appimage-i18n.c src/debug-log.c $(GTK_LIBS) -lutil

strict:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Werror $(GTK_CFLAGS) $(LDFLAGS) -o edpkg-gui.strict src/edpkg-gui.c src/flatpak-page.c src/appimage-page.c src/appimage-data.c src/appimage-i18n.c src/debug-log.c $(GTK_LIBS) -lutil
	rm -f edpkg-gui.strict

install: edpkg-gui
	install -Dm0755 edpkg-gui "$(DESTDIR)$(LIBDIR)/edpkg-gui"
	install -Dm0644 data/edpkg-manager.svg "$(DESTDIR)$(LIBDIR)/edpkg-manager.svg"
	install -Dm0644 data/edpkg-manager.png "$(DESTDIR)$(LIBDIR)/edpkg-manager.png"
	install -Dm0644 data/package-installed.svg "$(DESTDIR)$(LIBDIR)/package-installed.svg"
	install -Dm0644 data/package-installed.png "$(DESTDIR)$(LIBDIR)/package-installed.png"
	install -Dm0644 data/package-broken.svg "$(DESTDIR)$(LIBDIR)/package-broken.svg"
	install -Dm0644 data/package-broken.png "$(DESTDIR)$(LIBDIR)/package-broken.png"
	install -Dm0644 data/package-upgradable.svg "$(DESTDIR)$(LIBDIR)/package-upgradable.svg"
	install -Dm0644 data/package-upgradable.png "$(DESTDIR)$(LIBDIR)/package-upgradable.png"
	for icon in essora-flatpak-64.png essora-flatpak-128.png essora-flatpak-256.png essora-appimage-64.png essora-appimage-128.png essora-appimage-256.png; do \
		install -Dm0644 "data/$$icon" "$(DESTDIR)$(LIBDIR)/$$icon"; \
	done
	install -Dm0644 data/app_flatpak.png "$(DESTDIR)$(PREFIX)/lib/eapt/app_flatpak.png"
	install -d "$(DESTDIR)$(DATADIR)/pixmaps"
	ln -sfn /usr/lib/edpkg-gui/essora-appimage-256.png "$(DESTDIR)$(DATADIR)/pixmaps/essora-appimage.png"
	ln -sfn /usr/lib/edpkg-gui/essora-flatpak-256.png "$(DESTDIR)$(DATADIR)/pixmaps/essora-flatpak.png"
	ln -sfn /usr/lib/eapt/app_flatpak.png "$(DESTDIR)$(DATADIR)/pixmaps/app_flatpak.png"
	# Bundled AppImage engine: GNU GPLv3, original project ivan-hc/AM 10.6.
	install -Dm0755 third_party/am-10.6/am "$(DESTDIR)$(LIBDIR)/appimage-engine/am"
	install -Dm0755 data/appimage-desktop-migrate "$(DESTDIR)$(LIBDIR)/appimage-desktop-migrate"
	for module in third_party/am-10.6/modules/*.am; do \
		install -Dm0644 "$$module" "$(DESTDIR)$(LIBDIR)/appimage-engine/modules/$${module##*/}"; \
	done
	install -Dm0644 third_party/am-10.6/LICENSE "$(DESTDIR)$(DATADIR)/doc/edpkg-gui/AM-GPL-3.0.txt"
	install -Dm0644 third_party/am-10.6/ATTRIBUTION.md "$(DESTDIR)$(DATADIR)/doc/edpkg-gui/AM-ATTRIBUTION.md"
	install -Dm0755 data/edpkg-gui-launcher "$(DESTDIR)$(BINDIR)/edpkg-gui"
	install -Dm0644 data/edpkg-gui.desktop "$(DESTDIR)$(DATADIR)/applications/edpkg-gui.desktop"

clean:
	rm -f edpkg-gui

check: edpkg-gui
	./tests/test-source-contract.sh
	./tests/test-layout-contract.sh
	./tests/test-performance-contract.sh
	./tests/test-fast-index-contract.sh
	./tests/test-build-path-spaces.sh
	./tests/test-flatpak-contract.sh
	./tests/test-flatpak-refresh-cache-contract.sh
	./tests/test-flatpak-live-progress-contract.sh
	./tests/test-deb-015-rollback-contract.sh
	./tests/test-auto-close-contract.sh
	./tests/test-gksu-launcher-contract.sh
	./tests/test-pymenu-focus-contract.sh
	./tests/test-debug-log-contract.sh
	./tests/test-post-apply-no-rebuild-contract.sh
	./tests/test-indeterminate-progress-contract.sh
	./tests/test-cleanup-warning-contract.sh
	./tests/test-build-declaration-contract.sh
	./tests/test-instant-startup-contract.sh
	./tests/test-kernel-block-contract.sh
	./tests/test-output-batching-contract.sh
	./tests/test-flatpak-pixbuf-cache-contract.sh
	./tests/test-cache-signature-contract.sh
	./tests/test-apt-policy-contract.sh
	./tests/test-0125-optimization-contract.sh
	./tests/test-0126-regression-contract.sh
	./tests/test-0127-safety-cancel-contract.sh
	./tests/test-0128-real-guard.sh
	./tests/test-0129-security-download.sh
	./tests/test-0130-search-refresh-contract.sh
	./tests/test-0131-confirmation-and-notices-contract.sh
	./tests/test-0132-selection-regression.sh
	./tests/test-0133-appimage-contract.sh
	./tests/test-0134-always-refresh-contract.sh
	./tests/test-0135-real-catalog-parser.sh
	./tests/test-0136-am-real-output.sh
	./tests/test-0137-filter-buttons.sh
	./tests/test-0138-keep-selection.sh
	./tests/test-0139-notice-and-startup.sh
	./tests/test-0138-bundled-engine.sh
	./tests/test-0139-launcher-compat.sh
	./tests/test-0140-github-release.sh

.PHONY: all install clean check strict

# No display or GTK headers required; can run independently of `make check`.
check-state-icons:
	./tests/test-0140-status-icon-contract.sh
	./tests/test-0140-github-release.sh

.PHONY: check-state-icons
