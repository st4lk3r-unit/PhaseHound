#define _GNU_SOURCE
#include "ph_uds_protocol.h"
#include "common.h"
#include "plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/select.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ph_version.h"
#ifndef PH_GIT_SHA
#define PH_GIT_SHA "unknown"
#endif

/* ========= Small utilities ========= */

// Safe path copy (trunc-safe): returns 0 on success, -1 on truncation.
static int path_copy(char *dst, size_t cap, const char *src){
    size_t n = strlen(src);
    if (n >= cap) { // needs space for '\0'
        if (cap) { dst[0] = '\0'; }
        return -1;
    }
    memcpy(dst, src, n+1);
    return 0;
}

static void json_send_kv_list(int fd, const char *type, const char *key, char **items, int n);

/* ========= Plugin table ========= */

typedef struct {
    void *dl;
    plugin_name_fn  f_name;
    plugin_init_fn  f_init;
    plugin_start_fn f_start;
    plugin_stop_fn  f_stop;
    char name[64];
    char path[512]; /* store exact load path for diagnostics */
} plug_t;

typedef struct {
    plug_t *v; size_t n, cap;
    pthread_mutex_t mu;
} plugtab_t;

static void plugtab_init(plugtab_t *t){ t->v=NULL; t->n=t->cap=0; pthread_mutex_init(&t->mu,NULL); }
static void plugtab_free(plugtab_t *t){
    for(size_t i=0;i<t->n;i++){
        if(t->v[i].f_stop) t->v[i].f_stop();
        if(t->v[i].dl) dlclose(t->v[i].dl);
    }
    free(t->v); pthread_mutex_destroy(&t->mu);
}
static int plugtab_find(plugtab_t *t, const char *name){
    for(size_t i=0;i<t->n;i++) if(strcmp(t->v[i].name,name)==0) return (int)i;
    return -1;
}
static void plugtab_add(plugtab_t *t, plug_t p){
    if(t->n==t->cap){ size_t nc=t->cap? t->cap*2:4; t->v=realloc(t->v,nc*sizeof(plug_t)); t->cap=nc; }
    t->v[t->n++] = p;
}
static void plugtab_remove(plugtab_t *t, size_t idx){
    for(size_t i=idx+1;i<t->n;i++) t->v[i-1]=t->v[i];
    t->n--;
}

/* ========= Global state ========= */

static volatile int g_run = 1;
static void on_sigint(int s){ (void)s; g_run = 0; }

static feedtab_t g_feeds;
static plugtab_t g_plugins;
static int g_listen_fd = -1;

/* ========= Feeds ========= */

static void broadcast_to_subs(const char *feed, const char *json, size_t len, int *fds, size_t nfds){
    int idx = feedtab_find(&g_feeds, feed);
    if(idx<0) return;
    pthread_mutex_lock(&g_feeds.mu);
    for(size_t i=0;i<g_feeds.v[idx].subs.n;i++){
        int fd = g_feeds.v[idx].subs.v[i];
        if(((nfds>0)? send_frame_json_with_fds(fd, json, len, fds, nfds) : send_frame_json(fd, json, len))<0){
            /* drop errors silently; cleanup happens on disconnect */
        }
    }
    pthread_mutex_unlock(&g_feeds.mu);
}

/* ========= Add-on discovery (autoload) ========= */

static int scan_addon_paths(char paths[][512], int maxn){
    int n=0;
    const char *roots[] = {"./src/addons", "./addons", "./"};
    for(size_t r=0;r<sizeof(roots)/sizeof(roots[0]);r++){
        DIR *d = opendir(roots[r]);
        if(!d) continue;
        struct dirent *de;
        while((de=readdir(d))){
            if(de->d_name[0]=='.') continue;
            char sub[512];
            int ok = snprintf(sub, sizeof sub, "%s/%s", roots[r], de->d_name);
            if(ok < 0 || (size_t)ok >= sizeof sub) continue; // trunc guard

            struct stat st; if(stat(sub,&st)<0) continue;
            if(S_ISDIR(st.st_mode)){
                DIR *d2 = opendir(sub); if(!d2) continue;
                struct dirent *de2;
                while((de2=readdir(d2))){
                    if(de2->d_name[0]=='.') continue;
                    if(!strstr(de2->d_name,".so")) continue;
                    char so[512];
                    ok = snprintf(so, sizeof so, "%s/%s", sub, de2->d_name);
                    if(ok < 0 || (size_t)ok >= sizeof so) continue; // trunc guard
                    if(access(so,R_OK)==0 && n<maxn){
                        path_copy(paths[n++], 512, so);
                    }
                }
                closedir(d2);
            } else if(S_ISREG(st.st_mode)){
                if(strstr(sub,".so") && access(sub,R_OK)==0 && n<maxn){
                    path_copy(paths[n++], 512, sub);
                }
            }
        }
        closedir(d);
    }
    return n;
}

