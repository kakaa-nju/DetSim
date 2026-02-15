/*
 * syscall_fmt.h - System call string formatting
 */

#ifndef __SYSCALL_FMT_H
#define __SYSCALL_FMT_H

#include "types.h"

/* Forward declarations */
struct tracee_state;
struct syscall_info;

namespace syscall_fmt {

/* Main formatting function - formats a syscall into a human-readable string */
void format(char *buf, tracee_state *t, syscall_info *info);

} /* namespace syscall_fmt */

#endif /* __SYSCALL_FMT_H */
