#pragma once
#include <cstddef>
#include <sys/stat.h>
#include <poll.h>
#include <cstdint>
#include <sys/uio.h>

// One virtual descriptor boundary for files, stdio, pipes, sockets and epoll.
long emu_read(int fd, void* destination, size_t count);
long emu_write(int fd, const void* source, size_t count);
long emu_writev(int fd, const struct iovec* guest, size_t raw_count);
long emu_close(int fd);
long emu_fstat(int fd, struct stat* destination);
int emu_fcntl(int fd, int command, long argument);
int emu_ftruncate(int fd, int64_t length);
int emu_fsync(int fd);
int emu_fdatasync(int fd);
int emu_ioctl(int fd, unsigned long request, void* argument);

long emu_poll(struct pollfd* fds, uint32_t count, int timeout);

/* Readiness scan for pselect6/select over virtual descriptors. Guest fd_sets are
 * staged in and rewritten with the ready subset; returns the ready count or a
 * negative errno without touching the guest sets. */
long emu_select_scan(int nfds, uintptr_t readfds, uintptr_t writefds,
                     uintptr_t exceptfds);

/* Clear the staged guest fd_sets after a pselect6 timeout. */
void emu_select_clear(int nfds, uintptr_t readfds, uintptr_t writefds,
                      uintptr_t exceptfds);
