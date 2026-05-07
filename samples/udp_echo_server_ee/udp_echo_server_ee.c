/* UDP burst client — EE-side path (eeip_driver).
 *
 * Brings up the EE-side lwIP stack via configure_eeip_network (DHCP),
 * then bursts N UDP requests to a fixed target host:port and verifies
 * each echo. Prints per-iter result and a final summary.
 *
 * Designed as a PCSX2-and-real-hardware-testable reproducer for the
 * EE-side SMAP TX/RX wedge surfaced by ps2_http (request #1 OK, all
 * rest die). UDP exercises the same EE-side data path:
 *   sendto -> udp_send -> ip_output -> SMapLowLevelOutput ->
 *     NetMan TX (EE) -> SIF DMA -> netman.irx (IOP) -> smap.irx -> wire
 *   wire -> smap.irx -> netman.irx -> SIF DMA -> NETMAN_RxThread (EE)
 *     -> tcpip_input -> ip_input -> udp_input -> sockets recvfrom
 *
 * but is connectionless, so the test is independent of any TCP listen-
 * state machinery. If UDP wedges after #1 too, the bug is below TCP
 * (in SMAP/NetMan/SIF DMA). If UDP keeps round-tripping while ps2_http
 * dies, the bug is TCP-listen-state-specific in lwIP.
 *
 * The PS2 acts as the *client* so PCSX2's stock Sockets-mode NAT can
 * deliver outbound (no EthUDPPorts patch needed). The host runs a
 * trivial echo server (tools/udp_echo_host.py).
 *
 * Build: ps2_drivers cmake target `udp_echo_server_ee.elf`.
 * Defaults: target = 192.168.31.233:7777 (host LAN IP, override at edit
 * time). N = 100 iters, then SleepThread.
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

/* Indent every screen line so it clears the left side of the TV's overscan. */
#define dprintf(args...)               \
    do {                               \
        scr_printf("        ");        \
        scr_printf(args);              \
        printf(args);                  \
    } while (0)

