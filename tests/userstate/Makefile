# UserState plugin test.
# Builds the guest program and the user-state plugin, plus the rewind
# regression case that checks the shadow state follows every restore.
#
#   make -C tests/userstate all
#   cd tests/userstate && ../../tracer -c userstate.json -b userstate.cmds
#   cd tests/userstate && ../../tracer -c rewind.json -b rewind.cmds
# Both must finish without a "[CHECK FAILED]" line.

CXXFLAGS ?= -O2 -gdwarf-4 -g -fPIC -shared \
            -I. -I../../src -I../../src/core -I../../src/utils -I../../src/subsys \
            -DNP=3 -D_FORTIFY_SOURCE=0
CFLAGS ?= -gdwarf-4 -g --static

.PHONY: all clean

all: userstate check.so rewind rewind_check.so

userstate: userstate.c
	$(CC) $(CFLAGS) $< -o $@

check.so: check.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

rewind: rewind.c
	$(CC) $(CFLAGS) $< -o $@

rewind_check.so: rewind_check.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f userstate check.so rewind rewind_check.so
