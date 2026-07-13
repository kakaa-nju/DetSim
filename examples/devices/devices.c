#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int all_zero(const unsigned char *buf, size_t len)
{
  for (size_t i = 0; i < len; i++)
  {
    if (buf[i] != 0)
      return 0;
  }
  return 1;
}

int main(void)
{
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
  {
    perror("open");
    return 1;
  }

  unsigned char buf[32];
  ssize_t n = read(fd, buf, sizeof(buf));
  if (n != (ssize_t)sizeof(buf))
  {
    fprintf(stderr, "read returned %zd\n", n);
    close(fd);
    return 2;
  }

  if (!all_zero(buf, sizeof(buf)))
  {
    fprintf(stderr, "/dev/urandom did not return zeros\n");
    close(fd);
    return 3;
  }

  if (close(fd) != 0)
  {
    perror("close");
    return 4;
  }

  puts("devices test passed");
  return 0;
}
