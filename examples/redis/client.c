#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define REDIS_HOST "192.168.0.1"
#define REDIS_PORT 6379
#define BUFFER_SIZE 1024

int build_resp(char *buf, const char **args, int argc) {
    int pos = 0;
    pos += sprintf(buf + pos, "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        pos += sprintf(buf + pos, "$%zu\r\n%s\r\n", strlen(args[i]), args[i]);
    }
    return pos;
}

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(REDIS_PORT)
    };
    inet_pton(AF_INET, REDIS_HOST, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    char send_buf[BUFFER_SIZE], recv_buf[BUFFER_SIZE];
    int len;
    const char *set_cmd[] = {"SET", "x", "1"};
    const char *get_cmd[] = {"GET", "x"};

    // SET x 1
    len = build_resp(send_buf, set_cmd, 3);
    write(sock, send_buf, len);
    
    memset(recv_buf, 0, BUFFER_SIZE);
    read(sock, recv_buf, BUFFER_SIZE - 1);
    printf("SET response: %s", recv_buf);

    // GET x
    len = build_resp(send_buf, get_cmd, 2);
    write(sock, send_buf, len);
    
    memset(recv_buf, 0, BUFFER_SIZE);
    read(sock, recv_buf, BUFFER_SIZE - 1);
    printf("GET response: %s", recv_buf);

    close(sock);
    return 0;
}
