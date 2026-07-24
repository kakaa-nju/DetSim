#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

void *child_thread(void *arg)
{
    write(1, "Hello from child thread!\n", 25);
    return NULL;
}

int main()
{
    pthread_t tid;
    struct timespec ts = {0, 100000000};

    pthread_create(&tid, NULL, child_thread, NULL);
    nanosleep(&ts, NULL);
    pthread_join(tid, NULL);

    write(1, "Main thread done.\n", 18);
    return 0;
}
