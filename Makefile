.PHONY: all debug release wc clean clear benchmark pgo-generate pgo-use pgo-clean

CC = ccache gcc
CXX = ccache g++
LD = ccache g++ -L/usr/local/lib

# Build directory
BUILD_DIR = build

# Include paths (updated for new directory structure)
INCLUDES = -I. -Isrc -Isrc/core -Isrc/core/engine -Isrc/core/syscall -Isrc/core/scheduler -Isrc/core/state -Isrc/subsys -Isrc/subsys/fs -Isrc/subsys/net -Isrc/subsys/sync -Isrc/subsys/time -Isrc/utils -Isrc/ui -Isrc/ui/cli -Ithird_party -Iexamples/redisraft/plugins -Isrc/subsys

CXXFLAGS ?= $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -g -msse4.2 -D_FORTIFY_SOURCE=0 -fno-omit-frame-pointer \
						-mavx2 -mbmi -mbmi2
LDFLAGS = -g -ldwarf -lreadline -ldw -lzstd -lxxhash -rdynamic \
				-lunwind-ptrace -lunwind-x86_64 -lunwind -lelf -lfmt \
				-lncurses -lpthread -fno-omit-frame-pointer

# Source files with new directory structure
SRCS = src/main.cpp\
       src/core/scheduler.cpp src/core/guest.cpp src/core/monitor.cpp src/core/config.cpp src/core/syscall_fmt.cpp src/core/resolve.cpp \
       src/core/scheduler/exploration.cpp \
       src/core/dwarf.cpp \
       src/core/state/state.cpp src/core/state/state_store.cpp src/core/state/state_store_packed.cpp src/core/state/sysstate_store.cpp src/core/state/state_transition.cpp src/core/state/serialize.cpp \
       src/core/engine/thread_manager.cpp src/core/engine/signal_handler.cpp src/core/engine/exec_engine.cpp \
       src/core/syscall/dispatcher.cpp src/core/syscall/handlers.cpp \
       src/subsys/net/sockstate.cpp src/subsys/net/emu.cpp \
       src/subsys/fs/fsstate.cpp src/subsys/fs/fd_manager.cpp \
       src/subsys/sync/futexstate.cpp \
       src/utils/utils.cpp src/utils/expr.cpp src/utils/expr_ast.cpp \
       src/utils/expr_lexer.cpp src/utils/expr_parser.cpp \
       src/utils/file_lock.cpp \
       examples/redisraft/plugins/raft_msg_parser.cpp \
       src/ui/ncurses_ui.cpp src/ui/log_wrapper.cpp src/ui/cli/commands.cpp

# Object files in build directory
OBJS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(patsubst %.cpp,$(BUILD_DIR)/%.d,$(SRCS))

# Lex/Yacc generated files
LEX_SRC = src/utils/expr_lexer.cpp
YACC_SRC = src/utils/expr_parser.cpp
YACC_HDR = src/utils/expr_parser.hpp

all: release

release: CXXFLAGS = $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -msse4.2 -mavx2 -mbmi -mbmi2
release: tracer

debug: CXXFLAGS = $(INCLUDES) -MMD -MP -std=gnu++2a -fno-stack-protector -O3 -g -msse4.2 -mavx2 -mbmi -mbmi2
debug: LDFLAGS += 
debug: tracer

tracer: $(OBJS) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) -o tracer
	-mkdir memory sstate

$(BUILD_DIR)/nr2call.o: nr2call.c | $(BUILD_DIR)
	gcc -c $< -O3 -g -o $@

$(LEX_SRC): src/utils/expr_lexer.l $(YACC_HDR)
	flex -o $@ $<

$(YACC_SRC) $(YACC_HDR): src/utils/expr_parser.y
	bison -d -o $(YACC_SRC) $<

# Compile C++ files to build directory
$(BUILD_DIR)/%.o: %.cpp | $(BUILD_DIR)
	@if [ -z "$(NP)" ]; then \
		echo "NP not set"; exit 1; \
	fi
	@mkdir -p $(dir $@)
	@echo "Compiling $<"
	@start=$$(date +%s.%N); \
	$(CXX) $< $(CXXFLAGS) -c -o $@ -DNP=$(NP); \
	end=$$(date +%s.%N); \
	dur=$$(echo "$$end - $$start" | bc); \
	echo "Compiled $< in $$dur seconds"

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)/src/core $(BUILD_DIR)/src/core/engine $(BUILD_DIR)/src/core/syscall $(BUILD_DIR)/src/core/scheduler $(BUILD_DIR)/src/core/state
	mkdir -p $(BUILD_DIR)/src/subsys $(BUILD_DIR)/src/subsys/fs $(BUILD_DIR)/src/subsys/net $(BUILD_DIR)/src/subsys/sync $(BUILD_DIR)/src/subsys/time
	mkdir -p $(BUILD_DIR)/src/utils $(BUILD_DIR)/src/ui $(BUILD_DIR)/src/ui/cli
	mkdir -p $(BUILD_DIR)/examples/redisraft/plugins