/* ========= Loader / Unloader (centralized) ========= */

static int load_plugin_from_path(const char *so_path){
    if(!so_path || !strstr(so_path, ".so") || access(so_path, R_OK)!=0){
        log_msg(LOG_ERROR, "load: invalid or unreadable path: %s", so_path?so_path:"(null)");
        return -1;
    }

    void *dl = dlopen(so_path, RTLD_NOW);
    if(!dl){ log_msg(LOG_ERROR, "dlopen(%s): %s", so_path, dlerror()); return -2; }

    plug_t p = (plug_t){0};
    p.dl     = dl;
    p.f_name = (plugin_name_fn)dlsym(dl, "plugin_name");
    p.f_init = (plugin_init_fn)dlsym(dl, "plugin_init");
    p.f_start= (plugin_start_fn)dlsym(dl, "plugin_start");
    p.f_stop = (plugin_stop_fn)dlsym(dl, "plugin_stop");

    if(!p.f_name || !p.f_init || !p.f_start || !p.f_stop){
        log_msg(LOG_ERROR,"bad plugin ABI in %s", so_path);
        dlclose(dl);
        return -3;
    }

    snprintf(p.name, sizeof p.name, "%s", p.f_name());
    if(plugtab_find(&g_plugins, p.name) >= 0){
        log_msg(LOG_INFO, "skip %s (already loaded)", p.name);
        dlclose(dl);
        return 1; /* not an error, just a skip */
    }

    if (path_copy(p.path, sizeof p.path, so_path) != 0) {
        log_msg(LOG_WARN, "path truncated for %s", p.name);
        /* still proceed; p.path is empty or truncated */
        snprintf(p.path, sizeof p.path, "%s", so_path); // best effort, snprintf is fine here
    }

    /* Build ctx for ABI 1.0 */
    plugin_ctx_t ctx = {
        .abi_major     = PLUGIN_ABI_MAJOR,
        .abi_minor     = PLUGIN_ABI_MINOR,
        .ctx_size      = sizeof(plugin_ctx_t),
        .sock_path     = PH_SOCK_PATH,
        .name          = p.name,
        .core_features = 0u
    };

    plugin_caps_t caps = (plugin_caps_t){0};
    if(!p.f_init(&ctx, &caps)){
        log_msg(LOG_ERROR, "plugin %s: plugin_init failed", p.name);
        dlclose(dl);
        return -4;
    }

    if (caps.caps_size < sizeof(plugin_caps_t)) {
        log_msg(LOG_ERROR,
                "plugin %s: incompatible caps (size=%u < core=%zu); refusing (core ABI %u.%u)",
                p.name, (unsigned)caps.caps_size, sizeof(plugin_caps_t),
                (unsigned)PLUGIN_ABI_MAJOR, (unsigned)PLUGIN_ABI_MINOR);
        dlclose(dl);
        return -5;
    }

    if (!caps.name)    caps.name    = p.name;
    if (!caps.version) caps.version = "(unknown)";

    log_msg(LOG_INFO, "caps %s v%s", caps.name, caps.version);

    if(!p.f_start()){
        log_msg(LOG_ERROR, "plugin %s: plugin_start failed", p.name);
        if(p.f_stop) p.f_stop();
        dlclose(dl);
        return -6;
    }

    plugtab_add(&g_plugins, p);
    log_msg(LOG_INFO, "loaded plugin %s (%s)", p.name, p.path[0]?p.path:"(unknown)");
    return 0;
}

