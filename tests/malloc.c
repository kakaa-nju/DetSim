#include <stdio.h>
#include <stdlib.h>

void *ptr[16];
int main() {
  setbuf(stdout, NULL);
  for (int i = 0; i < 20; i++) {
    ptr[i] = malloc(1 << i);
    printf("hello");
  }
  for (int i = 0; i < 20; i++) {
    free(ptr[i]);
    printf("hello");
  }
}
