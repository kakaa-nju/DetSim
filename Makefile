.PHONY: all debug wc
CC = gcc
CXX = ccache g++
LD = g++
CXXFLAGS = -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -g -msse4.2
LDFLAGS = -ldwarf -lreadline -lcjson -lunwind -ldw -lzstd -rdynamic

SRCS = monitor.cpp main.cpp state.cpp sockstate.cpp guest.cpp fsstate.cpp utils.cpp crc32.cpp engine.cpp expr.cpp emu.cpp
OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

tracer: $(OBJS)
	$(LD) $^ $(LDFLAGS) -o tracer 
	-mkdir mappings memory filesystem tstate sstate

-include $(DEPS)

%.o: %.cpp
	@if [ -z "$(NP)" ]; then \
		echo "NP not set"; exit 1; \
	fi
	$(CXX) $< $(CXXFLAGS) -c -o $@ -DNP=$(NP)


all: tracer


wc:
	find . -regextype posix-extended -regex '.*\.(cpp|h)' | xargs wc -l

clear:
	-/bin/rm -r mappings memory filesystem tstate sstate
	-mkdir mappings memory filesystem tstate sstate

clean:
	-/bin/rm *.o *.d tracer *.so