static int unload_plugin_by_name(const char *name){
    int idx = plugtab_find(&g_plugins, name);
    if(idx < 0) return -1;
    plug_t *pl = &g_plugins.v[idx];
    if(pl->f_stop) pl->f_stop();
    if(pl->dl) dlclose(pl->dl);
    log_msg(LOG_INFO, "unloaded plugin %s (from %s)", pl->name, pl->path[0]?pl->path:"(unknown)");
    plugtab_remove(&g_plugins, (size_t)idx);
    return 0;
}

/* ========= Autoload ========= */

static void autoload_addons(void){
    char paths[128][512];
    int n = scan_addon_paths(paths, 128);
    for(int i=0;i<n;i++){
        (void)load_plugin_from_path(paths[i]);
    }
}

/* ========= Command handling (broker JSON) ========= */

static void handle_msg(int fd, const char *js, int *fds, size_t nfds){
    char type[32]; if(json_get_type(js, type, sizeof type)<0){ log_msg(LOG_WARN, "bad message"); return; }

    if(strcmp(type,"create_feed")==0){
        char name[POC_MAX_FEED]; if(json_get_string(js,"feed",name,sizeof name)==0) feedtab_ensure(&g_feeds, name);

    } else if(strcmp(type,"subscribe")==0){
        char name[POC_MAX_FEED]; if(json_get_string(js,"feed",name,sizeof name)==0) feedtab_sub(&g_feeds, name, fd);

    } else if(strcmp(type,"unsubscribe")==0){
        /* left as exercise */

    } else if(strcmp(type,"publish")==0){
        char name[POC_MAX_FEED]; if(json_get_string(js,"feed",name,sizeof name)==0) broadcast_to_subs(name, js, strlen(js), fds, nfds);

    } else if(strcmp(type,"command")==0){
        char feed[POC_MAX_FEED]; if(json_get_string(js,"feed",feed,sizeof feed)<0) return;
        if(strcmp(feed,"cli-control")!=0) return;
        char cmd[256]; if(json_get_string(js,"data",cmd,sizeof cmd)<0) return;

        if(strcmp(cmd,"help")==0){
            const char *h = "{\"type\":\"info\",\"msg\":\"commands: help, feeds, load <path>, unload <name>, plugins, available-addons, exit\"}";
            send_frame_json(fd, h, strlen(h));

        } else if(strcmp(cmd,"feeds")==0 || strcmp(cmd,"list feeds")==0){
            feedtab_list(&g_feeds, fd);

        } else if(strcmp(cmd,"plugins")==0 || strcmp(cmd,"list addons")==0){
            char buf[POC_MAX_JSON];
            for(size_t i=0;i<g_plugins.n;i++){
                int len = snprintf(buf, sizeof buf, "{\"type\":\"info\",\"plugin\":\"%s\",\"path\":\"%s\"}",
                                   g_plugins.v[i].name,
                                   g_plugins.v[i].path[0]?g_plugins.v[i].path:"");
                send_frame_json(fd, buf, (size_t)len);
            }

        } else if(strcmp(cmd,"available-addons")==0){
            char paths[128][512]; int n = scan_addon_paths(paths, 128);
            char *items[128]; for(int i=0;i<n;i++) items[i]=paths[i];
            json_send_kv_list(fd, "available-addons", "paths", items, n);

        } else if(strncmp(cmd,"load ",5)==0){
            char arg[256]; strncpy(arg, cmd+5, sizeof arg-1); arg[sizeof arg-1]='\0';
            /* Require explicit .so path */
            if(access(arg, R_OK)!=0 || !strstr(arg, ".so")){
                log_msg(LOG_ERROR, "load: provide a readable .so file path");
            } else {
                int rc = load_plugin_from_path(arg);
                if(rc==0){
                    char buf[POC_MAX_JSON];
                    int len = snprintf(buf, sizeof buf, "{\"type\":\"info\",\"msg\":\"loaded %s\"}", arg);
                    send_frame_json(fd, buf, (size_t)len);
                }
            }

        } else if(strncmp(cmd,"unload ",7)==0){
            char name[128]; strncpy(name, cmd+7, sizeof name-1); name[sizeof name-1]='\0';
            int rc = unload_plugin_by_name(name);
            if(rc<0) log_msg(LOG_WARN, "unload: %s not found", name);

        } else if(strcmp(cmd,"exit")==0){
            g_run = 0;

        } else {
            log_msg(LOG_WARN, "unknown command: %s", cmd);
        }

    } else if(strcmp(type,"ping")==0){
        const char *pong = "{\"type\":\"pong\"}";
        send_frame_json(fd, pong, strlen(pong));
    }
}

