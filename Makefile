# DetSim build.
#
#   make NP=N [debug|release]   build ./tracer for N modelled processes
#   make NP=N benchmark         build the storage/hash benchmarks
#   make NP=N tools             build the sstate query/verify utilities
#   make clean | clear | format | wc | help
#   make NP=N pgo-generate|pgo-use
#
# NP is a compile-time part of the state layout, so it also selects the object
# directory: build/<mode>/np<N>/.

.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

.PHONY: all release debug tracer benchmark tools help wc format clear clean \
        pgo-generate pgo-use pgo-clean ss_bench sys_bench xxh_bench FORCE

# --- toolchain ---------------------------------------------------------------
# Standard CC/CXX overrides are honored; ccache stays optional
# (e.g. CXX="ccache g++").
ifeq ($(origin CC),default)
CC = gcc
endif
ifeq ($(origin CXX),default)
CXX = g++
endif
LD = $(CXX)

MODE := $(if $(filter debug,$(MAKECMDGOALS)),debug,release)
BUILD_DIR = build/$(MODE)/np$(NP)
BIN_DIR = $(BUILD_DIR)/bin

INCLUDES = -I. -Isrc -Isrc/core -Isrc/core/engine -Isrc/core/syscall \
           -Isrc/core/scheduler -Isrc/core/state -Isrc/subsys -Isrc/subsys/fs \
           -Isrc/subsys/net -Isrc/subsys/sync -Isrc/utils -Isrc/ui \
           -Isrc/ui/cli -Ithird_party -Ithird_party/cereal/include
PROJECT_CPPFLAGS = $(INCLUDES) -DNP=$(NP)
CXXFLAGS ?= -std=gnu++20 -fno-stack-protector -O3 -msse4.2 -mavx2 -mbmi -mbmi2 \
            $(if $(filter debug,$(MODE)),-g,)
CFLAGS ?= -O3 -g
LDFLAGS ?= -g -rdynamic -fno-omit-frame-pointer
LDLIBS = -ldwarf -lreadline -ldw -lzstd -lxxhash -lunwind-ptrace \
         -lunwind-x86_64 -lunwind -lelf -lfmt -lncurses -lpthread

# --- sources -----------------------------------------------------------------
SRCS = src/main.cpp \
       src/core/scheduler.cpp src/core/guest.cpp src/core/monitor.cpp \
       src/core/config.cpp src/core/syscall_fmt.cpp src/core/resolve.cpp \
       src/core/dwarf.cpp src/core/user_plugin.cpp src/core/runtime_profile.cpp \
       src/core/state/state_view.cpp src/core/state/resident_memory.cpp \
       src/core/state/state.cpp src/core/state/snapshot_bundle.cpp \
       src/core/state/shared_object_pool.cpp src/core/state/page_snapshot.cpp \
       src/core/state/page_capture.cpp src/core/state/xstate.cpp \
       src/core/state/state_store.cpp src/core/state/state_store_packed.cpp \
       src/core/state/sysstate_store.cpp src/core/state/state_transition.cpp \
       src/core/state/serialize.cpp \
       src/core/engine/pending_syscall.cpp src/core/engine/thread_manager.cpp \
       src/core/engine/thread_control.cpp src/core/engine/thread_carrier.cpp \
       src/core/engine/thread_pool.cpp src/core/engine/signal_handler.cpp \
       src/core/engine/exec_engine.cpp \
       src/core/syscall/dispatcher.cpp src/core/syscall/handlers.cpp \
       src/core/search/frontier.cpp src/core/search/protocol.cpp \
       src/core/search/worker.cpp src/core/search/parallel.cpp \
       src/core/search/compatibility.cpp src/core/search/object_transport.cpp \
       src/subsys/net/sockstate.cpp src/subsys/net/emu.cpp \
       src/subsys/fs/fsstate.cpp src/subsys/fs/fd_manager.cpp \
       src/subsys/io.cpp src/subsys/pending_io.cpp \
       src/subsys/sync/futexstate.cpp src/subsys/process/process_state.cpp \
       src/utils/utils.cpp src/utils/expr.cpp src/utils/expr_ast.cpp \
       src/utils/expr_lexer.cpp src/utils/expr_parser.cpp \
       src/utils/file_lock.cpp \
       src/ui/ncurses_ui.cpp src/ui/log_wrapper.cpp src/ui/cli/commands.cpp

OBJS = $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRCS))
TRACER_OBJS_NO_MAIN = $(filter-out $(BUILD_DIR)/src/main.o,$(OBJS))

LEX_SRC = src/utils/expr_lexer.cpp
YACC_SRC = src/utils/expr_parser.cpp
YACC_HDR = src/utils/expr_parser.hpp

# --- tracer ------------------------------------------------------------------
all release debug: tracer

# Relink every invocation: the object stamp covers compiler/flags, not link flags.
tracer: $(OBJS) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

FORCE:

# A content stamp invalidates objects when the compiler or flags change. FORCE
# re-runs the comparison; unchanged content keeps the timestamp.
$(BUILD_DIR)/.config: FORCE
	@case '$(NP)' in ''|*[!0-9]*|0*) \
		echo 'NP must be a positive integer (e.g. make NP=1)' >&2; exit 1;; esac
	@mkdir -p $(dir $@)
	@printf '%s\n' 'CC=$(CC)' 'CXX=$(CXX)' 'CPPFLAGS=$(CPPFLAGS)' \
		'PROJECT_CPPFLAGS=$(PROJECT_CPPFLAGS)' 'CFLAGS=$(CFLAGS)' \
		'CXXFLAGS=$(CXXFLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BUILD_DIR)/%.o: %.cpp $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/nr2call.o: nr2call.c $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(LEX_SRC): src/utils/expr_lexer.l $(YACC_HDR)
	flex -o $@ $<

