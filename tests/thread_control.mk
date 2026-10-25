# Run with make -f Makefile -f tests/thread_control.mk NP=1 check-thread-control.
.PHONY: check-thread-control check-pending-status-reader
check-thread-control: $(BUILD_DIR)/tests/thread_control_integration
	$(BUILD_DIR)/tests/thread_control_integration

check-pending-status-reader: $(BUILD_DIR)/tests/pending_status_reader_integration
	$(BUILD_DIR)/tests/pending_status_reader_integration

$(BUILD_DIR)/tests/thread_control_integration: $(BUILD_DIR)/tests/thread_control_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=ptrace -Wl,--wrap=readdir -Wl,--wrap=closedir -o $@

$(BUILD_DIR)/tests/pending_status_reader_integration: $(BUILD_DIR)/tests/pending_status_reader_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=open -Wl,--wrap=lseek -Wl,--wrap=read -Wl,--wrap=close -Wl,--wrap=opendir -Wl,--wrap=rewinddir -Wl,--wrap=readdir -Wl,--wrap=closedir -o $@

-include $(BUILD_DIR)/tests/thread_control_integration.d
-include $(BUILD_DIR)/tests/pending_status_reader_integration.d
