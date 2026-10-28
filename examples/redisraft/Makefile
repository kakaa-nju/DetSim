# RedisRaft Example Makefile
# Builds raft server and plugins

NP ?= 3
DETSIM_DIR = ../..
DETSIM_INC = -I$(DETSIM_DIR) -I$(DETSIM_DIR)/src -I$(DETSIM_DIR)/src/core \
             -I$(DETSIM_DIR)/src/core/engine -I$(DETSIM_DIR)/src/core/state \
             -I$(DETSIM_DIR)/src/core/syscall -I$(DETSIM_DIR)/src/utils \
             -I$(DETSIM_DIR)/src/subsys -I$(DETSIM_DIR)/src/subsys/fs \
             -I$(DETSIM_DIR)/src/subsys/net -I$(DETSIM_DIR)/src/subsys/sync \
             -I$(DETSIM_DIR)/src/ui -I$(DETSIM_DIR)/src/ui/cli \
             -I$(DETSIM_DIR)/third_party -I$(DETSIM_DIR)/third_party/cereal/include
CFLAGS = -O3 -gdwarf-4 -g -D_FORTIFY_SOURCE=0 -I./raft/include $(DETSIM_INC) -DNP=$(NP)
CXXFLAGS = -std=gnu++20 -O3 -gdwarf-4 -g -I./raft/include $(DETSIM_INC) -D_FORTIFY_SOURCE=0 -DNP=$(NP)

RAFT_LIB = ./raft/libraft.a

.PHONY: all clean raft plugins

all: raft plugins

raft: $(RAFT_LIB) raft/main.c
	$(CC) $(CFLAGS) raft/main.c $(RAFT_LIB) -o raft/raft

$(RAFT_LIB):
	$(MAKE) -C raft static

plugins: plugins/check.so plugins/choose.so plugins/raft_msg_parser.so

plugins/check.so: plugins/check.cpp
	$(CXX) -shared -fPIC $(CXXFLAGS) $< -o $@

plugins/choose.so: plugins/choose.cpp
	$(CXX) -shared -fPIC $(CXXFLAGS) $< -o $@

plugins/raft_msg_parser.so: plugins/raft_msg_parser.cpp
	$(CXX) -shared -fPIC $(CXXFLAGS) $< -o $@

clean:
	$(MAKE) -C raft clean
	rm -f raft/raft plugins/*.so
