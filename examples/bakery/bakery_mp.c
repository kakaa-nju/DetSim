/* #include <assert.h> */
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
/* #define USE_CHOOSING */

#define max(a, b) ((a > b) ? (a) : (b))
long critical;

int lk;

int Read(int index) {
  int ret;
  lseek(lk, 4 * index, SEEK_SET);
  read(lk, &ret, 4);
  return ret;
}

void Write(int index, int value) {
  lseek(lk, 4 * index, SEEK_SET);
  write(lk, &value, 4);
}

void bakery_lock(int arg) {
  int i = arg;
#ifdef USE_CHOOSING
  Write(2 + i, 1);
#endif
  int number0 = Read(0), number1 = Read(1);
  Write(i, max(number0, number1) + 1);
#ifdef USE_CHOOSING
  Write(2 + i, 0);
#endif
  for (int j = 0; j < 2; j++) {
    if (j == i) continue;
#ifdef USE_CHOOSING
L2:;
    int Choosingj = Read(2 + j);
    if (Choosingj == 1) goto L2;
#endif
L3:;
    int numberj = Read(j), numberi = Read(i);
    if (numberj != 0 && (numberj < numberi || (numberj == numberi && j < i))) goto L3;
  }
}

void bakery_unlock(int arg) {
  Write(arg, 0);
}

void bakery(int me) {
  while (1) {
    bakery_lock(me);
    critical = 1;
    sched_yield();
    /* putchar("()"[me]); */
    critical = 0;
    bakery_unlock(me);
  }
}

int main(int argc, char *argv[]) {
  lk = open("lock", O_RDWR);
  bakery(atoi(argv[1]));
  close(lk);
}
