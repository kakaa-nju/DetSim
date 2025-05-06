#ifndef __DEBUG_H__
#define __DEBUG_H__

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>

#ifdef DEBUG
extern FILE *LOG;
# define _Log(...) \
  do { \
    fprintf(LOG, __VA_ARGS__); \
  } while (0)
#else
# define LOG stdin
# define _Log(...) 
#endif

#define Log(format, ...) \
    _Log("\33[1;34m[%s,%d,%s] " format "\33[0m\n", \
        __FILE__, __LINE__, __func__, ## __VA_ARGS__)

#define Assert(cond, ...) \
  do { \
    if (!(cond)) { \
      fflush(LOG); \
      fprintf(stderr, "\33[1;31m"); \
      fprintf(stderr, __VA_ARGS__); \
      fprintf(stderr, "\33[0m\n"); \
      assert(cond); \
    } \
  } while (0)

#define panic(...) Assert(0, __VA_ARGS__)

#define TODO() panic("please implement me")

static inline void *Lmalloc(size_t size) {
  /* Log("malloc: %d bytes", size); */
  return malloc(size);
}

static inline void *Lrealloc(void *raw, size_t size) {
  /* Log("realloc: %d bytes", size); */
  return realloc(raw, size);
}


#endif
