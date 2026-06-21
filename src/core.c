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
#include <sys/epoll.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <execinfo.h>

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
    if(t->n==t->cap){
        size_t nc=t->cap? t->cap*2:4;
        plug_t *nv = realloc(t->v, nc*sizeof(plug_t));
        if(!nv){ log_msg(LOG_ERROR, "plugtab_add: OOM — plugin not registered"); return; }
        t->v=nv; t->cap=nc;
    }
    t->v[t->n++] = p;
}
static void plugtab_remove(plugtab_t *t, size_t idx){
    for(size_t i=idx+1;i<t->n;i++) t->v[i-1]=t->v[i];
    t->n--;
}

/* ========= Global state ========= */

static volatile sig_atomic_t g_run = 1;
static void on_sigint(int s){ (void)s; g_run = 0; }

static void crash_handler(int sig) {
    void *bt[64];
    int n = backtrace(bt, 64);
    char **syms = backtrace_symbols(bt, n);
    dprintf(STDERR_FILENO, "\n[ph-core] FATAL signal %d — backtrace:\n", sig);
    for (int i = 0; i < n; i++)
        dprintf(STDERR_FILENO, "  %s\n", syms ? syms[i] : "?");
    free(syms);
    signal(sig, SIG_DFL);
    raise(sig);
}

static feedtab_t g_feeds;
static plugtab_t g_plugins;
static int g_listen_fd = -1;
static int g_client_count = 0;
#define PH_MAX_CLIENTS 512

/* ========= Feeds ========= */

/* RT pipeline can have many consumers on a feed (TDOA array elements, etc.) */
#define PH_MAX_SNAP_SUBS 1024

static void broadcast_to_subs(const char *feed, const char *json, size_t len, int *fds, size_t nfds){
    /* Snapshot subscriber list under lock, then send outside lock.
       Prevents a slow subscriber from blocking all others. */
    int snap[PH_MAX_SNAP_SUBS]; size_t snap_n = 0;
    pthread_mutex_lock(&g_feeds.mu);
    for(size_t i = 0; i < g_feeds.n; i++){
        if(strcmp(g_feeds.v[i].name, feed) == 0){
            intvec_t *iv = &g_feeds.v[i].subs;
            if(iv->n > PH_MAX_SNAP_SUBS)
                log_msg(LOG_WARN, "feed %s: %zu subscribers exceed snapshot cap %d; extras dropped",
                        feed, iv->n, PH_MAX_SNAP_SUBS);
            snap_n = iv->n < PH_MAX_SNAP_SUBS ? iv->n : PH_MAX_SNAP_SUBS;
            for(size_t j = 0; j < snap_n; j++) snap[j] = iv->v[j];
            break;
        }
    }
    pthread_mutex_unlock(&g_feeds.mu);

    for(size_t i = 0; i < snap_n; i++){
        if(nfds > 0)
            send_frame_json_with_fds(snap[i], json, len, fds, nfds);
        else
            send_frame_json(snap[i], json, len);
    }
}

/* ========= Add-on discovery (autoload) ========= */

/* Match only unversioned .so files — strstr would hit libfoo.so.1.2.3 */
static int name_ends_with_so(const char *name){
    size_t n = strlen(name);
    return n > 3 && strcmp(name + n - 3, ".so") == 0;
}

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
            if(ok < 0 || (size_t)ok >= sizeof sub) continue;

            struct stat st; if(stat(sub,&st)<0) continue;
            if(S_ISDIR(st.st_mode)){
                DIR *d2 = opendir(sub); if(!d2) continue;
                struct dirent *de2;
                while((de2=readdir(d2))){
                    if(de2->d_name[0]=='.') continue;
                    if(!name_ends_with_so(de2->d_name)) continue;
                    char so[512];
                    ok = snprintf(so, sizeof so, "%s/%s", sub, de2->d_name);
                    if(ok < 0 || (size_t)ok >= sizeof so) continue;
                    if(access(so,R_OK)==0 && n<maxn){
                        path_copy(paths[n++], 512, so);
                    }
                }
                closedir(d2);
            } else if(S_ISREG(st.st_mode)){
                if(name_ends_with_so(de->d_name) && access(sub,R_OK)==0 && n<maxn){
                    path_copy(paths[n++], 512, sub);
                }
            }
        }
        closedir(d);
    }
    return n;
}


