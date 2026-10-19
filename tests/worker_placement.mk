.PHONY: check-worker-placement
check-worker-placement: $(BUILD_DIR)/tests/worker_placement_integration
	$(BUILD_DIR)/tests/worker_placement_integration
$(BUILD_DIR)/tests/worker_placement_integration: $(BUILD_DIR)/tests/worker_placement_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/worker_placement_integration.d
