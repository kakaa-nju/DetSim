/*
 * userstate.c - Guest program for the UserState plugin test.
 *
 * The program maintains a monotonically increasing global counter and yields
 * at every gettimeofday() call so that DetSim checkpoints it.  The companion
 * plugin (check.cpp) remembers the counter through DetSim's user-state plugin
 * interface and fails the check if the counter ever goes backwards.  Because
 * the counter is restored together with the program state, rewinding to an
 * earlier state must also rewind the plugin's shadow copy.
 */

#include <sys/time.h>

volatile long user_counter = 0;

int main(void)
{
  struct timeval tv;

  for (int i = 0; i < 3; i++)
  {
    user_counter += 1;
    gettimeofday(&tv, 0);
  }
  user_counter -= 1;
  gettimeofday(&tv, 0);
  user_counter += 2;
  gettimeofday(&tv, 0);
  user_counter -= 3;
  gettimeofday(&tv, 0);

  return 0;
}
