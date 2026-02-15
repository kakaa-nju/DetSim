.PHONY: all debug release wc clean clear

CC = gcc
CXX = ccache g++
LD = g++ -L/usr/local/lib

# Include paths
INCLUDES = -I. -Isrc -Isrc/core -Isrc/subsys -Isrc/utils -Ithird_party

CXXFLAGS ?= $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -g -msse4.2
LDFLAGS = -g -ldwarf -lreadline -lcjson -ldw -lzstd -rdynamic \
					-lunwind-ptrace -lunwind-x86_64 -lunwind -lelf -lfmt

# Source files with new directory structure
SRCS = src/main.cpp \
       src/core/scheduler.cpp src/core/guest.cpp src/core/monitor.cpp src/core/state.cpp src/core/config.cpp src/core/syscall_fmt.cpp \
       src/subsys/serialize.cpp src/subsys/sockstate.cpp src/subsys/fsstate.cpp src/subsys/emu.cpp \
       src/utils/utils.cpp src/utils/expr.cpp src/utils/resolve.cpp src/utils/crc32.cpp

OBJS = $(SRCS:.cpp=.o)
DEPS = $(SRCS:.cpp=.d)

all: release

release: CXXFLAGS = $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -msse4.2
release: tracer

debug: CXXFLAGS = $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O0 -g -msse4.2 -DNOCOMPRESS
debug: tracer

tracer: $(OBJS) nr2call.o
	$(LD) $^ $(LDFLAGS) -o tracer
	-mkdir mappings memory filesystem tstate sstate

nr2call.o: nr2call.c
	gcc -c $< -O3 -g

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
	@find src -name "*.[ch]" -o -name "*.cpp" | xargs wc

clear:
	-/bin/rm -rf mappings memory filesystem tstate sstate
	-mkdir mappings memory filesystem tstate sstate

clean:
	-/bin/rm -f $(OBJS) $(DEPS) nr2call.o tracer *.so
