#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  on the rights to use, copy, modify, merge, publish, distribute, sub
#  license, and/or sell copies of the Software, and to permit persons to whom
#  the Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
#  ADAM JACKSON BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
#  IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
#  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

AC_PREREQ(2.60)
AC_INIT(libtbm-sprd, 1.0.0)
AC_USE_SYSTEM_EXTENSIONS
AC_CONFIG_SRCDIR([Makefile.am])
AM_INIT_AUTOMAKE([dist-bzip2])

AM_CONFIG_HEADER([config.h])

AC_DISABLE_STATIC
AC_PROG_LIBTOOL
AC_PROG_CC

AC_HEADER_STDC
AC_SYS_LARGEFILE
AC_FUNC_ALLOCA

# Enable quiet compiles on automake 1.11.
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

PKG_CHECK_MODULES(LIBDRM, libdrm)
PKG_CHECK_MODULES(LIBTBM, libtbm)
PKG_CHECK_MODULES(LIBDLOG, dlog)
LIBTBM_SPRD_CFLAGS="$LIBDRM_CFLAGS $LIBTBM_CFLAGS $LIBDLOG_CFLAGS $LIBUDEV_CLFAGS "
LIBTBM_SPRD_LIBS="$LIBDRM_LIBS $LIBTBM_LIBS $LIBDLOG_LIBS $LIBUDEV_LIBS "

PKG_CHECK_MODULES(LIBUDEV, libudev, [udev=yes], [udev=no])
if test x"$udev" = xyes; then
	AC_DEFINE(HAVE_UDEV,1,[Enable udev-based monitor hotplug detection])
	LIBTBM_SPRD_CFLAGS="$LIBTBM_SPRD_CFLAGS $LIBUDEV_CFLAGS"
	LIBTBM_SPRD_LIBS="$LIBTBM_SPRD_LIBS $LIBUDEV_LIBS"
fi

AC_SUBST(LIBTBM_SPRD_CFLAGS)
AC_SUBST(LIBTBM_SPRD_LIBS)

bufmgr_dir=${libdir#*/}
AC_SUBST(bufmgr_dir)

AC_OUTPUT([
	Makefile
	src/Makefile])

echo ""
echo "CFLAGS  : $CFLAGS"
echo "LDFLAGS : $LDFLAGS"
echo "LIBTBM_SPRD_CFLAGS : $LIBTBM_SPRD_CFLAGS"
echo "LIBTBM_SPRD_LIBS   : $LIBTBM_SPRD_LIBS"
echo "bufmgr_dir : $bufmgr_dir"
echo ""

