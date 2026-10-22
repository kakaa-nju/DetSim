# UserState plugin test.
# Builds the guest program and the user-state plugin.

CXXFLAGS = -O2 -gdwarf-4 -g -fPIC -shared \
           -I. -I../../src -I../../src/core -I../../src/utils -I../../src/subsys \
           -DNP=3 -D_FORTIFY_SOURCE=0
CFLAGS = -gdwarf-4 -g --static

.PHONY: all clean

all: userstate check.so

userstate: userstate.c
	$(CC) $(CFLAGS) $< -o $@

check.so: check.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f userstate check.so
