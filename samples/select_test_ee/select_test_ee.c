/* Minimal select() reproducer — EE-side path (eeip_driver).
 *
 * Brings up the EE-side lwIP stack via configure_eeip_network (DHCP),
 * creates a TCP listening socket, and in a loop calls select() with the
 * listen fd in all three fdsets (rset/wset/eset) using a non-blocking
 * timeout. Prints the post-select state of each bit.
 *
 * Expected behaviour for an idle listening TCP socket:
 *   r=0  (no pending accept)
 *   w=0  (listen socket is not "writable")
 *   e=0  (no exception)
 *
 * Any deviation is a select() bug. Spurious e=1 on a fresh listen is the
 * exact signal that breaks ps2_http on the IOP-side path; we run this on
 * both eeip and iopip variants to localise where the bug lives.
 */

#define LIBCGLUE_SYS_SOCKET_ALIASES 1

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <kernel.h>
#include <delaythread.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <debug.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <unistd.h>

#include <ps2_network_driver.h>

#define dprintf(args...)         \
    do {                         \
        scr_printf(args);        \
        printf(args);            \
    } while (0)

static const char *eeip_event_name(enum EEIP_PROGRESS_EVENT ev) {
    switch (ev) {
        case EEIP_PROGRESS_SETTING_LINK_MODE:  return "setting link mode";
        case EEIP_PROGRESS_TCPIP_INIT:         return "lwIP init";
        case EEIP_PROGRESS_APPLYING_IP_CONFIG: return "applying IP config";
        case EEIP_PROGRESS_WAITING_LINK_UP:    return "waiting for link up";
        case EEIP_PROGRESS_LINK_UP:            return "link up";
        case EEIP_PROGRESS_WAITING_DHCP:       return "waiting for DHCP lease";
        case EEIP_PROGRESS_DHCP_BOUND:         return "DHCP bound";
        case EEIP_PROGRESS_READY:              return "ready";
    }
    return "?";
}

static void on_progress(enum EEIP_PROGRESS_EVENT ev, void *user) {
    (void)user;
    dprintf("[net] %s\n", eeip_event_name(ev));
}

static void reset_IOP(void) {
    SifInitRpc(0);
    while (!SifIopReset(NULL, 0)) {};
    while (!SifIopSync()) {};
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
}

int main(int argc, char **argv) {
    init_scr();
    dprintf("\n\n\nselect_test_ee starting...\n");

    reset_IOP();

    if (init_network_driver(true) != EEIP_INIT_STATUS_OK) {
        dprintf("Failed to load EE-side network IRX modules\n");
        SleepThread();
        return -1;
    }

    eeip_network_config_t cfg;
    eeip_network_config_default_dhcp(&cfg);
    cfg.on_progress = on_progress;
    cfg.timeout_seconds = 30;

    if (configure_eeip_network(&cfg) != EEIP_NET_STATUS_OK) {
        dprintf("configure_eeip_network failed\n");
        SleepThread();
        return -1;
    }

    struct ip4_addr ip;
    if (eeip_get_current_config(&ip, NULL, NULL) == 0) {
        dprintf("IP: %d.%d.%d.%d\n",
                ip4_addr1(&ip), ip4_addr2(&ip), ip4_addr3(&ip), ip4_addr4(&ip));
    }

    /* Listening TCP socket on :6789 */
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { dprintf("socket() failed: errno=%d\n", errno); SleepThread(); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(6789);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        dprintf("bind() failed: errno=%d\n", errno);
        close(s); SleepThread(); return -1;
    }
    if (listen(s, 5) < 0) {
        dprintf("listen() failed: errno=%d\n", errno);
        close(s); SleepThread(); return -1;
    }
    dprintf("listening on :6789, fd=%d\n", s);
    dprintf("expected: r=0 w=0 e=0 (no traffic)\n");

    int tick = 0;
    for (;;) {
        fd_set rset, wset, eset;
        FD_ZERO(&rset); FD_ZERO(&wset); FD_ZERO(&eset);
        FD_SET(s, &rset); FD_SET(s, &wset); FD_SET(s, &eset);

        struct timeval tv = {0, 0};
        int rc = select(s + 1, &rset, &wset, &eset, &tv);
        int r = FD_ISSET(s, &rset) ? 1 : 0;
        int w = FD_ISSET(s, &wset) ? 1 : 0;
        int e = FD_ISSET(s, &eset) ? 1 : 0;
        dprintf("tick=%d rc=%d r=%d w=%d e=%d\n", tick, rc, r, w, e);

        tick++;
        DelayThread(1000 * 1000); /* 1 s */
    }

    /* unreachable */
    return 0;
}
