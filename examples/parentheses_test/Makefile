CC = gcc
CFLAGS = -O2 -g -pthread -D_GNU_SOURCE

TARGETS = simple_write parentheses

.PHONY: all clean

all: $(TARGETS)

simple_write: simple_write.c
	$(CC) $(CFLAGS) -o $@ $^

parentheses: parentheses.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGETS)
