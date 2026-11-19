/* Cross-process snapshot reproducibility fixture.
 *
 * Each printf is flushed separately so the tracee offers several write
 * syscalls after the startup boundary: every completed syscall saves a
 * snapshot whose hash must be identical in two independent tracer runs.
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
