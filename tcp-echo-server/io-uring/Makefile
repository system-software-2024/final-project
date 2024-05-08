CC = gcc
CFLAGS = -Wall -O2 -D_GNU_SOURCE -g
LIB = uring

.PHONY: clean

all:
	$(CC) $(CFLAGS) -o server server.c -l$(LIB)

clean:
	rm server