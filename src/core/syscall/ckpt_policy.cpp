/*
 * ckpt_policy.cpp - Configurable checkpoint policy implementation
 */

#include "ckpt_policy.h"
#include "guest.h"
#include "ui/log_wrapper.h"
#include <cstring>

CkptPolicy g_ckpt_policy;

CkptPolicy::CkptPolicy()
{
  for (int i = 0; i < CKPT_MAX_SYSCALLS; i++)
  {
    config_yes_[i] = false;
    current_yes_[i] = false;
  }
}

int CkptPolicy::resolve_syscall(const char *name)
{
  if (!name)
    return -1;

  for (int i = 0; i < CKPT_MAX_SYSCALLS; i++)
  {
    if (syscalls[i] && strcmp(syscalls[i], name) == 0)
      return i;
  }
  return -1;
}

void CkptPolicy::config_set_yes(int nr)
{
  if (nr < 0 || nr >= CKPT_MAX_SYSCALLS)
    return;
  config_yes_[nr] = true;
  current_yes_[nr] = true;
}

void CkptPolicy::config_set_no(int nr)
{
  if (nr < 0 || nr >= CKPT_MAX_SYSCALLS)
    return;
  config_yes_[nr] = false;
  current_yes_[nr] = false;
}

bool CkptPolicy::config_is_yes(int nr) const
{
  if (nr < 0 || nr >= CKPT_MAX_SYSCALLS)
    return false;
  return config_yes_[nr];
}

void CkptPolicy::cli_set(int nr, bool yes)
{
  if (nr < 0 || nr >= CKPT_MAX_SYSCALLS)
    return;
  current_yes_[nr] = yes;
}

void CkptPolicy::cli_all_yes()
{
  for (int i = 0; i < CKPT_MAX_SYSCALLS; i++)
    current_yes_[i] = true;
}

void CkptPolicy::cli_restore()
{
  for (int i = 0; i < CKPT_MAX_SYSCALLS; i++)
    current_yes_[i] = config_yes_[i];
}

bool CkptPolicy::is_ckpt_yes(int nr) const
{
  if (nr < 0 || nr >= CKPT_MAX_SYSCALLS)
    return false;
  return current_yes_[nr];
}

void CkptPolicy::dump() const
{
  int count = 0;
  for (int i = 0; i < CKPT_MAX_SYSCALLS; i++)
  {
    if (current_yes_[i] && syscalls[i])
    {
      detsim::ui::ui_printf("  ckpt %s (nr=%d)\n", syscalls[i], i);
      count++;
    }
  }
  if (count == 0)
  {
    detsim::ui::ui_printf("  (no syscalls are set to CKPT_YES)\n");
  }
  else
  {
    detsim::ui::ui_printf("  %d syscall(s) set to CKPT_YES\n", count);
  }
}
