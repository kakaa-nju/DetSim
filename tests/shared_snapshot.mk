.PHONY: check-shared-snapshot
check-shared-snapshot: $(BUILD_DIR)/tests/shared_snapshot_integration
	timeout --kill-after=5s 90s $<
$(BUILD_DIR)/tests/shared_snapshot_integration: $(BUILD_DIR)/tests/shared_snapshot_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) -Wl,--wrap=fsync \
	    -Wl,--wrap=_ZNK12SnapshotView4pageEmm \
	    -Wl,--wrap=_ZN6detsim8snapshot16SharedObjectPool12publish_blobEmPKvm \
	    -Wl,--wrap=_ZN6detsim8snapshot16SharedObjectPool13publish_readyEmRKSt6vectorIhSaIhEE \
	    -Wl,--wrap=_ZN13SysStateStore4saveEmRKSt6vectorIhSaIhEE $(LDLIBS) -o $@
-include $(BUILD_DIR)/tests/shared_snapshot_integration.d
