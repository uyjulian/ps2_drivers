/* Pure TCP echo server — EE-side path (eeip_driver).
 *
 * Brings up the EE-side lwIP stack via configure_eeip_network (DHCP),
 * then runs a TCP echo server on port 6789. The same EE-side lwIP
 * server-side path that ps2_http uses, minus mongoose.
 *
 * Pattern (mirror of ps2_http per request):
 *   accept() -> recv() -> send() -> close() -> back to accept()
 *
 * Drive it from the host with tools/tcp_burst.py (or any other TCP
 * burst tool) connecting repeatedly to <PS2_IP>:6789. If this wedges
 * after a few requests on real hardware (just like ps2_http), the bug
 * is in lwIP's TCP listen/accept path on EE, not in mongoose. If it
 * sustains 100/100, the bug is mongoose-specific.
 *
 * NOTE: PCSX2 in stock Sockets mode cannot deliver inbound TCP to the
 * guest, so this test must be run on real hardware.
 */

#define LIBCGLUE_SYS_SOCKET_ALIASES 1

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <kernel.h>
#include <delaythread.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <debug.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include <ps2_network_driver.h>

#define dprintf(args...)               \
    do {                               \
        scr_printf("        ");        \
        scr_printf(args);              \
        printf(args);                  \
    } while (0)

#define ECHO_PORT 6789

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
    int s, c, n;
    struct sockaddr_in addr, peer;
    socklen_t plen;
    char buf[256];
    int n_acc = 0;

    init_scr();
    dprintf("\n\n\ntcp_server_ee starting...\n");

    reset_IOP();

    if (init_network_driver(true) != EEIP_INIT_STATUS_OK) {
        dprintf("init_network_driver failed\n");
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

    struct ip4_addr local_ip;
    if (eeip_get_current_config(&local_ip, NULL, NULL) != 0) {
        dprintf("read IP failed\n");
        SleepThread();
        return -1;
    }
    dprintf("local IP: %d.%d.%d.%d, port %d\n",
            ip4_addr1(&local_ip), ip4_addr2(&local_ip),
            ip4_addr3(&local_ip), ip4_addr4(&local_ip), ECHO_PORT);

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { dprintf("socket err=%d\n", errno); SleepThread(); return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ECHO_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        dprintf("bind err=%d\n", errno); SleepThread(); return -1;
    }
    if (listen(s, 5) < 0) {
        dprintf("listen err=%d\n", errno); SleepThread(); return -1;
    }
    dprintf("listening on :%d (fd=%d)\n", ECHO_PORT, s);

    for (;;) {
        plen = sizeof(peer);
        c = accept(s, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            dprintf("accept err=%d (acc#%d)\n", errno, n_acc);
            DelayThread(500 * 1000);
            continue;
        }
        n_acc++;
        dprintf("acc#%d fd=%d from %d.%d.%d.%d:%d\n", n_acc, c,
                (int)((peer.sin_addr.s_addr      ) & 0xff),
                (int)((peer.sin_addr.s_addr >>  8) & 0xff),
                (int)((peer.sin_addr.s_addr >> 16) & 0xff),
                (int)((peer.sin_addr.s_addr >> 24) & 0xff),
                (int)ntohs(peer.sin_port));

        n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            int sent = send(c, buf, n, 0);
            if (sent < 0)
                dprintf("acc#%d send err=%d\n", n_acc, errno);
        } else if (n < 0) {
            dprintf("acc#%d recv err=%d\n", n_acc, errno);
        }

        if (close(c) < 0)
            dprintf("acc#%d close err=%d\n", n_acc, errno);
    }

    return 0;
}
