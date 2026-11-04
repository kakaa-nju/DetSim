#ifndef __EMU_H
#define __EMU_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

struct syscall_info;

/* Inputs a choose_<syscall> plugin may inspect for one enumerated outcome.
 *
 * The historical gettimeofday view stays first so plugins built against the
 * old header keep reading the same offset. The generic syscall fields are
 * appended after it and are populated for every choice point. */
typedef struct choose_in
{
  /* Historical gettimeofday view. Kept first so plugins built against the old
   * header keep reading the same offset. */
  struct timeval gettimeofday_now;

  int nr;           /* syscall number */
  int pid;          /* physical tid used to access guest memory */
  int node;         /* process index */
  uint64_t args[6]; /* raw syscall arguments at the choice point */

  choose_in() : gettimeofday_now{}, nr(-1), pid(-1), node(-1), args{} {}

  choose_in(struct timeval tv) : choose_in() { gettimeofday_now = tv; }

  choose_in(int syscall_nr, int process, int node_id, const uint64_t a[6],
            struct timeval tv)
      : choose_in(tv)
  {
    nr = syscall_nr;
    pid = process;
    node = node_id;
    for (int i = 0; i < 6; i++)
      args[i] = a[i];
  }
} choose_in;

/* Values one enumerated outcome writes back; len[i] == 0 means "unmodified". */
typedef struct choose_out
{
  void *args[6] = {};
  int len[6] = {};
  int rval = 0;

  choose_out() = default;

  ~choose_out()
  {
    for (int i = 0; i < 6; i++)
    {
      if (len[i])
        free(args[i]);
    }
  }
} choose_out;

typedef choose_out *(*choose_func)(int pid, int choice, const choose_in &in);

/* Per-syscall outcomes of the configured ChoosePoint entries. */
extern int choose_many[450];
/* Resolved choose_<syscall> symbols of the configured ChooseFunc plugin. */
extern choose_func choose_syswhat[450];

/* True when detsim has a value- or candidate-materialization path for this
 * syscall. Configuring a choice point for an unsupported syscall is rejected. */
bool emu_syscall_choice_supported(int nr);

/* True when the default result of this syscall is produced by
 * emu_process_syscall(), so a plugin outcome can safely override it. */
bool emu_syscall_uses_generic_choice(int nr);

/* Apply a user-provided value branch: write each non-zero len[i] buffer to the
 * guest address in syscall argument i and return the branch's rval. */
long emu_apply_choose_out(int pid, const syscall_info &info, const choose_out &out);

int emu_gettimeofday(struct timeval *tv, struct timezone *tz);

#endif /* __EMU_H */
