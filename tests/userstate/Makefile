# UserState plugin test.
# Builds the guest program and the user-state plugin, plus the rewind
# regression case that checks the shadow state follows every restore.
#
#   cd tests/userstate && ./check.sh
#
# check.sh drives the control/violation pair: userstate.json (monotone counter,
# checker stays silent) and userstate_violation.json (the same program driven
# backwards, which the checker must report). The rewind case additionally needs
# a "[CHECK FAILED]" to stay absent.

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
