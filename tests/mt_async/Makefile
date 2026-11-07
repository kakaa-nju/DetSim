CFLAGS = -gdwarf-4 -g -pthread

.PHONY: all clean

all: mt_async mt_poll

%: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f mt_async mt_poll
