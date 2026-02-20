#include <stdio.h>

int main() {
  FILE *fa = fopen("/a.txt", "w+");
  fwrite("hello\n", 6, 1, fa);
  FILE *fb = fopen("/b.txt", "w+");
  fclose(fa);
  fclose(fb);
}
