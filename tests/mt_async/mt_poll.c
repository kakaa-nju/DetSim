#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static int pfd[2];

static void *worker(void *arg)
{
    (void)arg;
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
    write(pfd[1], "x", 1);
    return NULL;
}

int main(void)
{
    if (pipe(pfd) != 0) return 2;
    pthread_t tid;
    if (pthread_create(&tid, NULL, worker, NULL) != 0) return 2;

    struct pollfd p = {pfd[0], POLLIN, 0};
    if (poll(&p, 1, -1) != 1) return 3;

    char c;
    if (read(pfd[0], &c, 1) != 1) return 4;
    pthread_join(tid, NULL);
    write(1, "done\n", 5);
    return c == 'x' ? 0 : 5;
}
