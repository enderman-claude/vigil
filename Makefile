VERSION    := $(shell cat VERSION)
CC         ?= cc
CFLAGS     ?= -O2 -Wall -Wextra -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS    ?= -pie -Wl,-z,relro,-z,now
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
UNITDIR    ?= $(PREFIX)/lib/systemd/system

vigil: vigil.c VERSION
	$(CC) $(CPPFLAGS) -DVIGIL_VERSION='"$(VERSION)"' $(CFLAGS) $(LDFLAGS) -o $@ vigil.c

install: vigil
	install -Dm755 vigil $(DESTDIR)$(BINDIR)/vigil
	install -d $(DESTDIR)$(UNITDIR)
	sed 's,@BINDIR@,$(BINDIR),g' systemd/vigil@.service.in > $(DESTDIR)$(UNITDIR)/vigil@.service
	chmod 644 $(DESTDIR)$(UNITDIR)/vigil@.service
	test -e $(DESTDIR)$(SYSCONFDIR)/vigil.conf || install -Dm644 vigil.conf $(DESTDIR)$(SYSCONFDIR)/vigil.conf

clean:
	rm -f vigil

.PHONY: install clean
