# Multithreaded readiness-wait regression

`mt_async` and `mt_poll` start a helper thread that sleeps and then writes to a
pipe, while the main thread suspends in `pselect6`/`poll` with an unbounded
timeout.  A deterministic engine must wake the main thread when the helper
writes; otherwise every thread is suspended and automatic exploration treats a
reachable completion as a deadlocked leaf.

`check.sh` drives the scheduler manually: step the main thread until it blocks,
advance the helper through its sleep, let it write, and require the waiter to be
reported as ready.
