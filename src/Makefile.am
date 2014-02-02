bin_PROGRAMS = scafd
scafd_SOURCES = scafd.c scaf.h uthash.h
scafd_LDADD = -lm -lpthread -lcurses
scafd_CFLAGS = -O2

AUTOMAKE_OPTIONS = gnu
lib_LTLIBRARIES = libscaf.la
libscaf_la_SOURCES = libscaf.c
libscaf_la_CFLAGS = -O2
libscaf_la_LDFLAGS = -version-info 0:0:0

include_HEADERS = scaf.h uthash.h
