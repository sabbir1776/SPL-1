CC     = gcc
CFLAGS = -Wall -Wextra -O0 -g

mydbg: src/mydbg.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f mydbg

.PHONY: clean
