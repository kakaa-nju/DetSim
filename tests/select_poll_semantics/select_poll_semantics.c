#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int failures;
static void report(const char *name, int ok)
{
    char line[160];
    int n = snprintf(line, sizeof(line), "%s: %s\n", name, ok ? "PASS" : "FAIL");
    write(1, line, n);
    if (!ok) failures++;
}

int main(void)
{
    int p[2];
    if (pipe(p) != 0) return 2;
    unsigned char set[128];
    struct timespec ten_ms = {0, 10000000};

    /* Linux only reports bytes covering nfds; the tail of the fd_set is not
     * part of the pselect6 request and must survive a timeout. */
    memset(set, 0xaa, sizeof(set));
    set[0] = (unsigned char)(1u << p[0]);
    errno = 0;
    long r = syscall(SYS_pselect6, p[0] + 1, set, NULL, NULL, &ten_ms, NULL);
    report("pselect timeout leaves bytes past the requested range",
           r == 0 && set[0] == 0 && set[8] == 0xaa && set[127] == 0xaa);

    memset(set, 0xaa, sizeof(set));
    r = syscall(SYS_pselect6, 0, set, NULL, NULL, &ten_ms, NULL);
    report("pselect nfds=0 leaves fd_sets untouched",
           r == 0 && set[0] == 0xaa && set[127] == 0xaa);

    char byte = 'x';
    if (write(p[1], &byte, 1) != 1) return 2;
    memset(set, 0xaa, sizeof(set));
    set[0] = (unsigned char)(1u << p[0]);
    struct timespec zero = {0, 0};
    r = syscall(SYS_pselect6, p[0] + 1, set, NULL, NULL, &zero, NULL);
    report("pselect readiness writes only the requested range",
           r == 1 && set[0] == (unsigned char)(1u << p[0]) &&
           set[8] == 0xaa && set[127] == 0xaa);

    memset(set, 0xaa, sizeof(set));
    r = syscall(SYS_pselect6, 0, set, NULL, NULL, &zero, NULL);
    report("pselect nfds=0 zero timeout leaves fd_sets untouched",
           r == 0 && set[0] == 0xaa && set[127] == 0xaa);

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(p[1], &wfds);
    r = syscall(SYS_pselect6, p[1] + 1, NULL, &wfds, NULL, &zero, NULL);
    report("pselect writefds reports writable pipe",
           r == 1 && FD_ISSET(p[1], &wfds));

    struct sigset_argpack { const sigset_t *ss; size_t ss_len; };
    sigset_t sigmask;
    sigemptyset(&sigmask);
    struct sigset_argpack wrapper = {&sigmask, 8};
    r = syscall(SYS_pselect6, 0, NULL, NULL, NULL, &zero, &wrapper);
    report("pselect valid temporary sigmask is accepted", r == 0);
    wrapper.ss_len = 16;
    errno = 0;
    r = syscall(SYS_pselect6, 0, NULL, NULL, NULL, &zero, &wrapper);
    report("pselect wrong sigmask size reports EINVAL", r == -1 && errno == EINVAL);
    wrapper.ss = (const sigset_t *)0x1234;
    wrapper.ss_len = 8;
    errno = 0;
    r = syscall(SYS_pselect6, 0, NULL, NULL, NULL, &zero, &wrapper);
    report("pselect unreadable sigmask reports EFAULT", r == -1 && errno == EFAULT);

    struct pollfd pf = {p[0], POLLIN, 0x1234};
    if (read(p[0], &byte, 1) != 1) return 2;
    r = poll(&pf, 1, 10);
    report("poll timeout clears revents only",
           r == 0 && pf.revents == 0 && pf.fd == p[0] && pf.events == POLLIN);
    if (write(p[1], &byte, 1) != 1) return 2;
    pf.revents = 0x1234;
    r = poll(&pf, 1, 0);
    report("poll readiness writes revents only",
           r == 1 && (pf.revents & POLLIN) != 0 && pf.fd == p[0] && pf.events == POLLIN);

    struct pollfd bad = {p[0] + 1000, POLLIN, 0x1234};
    r = poll(&bad, 1, 0);
    report("poll invalid descriptor reports POLLNVAL",
           r == 1 && (bad.revents & POLLNVAL) != 0);

    fd_set invalid;
    FD_ZERO(&invalid);
    FD_SET(p[0] + 1000, &invalid);
    errno = 0;
    r = syscall(SYS_pselect6, p[0] + 1001, &invalid, NULL, NULL, &zero, NULL);
    report("pselect invalid descriptor reports EBADF",
           r == -1 && errno == EBADF);

    long page = sysconf(_SC_PAGESIZE);
    void *ro = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ro == MAP_FAILED) return 2;
    unsigned char *roset = ro;
    memset(roset, 0, 128);
    roset[0] = (unsigned char)(1u << p[0]);
    struct pollfd *ropf = ro;
    ropf[0].fd = p[0] + 1000;
    ropf[0].events = POLLIN;
    ropf[0].revents = 0;
    if (mprotect(ro, page, PROT_READ) != 0) return 2;

    errno = 0;
    r = syscall(SYS_pselect6, p[0] + 1, roset, NULL, NULL, &zero, NULL);
    report("pselect read-only fd_set reports EFAULT",
           r == -1 && errno == EFAULT);

    errno = 0;
    r = poll(ropf, 1, 0);
    report("poll read-only pollfd reports EFAULT",
           r == -1 && errno == EFAULT);

    return failures ? 1 : 0;
}
