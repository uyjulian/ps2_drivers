/* TCP burst client — EE-side path (eeip_driver).
 *
 * Brings up the EE-side lwIP stack via configure_eeip_network (DHCP),
 * then performs N rapid TCP request/response cycles to a fixed host:port.
 * Each iter: socket()->connect()->send()->recv()->close().
 *
 * Designed as a focused reproducer for the EE-side ps2_http close-cycle
 * wedge (after ~5 successful HTTP request/responses, the EE-side network
 * stack stops responding to ICMP and TCP). This one drops mongoose and
 * exercises the same EE-side TCP close path directly via libcglue/lwIP.
 *
 * Use with tools/tcp_echo_host.py on the host (port 7777). PS2 EE is the
 * client, so PCSX2's Sockets-mode NAT can deliver outbound TCP — making
 * this PCSX2-iterable (unlike a TCP server-on-PS2 test which requires the
 * EthUDPPorts-style patch for inbound).
 *
 * Pass criterion: ok=N, fail=0. Pre-fix the EE-side path wedges after
 * ~5-7 cycles and remaining iters time out.
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
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <ps2_network_driver.h>

#define dprintf(args...)               \
    do {                               \
        scr_printf("        ");        \
        scr_printf(args);              \
        printf(args);                  \
    } while (0)

/* Edit these to point at your host echo server. */
#define TARGET_IP_STR  "192.168.31.233"
#define TARGET_PORT    7777
#define BURST_N        20
#define INTER_CONN_US  (200 * 1000) /* 200 ms between iters */
#define RECV_TIMEOUT_S 3

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

static int do_one(int seq, struct sockaddr_in *target, char *buf, int buflen) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        dprintf("#%d socket err=%d\n", seq, errno);
        return 0;
    }

    int len = snprintf(buf, buflen, "ping #%d", seq);

    if (connect(s, (struct sockaddr *)target, sizeof(*target)) < 0) {
        dprintf("#%d connect err=%d\n", seq, errno);
        close(s);
        return 0;
    }

    if (send(s, buf, len, 0) < 0) {
        dprintf("#%d send err=%d\n", seq, errno);
        close(s);
        return 0;
    }

    /* Manual select-based timeout since SO_RCVTIMEO isn't supported. */
    fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
    struct timeval to = { .tv_sec = RECV_TIMEOUT_S, .tv_usec = 0 };
    int sr = select(s + 1, &rfds, NULL, NULL, &to);
    if (sr <= 0) {
        dprintf("#%d select sr=%d\n", seq, sr);
        close(s);
        return 0;
    }

    int n = recv(s, buf, buflen - 1, 0);
    if (n < 0) {
        dprintf("#%d recv err=%d\n", seq, errno);
        close(s);
        return 0;
    }
    buf[n] = 0;

    if (close(s) < 0) {
        dprintf("#%d close err=%d\n", seq, errno);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    struct sockaddr_in target;
    char buf[128];

    init_scr();
    dprintf("\n\n\ntcp_burst_client_ee starting...\n");

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
    dprintf("local IP: %d.%d.%d.%d\n",
            ip4_addr1(&local_ip), ip4_addr2(&local_ip),
            ip4_addr3(&local_ip), ip4_addr4(&local_ip));

    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(TARGET_PORT);
    target.sin_addr.s_addr = inet_addr(TARGET_IP_STR);
    dprintf("target: %s:%d N=%d\n", TARGET_IP_STR, TARGET_PORT, BURST_N);

    DelayThread(2 * 1000 * 1000);
    scr_clear();
    scr_setXY(0, 0);
    dprintf("=== START BURST ===\n");
    dprintf("local: %d.%d.%d.%d, target: %s:%d, N=%d\n",
            ip4_addr1(&local_ip), ip4_addr2(&local_ip),
            ip4_addr3(&local_ip), ip4_addr4(&local_ip),
            TARGET_IP_STR, TARGET_PORT, BURST_N);

    int ok = 0, fail = 0, first_fail = -1;
    for (int i = 1; i <= BURST_N; i++) {
        if (do_one(i, &target, buf, sizeof(buf))) {
            ok++;
            if (i <= 3 || i % 5 == 0) dprintf("#%d ok\n", i);
        } else {
            fail++;
            if (first_fail < 0) first_fail = i;
        }
        DelayThread(INTER_CONN_US);
    }

    dprintf("\n=== summary ===\n");
    dprintf("ok=%d fail=%d first_fail=%d\n", ok, fail, first_fail);

    SleepThread();
    return 0;
}
