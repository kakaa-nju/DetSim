/* Same fixture, statically linked. The static startup reaches its boundary
 * at mprotect (RuntimeProfile startup_syscall=10), and the flushed printfs
 * then exercise the same write sequence across process restarts.
 */
#include <stdio.h>

int main(void)
{
    printf("one\n");
    fflush(stdout);
    printf("two\n");
    fflush(stdout);
    printf("three\n");
    return 0;
}
