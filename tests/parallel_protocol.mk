.PHONY: check-parallel-protocol
check-parallel-protocol: $(BUILD_DIR)/tests/parallel_protocol_test
	$<

$(BUILD_DIR)/tests/parallel_protocol_test: tests/parallel_protocol_test.cpp src/core/search/protocol.cpp src/core/search/protocol.h src/core/search/frontier.h src/core/state/snapshot_bundle.h src/core/state/state_transition.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -Werror -MMD -MP -MF $@.d -MT $@ tests/parallel_protocol_test.cpp src/core/search/protocol.cpp -pthread -o $@

-include $(BUILD_DIR)/tests/parallel_protocol_test.d
