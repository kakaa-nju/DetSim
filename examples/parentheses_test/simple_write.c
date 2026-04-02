/*
 * simple_write.c - Simple multi-threaded write test
 *
 * Two threads writing different characters to stdout.
 * Used to test DetSim's ability to interleave thread execution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <pthread.h>

static volatile int should_stop = 0;
static volatile int count = 0;

static void *thread_a(void *arg) {
    (void)arg;
    while (!should_stop && count < 100) {
        write(STDOUT_FILENO, "A", 1);
        __sync_fetch_and_add(&count, 1);
        syscall(SYS_sched_yield);
    }
    return NULL;
}

static void *thread_b(void *arg) {
    (void)arg;
    while (!should_stop && count < 100) {
        write(STDOUT_FILENO, "B", 1);
        __sync_fetch_and_add(&count, 1);
        syscall(SYS_sched_yield);
    }
    return NULL;
}

int main(void) {
    pthread_t a, b;

    printf("Starting simple write test\n");

    pthread_create(&a, NULL, thread_a, NULL);
    pthread_create(&b, NULL, thread_b, NULL);

    sleep(1);
    should_stop = 1;

    pthread_join(a, NULL);
    pthread_join(b, NULL);

    printf("\nDone\n");
    return 0;
}
