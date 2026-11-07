#include <pthread.h>
#include <sys/select.h>
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

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(pfd[0], &rfds);
    if (pselect(pfd[0] + 1, &rfds, NULL, NULL, NULL, NULL) != 1) return 3;

    char c;
    if (read(pfd[0], &c, 1) != 1) return 4;
    pthread_join(tid, NULL);
    write(1, "done\n", 5);
    return c == 'x' ? 0 : 5;
}