static int resolve_addon_arg(const char *arg, char out[512]){
    if(!arg || !out) return -1;
    out[0] = '\0';

    if(strstr(arg, ".so") && access(arg, R_OK)==0){
        return path_copy(out, 512, arg);
    }

    char want1[512], want2[512];
    int n1 = snprintf(want1, sizeof want1, "ph-lib%s.so", arg);
    int n2 = snprintf(want2, sizeof want2, "%s.so", arg);
    if(n1 < 0 || n2 < 0 || (size_t)n1 >= sizeof want1 || (size_t)n2 >= sizeof want2)
        return -1;

    char paths[128][512];
    int n = scan_addon_paths(paths, 128);
    for(int i=0;i<n;i++){
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        if(strcmp(base, want1)==0 || strcmp(base, want2)==0 || strcmp(base, arg)==0){
            return path_copy(out, 512, paths[i]);
        }
    }
    return -1;
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

    snprintf(p.path, sizeof p.path, "%s", so_path);

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
        char name[POC_MAX_FEED];
        if(json_get_string(js,"feed",name,sizeof name)==0)
            feedtab_unsub(&g_feeds, name, fd);

    } else if(strcmp(type,"publish")==0){
        char name[POC_MAX_FEED];
        if(json_get_string(js,"feed",name,sizeof name)==0){
            broadcast_to_subs(name, js, strlen(js), fds, nfds);
            /* CLI publishers can request a broker-level dispatch acknowledgement.
             * It confirms ordered delivery into subscriber sockets, not addon success. */
            char ack_req[16];
            if(json_get_string(js,"ack",ack_req,sizeof ack_req)==0 &&
               (!strcmp(ack_req,"true") || !strcmp(ack_req,"1"))){
                char feed_esc[POC_MAX_FEED * 2], ack[POC_MAX_JSON];
                ph_json_escape_string(name, feed_esc, sizeof feed_esc);
                int n = snprintf(ack,sizeof ack,
                    "{\"type\":\"ack\",\"op\":\"publish\",\"feed\":\"%s\"}", feed_esc);
                if(n > 0 && (size_t)n < sizeof ack) (void)send_frame_json(fd,ack,(size_t)n);
            }
        }

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
                char ne[128], pe[1024];
                ph_json_escape_string(g_plugins.v[i].name, ne, sizeof ne);
                ph_json_escape_string(g_plugins.v[i].path[0]?g_plugins.v[i].path:"", pe, sizeof pe);
                int len = snprintf(buf, sizeof buf, "{\"type\":\"info\",\"plugin\":\"%s\",\"path\":\"%s\"}",
                                   ne, pe);
                if(len > 0 && (size_t)len < sizeof buf) send_frame_json(fd, buf, (size_t)len);
            }

        } else if(strcmp(cmd,"available-addons")==0){
            char paths[128][512]; int n = scan_addon_paths(paths, 128);
            char *items[128]; for(int i=0;i<n;i++) items[i]=paths[i];
            json_send_kv_list(fd, "available-addons", "paths", items, n);

        } else if(strncmp(cmd,"load ",5)==0){
            char arg[256]; snprintf(arg,sizeof arg,"%s",cmd+5);
            char resolved[512], arg_esc[512];
            ph_json_escape_string(arg,arg_esc,sizeof arg_esc);
            if(resolve_addon_arg(arg, resolved)!=0){
                log_msg(LOG_ERROR, "load: addon '%s' not found; use available-addons or pass a readable .so path", arg);
                char buf[POC_MAX_JSON];
                int len = snprintf(buf, sizeof buf, "{\"type\":\"error\",\"msg\":\"addon not found: %s\"}", arg_esc);
                if(len > 0 && (size_t)len < sizeof buf) send_frame_json(fd, buf, (size_t)len);
            } else {
                int rc = load_plugin_from_path(resolved);
                char buf[POC_MAX_JSON], resolved_esc[1024];
                ph_json_escape_string(resolved,resolved_esc,sizeof resolved_esc);
                int len;
                if(rc==0 || rc==1)
                    len = snprintf(buf, sizeof buf, "{\"type\":\"info\",\"msg\":\"loaded %s\"}", resolved_esc);
                else
                    len = snprintf(buf, sizeof buf, "{\"type\":\"error\",\"msg\":\"failed to load %s (rc=%d)\"}", resolved_esc, rc);
                if(len > 0 && (size_t)len < sizeof buf) send_frame_json(fd, buf, (size_t)len);
            }

        } else if(strncmp(cmd,"unload ",7)==0){
            char name[256]; snprintf(name,sizeof name,"%s",cmd+7);
            int rc = unload_plugin_by_name(name);
            char buf[POC_MAX_JSON], name_esc[256];
            ph_json_escape_string(name,name_esc,sizeof name_esc);
            if(rc<0){
                log_msg(LOG_WARN, "unload: %s not found", name);
                int len = snprintf(buf, sizeof buf, "{\"type\":\"error\",\"msg\":\"addon not loaded: %s\"}", name_esc);
                if(len > 0 && (size_t)len < sizeof buf) send_frame_json(fd, buf, (size_t)len);
            } else {
                int len = snprintf(buf, sizeof buf, "{\"type\":\"info\",\"msg\":\"unloaded %s\"}", name_esc);
                if(len > 0 && (size_t)len < sizeof buf) send_frame_json(fd, buf, (size_t)len);
            }

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
    char buf[POC_MAX_JSON]; size_t pos=0; int w;
    w = snprintf(buf, sizeof buf, "{\"type\":\"%s\",\"%s\":[", type, key);
    if(w < 0 || (size_t)w >= sizeof buf) return;
    pos = (size_t)w;
    for(int i=0;i<n;i++){
        char esc[512];
        ph_json_escape_string(items[i], esc, sizeof esc);
        const char *comma = (i+1<n)?",":"";
        w = snprintf(buf+pos, sizeof buf - pos, "\"%s\"%s", esc, comma);
        if(w < 0 || (size_t)w >= sizeof buf - pos) break;
        pos += (size_t)w;
    }
    if(pos + 2 < sizeof buf){ buf[pos++]=']'; buf[pos++]='}'; }
    send_frame_json(fd, buf, pos);
}

