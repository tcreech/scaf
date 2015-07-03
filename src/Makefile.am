bin_PROGRAMS = scafd
scafd_SOURCES = scafd.c scaf.h uthash.h intpart.c intpart.h
scafd_LDADD = -lm -lpthread
if FORFREEBSD
scafd_LDADD += -lutil
endif
scafd_CFLAGS = -O2

AUTOMAKE_OPTIONS = gnu
lib_LTLIBRARIES = libscaf.la libscafwrap.la
libscaf_la_SOURCES = libscaf.c
libscaf_la_CFLAGS = -O2
libscaf_la_LDFLAGS = -version-info 0:0:0

libscafwrap_la_SOURCES = libscafwrap.c
libscafwrap_la_CFLAGS = -O2 -fPIC -D_GNU_SOURCE
if FORFREEBSD
libscafwrap_la_LDFLAGS = -Wl,-rpath -Wl,$(libdir)
else
libscafwrap_la_LDFLAGS = -ldl -Wl,-rpath -Wl,$(libdir)
endif

libscafwrap_la_LIBADD = libscaf.la

bin_SCRIPTS = scafwrap
CLEANFILES = $(bin_SCRIPTS)

include_HEADERS = scaf.h uthash.h
