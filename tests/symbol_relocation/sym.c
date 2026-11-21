/*
 * Symbol-resolution fixture.
 *
 * `g_marker` holds a known value from exec time, so any consumer of the symbol
 * service (the Assertions surface and the `p` command) proves that the tracer
 * located the global at its *runtime* address: reading a stale link-time vaddr
 * lands outside the captured regions or returns garbage.
 */
#include <unistd.h>

volatile long g_marker = 4660; /* 0x1234 */

int main(void)
{
  for (int i = 0; i < 3; ++i)
  {
    /* Keep the global live and give the tracer several syscalls to step over. */
    (void)!write(STDOUT_FILENO, "x", 1);
    g_marker += 0;
  }
  return 0;
}
