CFLAGS?=-O3 -march=native -mtune=native -fexpensive-optimizations -ggdb3 -gdwarf-5 -pipe -Wall -pedantic
INC=-I/usr/include/SDL2
LIBS=-lSDL2 -lc
CC=gcc

all: sanplay

FOBJS = 		\
	sandec.o	\
	sanplay.o

sanplay: $(FOBJS)
	$(CC) $(LIBS) -o sanplay $(FOBJS)

clean:
	@rm -f sanplay $(FOBJS) *~

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -o $@ -c $<
