ACLOCAL_AMFLAGS = -I m4

EXTRA_DIST = COPYING README

docdir = $(datadir)/doc/@PACKAGE@
doc_DATA = COPYING README

library_includedir = $(includedir)
library_include_HEADERS = libhsmp.h

AM_CFLAGS = -Wall

if WITH_LIBHSMP_DEBUG
AM_CFLAGS += -DDEBUG_HSMP
endif

if WITH_FAM17_SUPPORT
AM_CFLAGS += -DHSMP_FAMILY_0x17
endif

lib_LTLIBRARIES = libhsmp.la
libhsmp_la_SOURCES = libhsmp.c

libhsmp_la_LDFLAGS = -version-info 1:0:0

noinst_SCRIPTS = run_tests.sh
EXTRA_DIST += run_tests.sh

noinst_PROGRAMS = hsmp_test hsmp_test_static
hsmp_test_SOURCES = hsmp_test.c
hsmp_test_LDADD = libhsmp.la

hsmp_test_static_SOURCES = hsmp_test.c
hsmp_test_static_CFLAGS = -DBUILD_STATIC
if WITH_LIBHSMP_DEBUG
hsmp_test_static_CFLAGS += -DDEBUG_HSMP
endif

if WITH_FAM17_SUPPORT
hsmp_test_static_CFLAGS += -DHSMP_FAMILY_0x17
endif

