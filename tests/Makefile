CFLAGS += -static -gdwarf-4 -O0 -fno-inline -fno-inline-functions -fno-inline-small-functions -fno-omit-frame-pointer
CC = gcc

TESTS = dummy write loop loop3 bakery_mp bakery member rand brk bt fs
SRCS = $(TESTS:=.c)

all: $(TESTS)

%: %.c
	$(CC) $(CFLAGS) $@.c -o $@
