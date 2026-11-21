# Examples Makefile
# Build all example programs

EXAMPLES = brk bt cast_test complex expr_test fs interrupt loop loop_inf malloc member mt pipe simple_complex write

.PHONY: all clean regress $(EXAMPLES)

all: $(EXAMPLES)

# One-command regression: builds the NP=1/2/3 tracers and every fixture, runs
# the dedicated suites and all generic configs. See run_all.sh for details.
regress:
	./run_all.sh

$(EXAMPLES):
	@echo "Building $@..."
	$(MAKE) -C $@

clean:
	@for dir in $(EXAMPLES); do \
		echo "Cleaning $$dir..."; \
		$(MAKE) -C $$dir clean; \
	done
