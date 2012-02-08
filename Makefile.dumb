all: scafc scafd libscaf.a libscaf.so.1.0.1

INCLUDE = /home/tcreech/opt/include
LIBS = /home/tcreech/opt/lib
LDFLAGS=-L$(LIBS) -lm -lzmq -fopenmp
CFLAGS=-I$(INCLUDE) -fopenmp -fPIC

scafc: libscaf.o scafc.c

%.a : %.o
	$(AR) rcs $@ $<

%.so.1.0.1 : %.o
	$(CC) -shared -Wl,-soname,libscaf.so.1 -o $@ $<

clean:
	-rm -rf scafc
	-rm -rf scafd
	-rm -rf libscaf.o
	-rm -rf libscaf.a
	-rm -rf libscaf.so.1.0.1