-include $(DEPS)

wc:
	@find src -name "*.[ch]" -o -name "*.cpp" | xargs wc

clear:
	-/bin/rm -rf memory sstate
	-mkdir memory sstate

clean:
	-/bin/rm -rf $(BUILD_DIR) tracer *.so
	-/bin/rm -f $(LEX_SRC) $(YACC_SRC) $(YACC_HDR) src/utils/expr_parser.output

# --- Benchmark Targets ---
# Exclude the main tracer entry point from the object list for linking the benchmark
TRACER_OBJS_NO_MAIN = $(filter-out $(BUILD_DIR)/src/main.o, $(OBJS))

# StateStore benchmark
BENCH1_SRC = perf/state_store_benchmark.cpp
BENCH1_OBJ = $(BUILD_DIR)/perf/state_store_benchmark.o
BENCH1_EXE = ss_bench

# SysStateStore benchmark  
BENCH2_SRC = perf/sysstate_store_benchmark.cpp
BENCH2_OBJ = $(BUILD_DIR)/perf/sysstate_store_benchmark.o
BENCH2_EXE = sys_bench

# XXHash benchmark
BENCH3_SRC = perf/xxhash_benchmark.cpp
BENCH3_OBJ = $(BUILD_DIR)/perf/xxhash_benchmark.o
BENCH3_EXE = xxh_bench

benchmark: $(BENCH1_EXE) $(BENCH2_EXE) $(BENCH3_EXE)

$(BENCH1_EXE): $(BENCH1_OBJ) $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	@echo "==> Linking Benchmark: $(BENCH1_EXE)"
	$(LD) $^ $(LDFLAGS) -o $@

$(BENCH2_EXE): $(BENCH2_OBJ) $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	@echo "==> Linking Benchmark: $(BENCH2_EXE)"
	$(LD) $^ $(LDFLAGS) -o $@

$(BENCH3_EXE): $(BENCH3_OBJ) $(BUILD_DIR)/nr2call.o
	@echo "==> Linking Benchmark: $(BENCH3_EXE)"
	$(CXX) $^ -O3 -o $@

# Compile benchmark files to build directory
$(BUILD_DIR)/perf/%.o: perf/%.cpp | $(BUILD_DIR)
	@if [ -z "$(NP)" ]; then \
		echo "NP not set"; exit 1; \
	fi
	@mkdir -p $(dir $@)
	@echo "Compiling benchmark $<"
	@start=$$(date +%s.%N); \
	$(CXX) $< $(CXXFLAGS) -c -o $@ -DNP=$(NP); \
	end=$$(date +%s.%N); \
	dur=$$(echo "$$end - $$start" | bc); \
	echo "Compiled $< in $$dur seconds"

# --- Tool Targets ---
TOOLS_DIR = tools

# Tool executables
SSTATE_QUERY_SRC = $(TOOLS_DIR)/sstate_query.cpp
SSTATE_QUERY_OBJ = $(BUILD_DIR)/$(TOOLS_DIR)/sstate_query.o
SSTATE_QUERY_EXE = $(TOOLS_DIR)/sstate_query

VERIFY_SSTATE_SRC = $(TOOLS_DIR)/verify_sstate.cpp
VERIFY_SSTATE_OBJ = $(BUILD_DIR)/$(TOOLS_DIR)/verify_sstate.o
VERIFY_SSTATE_EXE = $(TOOLS_DIR)/verify_sstate

# Tool dependencies - need UI objects too
TOOL_DEPS = $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o \
            $(BUILD_DIR)/src/ui/ncurses_ui.o $(BUILD_DIR)/src/ui/log_wrapper.o

tools: $(SSTATE_QUERY_EXE) $(VERIFY_SSTATE_EXE)

$(SSTATE_QUERY_EXE): $(SSTATE_QUERY_OBJ) $(TOOL_DEPS)
	@echo "==> Linking Tool: $(SSTATE_QUERY_EXE)"
	$(LD) $^ $(LDFLAGS) -o $@

