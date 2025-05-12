#ifndef __EMU_H
#define __EMU_H

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
typedef struct choose_in
{
  struct timeval now;
  choose_in(struct timeval tv) { now = tv; }
} choose_in;

typedef struct choose_out
{
  void* args[6];
  int len[6]; /* 0 represents for unmodified */
  int rval;

  choose_out() { memset(this, 0, sizeof(*this)); }

  ~choose_out()
  {
    for (int i = 0; i < 6; i++)
    {
      if (len[i])
        free(args[i]);
    }
  }
} choose_out;

typedef choose_out* (*choose_func)(int pid, int choice, const choose_in& in);

extern choose_func choose_syswhat[450];
extern int choose_many[450];

int emu_gettimeofday(struct timeval* tv, struct timezone* tz);

#endif
