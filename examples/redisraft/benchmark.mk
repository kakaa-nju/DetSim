.PHONY: benchmark-redisraft
.PHONY: benchmark-redisraft-parallel
benchmark-redisraft-parallel: tracer $(BUILD_DIR)/examples/redisraft/choose.so $(BUILD_DIR)/examples/redisraft/check.so $(BUILD_DIR)/examples/redisraft/seed.so
	@test "$(NP)" = 3 || { echo 'benchmark-redisraft-parallel requires NP=3' >&2; exit 1; }
	python3 examples/redisraft/parallel_benchmark.py --tracer ./tracer --plugin-dir $(BUILD_DIR)/examples/redisraft $(REDISRAFT_PARALLEL_ARGS)
# Existing workload binary only; never rebuild or modify the Raft submodule.
benchmark-redisraft: tracer $(BUILD_DIR)/examples/redisraft/choose.so $(BUILD_DIR)/examples/redisraft/check.so $(BUILD_DIR)/examples/redisraft/seed.so
	python3 examples/redisraft/benchmark.py --tracer ./tracer --choose $(BUILD_DIR)/examples/redisraft/choose.so --check $(BUILD_DIR)/examples/redisraft/check.so --seed-shim $(BUILD_DIR)/examples/redisraft/seed.so --depth 8 $(REDISRAFT_BENCH_ARGS)

$(BUILD_DIR)/examples/redisraft/choose.so: examples/redisraft/plugins/choose.cpp examples/raft/plugins/choose_common.hpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -shared -fPIC -MMD -MP -MF $@.d $< -o $@
$(BUILD_DIR)/examples/redisraft/check.so: examples/redisraft/plugins/check.cpp examples/raft/plugins/check_common.hpp src/api/plugin.h $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CXX) $(PROJECT_CPPFLAGS) $(CPPFLAGS) $(CXXFLAGS) -shared -fPIC -MMD -MP -MF $@.d $< -o $@
-include $(BUILD_DIR)/examples/redisraft/choose.so.d $(BUILD_DIR)/examples/redisraft/check.so.d

$(BUILD_DIR)/examples/redisraft/seed.so: examples/redisraft/seed.c $(BUILD_DIR)/.config
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -shared -fPIC $< -ldl -o $@