$(VERIFY_SSTATE_EXE): $(VERIFY_SSTATE_OBJ) $(TOOL_DEPS)
	@echo "==> Linking Tool: $(VERIFY_SSTATE_EXE)"
	$(LD) $^ $(LDFLAGS) -o $@

# Compile tool files to build directory
$(BUILD_DIR)/$(TOOLS_DIR)/%.o: $(TOOLS_DIR)/%.cpp | $(BUILD_DIR)
	@if [ -z "$(NP)" ]; then \
		echo "NP not set"; exit 1; \
	fi
	@mkdir -p $(dir $@)
	@echo "Compiling tool $<"
	@start=$$(date +%s.%N); \
	$(CXX) $< $(CXXFLAGS) -c -o $@ -DNP=$(NP)

# ==============================================================================
# Profile-Guided Optimization (PGO) Targets - Simplified Design
# ==============================================================================
# Usage:
#   1. make pgo-generate NP=3     # Build instrumented binary
#   2. ./tracer-pgo <workload>    # Run to generate .gcda files
#   3. make pgo-use NP=3          # Recompile with profile data
#   4. ./tracer-pgo               # Run optimized binary
# ==============================================================================

PGO_BUILD_DIR = build-pgo
PGO_PROF_DIR = pgo-profiles

# PGO object files
PGO_OBJS = $(patsubst %.cpp,$(PGO_BUILD_DIR)/%.o,$(SRCS))
PGO_NRCALL = $(PGO_BUILD_DIR)/nr2call.o

# Step 1: Generate profile (instrumented build)
.PHONY: pgo-generate
pgo-generate: PGO_CXXFLAGS = $(CXXFLAGS) -fprofile-generate -fprofile-dir=$(PGO_PROF_DIR) -ffile-prefix-map=$(CURDIR)=.
pgo-generate: PGO_LDFLAGS = $(LDFLAGS) -fprofile-generate -fprofile-dir=$(PGO_PROF_DIR)
pgo-generate: $(PGO_BUILD_DIR) $(PGO_OBJS) $(PGO_NRCALL)
	$(LD) $(PGO_OBJS) $(PGO_NRCALL) $(PGO_LDFLAGS) -o tracer-pgo
	@echo "==> PGO instrumented binary: ./tracer-pgo"
	@echo "==> Run it to generate profile data, then: make pgo-use NP=$(NP)"

# Step 2: Use profile (optimized build)  
.PHONY: pgo-use
pgo-use: PGO_CXXFLAGS = $(CXXFLAGS) -fprofile-use -fprofile-dir=$(PGO_PROF_DIR) -fprofile-correction -ffile-prefix-map=$(CURDIR)=.
pgo-use: PGO_LDFLAGS = $(LDFLAGS)
pgo-use: pgo-clean-objs $(PGO_OBJS) $(PGO_NRCALL)
	$(LD) $(PGO_OBJS) $(PGO_NRCALL) $(PGO_LDFLAGS) -o tracer-pgo
	@echo "==> PGO optimized binary: ./tracer-pgo"

# Clean only object files (keep .gcda profile data)
.PHONY: pgo-clean-objs
pgo-clean-objs:
	@rm -f $(PGO_OBJS) $(PGO_NRCALL)

# Clean everything including profile data
.PHONY: pgo-clean
pgo-clean:
	@rm -rf $(PGO_BUILD_DIR) $(PGO_PROF_DIR)
	@rm -f tracer-pgo
	@echo "==> PGO files cleaned"

# Build directory
$(PGO_BUILD_DIR):
	@mkdir -p $(PGO_BUILD_DIR)/src/core $(PGO_BUILD_DIR)/src/subsys \
		$(PGO_BUILD_DIR)/src/utils $(PGO_BUILD_DIR)/src/ui \
		$(PGO_BUILD_DIR)/examples/redisraft/plugins
	@mkdir -p $(PGO_PROF_DIR)

# Generic rule for PGO object files (flags determined by target)
$(PGO_BUILD_DIR)/%.o: %.cpp | $(PGO_BUILD_DIR)
	@if [ -z "$(NP)" ]; then echo "NP not set"; exit 1; fi
	@mkdir -p $(dir $@)
	$(CXX) $< $(PGO_CXXFLAGS) -c -o $@ -DNP=$(NP)

$(PGO_BUILD_DIR)/nr2call.o: nr2call.c | $(PGO_BUILD_DIR)
	$(CC) -c $< -O3 -g $(PGO_CFLAGS) -o $@
