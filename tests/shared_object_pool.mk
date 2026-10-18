.PHONY: check-shared-object-pool
check-shared-object-pool: $(BUILD_DIR)/tests/shared_object_pool_test $(BUILD_DIR)/tests/shared_store_integration
	$(BUILD_DIR)/tests/shared_object_pool_test
	$(BUILD_DIR)/tests/shared_store_integration

$(BUILD_DIR)/tests/shared_object_pool_test: tests/shared_object_pool_test.cpp src/core/state/shared_object_pool.cpp src/core/state/shared_object_pool.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -Wall -Wextra -Werror -MMD -MP -MF $@.d -MT $@ tests/shared_object_pool_test.cpp src/core/state/shared_object_pool.cpp -pthread -lzstd -lxxhash -Wl,--wrap=fsync -Wl,--wrap=write -Wl,--wrap=read -Wl,--wrap=flock -Wl,--wrap=fstat -Wl,--wrap=syncfs -o $@

$(BUILD_DIR)/tests/shared_store_integration: $(BUILD_DIR)/tests/shared_store_integration.o $(TRACER_OBJS_NO_MAIN) $(BUILD_DIR)/nr2call.o
	$(LD) $^ $(LDFLAGS) $(LDLIBS) -Wl,--wrap=fsync -Wl,--wrap=syncfs -Wl,--wrap=_ZN16StateStorePacked8compressERKSt6vectorIhSaIhEE -o $@

-include $(BUILD_DIR)/tests/shared_object_pool_test.d $(BUILD_DIR)/tests/shared_store_integration.d
