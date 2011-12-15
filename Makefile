CFLAGS=-Wall -O2
# Conditionally assign DESTDIR
DESTDIR ?= /usr/local

.PHONY: install upstart all clean

all: gateman
install: gateman upstart
	install --mode=0755 --owner=root --group=root ./gateman $(DESTDIR)/sbin
upstart: upstart-gateman.conf
	install --mode=0644 --owner=root --group=root ./upstart-gateman.conf /etc/init/noisebridge-gateman.conf
test:
	@echo "DESTDIR is ${DESTDIR}"
clean:
	rm gateman
