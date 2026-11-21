/*
 * userstate.c - Guest program for the UserState plugin test.
 *
 * The program exposes one monotone global counter and yields at every
 * gettimeofday(), so DetSim checkpoints each value. The companion plugin
 * (check.cpp) remembers the counter through DetSim's user-state plugin
 * interface and reports a failure if the counter ever goes backwards.
 *
 * The fixture is a controlled pair, selected by the first argument:
 *
 *   ./userstate 0 good      counter only increases; the checker must stay silent
 *   ./userstate 0 violate   the counter is driven backwards, which is exactly
 *                           the oracle breach the checker has to report
 *
 * Both runs use the same binary and the same plugin, so the only difference
 * between the two outcomes is the program's own behaviour. tests/userstate/
 * check.sh asserts each expectation; neither direction may be inferred from a
 * single run.
 *
 * Because the counter is restored together with the program state, rewinding to
 * an earlier state must also rewind the plugin's shadow copy -- rewind.c covers
 * that separately.
 */

#include <string.h>
#include <sys/time.h>

volatile long user_counter = 0;

/* Advance the counter and yield, so the new value becomes a checkpoint. */
static void bump(long delta)
{
  struct timeval tv;
  user_counter += delta;
  gettimeofday(&tv, 0);
}

int main(int argc, char **argv)
{
  /* argv[1] is the node id this fixture does not use; argv[2] selects the mode. */
  const int violate = argc > 2 && strcmp(argv[2], "violate") == 0;

  for (int i = 0; i < 3; i++)
    bump(1); /* 1, 2, 3 -- holds for both modes */

  if (violate)
  {
    /* 3 -> 2 -> 4 -> 1: two genuine regressions for the checker to report. */
    bump(-1);
    bump(2);
    bump(-3);
  }

  return 0;
}
