bin_PROGRAMS = scafd scafc
scafd_SOURCES = scafd.c scaf.h
scafd_LDADD = -L@ZMQ_LOCATION@/lib -lzmq
scafd_CFLAGS = -I@ZMQ_LOCATION@/include

scafc_SOURCES = scafc.c scaf.h libscaf.c
scafc_LDADD = -L@ZMQ_LOCATION@/lib -lzmq -lm
scafc_LDFLAGS = -fopenmp
scafc_CFLAGS = -I@ZMQ_LOCATION@/include -fopenmp

AUTOMAKE_OPTIONS = gnu
lib_LTLIBRARIES = libscaf.la
libscaf_la_SOURCES = libscaf.c
libscaf_la_CFLAGS = -I@ZMQ_LOCATION@/include
libscaf_la_LDFLAGS = -version-info 0:0:0

include_HEADERS = scaf.h
