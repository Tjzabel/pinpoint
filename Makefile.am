SUBDIRS=transitions
bin_PROGRAMS=pinpoint

AM_CFLAGS = $(DEPS_CFLAGS) $(DEPRECATED_CFLAGS) $(MAINTAINER_CFLAGS) -D_GNU_SOURCE -DPKGDATADIR=\"$(pkgdatadir)/\" -DPINPOINT_SRCDIR=\"$(abs_top_srcdir)\"
ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}

if USE_DAX
DAX_SOURCES = pp-super-aa.c pp-super-aa.h
endif

pinpoint_LDADD  = $(DEPS_LIBS)
pinpoint_SOURCES = \
  pinpoint.c \
  pinpoint.h \
  pp-cairo.c \
  pp-clutter.c \
  gst-video-thumbnailer.h \
  gst-video-thumbnailer.c \
  $(DAX_SOURCES)

EXTRA_DIST=introduction.pin bowls.jpg bg.jpg linus.jpg pp-super-aa.c pp-super-aa.h

MAINTAINERCLEANFILES = aclocal.m4 compile config.guess config.sub configure depcomp install-sh ltmain.sh Makefile.in missing

snapshot:
	$(MAKE) dist distdir=$(PACKAGE)-snap`date +"%Y%m%d"`

include $(top_srcdir)/build/autotools/Makefile.am.release