/* ========= Main ========= */

int main(void){
    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGBUS,  crash_handler);
    feedtab_init(&g_feeds);
    plugtab_init(&g_plugins);

    g_listen_fd = uds_listen_create(PH_SOCK_PATH);
    if(g_listen_fd<0){ log_msg(LOG_ERROR, "failed to create UDS server"); return 1; }
    log_msg(LOG_INFO, "PhaseHound-core %s (%s)  listening on %s", PH_VERSION_STRING, PH_GIT_SHA, PH_SOCK_PATH);

    /* core subscribes to cli-control */
    feedtab_ensure(&g_feeds, "cli-control");

    /* autoload addons present */
    autoload_addons();

    /* event loop — epoll-based, no FD_SETSIZE limit */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if(epfd < 0){ log_msg(LOG_ERROR,"epoll_create1: %s", strerror(errno)); return 1; }

    { struct epoll_event ev = {0}; ev.events = EPOLLIN; ev.data.fd = g_listen_fd;
      epoll_ctl(epfd, EPOLL_CTL_ADD, g_listen_fd, &ev); }

    struct epoll_event evbuf[64];
    for(;;){
        if(!g_run) break;
        int nr = epoll_wait(epfd, evbuf, 64, 200);
        if(nr < 0){ if(errno==EINTR) continue; log_msg(LOG_ERROR,"epoll_wait: %s", strerror(errno)); break; }

        for(int ei = 0; ei < nr; ei++){
            int fd = evbuf[ei].data.fd;

            if(fd == g_listen_fd){
                int cfd = accept(g_listen_fd, NULL, NULL);
                if(cfd >= 0){
                    if(g_client_count >= PH_MAX_CLIENTS){
                        log_msg(LOG_WARN, "client limit %d reached, rejecting fd=%d", PH_MAX_CLIENTS, cfd);
                        close(cfd);
                    } else {
                        set_nonblock(cfd);
                        struct epoll_event cev = {0};
                        cev.events = EPOLLIN;
                        cev.data.fd = cfd;
                        if(epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev) < 0){
                            log_msg(LOG_ERROR,"epoll_ctl add: %s", strerror(errno));
                            close(cfd);
                        } else {
                            g_client_count++;
                            log_msg(LOG_INFO,"client connected fd=%d (total=%d)", cfd, g_client_count);
                        }
                    }
                }
            } else {
                char js[POC_MAX_JSON];
                int ancfds[16];
                size_t anccnt = sizeof(ancfds)/sizeof(ancfds[0]);
                int got = recv_frame_json_with_fds(fd, js, sizeof js-1, ancfds, &anccnt, 10);
                if(got <= 0){
                    g_client_count--;
                    log_msg(LOG_INFO,"client fd=%d disconnected (total=%d)", fd, g_client_count);
                    feedtab_unsub_all_fd(&g_feeds, fd);
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                } else {
                    handle_msg(fd, js, ancfds, anccnt);
                    for(size_t k = 0; k < anccnt; k++) if(ancfds[k] >= 0) close(ancfds[k]);
                }
            }
        }
    }
    close(epfd);

    printf ("\t(8D)\n");
    log_msg(LOG_INFO, "core shutting down...");
    close(g_listen_fd);
    plugtab_free(&g_plugins);
    feedtab_free(&g_feeds);
    unlink(PH_SOCK_PATH);
    return 0;
}
