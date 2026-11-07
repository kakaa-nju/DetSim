# pselect6/poll semantics regression

`select_poll_semantics.c` checks the modeled boundary conditions that the
readiness-wait fix depends on:

* pselect6 and poll preserve output bytes/fields outside the requested range;
* `nfds=0` leaves every fd_set untouched;
* a timeout clears only the requested fd_set bytes/revents;
* invalid descriptors report `EBADF`/`POLLNVAL`;
* read-only guest output buffers report `EFAULT`, matching Linux;
* a valid temporary pselect6 sigmask is accepted, while a wrong size or an
  unreadable mask reports `EINVAL`/`EFAULT`.

`check.sh` builds the target, runs automatic exploration, and requires every
line to report PASS.