/* ========= JSON list helper ========= */

static void json_send_kv_list(int fd, const char *type, const char *key, char **items, int n){
    char buf[POC_MAX_JSON]; size_t pos=0;
    pos += snprintf(buf+pos, sizeof buf - pos, "{\"type\":\"%s\",\"%s\":[", type, key);
    for(int i=0;i<n;i++){
        const char *comma = (i+1<n)?",":"";
        pos += snprintf(buf+pos, sizeof buf - pos, "\"%s\"%s", items[i], comma);
    }
    pos += snprintf(buf+pos, sizeof buf - pos, "]}");
    send_frame_json(fd, buf, pos);
}

/* ========= Main ========= */

int main(void){
    signal(SIGINT, on_sigint);
    feedtab_init(&g_feeds);
    plugtab_init(&g_plugins);

    g_listen_fd = uds_listen_create(PH_SOCK_PATH);
    if(g_listen_fd<0){ log_msg(LOG_ERROR, "failed to create UDS server"); return 1; }
    log_msg(LOG_INFO, "PhaseHound-core %s (%s)  listening on %s", PH_VERSION_STRING, PH_GIT_SHA, PH_SOCK_PATH);

    /* core subscribes to cli-control */
    feedtab_ensure(&g_feeds, "cli-control");

    /* autoload addons present */
    autoload_addons();

    /* event loop */
    int fds[1024]; size_t nfds = 0;
    for(;;){
        if(!g_run) break;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(g_listen_fd, &r);
        int maxfd = g_listen_fd;
        for(size_t i=0;i<nfds;i++){ FD_SET(fds[i], &r); if(fds[i]>maxfd) maxfd=fds[i]; }
        struct timeval tv = { .tv_sec=0, .tv_usec=200000 };
        int rc = select(maxfd+1, &r, NULL, NULL, &tv);
        if(rc < 0){ if(errno==EINTR) continue; log_msg(LOG_ERROR,"select: %s", strerror(errno)); break; }

        if(FD_ISSET(g_listen_fd, &r)){
            int cfd = accept(g_listen_fd, NULL, NULL);
            if(cfd>=0){ set_nonblock(cfd); fds[nfds++] = cfd; log_msg(LOG_INFO,"client connected fd=%d", cfd); }
        }

        for(size_t i=0;i<nfds;){
            int fd = fds[i];
            if(FD_ISSET(fd, &r)){
                char js[POC_MAX_JSON];
                int ancfds[16];
                size_t anccnt = sizeof(ancfds)/sizeof(ancfds[0]);
                int got = recv_frame_json_with_fds(fd, js, sizeof js-1, ancfds, &anccnt, 10);
                if(got <= 0){
                    log_msg(LOG_INFO, "client fd=%d disconnected", fd);
                    feedtab_unsub_all_fd(&g_feeds, fd);
                    close(fd);
                    for(size_t k=i+1;k<nfds;k++) fds[k-1]=fds[k];
                    nfds--; continue;
                } else {
                    handle_msg(fd, js, ancfds, anccnt);
                    for(size_t k=0;k<anccnt;k++){ if(ancfds[k]>=0) close(ancfds[k]); }
                }
            }
            i++;
        }
    }

    printf ("\t(8D)\n");
    log_msg(LOG_INFO, "core shutting down...");
    close(g_listen_fd);
    plugtab_free(&g_plugins);
    feedtab_free(&g_feeds);
    unlink(PH_SOCK_PATH);
    return 0;
}
