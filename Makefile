.PHONY: all debug release wc clean clear

CC = gcc
CXX = ccache g++
LD = g++ -L/usr/local/lib

CXXFLAGS ?= -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -g -msse4.2
LDFLAGS = -g -ldwarf -lreadline -lcjson -ldw -lzstd -rdynamic \
					-lunwind-ptrace -lunwind-x86_64 -lunwind -lelf -lfmt

SRCS = serialize.cpp sockstate.cpp guest.cpp fsstate.cpp utils.cpp crc32.cpp engine.cpp expr.cpp emu.cpp resolve.cpp monitor.cpp main.cpp state.cpp 
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

all: release

release: CXXFLAGS = -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -msse4.2
release: tracer

debug: CXXFLAGS = -MMD -MP -std=gnu++2a -fno-stack-protector -O0 -g -msse4.2 -DNOCOMPRESS
debug: tracer

tracer: $(OBJS) nr2call.o
	$(LD) $^ $(LDFLAGS) -o tracer
	-mkdir mappings memory filesystem tstate sstate

nr2call.o: nr2call.c
	gcc -c nr2call.c -O3 -g

%.o: %.cpp
	@if [ -z "$(NP)" ]; then \
		echo "NP not set"; exit 1; \
	fi
	@echo "Compiling $<"
	@start=$$(date +%s.%N); \
	$(CXX) $< $(CXXFLAGS) -c -o $@ -DNP=$(NP); \
	end=$$(date +%s.%N); \
	dur=$$(echo "$$end - $$start" | bc); \
	echo "Compiled $< in $$dur seconds"


-include $(DEPS)

wc:
	wc *.[ch] *.cpp

clear:
	-/bin/rm -rf mappings memory filesystem tstate sstate
	-mkdir mappings memory filesystem tstate sstate

clean:
	-/bin/rm *.o *.d tracer *.so