/* Edit these to point at your host echo server. */
#define TARGET_IP_STR  "192.168.31.233"
#define TARGET_PORT    7777
#define BURST_N        100
#define INTER_PACKET_US (50 * 1000)  /* 50 ms */
#define RECV_TIMEOUT_S 2

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
    int s, n;
    struct sockaddr_in target;
    char tx[256], rx[256];
    struct timeval tv;
    int ok = 0, fail = 0, first_fail = -1;

    init_scr();
    dprintf("\n\n\nudp_burst_client_ee starting...\n");

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

    struct ip4_addr local_ip, local_mask, local_gw;
    if (eeip_get_current_config(&local_ip, &local_mask, &local_gw) != 0) {
        dprintf("Failed to read current IP\n");
        SleepThread();
        return -1;
    }
    dprintf("local IP : %d.%d.%d.%d\n",
            ip4_addr1(&local_ip), ip4_addr2(&local_ip),
            ip4_addr3(&local_ip), ip4_addr4(&local_ip));
    dprintf("netmask  : %d.%d.%d.%d\n",
            ip4_addr1(&local_mask), ip4_addr2(&local_mask),
            ip4_addr3(&local_mask), ip4_addr4(&local_mask));
    dprintf("gateway  : %d.%d.%d.%d\n",
            ip4_addr1(&local_gw), ip4_addr2(&local_gw),
            ip4_addr3(&local_gw), ip4_addr4(&local_gw));

    (void)tv;
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { dprintf("socket failed errno=%d\n", errno); SleepThread(); return -1; }

    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(TARGET_PORT);
    target.sin_addr.s_addr = inet_addr(TARGET_IP_STR);
    {
        unsigned int a = (unsigned int)target.sin_addr.s_addr;
        dprintf("inet_addr(\"%s\") => 0x%08x => %d.%d.%d.%d\n",
            TARGET_IP_STR, a,
            (int)((a      ) & 0xff), (int)((a >>  8) & 0xff),
            (int)((a >> 16) & 0xff), (int)((a >> 24) & 0xff));
    }

    /* Let DHCP-related noise settle before the burst, so our
     * #1 prints are easy to find on the TV. */
    DelayThread(10 * 1000 * 1000);
    /* Clear the screen so DHCP traces are gone and only the burst output
     * is visible. Reset cursor to top-left as well. */
    scr_clear();
    scr_setXY(0, 0);
    dprintf("=== START BURST ===\n");
    dprintf("local IP : %d.%d.%d.%d\n",
            ip4_addr1(&local_ip), ip4_addr2(&local_ip),
            ip4_addr3(&local_ip), ip4_addr4(&local_ip));
    dprintf("netmask  : %d.%d.%d.%d\n",
            ip4_addr1(&local_mask), ip4_addr2(&local_mask),
            ip4_addr3(&local_mask), ip4_addr4(&local_mask));
    dprintf("gateway  : %d.%d.%d.%d\n",
            ip4_addr1(&local_gw), ip4_addr2(&local_gw),
            ip4_addr3(&local_gw), ip4_addr4(&local_gw));
    dprintf("target   : %s:%d, burst N=%d\n", TARGET_IP_STR, TARGET_PORT, BURST_N);

    for (int i = 1; i <= BURST_N; i++) {
        /* Pad payload to 80 bytes so the full ethernet frame (eth+ip+udp+payload
         * = 14+20+8+80 = 122) is comfortably above the 60-byte ethernet minimum.
         * If UDP suddenly works at 80 bytes when it failed at 7, the IOP smap
         * runt-frame handling is the culprit. */
        memset(tx, 'A', sizeof(tx));
        int len = snprintf(tx, sizeof(tx), "ping #%d", i);
        memset(tx + len, 'A', 80 - len);
        len = 80;
        tx[len] = 0;
        dprintf("#%d pre-sendto\n", i);
        int sent = sendto(s, tx, len, 0, (struct sockaddr *)&target, sizeof(target));
        dprintf("#%d post-sendto sent=%d errno=%d\n", i, sent, errno);
        if (sent < 0) {
            fail++;
            if (first_fail < 0) first_fail = i;
            DelayThread(INTER_PACKET_US);
            continue;
        }

        /* Wait up to RECV_TIMEOUT_S for a readable reply via select(),
         * since SO_RCVTIMEO isn't supported on this stack. */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        struct timeval to = { .tv_sec = RECV_TIMEOUT_S, .tv_usec = 0 };
        dprintf("#%d pre-select\n", i);
        int sr = select(s + 1, &rfds, NULL, NULL, &to);
        dprintf("#%d post-select sr=%d errno=%d\n", i, sr, errno);
        if (sr <= 0) {
            dprintf("#%d select TIMEOUT/err sr=%d\n", i, sr);
            fail++;
            if (first_fail < 0) first_fail = i;
            DelayThread(INTER_PACKET_US);
            continue;
        }
        dprintf("#%d pre-recvfrom\n", i);
        n = recvfrom(s, rx, sizeof(rx) - 1, 0, NULL, NULL);
        dprintf("#%d post-recvfrom n=%d errno=%d\n", i, n, errno);
        if (n < 0) {
            fail++;
            if (first_fail < 0) first_fail = i;
        } else {
            rx[n] = 0;
            if (n == len && memcmp(tx, rx, len) == 0) {
                ok++;
                dprintf("#%d ok\n", i);
            } else {
                dprintf("#%d mismatch n=%d \"%s\"\n", i, n, rx);
                fail++;
                if (first_fail < 0) first_fail = i;
            }
        }
        DelayThread(INTER_PACKET_US);
    }

    dprintf("\n=== summary ===\n");
    dprintf("ok=%d fail=%d first_fail=%d\n", ok, fail, first_fail);

    close(s);
    SleepThread();
    return 0;
}
