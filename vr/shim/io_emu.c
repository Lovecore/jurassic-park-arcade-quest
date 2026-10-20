/*
 * io_emu.c — Raw Thrills RIO I/O board emulation.
 *
 * The cabinet's RIO board (USB 0c70:0780) carries coin, start, service and
 * test switches plus analog channels; its library is statically linked
 * (RIO_*). Without it the game shows "RIO Disconnected". We answer at the
 * API level: RIO_Connect marks the board connected without opening USB or
 * starting the reader thread, and report I/O succeeds as a no-op. Switch
 * and ADC reads then come from the (zeroed) report buffer, i.e. "connected,
 * nothing pressed". Later, VR controller input is written straight into
 * that buffer:
 *
 *   connected flag      0x8e7d61c (int)
 *   switch descriptors  0x8e7d2a0 + sw * 13  ([0] enabled, [1..4] offset)
 *   report buffers      0x8e7d880 + idx * 0x1b5, idx = byte at 0x8bb32cb
 *
 * Env: JPVR_RIO=0 disables (real USB path).
 */

#include <stdint.h>
#include <stdlib.h>

#include "shim.h"

#define A_RIO_CONNECT        0x836b128
#define A_RIO_CONNECT_EX     0x836b1ed
#define A_RIO_DISCONNECT     0x836b2a6
#define A_RIO_SEND_REPORT    0x836b33a
#define A_RIO_GET_REPORT     0x836b4bd
#define A_RIO_CONNECTED_FLAG 0x8e7d61c

static volatile int32_t *const rio_connected = (volatile int32_t *)A_RIO_CONNECTED_FLAG;

static int rio_connect(void)
{
    *rio_connected = 1;
    logmsg("[io_emu] RIO_Connect: emulated board connected\n");
    return 0;
}

static int rio_disconnect(void)
{
    *rio_connected = 0;
    return 0;
}

static int rio_report_ok(void *report)
{
    return 0;
}

HIDDEN void install_io_emulation(void)
{
    const char *v = getenv("JPVR_RIO");
    if (v && !atoi(v))
        return;
    patch_jmp(A_RIO_CONNECT, (void *)rio_connect);
    patch_jmp(A_RIO_CONNECT_EX, (void *)rio_connect);
    patch_jmp(A_RIO_DISCONNECT, (void *)rio_disconnect);
    patch_jmp(A_RIO_SEND_REPORT, (void *)rio_report_ok);
    patch_jmp(A_RIO_GET_REPORT, (void *)rio_report_ok);
    logmsg("[io_emu] RIO emulation installed\n");
}
