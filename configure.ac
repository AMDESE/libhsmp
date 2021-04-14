#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([libhsmp], [1.0.0], [nathan.fontenot@amd.com])
AC_CONFIG_SRCDIR([libhsmp.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])

AM_INIT_AUTOMAKE
LT_INIT

# Checks for programs.
AC_PROG_CC

# Checks for libpci.
AC_CHECK_LIB([pci], [pci_write_long])
AC_CHECK_HEADER([pci/pci.h], [], [AC_MSG_ERROR([libhsmp requires libpci devel])])

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h stdint.h stdlib.h string.h sys/file.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T
AC_TYPE_UID_T

# Option to build libhsmp with debug enabled
AC_ARG_ENABLE([debug],
	      AS_HELP_STRING([--enable-debug], [enable libhsmp debug output]))

AS_IF([test "x$enable-debug" = "xyes"],
      [AC_DEFINE([WITH_LIBHSMP_DEBUG], [1], [enable libhsmp debug output])])

AM_CONDITIONAL([WITH_LIBHSMP_DEBUG], [test "x$enable_debug" = "xyes"])

# Option to build libhsmp with debug-pci enabled
AC_ARG_ENABLE([debug-pci],
	      AS_HELP_STRING([--enable-debug-pci], [enable libhsmp PCI debug output]))

AS_IF([test "x$enable-debug-pci" = "xyes"],
      [AC_DEFINE([WITH_LIBHSMP_DEBUG_PCI], [1], [enable libhsmp PCI debug output])])

AM_CONDITIONAL([WITH_LIBHSMP_DEBUG_PCI], [test "x$enable_debug_pci" = "xyes"])

# Option for Family 0x17
AC_ARG_ENABLE([fam17],
	      AS_HELP_STRING([--enable-fam17], [UNSUPPORTED: enable libhsmp Family 0x17 CPUs]))

AS_IF([test "x$enable-fam17" = "xyes"],
      [AC_DEFINE([WITH_FAM17_SUPPORT], [1], [UNSUPPORTED: enable libhsmp Family 0x17 CPUs])])

AM_CONDITIONAL([WITH_FAM17_SUPPORT], [test "x$enable_fam17" = "xyes"])

# Checks for library functions.
AC_CHECK_FUNCS([strerror strtol])

AC_CONFIG_FILES([Makefile libhsmp.spec])
AC_OUTPUT
