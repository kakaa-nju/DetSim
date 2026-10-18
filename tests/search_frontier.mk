# Isolated coordinator policy tests: no tracer, ptrace, snapshots, or dependencies.
FRONTIER_BUILD ?= build/tests/search-frontier
.PHONY: check-search-frontier
check-search-frontier: $(FRONTIER_BUILD)/search_frontier_test
	$<
$(FRONTIER_BUILD)/search_frontier_test: tests/search_frontier_test.cpp src/core/search/frontier.cpp src/core/search/frontier.h
	@mkdir -p $(@D)
	$(CXX) -std=c++20 -Wall -Wextra -Werror -O2 -g $(CPPFLAGS) $(CXXFLAGS) -Isrc tests/search_frontier_test.cpp src/core/search/frontier.cpp $(LDFLAGS) -o $@
