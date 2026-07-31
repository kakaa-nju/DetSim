/*
 * ckpt_policy.h - Configurable checkpoint policy
 *
 * Determines whether a syscall should trigger a checkpoint (CKPT_YES) or not
 * (CKPT_NO). Default is CKPT_NO for all syscalls. The config file can set
 * specific syscalls to CKPT_YES. The CLI can temporarily override individual
 * syscalls, set all to YES, or restore the config values.
 */

#ifndef __CKPT_POLICY_H
#define __CKPT_POLICY_H

#include "handlers.h"
#include <cstddef>

/* Number of syscalls (matches syscalls[450]) */
#define CKPT_MAX_SYSCALLS 450

class CkptPolicy
{
  public:
  CkptPolicy();

  /* Resolve a syscall name to its number, or -1 if unknown */
  static int resolve_syscall(const char *name);

  /* --- Config baseline (never modified by CLI) --- */
  void config_set_yes(int nr);
  void config_set_no(int nr);
  bool config_is_yes(int nr) const;

  /* --- CLI temporary overrides --- */
  void cli_set(int nr, bool yes);
  void cli_all_yes();
  void cli_restore();

  /* --- Query --- */
  bool is_ckpt_yes(int nr) const;

  /* Print current policy state */
  void dump() const;

  private:
  bool config_yes_[CKPT_MAX_SYSCALLS];
  bool current_yes_[CKPT_MAX_SYSCALLS];
};

/* Global checkpoint policy instance */
extern CkptPolicy g_ckpt_policy;

/* Return the policy-based CKPT code (CKPT_YES or CKPT_NO) for a syscall.
 * Handlers use this instead of hardcoded CKPT_YES/CKPT_NO returns. */
static inline int ckpt_for_syscall(int nr)
{
  return g_ckpt_policy.is_ckpt_yes(nr) ? CKPT_YES : CKPT_NO;
}

#endif /* __CKPT_POLICY_H */
