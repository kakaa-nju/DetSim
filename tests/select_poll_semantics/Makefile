CFLAGS = -gdwarf-4 -g -O0

.PHONY: all clean

all: select_poll_semantics

%: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f select_poll_semantics
