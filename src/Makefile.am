bin_PROGRAMS = scafd scafc
scafd_SOURCES = scafd.c scaf.h uthash.h
scafd_LDADD = -L@ZMQ_LOCATION@/lib -lm -lzmq -lgomp -lpthread -lcurses -lpapi
scafd_CFLAGS = -I@ZMQ_LOCATION@/include

scafc_SOURCES = scafc.c scaf.h libscaf.c uthash.h
scafc_LDADD = -L@ZMQ_LOCATION@/lib -lzmq -lm -lpapi
scafc_LDFLAGS = -fopenmp
scafc_CFLAGS = -I@ZMQ_LOCATION@/include -fopenmp

AUTOMAKE_OPTIONS = gnu
lib_LTLIBRARIES = libscaf.la
libscaf_la_SOURCES = libscaf.c
libscaf_la_CFLAGS = -I@ZMQ_LOCATION@/include
libscaf_la_LDFLAGS = -version-info 0:0:0 -L@ZMQ_LOCATION@/lib -lzmq -lpapi

include_HEADERS = scaf.h uthash.h
