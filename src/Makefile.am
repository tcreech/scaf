bin_PROGRAMS = scafd scafc
scafd_SOURCES = scafd.c scaf.h uthash.h
scafd_LDADD = -lm -lgomp -lpthread -lcurses
scafd_CFLAGS =

scafc_SOURCES = scafc.c scaf.h libscaf.c uthash.h
scafc_LDADD = -lm
scafc_LDFLAGS = -fopenmp
scafc_CFLAGS = -fopenmp

AUTOMAKE_OPTIONS = gnu
lib_LTLIBRARIES = libscaf.la
libscaf_la_SOURCES = libscaf.c
libscaf_la_CFLAGS =
libscaf_la_LDFLAGS = -version-info 0:0:0

include_HEADERS = scaf.h uthash.h
