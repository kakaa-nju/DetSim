/* bakery_lock is too long with ~50 instructions, making it hard to     *
 * INSTRUCTION LEVEL model check it. bakery_mp.c provide a multiprocess *
 * version of this algorithm. It uses `lock` file as "shared memory",   * 
 * which in itself act like some kind of atomic register.               *
 * In fact, bakery algorithm highly depends on register's atomicity and *
 * is very sensible to compiler intstruction reordering and CPU out-of- *
 * order execution.                                                     */

/* In this example, any compile optimization (e.g. -O1) will lead to    *
 * liveness problem. */
   
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#define USE_CHOOSING

#define max(a, b) ((a > b) ? (a) : (b))
volatile long critical[2];

int number[2];
bool entering[2];

void bakery_lock(int arg) {
  int i = arg;
#ifdef USE_CHOOSING
  entering[i] = true;
#endif
  number[i] = max(number[0], number[1]) + 1;
#ifdef USE_CHOOSING
  entering[i] = false;
#endif
  for (int j = 0; j < 2; j++) {
    if (j == i) continue;
#ifdef USE_CHOOSING
    while (entering[j] == true);
#endif
    while (number[j] != 0 && (number[j] < number[i] || (number[j] == number[i] && j < i)));
  }
}

void bakery_unlock(int arg) {
  number[arg] = 0;
}

void *bakery(void *me) {
  long i = (long)me;
  while (1) {
    bakery_lock(i);
    for (int j = 0; j < 100; j++) {
      putchar('a' + i);
      fsync(1);
    }
    putchar('\n');
    bakery_unlock(i);
  }
}

int main(int argc, char *argv[]) {
  setbuf(stdout, NULL);
  pthread_t p[2];
  pthread_create(&p[0], NULL, bakery, (void *)0);
  pthread_create(&p[1], NULL, bakery, (void *)1);

  pthread_join(p[0], NULL);
  pthread_join(p[1], NULL);
}
