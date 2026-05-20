CFLAGS?=-O3 -march=native -mtune=native -ggdb3 -gdwarf-5 -pipe -Wall -pedantic
INC=-I/usr/include/SDL3
LIBS=-lSDL3
CC=cc

# GCC PR120120: with this gcc generates *much* faster code for codec47/bl16
CFLAGS+=-fno-optimize-sibling-calls

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
