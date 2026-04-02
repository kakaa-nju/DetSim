#include "emu.h"
#include "guest.h"

static struct timeval tv_addmsec(struct timeval tv, int msec)
{
  long sec = tv.tv_sec;
  long usec = tv.tv_usec;
  usec += msec * 1000;
  sec += usec / 1000000;
  usec %= 1000000;
  return (struct timeval){sec, usec};
}

extern "C"
{
  // Simple choose function for gettimeofday
  // choice 0: return current time
  // choice 1: advance time by a small amount
  choose_out *choose_gettimeofday(int pid, int choice, const choose_in &in)
  {
    struct timeval tv = in.now;
    struct timeval *tv_out = (struct timeval *)malloc(sizeof(*tv_out));

    if (choice == 0)
      *tv_out = tv;
    else
      *tv_out = tv_addmsec(tv, 10);  // Advance 10ms

    choose_out *out = new choose_out();
    out->args[0] = tv_out;
    out->len[0] = sizeof(struct timeval);
    out->rval = 0;
    return out;
  }
}
