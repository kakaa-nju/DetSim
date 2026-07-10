#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    if (argc != 2) return 1;

    if (atoi(argv[1]) == 0) {
        int listenfd = socket(AF_INET, SOCK_STREAM, 0);

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9090);
        inet_pton(AF_INET, "192.168.0.1", &addr.sin_addr);

        bind(listenfd, (struct sockaddr*)&addr, sizeof(addr));
        listen(listenfd, 5);

        struct pollfd pfd;
        pfd.fd = listenfd;
        pfd.events = POLLIN;

        printf("waiting for connection...\n");

        while (1) {
            int ret = poll(&pfd, 1, -1);

            if (ret > 0 && (pfd.revents & POLLIN)) {
                printf("listenfd readable\n");

                int connfd = accept(listenfd, NULL, NULL);

                printf("accept = %d\n", connfd);
                char buf[1024];
                int n = recv(connfd, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    printf("recv: %s\n", buf);
                }
                close(connfd);
                break;
            }
        }

        close(listenfd);
    } else {
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        struct sockaddr_in local = {0};
        local.sin_family = AF_INET;
        local.sin_port = htons(10000);
        inet_pton(AF_INET, "192.168.0.2", &local.sin_addr);

        bind(fd, (struct sockaddr*)&local, sizeof(local));

        struct sockaddr_in server = {0};
        server.sin_family = AF_INET;
        server.sin_port = htons(9090);
        inet_pton(AF_INET, "192.168.0.1", &server.sin_addr);

        connect(fd, (struct sockaddr*)&server, sizeof(server));
        char buf[128];
        snprintf(buf, sizeof(buf), "msg %d", 0);
        send(fd, buf, strlen(buf), 0);
        sleep(2);

        close(fd);
    }

    return 0;
}