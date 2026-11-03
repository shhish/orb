#pragma once

#include "../orb_app_host.h"
#include "orb_shell.h"

enum { ORB_MAC_SCREEN_ID = 0xff03 };
typedef enum { ORB_MAC_CONNECTION, ORB_MAC_MAINTENANCE, ORB_MAC_FORGET, ORB_MAC_UNPAIR } orb_mac_page_t;
typedef struct {
    orb_mac_page_t page;
    orb_shell_net_t net;
    orb_shell_link_t link;
    char ssid[33], hub_name[64];
    int16_t scroll;
} orb_mac_model_t;

const orb_screen_descriptor_t *orb_mac_descriptor(void);
orb_app_host_t *orb_mac_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context);
bool orb_mac_host_attach(orb_app_host_t *host, lv_obj_t *parent, orb_mac_page_t page,
    const orb_shell_snapshot_t *status, int16_t scroll);
lv_obj_t *orb_mac_host_reader(orb_app_host_t *host);
