bin_PROGRAMS = scafd
scafd_SOURCES = scafd.c scaf.h uthash.h intpart.c intpart.h
scafd_LDADD = -lm -lpthread
scafd_CFLAGS = -O2

AUTOMAKE_OPTIONS = gnu
lib_LTLIBRARIES = libscaf.la libscafwrap.la
libscaf_la_SOURCES = libscaf.c
libscaf_la_CFLAGS = -O2
libscaf_la_LDFLAGS = -version-info 0:0:0

libscafwrap_la_SOURCES = libscafwrap.c
libscafwrap_la_CFLAGS = -O2 -fPIC -D_GNU_SOURCE
libscafwrap_la_LDFLAGS = -version-info 0:0:0 --as-needed -ldl -lscaf

bin_SCRIPTS = scafwrap
CLEANFILES = $(bin_SCRIPTS)

include_HEADERS = scaf.h uthash.h
