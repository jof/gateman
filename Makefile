CFLAGS=-Wall -O2
# Conditionally assign DESTDIR
DESTDIR ?= /
TOP := $(dir $(lastword $(MAKEFILE_LIST)))

.PHONY: install upstart all clean

all: gateman
install: gateman
	install --mode=0755 --owner=root --group=root -d $(DESTDIR)/usr/sbin
	install --mode=0755 --owner=root --group=root $(TOP)/gateman $(DESTDIR)/usr/sbin
	#
	install --mode=0644 --owner=root --group=root -d $(DESTDIR)/etc/init
	install --mode=0644 --owner=root --group=root -T $(TOP)/upstart.conf $(DESTDIR)/etc/init/gateman.conf
	#
	install --mode=0644 --owner=root --group=root -d $(DESTDIR)/etc/init.d
	install --mode=0644 --owner=root --group=root -T $(TOP)/init_script.sh $(DESTDIR)/etc/init.d/gateman
clean:
	-rm gateman
