.PHONY: check-shared-snapshot
check-shared-snapshot: $(BUILD_DIR)/tests/shared_snapshot_integration
	timeout --kill-after=5s 90s $<
$(BUILD_DIR)/tests/shared_snapshot_integration: $(BUILD_DIR)/tests/shared_snapshot_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) -Wl,--wrap=fsync $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/shared_snapshot_integration.d
