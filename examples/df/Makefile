# Target filename (and default Makefile target)
FILENAME = amd-df-util
SOURCES  = $(FILENAME).c nbio_discovery.c
HEADERS  = nbio_discovery.h
CC       = gcc
CFLAGS   =

# Arch flags for Zen. Set based on GCC version, fall back to core2.
# Note using any of core2, znver1 or znver2 is fine.
CPUFLAGS := -march=$(shell [ `gcc -dumpversion` -ge 6 ] && echo znver1 || echo core2)

# Optimization level
CFLAGS += -Os

# Debug flags
#CFLAGS += -g

# Warnings we want to see
CFLAGS += -Wall -Wno-parentheses -Wstrict-prototypes -Wmissing-prototypes
CFLAGS += -fstrict-aliasing -Wstrict-aliasing=1

$(FILENAME): $(SOURCES) $(HEADERS)
	$(CC) $(CPUFLAGS)$(CFLAGS) $(SOURCES) -o $(FILENAME) -lpci -lhsmp

clean:
	rm -rf $(FILENAME)

help:
	@echo "\nThe following make targets are supported:\n"
	@echo "$(FILENAME)\tDefault make target - the primary executable"
	@echo "clean"
	@echo

.PHONY: clean help
