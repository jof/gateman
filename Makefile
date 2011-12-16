CFLAGS=-Wall -O2
# Conditionally assign DESTDIR
DESTDIR ?= /usr/local
TOP := $(dir $(lastword $(MAKEFILE_LIST)))

.PHONY: install upstart all clean

all: gateman
install: gateman upstart
	install --mode=0755 --owner=root --group=root $(TOP)/gateman $(DESTDIR)/sbin
upstart: upstart-gateman.conf
	install --mode=0644 --owner=root --group=root $(TOP)/upstart-gateman.conf /etc/init/noisebridge-gateman.conf
test:
	@echo "DESTDIR is ${DESTDIR}"
clean:
	rm gateman
