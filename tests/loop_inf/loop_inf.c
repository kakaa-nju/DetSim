#include <unistd.h>
#include <fcntl.h>

char msg[16] = "Hello World  !\n";

int main(int argc, char *argv[]) {
  int fd = open("/a.txt", O_CREAT | O_RDWR, 0664);
  msg[11] = argv[1][0];
  for (int i = 0; ; i++)
  {
    msg[12] = '0' + i % 10;
    write(fd, msg, 15);
  }
  return 0;
}

