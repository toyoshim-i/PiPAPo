/*
 * test_poll.c — User-space test for ppoll syscall
 *
 * Exercises the ppoll_time64 syscall (SYS_PPOLL_TIME64 = 414) through the
 * full SVC → syscall_dispatch → do_ppoll path.
 *
 * Tests:
 *   1. Non-blocking poll on stdout (fd 1) → POLLOUT should be set
 *   2. Non-blocking poll on stdin  (fd 0) → should succeed (>= 0)
 *   3. Invalid fd (99) → POLLNVAL in revents
 */

#include "utest.h"

int main(void)
{
    struct pollfd pfd;

    /* 1. Non-blocking poll on stdout — POLLOUT should always be set */
    pfd.fd = 1;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int64_t ts64[2] = {0, 0};   /* zero timeout = non-blocking */
    int ret = ppoll(&pfd, 1, ts64, (void *)0, 0);
    UT_ASSERT(ret >= 0, "ppoll stdout should succeed");
    UT_ASSERT(pfd.revents & POLLOUT, "stdout should be writable");

    /* 2. Non-blocking poll on stdin — should return >= 0 */
    pfd.fd = 0;
    pfd.events = POLLIN;
    pfd.revents = 0;
    ts64[0] = 0; ts64[1] = 0;
    ret = ppoll(&pfd, 1, ts64, (void *)0, 0);
    UT_ASSERT(ret >= 0, "ppoll stdin non-blocking should succeed");

    /* 3. Invalid fd should return POLLNVAL */
    pfd.fd = 99;
    pfd.events = POLLIN;
    pfd.revents = 0;
    ts64[0] = 0; ts64[1] = 0;
    ret = ppoll(&pfd, 1, ts64, (void *)0, 0);
    UT_ASSERT(ret > 0, "ppoll bad fd returns ready count > 0");
    UT_ASSERT(pfd.revents & POLLNVAL, "ppoll bad fd sets POLLNVAL");

    UT_SUMMARY("test_poll");
}
