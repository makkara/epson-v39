# Build the epsonv39 SANE backend outside the sane-backends tree.
#
# The backend uses SANE's internal libraries (sanei_usb, sanei_config, ...),
# which are not installed by distributions. This Makefile therefore downloads
# a pinned sane-backends release, checks its SHA-256, configures it, builds
# the sanei objects it needs and links them into libsane-epsonv39.so.1.
#
#   make                       build (downloads sane-backends once, ~8 MB)
#   make SANE_SRC=/path        use an existing, configured sane-backends tree
#   sudo make install          install backend, config, man page, udev rule
#   sudo make uninstall        remove them again
#   make check-install         show where files would go
#
# To add the backend to a sane-backends checkout instead, see
# add-to-sane-backends.sh.

CC          ?= cc
CFLAGS      ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
SANE_VERSION = 1.4.0
SANE_SHA256  = f99205c903dfe2fb8990f0c531232c9a00ec9c2c66ac7cb0ce50b4af9f407a72
# upstream release, then Fedora's copy of the same file (GitLab rate-limits);
# the SHA-256 check below applies to whichever answers
SANE_URLS    = https://gitlab.com/-/project/429008/uploads/843c156420e211859e974f78f64c3ea3/sane-backends-$(SANE_VERSION).tar.gz \
               https://src.fedoraproject.org/repo/pkgs/sane-backends/sane-backends-$(SANE_VERSION).tar.gz/sha512/a5ec0cb9e12075be3c4d5eb445a290216c5699645bb6885845868a8323848cef3aafec30f0c380638195a0a8085a8cff4e190741e79ae8f2f70b4d71abeee572/sane-backends-$(SANE_VERSION).tar.gz

BUILD    ?= build
SANE_SRC ?= $(BUILD)/sane-backends-$(SANE_VERSION)

PREFIX        ?= /usr
DATADIR       ?= $(PREFIX)/share
SANE_CONFDIR  ?= /etc/sane.d
UDEV_RULESDIR ?= /etc/udev/rules.d
MANDIR        ?= $(DATADIR)/man

# Directory that holds the other SANE backends: the first of these that
# contains any libsane-*.so.1 (Debian/Ubuntu build the dll loader into
# libsane.so.1, so libsane-dll.so.1 itself is not a reliable marker).
MULTIARCH := $(strip $(shell $(CC) -print-multiarch 2>/dev/null))
SANE_LIBDIR ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
	$(if $(MULTIARCH),/usr/lib/$(MULTIARCH)/sane/libsane-*.so.1) \
	/usr/lib64/sane/libsane-*.so.1 \
	/usr/lib/sane/libsane-*.so.1 \
	/usr/local/lib/sane/libsane-*.so.1))))

SANEI_OBJS = sanei_usb sanei_config sanei_init_debug sanei_thread sanei_constrain_value
CONFIG_H   = $(SANE_SRC)/include/sane/config.h
SANEI_LIBS = $(SANEI_OBJS:%=$(SANE_SRC)/sanei/.libs/%.o)

all: libsane-epsonv39.so.1 epsonv39.conf sane-epsonv39.5

$(BUILD)/sane-backends-$(SANE_VERSION).tar.gz:
	mkdir -p $(BUILD)
	@for u in $(SANE_URLS); do \
	    echo "downloading $$u"; \
	    if command -v curl >/dev/null; then curl -fsSL -o $@.part "$$u"; else wget -q -O $@.part "$$u"; fi \
	        && echo "$(SANE_SHA256)  $@.part" | sha256sum -c --quiet - && exit 0; \
	    rm -f $@.part; \
	done; echo "could not download sane-backends $(SANE_VERSION)"; exit 1
	mv $@.part $@

$(BUILD)/sane-backends-$(SANE_VERSION)/configure: $(BUILD)/sane-backends-$(SANE_VERSION).tar.gz
	tar -C $(BUILD) -xzf $<
	touch $@

# configure only to get config.h and the sanei Makefiles; no backend is built
$(BUILD)/sane-backends-$(SANE_VERSION)/include/sane/config.h: $(BUILD)/sane-backends-$(SANE_VERSION)/configure
	cd $(BUILD)/sane-backends-$(SANE_VERSION) && ./configure -q BACKENDS=test \
	    --prefix=$(PREFIX) --sysconfdir=$(patsubst %/,%,$(dir $(SANE_CONFDIR))) --localstatedir=/var \
	    --disable-nls --disable-locking --without-avahi --without-snmp
	@grep -q '^#define HAVE_LIBUSB 1' $@ || { echo "sane-backends configure did not find libusb-1.0 (install its development package)"; exit 1; }

