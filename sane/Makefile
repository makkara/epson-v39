# Out-of-tree build of the SANE epsonv39 backend.
#   make                       build libsane-epsonv39.so.1
#   sudo make install          install backend, config and dll.d entry
# Set SANE_INC / USB_INC if the headers are not in /usr/include.

PREFIX   ?= /usr
LIBDIR   ?= $(PREFIX)/lib64/sane
CONFDIR  ?= /etc/sane.d
CC       ?= cc
SANE_INC ?= /usr/include
USB_INC  ?= /usr/include/libusb-1.0
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LIBUSB   ?= $(firstword $(wildcard /usr/lib64/libusb-1.0.so /usr/lib/libusb-1.0.so /usr/lib64/libusb-1.0.so.0 /usr/lib/x86_64-linux-gnu/libusb-1.0.so.0))

all: libsane-epsonv39.so.1

libsane-epsonv39.so.1: epsonv39.c
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=default -I$(SANE_INC) -I$(USB_INC) \
	    -Wl,-soname,libsane-epsonv39.so.1 -o $@ $< $(LIBUSB) -lpthread

install: libsane-epsonv39.so.1
	install -D -m 755 libsane-epsonv39.so.1 $(DESTDIR)$(LIBDIR)/libsane-epsonv39.so.1
	ln -sf libsane-epsonv39.so.1 $(DESTDIR)$(LIBDIR)/libsane-epsonv39.so
	install -D -m 644 epsonv39.conf $(DESTDIR)$(CONFDIR)/epsonv39.conf
	install -D -m 644 dll.d-epsonv39 $(DESTDIR)$(CONFDIR)/dll.d/epsonv39
	install -D -m 644 60-epsonv39.rules $(DESTDIR)/etc/udev/rules.d/60-epsonv39.rules

clean:
	rm -f libsane-epsonv39.so.1

.PHONY: all install clean
