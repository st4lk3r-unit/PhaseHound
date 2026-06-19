
#include "ph_uds_protocol.h"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static void usage(void){
    fprintf(stderr, "ph-cli usage:\n");
    fprintf(stderr, "  ph-cli help\n");
    fprintf(stderr, "  ph-cli cmd \"<text>\"\n");
    fprintf(stderr, "  ph-cli pub <feed> \"<data>\"\n");
    fprintf(stderr, "  ph-cli sub <feed> [feed2 ...]\n");
    fprintf(stderr, "  ph-cli list feeds | list addons | available-addons\n");
    fprintf(stderr, "  ph-cli load addon <name|/path/to/lib.so>\n");
    fprintf(stderr, "  ph-cli unload addon <name>\n");
}

static int extract_feed(const char *js, char *out, size_t cap){
    const char *p = strstr(js, "\"feed\":\"");
    if(!p) return -1;
    p += 8;
    const char *q = strchr(p, '"');
    if(!q) return -1;
    size_t n = (size_t)(q - p);
    if(n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

int main(int argc, char **argv){
    if(argc<2){ usage(); return 1; }
    int fd = uds_connect(PH_SOCK_PATH);
    if(fd<0){ perror("connect"); return 1; }
    char buf[POC_MAX_JSON];
    int wait_for_response = 1;
    int wait_for_publish_ack = 0;

    if(strcmp(argv[1],"help")==0){
        usage(); close(fd); return 0;
    } else if(strcmp(argv[1],"cmd")==0 && argc>=3){
        char esc[POC_MAX_JSON/2];
        ph_json_escape_string(argv[2], esc, sizeof esc);
        snprintf(buf, sizeof buf, "{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"%s\"}", esc);
        if(send_frame_json(fd, buf, strlen(buf)) < 0){ perror("send"); close(fd); return 1; }
    } else if(strcmp(argv[1],"pub")==0 && argc>=4){
        char feed[POC_MAX_FEED*2], data[POC_MAX_JSON/2];
        ph_json_escape_string(argv[2], feed, sizeof feed);
        ph_json_escape_string(argv[3], data, sizeof data);
        snprintf(buf, sizeof buf, "{\"type\":\"publish\",\"feed\":\"%s\",\"data\":\"%s\",\"encoding\":\"utf8\",\"ack\":true}", feed, data);
        if(send_frame_json(fd, buf, strlen(buf)) < 0){ perror("send"); close(fd); return 1; }
        wait_for_publish_ack = 1;
    } else if(strcmp(argv[1],"sub")==0 && argc>=3){
        for(int i=2;i<argc;i++){
            char feed[POC_MAX_FEED*2];
            ph_json_escape_string(argv[i], feed, sizeof feed);
            snprintf(buf, sizeof buf, "{\"type\":\"subscribe\",\"feed\":\"%s\"}", feed);
            if(send_frame_json(fd, buf, strlen(buf)) < 0){ perror("send"); close(fd); return 1; }
        }
        fprintf(stderr, "[ph-cli] subscribed to %d feed(s). Ctrl+C to stop.\n", argc-2);
        while(1){
            char js[POC_MAX_JSON];
            int got = recv_frame_json(fd, js, sizeof js, 2000);
            if(got<=0) continue;
            char tag[128];
            if(extract_feed(js, tag, sizeof tag)==0)
                fprintf(stdout, "[%s] %s\n", tag, js);
            else
                fprintf(stdout, "%s\n", js);
            fflush(stdout);
        }
    } else if(strcmp(argv[1],"list")==0 && argc>=3){
        if(strcmp(argv[2],"feeds")==0) snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"feeds\"}");
        else if(strcmp(argv[2],"addons")==0) snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"plugins\"}");
        else if(strcmp(argv[2],"available-addons")==0) snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"available-addons\"}");
        else { usage(); close(fd); return 1; }
        send_frame_json(fd, buf, strlen(buf));
    } else if(strcmp(argv[1],"available-addons")==0){
        snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"available-addons\"}");
        send_frame_json(fd, buf, strlen(buf));
    } else if(strcmp(argv[1],"load")==0 && argc>=4 && strcmp(argv[2],"addon")==0){
        char joined[512]; size_t pos=0; joined[0]=0;
        for(int i=3;i<argc;i++){
            int n=snprintf(joined+pos,sizeof joined-pos,"%s%s",i>3?" ":"",argv[i]);
            if(n<0 || (size_t)n>=sizeof joined-pos){ fprintf(stderr,"addon path too long\n"); close(fd); return 1; }
            pos+=(size_t)n;
        }
        char esc[1024]; ph_json_escape_string(joined,esc,sizeof esc);
        snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"load %s\"}", esc);
        if(send_frame_json(fd, buf, strlen(buf)) < 0){ perror("send"); close(fd); return 1; }
    } else if(strcmp(argv[1],"unload")==0 && argc>=4 && strcmp(argv[2],"addon")==0){
        char esc[256]; ph_json_escape_string(argv[3],esc,sizeof esc);
        snprintf(buf,sizeof buf,"{\"type\":\"command\",\"feed\":\"cli-control\",\"data\":\"unload %s\"}", esc);
        if(send_frame_json(fd, buf, strlen(buf)) < 0){ perror("send"); close(fd); return 1; }
    } else {
        usage(); close(fd); return 1;
    }

    if(!wait_for_response){ close(fd); return 0; }

    if(wait_for_publish_ack){
        char js[POC_MAX_JSON], type[32];
        int got = recv_frame_json(fd, js, sizeof js, 1500);
        if(got <= 0){ fprintf(stderr,"publish acknowledgement timed out\n"); close(fd); return 1; }
        if(json_get_type(js,type,sizeof type)<0 || strcmp(type,"ack")!=0){
            fprintf(stderr,"unexpected publish response: %s\n",js);
            close(fd); return 1;
        }
        close(fd);
        return 0;
    }

    // Print direct broker responses until timeout (~1.5s window)
    char js[POC_MAX_JSON];
    int printed = 0;
    while(1){
        size_t nfds=16; int fds[16]={0}; int got = recv_frame_json_with_fds(fd, js, sizeof js, fds, &nfds, 1500);
        if(got<=0) break;
        printf("%s%s\n", js, nfds? "  /* +FDs */": "");
        printed = 1;
    }
    close(fd);
    return printed ? 0 : 1;
}
