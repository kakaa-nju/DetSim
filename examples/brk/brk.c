#include <stdlib.h>
#include <malloc.h>

int main() {
  /* mallopt(M_MMAP_THRESHOLD, 0); */
  for (int i = 0; i < 1024; i++) {
    void *p = malloc(1024);
  }
}
