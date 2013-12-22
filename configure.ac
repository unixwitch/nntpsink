dnl nntpsink: dummy NNTP server
dnl 
dnl Copyright (c) 2011-2013 River Tarnell.
dnl
dnl Permission is granted to anyone to use this software for any purpose,
dnl including commercial applications, and to alter it and redistribute it
dnl freely. This software is provided 'as-is', without any express or implied
dnl warranty.

AC_PREREQ([2.68])
AC_INIT([nntpgen], [1.0-DEV], [river@loreley.flyingparchment.org.uk])
AC_CONFIG_SRCDIR([nntpsink.c])
AC_CONFIG_HEADERS([setup.h])

# Checks for programs.
AC_PROG_CC
AC_PROG_INSTALL

AC_CHECK_MEMBER([struct sockaddr_in.sin_len],
	AC_DEFINE([HAVE_SIN_LEN], 1, [Define if struct sockaddr_in has a sin_len member]),
	[],
	[
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>]
)

# Checks for libraries.
AC_ARG_ENABLE([ssl],
	      [AS_HELP_STRING([--disable-ssl], [don't use SSL])],
	      [if test "$enableval" = yes; then
		       use_ssl=yes
	       else
		       use_ssl=no
	       fi],
	      [use_ssl=yes])

if test "$use_ssl" = yes; then
	AC_CHECK_LIB([ssl], [SSL_CTX_new],
	     [LIBS="$LIBS -lssl"
	      AC_DEFINE([HAVE_OPENSSL], 1, [Define if OpenSSL is present])
	     ])
fi

AC_CHECK_LIB([ev], [ev_run], [], [AC_MSG_ERROR([cannot find libev])])
AC_CHECK_HEADER([ev.h], [], [AC_MSG_ERROR([cannot find ev.h])])

AC_CHECK_HEADERS([inttypes.h stdint.h])

AC_CHECK_FUNCS([strndup strlcpy strlcat setproctitle arc4random fdatasync])

AC_SUBST(EXTRA_SRCS)

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