$(SANEI_LIBS): $(CONFIG_H)
	$(MAKE) -C $(SANE_SRC)/lib
	$(MAKE) -C $(SANE_SRC)/sanei

# Flags as sane-backends' backend/Makefile uses them; USB/XML/thread
# libraries are read from the configured tree.
libsane-epsonv39.so.1: epsonv39.c $(SANEI_LIBS)
	cp epsonv39.c $(SANE_SRC)/backend/epsonv39.c
	cd $(SANE_SRC)/backend && $(CC) -DHAVE_CONFIG_H -I. -I../include/sane -I../include \
	    -D_REENTRANT -DPATH_SANE_CONFIG_DIR=$(SANE_CONFDIR) -DPATH_SANE_DATA_DIR=$(DATADIR) \
	    -DPATH_SANE_LOCK_DIR=/var/lock -DV_MAJOR=1 -DV_MINOR=4 \
	    $$(sed -n 's/^USB_CFLAGS = //p' Makefile) $(CFLAGS) -fPIC -c epsonv39.c -o epsonv39.o
	$(CC) -shared -fPIC -Wl,-soname,libsane-epsonv39.so.1 -Wl,--no-undefined $(LDFLAGS) \
	    -o $@ $(SANE_SRC)/backend/epsonv39.o $(SANEI_LIBS) \
	    $$(sed -n 's/^USB_LIBS = //p;s/^XML_LIBS = //p;s/^PTHREAD_LIBS = //p' $(SANE_SRC)/backend/Makefile) -lm

epsonv39.conf: epsonv39.conf.in
	sed -e 's|@DATADIR@|$(DATADIR)|g' $< > $@

sane-epsonv39.5: sane-epsonv39.man
	sed -e 's|@PACKAGEVERSION@|epsonv39|g' -e 's|@DATADIR@|$(DATADIR)|g' \
	    -e 's|@CONFIGDIR@|$(SANE_CONFDIR)|g' -e 's|@LIBDIR@|$(SANE_LIBDIR)|g' $< > $@

check-install:
	@test -n "$(SANE_LIBDIR)" || { echo "SANE backend directory not found; is sane-backends installed? Set SANE_LIBDIR=..."; exit 1; }
	@echo "backend: $(DESTDIR)$(SANE_LIBDIR)/libsane-epsonv39.so.1"
	@echo "config:  $(DESTDIR)$(SANE_CONFDIR)/epsonv39.conf"
	@echo "dll.d:   $(DESTDIR)$(SANE_CONFDIR)/dll.d/epsonv39"
	@echo "man:     $(DESTDIR)$(MANDIR)/man5/sane-epsonv39.5"
	@echo "udev:    $(DESTDIR)$(UDEV_RULESDIR)/60-epsonv39.rules"

install: all check-install
	install -D -m 755 libsane-epsonv39.so.1 $(DESTDIR)$(SANE_LIBDIR)/libsane-epsonv39.so.1
	ln -sf libsane-epsonv39.so.1 $(DESTDIR)$(SANE_LIBDIR)/libsane-epsonv39.so
	test -e $(DESTDIR)$(SANE_CONFDIR)/epsonv39.conf || \
	    install -D -m 644 epsonv39.conf $(DESTDIR)$(SANE_CONFDIR)/epsonv39.conf
	install -D -m 644 dll.d-epsonv39 $(DESTDIR)$(SANE_CONFDIR)/dll.d/epsonv39
	install -D -m 644 sane-epsonv39.5 $(DESTDIR)$(MANDIR)/man5/sane-epsonv39.5
	install -D -m 644 60-epsonv39.rules $(DESTDIR)$(UDEV_RULESDIR)/60-epsonv39.rules
	@echo
	@echo "Installed. Reload udev and replug the scanner:"
	@echo "  sudo udevadm control --reload && sudo udevadm trigger"

uninstall: check-install
	rm -f $(DESTDIR)$(SANE_LIBDIR)/libsane-epsonv39.so.1 $(DESTDIR)$(SANE_LIBDIR)/libsane-epsonv39.so
	rm -f $(DESTDIR)$(SANE_CONFDIR)/dll.d/epsonv39 $(DESTDIR)$(UDEV_RULESDIR)/60-epsonv39.rules
	rm -f $(DESTDIR)$(MANDIR)/man5/sane-epsonv39.5
	@echo "Kept $(SANE_CONFDIR)/epsonv39.conf; remove it by hand if you like."

clean:
	rm -f libsane-epsonv39.so.1 epsonv39.conf sane-epsonv39.5

distclean: clean
	rm -rf $(BUILD)

.PHONY: all check-install install uninstall clean distclean
