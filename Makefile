CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
LDFLAGS ?= -pie -Wl,-z,relro,-z,now
PREFIX  ?= /usr/local

vigil: vigil.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: vigil
	install -Dm755 vigil $(DESTDIR)$(PREFIX)/bin/vigil
	install -Dm644 systemd/vigil@.service $(DESTDIR)/etc/systemd/system/vigil@.service

clean:
	rm -f vigil

.PHONY: install clean
