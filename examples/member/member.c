#include "member.h"
#include <unistd.h>

struct a *test_member = 0;

int main() {
  struct a tmp;
  tmp.c.s = 114514;
  tmp.b = 42;
  test_member = &tmp;
  write(1, "hello", 5);
}

// assertion "tracee0:a->c.s == 114514"


