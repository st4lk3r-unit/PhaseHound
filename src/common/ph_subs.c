#include "ph_subs.h"
#include "ctrlmsg.h"
#include <string.h>
#include <stdio.h>

bool ph_handle_subscribe_cmd(
        ph_ctrl_t *c,
    const char *line,
    ph_subscribe_cb cb,
    void *user)
{
        if (!line || strncmp(line, "subscribe", 9) != 0)
        return false;

    const char *p = line + 9;
    while (*p == ' ' || *p == '\t') p++;

    char usage[32] = {0};
    char feed[128] = {0};

    if (sscanf(p, "%31s %127s", usage, feed) != 2) {
            ph_reply_err(c, "subscribe <usage> <feed>");
        return true;
    }

    if (cb(user, usage, feed) != 0) {
            ph_reply_err(c, "subscribe failed");
        return true;
    }

    ph_reply_okf(c, "subscribed %s %s", usage, feed);
    return true;
}


bool ph_handle_unsubscribe_cmd(
        ph_ctrl_t *c,
    const char *line,
    ph_unsubscribe_cb cb,
    void *user)
{
        if (!line || strncmp(line, "unsubscribe", 11) != 0)
        return false;

    const char *p = line + 11;
    while (*p == ' ' || *p == '\t') p++;

    char usage[32] = {0};
    if (sscanf(p, "%31s", usage) != 1) {
            ph_reply_err(c, "unsubscribe <usage>");
        return true;
    }

    if (cb(user, usage) != 0) {
            ph_reply_err(c, "unsubscribe failed");
        return true;
    }

    ph_reply_okf(c, "unsubscribed %s", usage);
    return true;
}
