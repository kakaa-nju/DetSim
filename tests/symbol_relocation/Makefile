CFLAGS = -gdwarf-4 -g

.PHONY: all clean

all: sym_pie sym_pie_stripped sym_exec

# Default image: PIE *with* DWARF, so the global resolves through global_vars.
sym_pie: sym.c
	$(CC) $(CFLAGS) $< -o $@

# Same PIE without debug sections: resolution must fall back to the ELF symbol
# table, which only works if the main image is loaded at its link-time vaddrs
# and then rebased onto the tracee's load bias.
sym_pie_stripped: sym_pie
	cp sym_pie $@
	strip --strip-debug $@

# Non-PIE image: the loader keeps link-time addresses, so the bias stays zero.
sym_exec: sym.c
	$(CC) $(CFLAGS) -no-pie $< -o $@

clean:
	rm -f sym_pie sym_pie_stripped sym_exec
