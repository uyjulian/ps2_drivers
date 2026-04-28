/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2001-2004, ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

/* Demonstrates the EE-side network configuration helpers exposed by
 * ps2_eeip_driver: DHCP by default, or static IP by defining USE_STATIC_IP.
 * The progress callback mirrors each phase to the GS debug screen and stdout. */

/* Uncomment to use a hard-coded static IP instead of DHCP. */
/* #define USE_STATIC_IP */

#include <stdio.h>
#include <stdbool.h>
#include <kernel.h>
#include <sifrpc.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <debug.h>

#include <ps2_network_driver.h>

#define dprintf(args...)         \
    do {                         \
        scr_printf(args);        \
        printf(args);            \
    } while (0)

static const char *progress_event_name(enum EEIP_PROGRESS_EVENT ev) {
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
    dprintf("[net] %s\n", progress_event_name(ev));
}

static void reset_IOP(void) {
    SifInitRpc(0);
#if !defined(DEBUG) || defined(BUILD_FOR_PCSX2)
    while (!SifIopReset(NULL, 0)) {};
#endif
    while (!SifIopSync()) {};
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
}

static void print_current_ip(void) {
    struct ip4_addr ip, nm, gw;
    if (eeip_get_current_config(&ip, &nm, &gw) != 0) {
        dprintf("Failed to read current IP config\n");
        return;
    }
    dprintf("IP:      %d.%d.%d.%d\n",
            ip4_addr1(&ip), ip4_addr2(&ip), ip4_addr3(&ip), ip4_addr4(&ip));
    dprintf("Netmask: %d.%d.%d.%d\n",
            ip4_addr1(&nm), ip4_addr2(&nm), ip4_addr3(&nm), ip4_addr4(&nm));
    dprintf("Gateway: %d.%d.%d.%d\n",
            ip4_addr1(&gw), ip4_addr2(&gw), ip4_addr3(&gw), ip4_addr4(&gw));
}

int main(int argc, char **argv) {
    init_scr();
    dprintf("\n\n\nnetwork_ee_sample starting...\n");

    reset_IOP();

    dprintf("Loading network IRX modules...\n");
    if (init_network_driver(true) != EEIP_INIT_STATUS_OK) {
        dprintf("Failed to load network IRX modules\n");
        SleepThread();
        return -1;
    }

    eeip_network_config_t cfg;
    eeip_network_config_default_dhcp(&cfg);
    cfg.on_progress = on_progress;

#ifdef USE_STATIC_IP
    cfg.use_dhcp = false;
    cfg.ip       = EEIP_ADDR(192, 168, 1, 10);
    cfg.netmask  = EEIP_ADDR(255, 255, 255, 0);
    cfg.gateway  = EEIP_ADDR(192, 168, 1, 1);
    dprintf("Mode: static IP\n");
#else
    dprintf("Mode: DHCP\n");
#endif

    enum EEIP_NET_STATUS rc = configure_eeip_network(&cfg);
    if (rc != EEIP_NET_STATUS_OK) {
        dprintf("configure_eeip_network failed: %d\n", rc);
        deinit_network_driver(true);
        SleepThread();
        return -1;
    }

    print_current_ip();

    dprintf("Sample done. Press POWER to exit.\n");
    SleepThread();
    return 0;
}
