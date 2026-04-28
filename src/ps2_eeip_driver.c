/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2005, ps2dev - http://www.ps2dev.org
# Licenced under GNU Library General Public License version 2
# Review ps2sdk README & LICENSE files for further details.
#
# PS2_EEIP_DRIVER
*/

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <ps2_dev9_driver.h>
#include <ps2_netman_driver.h>
#include <ps2_smap_driver.h>
#include <ps2_eeip_driver.h>
#include <irx_common_macros.h>

#include <sifrpc.h>
#include <loadfile.h>
#include <netman.h>
#include <ps2ip.h>

#define EEIP_DEFAULT_TIMEOUT_SECONDS 10

#ifdef F_internals_ps2_eeip_driver
enum EEIP_INIT_STATUS __eeip_init_status = EEIP_INIT_STATUS_UNKNOWN;
#else
extern enum EEIP_INIT_STATUS __eeip_init_status;
#endif

#ifdef F_init_ps2_eeip_driver
static enum EEIP_INIT_STATUS loadIRXs(void) {
    return EEIP_INIT_STATUS_OK;
}

enum EEIP_INIT_STATUS init_eeip_driver(bool init_dependencies) {

    if (init_dependencies) {
        // Requires to have DEV9
        if (init_dev9_driver() != DEV9_INIT_STATUS_OK)
            return EEIP_INIT_STATUS_DEPENDENCY_IRX_ERROR;

        // Requires to have NETMAN
        if (init_netman_driver() != NETMAN_INIT_STATUS_OK)
            return EEIP_INIT_STATUS_DEPENDENCY_IRX_ERROR;

        // Requires to have SMAP
        if (init_smap_driver() != SMAP_INIT_STATUS_OK)
            return EEIP_INIT_STATUS_DEPENDENCY_IRX_ERROR;
    }

    __eeip_init_status = loadIRXs();
    return __eeip_init_status;
}
#endif

#ifdef F_deinit_ps2_eeip_driver
static void deinitLibraries(void) {}

static void unloadIRXs(void) {}

void deinit_eeip_driver(bool deinit_dependencies) {
    deinitLibraries();
    unloadIRXs();

    if (deinit_dependencies) {
        // Requires to have SMAP
        deinit_smap_driver();
        // Requires to have NETMAN
        deinit_netman_driver();
        // Requires to have DEV9
        deinit_dev9_driver();
    }
}
#endif

#ifdef F_eeip_network_config_default_dhcp_ps2_eeip_driver
void eeip_network_config_default_dhcp(eeip_network_config_t *cfg) {
    if (cfg == NULL) return;

    cfg->use_dhcp        = true;
    ip4_addr_set_zero(&cfg->ip);
    ip4_addr_set_zero(&cfg->netmask);
    ip4_addr_set_zero(&cfg->gateway);
    cfg->link_mode       = NETMAN_NETIF_ETH_LINK_MODE_AUTO;
    cfg->timeout_seconds = EEIP_DEFAULT_TIMEOUT_SECONDS;
    cfg->on_progress     = NULL;
    cfg->user            = NULL;
}
#endif

#ifdef F_configure_eeip_network_ps2_eeip_driver
static inline void emit_progress(const eeip_network_config_t *cfg,
                                 enum EEIP_PROGRESS_EVENT ev) {
    if (cfg->on_progress != NULL) {
        cfg->on_progress(ev, cfg->user);
    }
}

static int link_is_up(void) {
    return NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0)
        == NETMAN_NETIF_ETH_LINK_STATE_UP;
}

static int dhcp_is_bound(void) {
    t_ip_info ip_info;
    if (ps2ip_getconfig("sm0", &ip_info) < 0) return 0;
    if (!ip_info.dhcp_enabled) return 0;
    return (ip_info.dhcp_status == DHCP_STATE_BOUND
         || ip_info.dhcp_status == DHCP_STATE_OFF);
}

static int wait_until(int (*check)(void), int timeout_seconds) {
    int retry;
    for (retry = 0; retry < timeout_seconds; retry++) {
        if (check()) return 0;
        usleep(1000 * 1000);
    }
    return -1;
}

static enum EEIP_NET_STATUS apply_ip_config(const eeip_network_config_t *cfg) {
    t_ip_info ip_info;

