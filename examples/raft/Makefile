# Raft example Makefile
# Builds raft server and tracer plugins

NP ?= 3
CFLAGS = -gdwarf-4 -g -D_FORTIFY_SOURCE=0 -static
# Plugins are shared objects loaded into the tracer: no -static, and NP must
# match the tracer build because the plugin sizes its per-node arrays from it.
PLUGIN_CXXFLAGS = -std=gnu++20 -O2 -gdwarf-4 -g -shared -fPIC -DNP=$(NP) \
                  -I . -I ../../src -I ../../src/core -I ../../src/utils \
                  -I ../../src/subsys -D_FORTIFY_SOURCE=0

.PHONY: all clean raft raft_dynamic plugins

all: raft raft_dynamic plugins

# Build raft server (delegates to raft subdir Makefile)
raft:
	$(MAKE) -C raft main

# Build raft_dynamic server
raft_dynamic:
	$(MAKE) -C raft raft_dynamic

# Build plugins
plugins: plugins/check.so plugins/choose.so

plugins/check.so: plugins/check.cpp
	$(CXX) $(PLUGIN_CXXFLAGS) $< -o $@

plugins/choose.so: plugins/choose.cpp
	$(CXX) $(PLUGIN_CXXFLAGS) $< -o $@

clean:
	$(MAKE) -C raft clean
	rm -f plugins/*.so
