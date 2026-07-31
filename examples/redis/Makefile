# Redis example Makefile
# Builds the client tracee program

CFLAGS = -gdwarf-4 -g

.PHONY: all clean

all: client

client: client.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f client
