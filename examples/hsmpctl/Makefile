# SPDX-License-Identifier: MIT License
#
# Copyright (C) 2021 Advanced Micro Devices, Inc. - All Rights Reserved
#
# Author: Nathan Fontenot <nathan.fontenot@amd.com>
#

HSMPCTL = hsmpctl
HSMPCTLD = hsmpctld
CC=gcc

prefix=/usr/local
bindir=$(prefix)/bin
sbindir=$(prefix)/sbin
mandir=$(prefix)/share/man/man1

IDIR=../..
CFLAGS=-I$(IDIR) -Wall -g

LIBS=-lhsmp
# LIBDIR=-L../../.libs

DEPS=hsmpctl.h

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

all: $(HSMPCTL) $(HSMPCTLD)

HSMPCTL_OBJS=$(HSMPCTL).o
$(HSMPCTL): $(HSMPCTL_OBJS)
	$(CC) -o $@ $^ $(CFLAGS)

HSMPCTLD_OBJS=$(HSMPCTLD).o
$(HSMPCTLD): $(HSMPCTLD_OBJS)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBDIR) $(LIBS)

HSMPCTL_MANPAGE=$(HSMPCTL).1.gz
$(HSMPCTL_MANPAGE): $(HSMPCTL).1
	@gzip -k $^

install: all $(HSMPCTL_MANPAGE)
	install -m 755 $(HSMPCTL) $(bindir)
	install -m 755 $(HSMPCTLD) $(sbindir)
	@if [ ! -d $(mandir) ]; then mkdir $(mandir); fi
	install -m 644 $(HSMPCTL_MANPAGE) $(mandir)
	@rm -f $(HSMPCTL_MANPAGE)

uninstall:
	rm -f $(bindir)/$(HSMPCTL)
	rm -f $(sbindir)/$(HSMPCTLD)
	rm -f $(mandir)/$(HSMPCTL_MANPAGE)

clean:
	rm -f $(HSMPCTL_OBJS) $(HSMPCTLD_OBJS)
	rm -f $(HSMPCTL) $(HSMPCTLD)
	rm -f *~

.PHONY: clean install uninstall
