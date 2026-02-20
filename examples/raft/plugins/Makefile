# Plugins Makefile
# Built separately from tracer runtime

CXXFLAGS = -gdwarf-4 -g -fPIC -shared \
           -I. -I../../../src -I../../../src/core -I../../../src/utils -I../../../src/subsys \
           -I../raft/include

.PHONY: all clean

all: check.so choose.so

check.so: check.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

choose.so: choose.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f *.so
