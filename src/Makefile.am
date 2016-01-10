AM_CFLAGS = \
	@LIBTBM_SPRD_CFLAGS@ \
	-I$(top_srcdir) \
	-I$(top_srcdir)/src \
        -I/usr/include/drm

libtbm_sprd_la_LTLIBRARIES = libtbm_sprd.la
libtbm_sprd_ladir = /${bufmgr_dir}
libtbm_sprd_la_LIBADD = @LIBTBM_SPRD_LIBS@

libtbm_sprd_la_SOURCES = \
	tbm_bufmgr_sprd.c \
	tbm_wayland.c
