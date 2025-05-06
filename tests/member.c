#include "member.h"

struct a *test_member = 0;

int main() {
  struct a tmp;
  tmp.c.s = 114514;
  test_member = &tmp;
}

// assertion "tracee0:a->c.s == 114514"


