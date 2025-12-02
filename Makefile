CFLAGS?=-O3 -march=native -mtune=native -ggdb3 -gdwarf-5 -pipe -Wall -pedantic
INC=-I/usr/include/SDL3
LIBS=-lSDL3
CC=cc

.PHONY: clean all zlib

all: sanplay

FOBJS = 		\
	sandec.o	\
	sanplay.o

zlib: CFLAGS += -DHAVE_ZLIB
zlib: LIBS += -lz
zlib: sanplay

sanplay: $(FOBJS)
	$(CC) $(LIBS) -o sanplay $(FOBJS)

clean:
	@rm -f sanplay $(FOBJS) *~ *.rej *.orig

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -o $@ -c $<
