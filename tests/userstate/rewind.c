/*
 * rewind.c - Guest program for the UserState rewind regression test.
 *
 * Three static peers each expose a monotone global epoch. The program only
 * ever increases it, so the checker in check.cpp may never observe a smaller
 * value. Every gettimeofday() is a checkpoint boundary, which lets DetSim
 * interleave the peers freely during a search: a state is reached by one peer
 * running ahead, and afterwards an earlier state of that same peer is explored
 * again. The plugin shadow copy must follow that rewind, otherwise the stale,
 * higher epoch is reported as a spurious regression.
 */

#include <sys/time.h>

volatile long epoch_a = 0;
volatile long epoch_b = 0;
volatile long epoch_c = 0;

static void advance(volatile long *epoch)
{
  *epoch += 1;
  struct timeval tv;
  gettimeofday(&tv, 0);
}

int main(void)
{
  for (int round = 0; round < 3; round++)
  {
    advance(&epoch_a);
    advance(&epoch_b);
    advance(&epoch_c);
  }
  return 0;
}
