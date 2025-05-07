CFLAGS += -static -gdwarf-4 -O0
CC = gcc

TESTS = dummy write loop loop3 bakery_mp bakery member rand brk 
SRCS = $(TESTS:=.c)

all: $(TESTS)

%: %.c
	$(CC) $(CFLAGS) $@.c -o $@
