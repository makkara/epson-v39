# Out-of-tree build of the SANE epsonv39 backend.
#
#   make                 build libsane-epsonv39.so.1
#   sudo make install    install backend, config, dll.d entry and udev rule
#   sudo make uninstall  remove them again
#   make check-install   show where files would go
#
# Needs the libusb-1.0 and sane-backends development packages and pkg-config.
# Override SANE_LIBDIR if your distribution keeps SANE backends elsewhere.

CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
PKG_CONFIG ?= pkg-config

DEP_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0 sane-backends 2>/dev/null)
DEP_LIBS   := $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null || echo -lusb-1.0)

# Directory that holds the other SANE backends (libsane-dll.so.1 lives there).
SANE_LIBDIR ?= $(patsubst %/,%,$(dir $(firstword $(wildcard \
	/usr/lib64/sane/libsane-dll.so.1 \
	/usr/lib/$(shell $(CC) -print-multiarch 2>/dev/null)/sane/libsane-dll.so.1 \
	/usr/lib/sane/libsane-dll.so.1 \
	/usr/local/lib/sane/libsane-dll.so.1))))
SANE_CONFDIR ?= /etc/sane.d
UDEV_RULESDIR ?= /etc/udev/rules.d

all: libsane-epsonv39.so.1

libsane-epsonv39.so.1: epsonv39.c
	$(CC) $(CPPFLAGS) $(DEP_CFLAGS) $(CFLAGS) -fPIC -shared \
	    -Wl,-soname,libsane-epsonv39.so.1 $(LDFLAGS) -o $@ $< $(DEP_LIBS) -lpthread

check-install:
	@test -n "$(SANE_LIBDIR)" || { echo "SANE backend directory not found; is sane-backends installed? Set SANE_LIBDIR=..."; exit 1; }
	@echo "backend: $(DESTDIR)$(SANE_LIBDIR)/libsane-epsonv39.so.1"
	@echo "config:  $(DESTDIR)$(SANE_CONFDIR)/epsonv39.conf"
	@echo "dll.d:   $(DESTDIR)$(SANE_CONFDIR)/dll.d/epsonv39"
	@echo "udev:    $(DESTDIR)$(UDEV_RULESDIR)/60-epsonv39.rules"

install: libsane-epsonv39.so.1 check-install
	install -D -m 755 libsane-epsonv39.so.1 $(DESTDIR)$(SANE_LIBDIR)/libsane-epsonv39.so.1
	ln -sf libsane-epsonv39.so.1 $(DESTDIR)$(SANE_LIBDIR)/libsane-epsonv39.so
	install -D -m 644 epsonv39.conf $(DESTDIR)$(SANE_CONFDIR)/epsonv39.conf
	install -D -m 644 dll.d-epsonv39 $(DESTDIR)$(SANE_CONFDIR)/dll.d/epsonv39
	install -D -m 644 60-epsonv39.rules $(DESTDIR)$(UDEV_RULESDIR)/60-epsonv39.rules
	@echo
	@echo "Installed. Reload udev and replug the scanner:"
	@echo "  sudo udevadm control --reload && sudo udevadm trigger"

uninstall: check-install
	rm -f $(DESTDIR)$(SANE_LIBDIR)/libsane-epsonv39.so.1 $(DESTDIR)$(SANE_LIBDIR)/libsane-epsonv39.so
	rm -f $(DESTDIR)$(SANE_CONFDIR)/dll.d/epsonv39 $(DESTDIR)$(UDEV_RULESDIR)/60-epsonv39.rules
	@echo "Kept $(SANE_CONFDIR)/epsonv39.conf; remove it by hand if you like."

clean:
	rm -f libsane-epsonv39.so.1

.PHONY: all check-install install uninstall clean
