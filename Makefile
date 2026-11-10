CC     = gcc
CFLAGS = -g -Wall -Wextra -O0

all: mydbg tests/loop

mydbg: src/mydbg.c
	$(CC) $(CFLAGS) -o $@ $<

tests/loop: tests/loop.c
	$(CC) -g -O0 -no-pie -o $@ $<

clean:
	rm -f mydbg tests/loop

.PHONY: all clean
