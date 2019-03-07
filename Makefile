.POSIX:

# Installation directory for executable files.
BIN = /usr/local/bin
# Basenames of terminals under "/dev/" that should be managed by HUPMon.
HUPMON_MANAGED_TTYS = ttyUSB0

# Build Settings
CC = clang -std=c99
CFLAGS = -lutil -Weverything -Wno-disabled-macro-expansion -O3 -s \
    -D_DEFAULT_SOURCE

# CC = c99
# CFLAGS = -lutil -D_DEFAULT_SOURCE

all: hupmon

$(BIN)/hupmon: hupmon
	install hupmon $(BIN)

$(BIN)/hupmon-login.sh: $(BIN)/hupmon
	install login.sh $@

install: $(BIN)/hupmon $(BIN)/hupmon-login.sh

configure-systemd: install
	for tty in $(HUPMON_MANAGED_TTYS); do (\
		service="hupmon-login@$$tty"; \
		set -x; \
		cp login.service /etc/systemd/system/$$service.service; \
		ln -f -s /etc/systemd/system/$$service.service \
			/etc/systemd/system/getty.target.wants; \
		systemctl daemon-reload; \
		systemctl enable $$service; \
		test ! -e "/dev/$$tty" || systemctl restart $$service; \
	) done

uninstall:
	for path in /etc/systemd/system/hupmon-login@*; do \
		test -e "$$path" || break; \
		service="$${path##*/}" && service="$${service%.service}"; \
		systemctl stop $$service; \
		systemctl disable $$service; \
		rm -f /etc/systemd/system/getty.target.wants/$$service.service \
			"$$path"; \
	done
	rm -f $(BIN)/hupmon $(BIN)/hupmon-login.sh
	systemctl daemon-reload

usage.h: usage.txt
	@echo "static const char usage[] = \"\\" > $@.tmp
	@sed -e 's/["\]/\\\0/g' -e 's/$$/\\n\\/' $? >> $@.tmp
	@echo '";' >> $@.tmp
	mv $@.tmp $@

hupmon: hupmon.c usage.h
	$(CC) $(CFLAGS) $< -o $@
	md5sum $@

clean:
	rm -f hupmon usage.h
