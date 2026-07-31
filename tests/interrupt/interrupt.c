#define _GNU_SOURCE

#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static int fut = 0;

static int futex_wait(int *addr, int val) {
    return syscall(SYS_futex,
                   addr,
                   FUTEX_WAIT,
                   val,
                   NULL,
                   NULL,
                   0);
}

void *worker(void *arg) {
    (void)arg;

    printf("worker: waiting on futex\n");
    fflush(stdout);

    futex_wait(&fut, 0);

    printf("worker: resumed\n");
    return NULL;
}

int main() {
    pthread_t th;

    pthread_create(&th, NULL, worker, NULL);

    sleep(1);

    printf("main: enter infinite loop\n");
    fflush(stdout);

    while (1) {
        sleep(1);
    }

    return 0;
}