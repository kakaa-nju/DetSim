.PHONY: check-parallel-protocol
check-parallel-protocol: $(BUILD_DIR)/tests/parallel_protocol_test check-parallel-protocol-objects
	$<

$(BUILD_DIR)/tests/parallel_protocol_test: tests/parallel_protocol_test.cpp src/core/search/protocol.cpp src/core/search/object_transport.cpp src/core/search/object_transport.h src/core/search/protocol.h src/core/search/frontier.h src/core/state/snapshot_bundle.h src/core/state/state_transition.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -Werror -MMD -MP -MF $@.d -MT $@ tests/parallel_protocol_test.cpp src/core/search/protocol.cpp src/core/search/object_transport.cpp -pthread -lxxhash -o $@

-include $(BUILD_DIR)/tests/parallel_protocol_test.d

.PHONY: benchmark-parallel-protocol
benchmark-parallel-protocol: $(BUILD_DIR)/tests/parallel_protocol_benchmark
	$<

$(BUILD_DIR)/tests/parallel_protocol_benchmark: tests/parallel_protocol_benchmark.cpp src/core/search/protocol.cpp src/core/search/object_transport.cpp src/core/search/object_transport.h src/core/search/protocol.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -Isrc/core/search -DDETSIM_PROTOCOL_BUFFER_STATS -Wall -Wextra -Werror -MMD -MP -MF $@.d -MT $@ tests/parallel_protocol_benchmark.cpp src/core/search/protocol.cpp src/core/search/object_transport.cpp -pthread -lxxhash -o $@

-include $(BUILD_DIR)/tests/parallel_protocol_benchmark.d

.PHONY: check-parallel-protocol-objects
check-parallel-protocol-objects: $(BUILD_DIR)/tests/parallel_protocol_objects
	$<

$(BUILD_DIR)/tests/parallel_protocol_objects: tests/parallel_protocol_objects.cpp src/core/search/protocol.cpp src/core/search/object_transport.cpp src/core/search/object_transport.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -Werror -MMD -MP -MF $@.d -MT $@ tests/parallel_protocol_objects.cpp src/core/search/protocol.cpp src/core/search/object_transport.cpp -pthread -lxxhash -o $@

-include $(BUILD_DIR)/tests/parallel_protocol_objects.d
