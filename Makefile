CFLAGS=-Wall -O2
# Conditionally assign DESTDIR
DESTDIR ?= /usr/local

.PHONY: install all clean

all: gateman
install: gateman
	install --mode=0755 --owner=root --group=root ./gateman $(DESTDIR)/sbin
test:
	@echo "DESTDIR is ${DESTDIR}"
clean:
	rm gateman
