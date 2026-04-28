/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2001-2004, ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
*/

/* Demonstrates the IOP-side network configuration helper exposed by
 * ps2_iopip_driver: static IP only (no DHCP). lwIP runs on the IOP inside
 * ps2ip-nm.irx; the EE talks to it via SIF RPC through ps2ips.irx. */

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

static const char *progress_event_name(enum IOPIP_PROGRESS_EVENT ev) {
    switch (ev) {
        case IOPIP_PROGRESS_SETTING_LINK_MODE:  return "setting link mode";
        case IOPIP_PROGRESS_APPLYING_IP_CONFIG: return "applying IP config";
        case IOPIP_PROGRESS_WAITING_LINK_UP:    return "waiting for link up";
        case IOPIP_PROGRESS_LINK_UP:            return "link up";
        case IOPIP_PROGRESS_READY:              return "ready";
    }
    return "?";
}

static void on_progress(enum IOPIP_PROGRESS_EVENT ev, void *user) {
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
    if (iopip_get_current_config(&ip, &nm, &gw) != 0) {
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
    dprintf("\n\n\nnetwork_iop_sample starting...\n");

    reset_IOP();

    dprintf("Loading IOP-side network IRX modules...\n");
    if (init_network_IOP_driver(true) != IOPIP_INIT_STATUS_OK) {
        dprintf("Failed to load IOP-side network IRX modules\n");
        SleepThread();
        return -1;
    }

    iopip_network_config_t cfg;
    iopip_network_config_default(&cfg);
    cfg.on_progress = on_progress;
    cfg.ip      = IOPIP_ADDR(192, 168, 1, 10);
    cfg.netmask = IOPIP_ADDR(255, 255, 255, 0);
    cfg.gateway = IOPIP_ADDR(192, 168, 1, 1);

    enum IOPIP_NET_STATUS rc = configure_iopip_network(&cfg);
    if (rc != IOPIP_NET_STATUS_OK) {
        dprintf("configure_iopip_network failed: %d\n", rc);
        deinit_network_IOP_driver(true);
        SleepThread();
        return -1;
    }

    print_current_ip();

    dprintf("Sample done. Press POWER to exit.\n");
    SleepThread();
    return 0;
}
