.PHONY: all debug release wc clean clear

CC = gcc
CXX = ccache g++
LD = g++ -L/usr/local/lib

# Include paths
INCLUDES = -I. -Isrc -Isrc/core -Isrc/subsys -Isrc/utils -Ithird_party -Iexamples/raft/plugins

CXXFLAGS ?= $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -g -msse4.2 -D_FORTIFY_SOURCE=0
LDFLAGS = -g -ldwarf -lreadline -lcjson -ldw -lzstd -rdynamic \
					-lunwind-ptrace -lunwind-x86_64 -lunwind -lelf -lfmt

# Source files with new directory structure
SRCS = src/main.cpp \
       src/core/scheduler.cpp src/core/guest.cpp src/core/monitor.cpp src/core/state.cpp src/core/config.cpp src/core/syscall_fmt.cpp src/core/dwarf.cpp src/core/state_store.cpp \
       src/subsys/serialize.cpp src/subsys/sockstate.cpp src/subsys/fsstate.cpp src/subsys/emu.cpp src/subsys/fd_manager.cpp \
       src/utils/utils.cpp src/utils/expr.cpp src/utils/expr_ast.cpp \
       src/utils/expr_lexer.cpp src/utils/expr_parser.cpp \
       src/utils/resolve.cpp src/utils/crc32.cpp \
       examples/raft/plugins/raft_msg_parser.cpp

OBJS = $(SRCS:.cpp=.o)
DEPS = $(SRCS:.cpp=.d)

# Lex/Yacc generated files
LEX_SRC = src/utils/expr_lexer.cpp
YACC_SRC = src/utils/expr_parser.cpp
YACC_HDR = src/utils/expr_parser.hpp

all: release

release: CXXFLAGS = $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -msse4.2
release: tracer

debug: CXXFLAGS = $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O0 -g -msse4.2 -DNOCOMPRESS
debug: tracer

tracer: $(OBJS) nr2call.o
	$(LD) $^ $(LDFLAGS) -o tracer
	-mkdir memory sstate

nr2call.o: nr2call.c
	gcc -c $< -O3 -g

$(LEX_SRC): src/utils/expr_lexer.l $(YACC_HDR)
	flex -o $@ $<

$(YACC_SRC) $(YACC_HDR): src/utils/expr_parser.y
	bison -d -o $(YACC_SRC) $<

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
	-/bin/rm -rf memory sstate
	-mkdir memory sstate

clean:
	-/bin/rm -f $(OBJS) $(DEPS) nr2call.o tracer *.so
	-/bin/rm -f $(LEX_SRC) $(YACC_SRC) $(YACC_HDR) src/utils/expr_parser.output