    if (ps2ip_getconfig("sm0", &ip_info) < 0)
        return EEIP_NET_STATUS_CONFIG_ERROR;

    if (cfg->use_dhcp) {
        ip_info.dhcp_enabled = 1;
    } else {
        ip_addr_set((struct ip4_addr *)&ip_info.ipaddr,  &cfg->ip);
        ip_addr_set((struct ip4_addr *)&ip_info.netmask, &cfg->netmask);
        ip_addr_set((struct ip4_addr *)&ip_info.gw,      &cfg->gateway);
        ip_info.dhcp_enabled = 0;
    }

    if (ps2ip_setconfig(&ip_info) < 0)
        return EEIP_NET_STATUS_CONFIG_ERROR;

    return EEIP_NET_STATUS_OK;
}

enum EEIP_NET_STATUS configure_eeip_network(const eeip_network_config_t *cfg) {
    if (cfg == NULL) return EEIP_NET_STATUS_CONFIG_ERROR;

    int timeout = (cfg->timeout_seconds > 0)
                ? cfg->timeout_seconds
                : EEIP_DEFAULT_TIMEOUT_SECONDS;

    /* 1. lwIP init with zero addrs; ps2ip_setconfig fills the real values.
       ps2ipInit also registers the IP stack with netman, which must happen
       before NetManSetLinkMode can dispatch to the NIC. */
    emit_progress(cfg, EEIP_PROGRESS_TCPIP_INIT);
    {
        struct ip4_addr zero_ip, zero_nm, zero_gw;
        ip4_addr_set_zero(&zero_ip);
        ip4_addr_set_zero(&zero_nm);
        ip4_addr_set_zero(&zero_gw);
        ps2ipInit(&zero_ip, &zero_nm, &zero_gw);
    }

    /* 2. Link mode. SMAP defaults to AUTO, and on PCSX2 the SET_LINK_MODE
       ioctl is a no-op stub that fails — so only push a non-AUTO request. */
    emit_progress(cfg, EEIP_PROGRESS_SETTING_LINK_MODE);
    if (cfg->link_mode != NETMAN_NETIF_ETH_LINK_MODE_AUTO) {
        if (NetManSetLinkMode(cfg->link_mode) != 0)
            return EEIP_NET_STATUS_LINK_MODE_ERROR;
    }

    /* 3. Apply IP/DHCP configuration. */
    emit_progress(cfg, EEIP_PROGRESS_APPLYING_IP_CONFIG);
    {
        enum EEIP_NET_STATUS rc = apply_ip_config(cfg);
        if (rc != EEIP_NET_STATUS_OK) return rc;
    }

    /* 4. Wait for link up. */
    emit_progress(cfg, EEIP_PROGRESS_WAITING_LINK_UP);
    if (wait_until(link_is_up, timeout) != 0)
        return EEIP_NET_STATUS_LINK_TIMEOUT;
    emit_progress(cfg, EEIP_PROGRESS_LINK_UP);

    /* 5. If DHCP, wait for a lease. */
    if (cfg->use_dhcp) {
        emit_progress(cfg, EEIP_PROGRESS_WAITING_DHCP);
        if (wait_until(dhcp_is_bound, timeout) != 0)
            return EEIP_NET_STATUS_DHCP_TIMEOUT;
        emit_progress(cfg, EEIP_PROGRESS_DHCP_BOUND);
    }

    emit_progress(cfg, EEIP_PROGRESS_READY);
    return EEIP_NET_STATUS_OK;
}
#endif

#ifdef F_eeip_get_current_config_ps2_eeip_driver
int eeip_get_current_config(struct ip4_addr *ip,
                            struct ip4_addr *nm,
                            struct ip4_addr *gw) {
    t_ip_info ip_info;

    if (ps2ip_getconfig("sm0", &ip_info) < 0)
        return -1;

    if (ip != NULL) ip_addr_set(ip, (struct ip4_addr *)&ip_info.ipaddr);
    if (nm != NULL) ip_addr_set(nm, (struct ip4_addr *)&ip_info.netmask);
    if (gw != NULL) ip_addr_set(gw, (struct ip4_addr *)&ip_info.gw);

    return 0;
}
#endif
