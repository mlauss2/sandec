CFLAGS?=-Og -ggdb3 -march=znver5 -mtune=znver5 -gdwarf-5 -pipe -std=c23
INC=-I/usr/include/SDL2
CC=gcc

all: sandec

sandec.o: sandec.c
	$(CC) -c $(CFLAGS) $(INC) -o sandec.o sandec.c

sandec: sandec.o
	$(CC) -o sandec sandec.o -lSDL2

clean:
	@rm sandec sandec.o *~
