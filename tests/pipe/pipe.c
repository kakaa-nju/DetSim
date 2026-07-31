#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

int pipefd[2];

void *worker(void *arg) {
    (void)arg;

    const char *msg = "hello";
    write(pipefd[1], msg, strlen(msg));

    return NULL;
}

int main(void) {
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return 1;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, worker, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(pipefd[0], &rfds);

    printf("waiting...\n");

    int ret = pselect(
        pipefd[0] + 1,
        &rfds,
        NULL,
        NULL,
        NULL,
        NULL);

    if (ret < 0) {
        perror("pselect");
        return 1;
    }

    if (FD_ISSET(pipefd[0], &rfds)) {
        char buf[128];
        int n = read(pipefd[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("recv: %s\n", buf);
        }
    }

    pthread_join(tid, NULL);

    close(pipefd[0]);
    close(pipefd[1]);

    return 0;
}