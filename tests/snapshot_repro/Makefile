# Snapshot reproducibility fixture.
# Builds the dynamic and static tracees; check.sh runs each twice in two
# independent tracer processes and requires identical saved-hash sequences.

.PHONY: all clean

all: dyn_hello stat_hello

dyn_hello: dyn_hello.c
	$(CC) -gdwarf-4 -g -O0 $< -o $@

stat_hello: stat_hello.c
	$(CC) -static -gdwarf-4 -g -O0 $< -o $@

clean:
	rm -f dyn_hello stat_hello
	rm -rf store1 store2 memory sstate seq1 seq2 sstate1 sstate2 run1.log run2.log
