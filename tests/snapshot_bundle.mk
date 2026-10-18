.PHONY: check-snapshot-bundle
check-snapshot-bundle: $(BUILD_DIR)/tests/snapshot_bundle_integration
	timeout --kill-after=5s 90s $<
$(BUILD_DIR)/tests/snapshot_bundle_integration: $(BUILD_DIR)/tests/snapshot_bundle_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) -Wl,--wrap=fsync $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/snapshot_bundle_integration.d $(BUILD_DIR)/src/core/state/snapshot_bundle.d
