.PHONY: check-parallel-bundle
check-parallel-bundle: $(BUILD_DIR)/tests/parallel_bundle_integration
	timeout --kill-after=5s 90s $<
$(BUILD_DIR)/tests/parallel_bundle_integration: $(BUILD_DIR)/tests/parallel_bundle_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/parallel_bundle_integration.d
