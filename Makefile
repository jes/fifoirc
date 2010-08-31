# Makefile for fifoirc
# James Stanley 2010

PREFIX=/usr
CFLAGS=-Wall

fifoirc: fifoirc.c
	$(CC) $(CFLAGS) -o fifoirc fifoirc.c

clean:
	-rm -f fifoirc
.PHONY: clean

install:
	install -m 0755 fifoirc $(DESTDIR)$(PREFIX)/bin
.PHONY: install
