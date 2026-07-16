# Examples Makefile
# Build all example programs

EXAMPLES = bakery brk bt cast_test complex devices expr_test fs loop loop_inf malloc member pipe simple_complex write

.PHONY: all clean $(EXAMPLES)

all: $(EXAMPLES)

$(EXAMPLES):
	@echo "Building $@..."
	$(MAKE) -C $@

clean:
	@for dir in $(EXAMPLES); do \
		echo "Cleaning $$dir..."; \
		$(MAKE) -C $$dir clean; \
	done