# Grouped outputs prevent concurrent bison invocations under make -j.
$(YACC_SRC) $(YACC_HDR) &: src/utils/expr_parser.y
	bison -d -o $(YACC_SRC) $<

# --- benchmarks --------------------------------------------------------------
benchmark: ss_bench sys_bench xxh_bench
ss_bench: $(BIN_DIR)/ss_bench
sys_bench: $(BIN_DIR)/sys_bench
xxh_bench: $(BIN_DIR)/xxh_bench

.PHONY: $(BIN_DIR)/ss_bench $(BIN_DIR)/sys_bench $(BIN_DIR)/xxh_bench
$(BIN_DIR)/ss_bench: $(BUILD_DIR)/perf/state_store_benchmark.o \
                     $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	@mkdir -p $(@D)
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BIN_DIR)/sys_bench: $(BUILD_DIR)/perf/sysstate_store_benchmark.o \
                      $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	@mkdir -p $(@D)
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(BIN_DIR)/xxh_bench: $(BUILD_DIR)/perf/xxhash_benchmark.o $(BUILD_DIR)/nr2call.o
	@mkdir -p $(@D)
	$(LD) $^ $(LDFLAGS) -lxxhash -o $@

# --- storage tools -----------------------------------------------------------
tools: tools/sstate_query tools/verify_sstate

tools/sstate_query: $(BUILD_DIR)/tools/storage/sstate_query.o \
                    $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

tools/verify_sstate: $(BUILD_DIR)/tools/storage/verify_sstate.o \
                     $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@

-include $(OBJS:.o=.d) $(BUILD_DIR)/nr2call.d \
         $(wildcard $(BUILD_DIR)/perf/*.d $(BUILD_DIR)/tools/storage/*.d)

# --- housekeeping ------------------------------------------------------------
help:
	@echo 'make NP=N [release|debug]      Build ./tracer for N processes'
	@echo 'make NP=N benchmark            Build the storage/hash benchmarks'
	@echo 'make NP=N tools                Build sstate query/verify utilities'
	@echo 'make format                    Run clang-format over src/'
	@echo 'make clear                     Remove runtime snapshots (memory/, sstate/)'
	@echo 'make clean                     Remove compile outputs'
	@echo 'make NP=N pgo-generate|pgo-use Profile-guided optimization'
	@echo 'Compiler overrides: CC=/usr/bin/gcc CXX=/usr/bin/g++'

wc:
	@find src -name '*.[ch]' -o -name '*.cpp' | xargs wc -l

format:
	@find src -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
		-not -path 'src/utils/expr_lexer.cpp' \
		-not -path 'src/utils/expr_parser.cpp' \
		-not -path 'src/utils/expr_parser.hpp' \
		-print0 | xargs -0 clang-format -i

# Only an explicit clear removes user runtime snapshots.
clear:
	rm -rf memory sstate
	mkdir -p memory sstate

clean:
	rm -rf build/release build/debug build-pgo pgo-profiles
	rm -f tracer tracer-pgo tools/sstate_query tools/verify_sstate

# --- profile-guided optimization ---------------------------------------------
# Both phases use the same profile paths. A phase stamp rebuilds objects in
# sequence, so no parallel clean prerequisite can race them.
PGO_BUILD_DIR = build-pgo/np$(NP)
PGO_PROF_DIR = $(abspath pgo-profiles/np$(NP))
PGO_OBJS = $(patsubst %.cpp,$(PGO_BUILD_DIR)/%.o,$(SRCS))
PGO_NRCALL = $(PGO_BUILD_DIR)/nr2call.o
PGO_FLAGS = $(if $(filter pgo-use,$(MAKECMDGOALS)),-fprofile-use -fprofile-correction,-fprofile-generate) \
            -fprofile-dir=$(PGO_PROF_DIR)

pgo-generate pgo-use: $(PGO_OBJS) $(PGO_NRCALL)
	$(LD) $^ $(LDFLAGS) $(PGO_FLAGS) $(LDLIBS) -o tracer-pgo

$(PGO_BUILD_DIR)/.config: FORCE
	@case '$(NP)' in ''|*[!0-9]*|0*) echo 'NP must be a positive integer' >&2; exit 1;; esac
	@mkdir -p $(dir $@) $(PGO_PROF_DIR)
	@printf '%s\n' 'CC=$(CC)' 'CXX=$(CXX)' 'CPPFLAGS=$(CPPFLAGS)' \
		'PROJECT_CPPFLAGS=$(PROJECT_CPPFLAGS)' 'CXXFLAGS=$(CXXFLAGS)' \
		'CFLAGS=$(CFLAGS)' 'PGO_FLAGS=$(PGO_FLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(PGO_BUILD_DIR)/%.o: %.cpp $(PGO_BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) $(PGO_FLAGS) -MMD -MP -c $< -o $@

$(PGO_BUILD_DIR)/nr2call.o: nr2call.c $(PGO_BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PGO_FLAGS) -MMD -MP -c $< -o $@

-include $(PGO_OBJS:.o=.d) $(PGO_NRCALL:.o=.d)

pgo-clean:
	rm -rf build-pgo pgo-profiles
	rm -f tracer-pgo
