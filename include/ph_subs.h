#pragma once
#include "ctrlmsg.h"

/* Callback invoked by the addon to actually perform subscription */
typedef int (*ph_subscribe_cb)(void *user, const char *usage, const char *feed);
typedef int (*ph_unsubscribe_cb)(void *user, const char *usage);

/* Parses "subscribe <usage> <feed>" commands */
bool ph_handle_subscribe_cmd(
    ph_ctrl_t *c,
    const char *line,
    ph_subscribe_cb cb,
    void *user);

/* Parses "unsubscribe <usage>" commands */
bool ph_handle_unsubscribe_cmd(
    ph_ctrl_t *c,
    const char *line,
    ph_unsubscribe_cb cb,
    void *user);