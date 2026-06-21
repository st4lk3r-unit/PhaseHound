// ph-waterfall — Real-time IQ waterfall + spectrum viewer
// Connects to ph-core, subscribes to an IQ feed SHM ring, renders scrolling
// power spectrum via OpenGL 3.3 + GLSL with inferno colourmap.
//
// Build: make waterfall   (requires libglfw3-dev, libGL-dev)
// Usage: ./ph-waterfall [--feed <name>] [--fft <N>] [--width <px>] [--height <px>]
//                       [--rows <N>] [--dbmin <dB>] [--dbmax <dB>]
//
// Keys:
//   A        Auto-set dB range from live signal (also auto-triggers ~2 s after first data)
//   + / -    Raise / lower upper dB limit by 5
//   [ / ]    Lower / raise lower dB limit by 5
//   Q / Esc  Quit

#define _GNU_SOURCE
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <GL/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "ph_uds_protocol.h"
#include "common.h"
#include "ph_shm.h"
#include "ph_ring.h"
#include "ph_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- GL 3.3 type aliases ---- */
typedef char      GLchar;
typedef ptrdiff_t GLsizeiptr;

/* ---- GL 3.3 function pointer types ---- */
typedef GLuint (*PFCREATE_SHADER)(GLenum);
typedef void   (*PFSHADERSOURCE)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void   (*PFCOMPILE_SHADER)(GLuint);
typedef void   (*PFGET_SHADERIV)(GLuint, GLenum, GLint *);
typedef void   (*PFGET_SHADER_INFOLOG)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void   (*PFDELETE_SHADER)(GLuint);
typedef GLuint (*PFCREATE_PROGRAM)(void);
typedef void   (*PFATTACH_SHADER)(GLuint, GLuint);
typedef void   (*PFLINK_PROGRAM)(GLuint);
typedef void   (*PFGET_PROGRAMIV)(GLuint, GLenum, GLint *);
typedef void   (*PFGET_PROGRAM_INFOLOG)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void   (*PFDELETE_PROGRAM)(GLuint);
typedef void   (*PFUSE_PROGRAM)(GLuint);
typedef void   (*PFGEN_VERTEX_ARRAYS)(GLsizei, GLuint *);
typedef void   (*PFBIND_VERTEX_ARRAY)(GLuint);
typedef void   (*PFDELETE_VERTEX_ARRAYS)(GLsizei, const GLuint *);
typedef void   (*PFGEN_BUFFERS)(GLsizei, GLuint *);
typedef void   (*PFBIND_BUFFER)(GLenum, GLuint);
typedef void   (*PFBUFFER_DATA)(GLenum, GLsizeiptr, const void *, GLenum);
typedef void   (*PFDELETE_BUFFERS)(GLsizei, const GLuint *);
typedef void   (*PFENABLE_VERTEX_ATTRIB_ARRAY)(GLuint);
typedef void   (*PFVERTEX_ATTRIB_POINTER)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef GLint  (*PFGET_UNIFORM_LOCATION)(GLuint, const GLchar *);
typedef void   (*PFUNIFORM1I)(GLint, GLint);
typedef void   (*PFUNIFORM1F)(GLint, GLfloat);
typedef void   (*PFACTIVE_TEXTURE)(GLenum);

static PFCREATE_SHADER           p_CreateShader;
static PFSHADERSOURCE            p_ShaderSource;
static PFCOMPILE_SHADER          p_CompileShader;
static PFGET_SHADERIV            p_GetShaderiv;
static PFGET_SHADER_INFOLOG      p_GetShaderInfoLog;
static PFDELETE_SHADER           p_DeleteShader;
static PFCREATE_PROGRAM          p_CreateProgram;
static PFATTACH_SHADER           p_AttachShader;
static PFLINK_PROGRAM            p_LinkProgram;
static PFGET_PROGRAMIV           p_GetProgramiv;
static PFGET_PROGRAM_INFOLOG     p_GetProgramInfoLog;
static PFDELETE_PROGRAM          p_DeleteProgram;
static PFUSE_PROGRAM             p_UseProgram;
static PFGEN_VERTEX_ARRAYS       p_GenVertexArrays;
static PFBIND_VERTEX_ARRAY       p_BindVertexArray;
static PFDELETE_VERTEX_ARRAYS    p_DeleteVertexArrays;
static PFGEN_BUFFERS             p_GenBuffers;
static PFBIND_BUFFER             p_BindBuffer;
static PFBUFFER_DATA             p_BufferData;
static PFDELETE_BUFFERS          p_DeleteBuffers;
static PFENABLE_VERTEX_ATTRIB_ARRAY p_EnableVertexAttribArray;
static PFVERTEX_ATTRIB_POINTER   p_VertexAttribPointer;
static PFGET_UNIFORM_LOCATION    p_GetUniformLocation;
static PFUNIFORM1I               p_Uniform1i;
static PFUNIFORM1F               p_Uniform1f;
static PFACTIVE_TEXTURE          p_ActiveTexture;

#define GLPROC(T, sym) ((T)(void*)(uintptr_t)glfwGetProcAddress(sym))

static int load_gl3(void) {
#define CHK(ptr, name) do { if (!(ptr)) { fprintf(stderr,"[waterfall] %s not found\n",name); return 0; } } while(0)
    p_CreateShader         = GLPROC(PFCREATE_SHADER,           "glCreateShader");         CHK(p_CreateShader,"glCreateShader");
    p_ShaderSource         = GLPROC(PFSHADERSOURCE,            "glShaderSource");          CHK(p_ShaderSource,"glShaderSource");
    p_CompileShader        = GLPROC(PFCOMPILE_SHADER,          "glCompileShader");         CHK(p_CompileShader,"glCompileShader");
    p_GetShaderiv          = GLPROC(PFGET_SHADERIV,            "glGetShaderiv");           CHK(p_GetShaderiv,"glGetShaderiv");
    p_GetShaderInfoLog     = GLPROC(PFGET_SHADER_INFOLOG,      "glGetShaderInfoLog");      CHK(p_GetShaderInfoLog,"glGetShaderInfoLog");
    p_DeleteShader         = GLPROC(PFDELETE_SHADER,           "glDeleteShader");          CHK(p_DeleteShader,"glDeleteShader");
    p_CreateProgram        = GLPROC(PFCREATE_PROGRAM,          "glCreateProgram");         CHK(p_CreateProgram,"glCreateProgram");
    p_AttachShader         = GLPROC(PFATTACH_SHADER,           "glAttachShader");          CHK(p_AttachShader,"glAttachShader");
    p_LinkProgram          = GLPROC(PFLINK_PROGRAM,            "glLinkProgram");           CHK(p_LinkProgram,"glLinkProgram");
    p_GetProgramiv         = GLPROC(PFGET_PROGRAMIV,           "glGetProgramiv");          CHK(p_GetProgramiv,"glGetProgramiv");
    p_GetProgramInfoLog    = GLPROC(PFGET_PROGRAM_INFOLOG,     "glGetProgramInfoLog");     CHK(p_GetProgramInfoLog,"glGetProgramInfoLog");
    p_DeleteProgram        = GLPROC(PFDELETE_PROGRAM,          "glDeleteProgram");         CHK(p_DeleteProgram,"glDeleteProgram");
    p_UseProgram           = GLPROC(PFUSE_PROGRAM,             "glUseProgram");            CHK(p_UseProgram,"glUseProgram");
    p_GenVertexArrays      = GLPROC(PFGEN_VERTEX_ARRAYS,       "glGenVertexArrays");       CHK(p_GenVertexArrays,"glGenVertexArrays");
    p_BindVertexArray      = GLPROC(PFBIND_VERTEX_ARRAY,       "glBindVertexArray");       CHK(p_BindVertexArray,"glBindVertexArray");
    p_DeleteVertexArrays   = GLPROC(PFDELETE_VERTEX_ARRAYS,    "glDeleteVertexArrays");    CHK(p_DeleteVertexArrays,"glDeleteVertexArrays");
    p_GenBuffers           = GLPROC(PFGEN_BUFFERS,             "glGenBuffers");            CHK(p_GenBuffers,"glGenBuffers");
    p_BindBuffer           = GLPROC(PFBIND_BUFFER,             "glBindBuffer");            CHK(p_BindBuffer,"glBindBuffer");
    p_BufferData           = GLPROC(PFBUFFER_DATA,             "glBufferData");            CHK(p_BufferData,"glBufferData");
    p_DeleteBuffers        = GLPROC(PFDELETE_BUFFERS,          "glDeleteBuffers");         CHK(p_DeleteBuffers,"glDeleteBuffers");
    p_EnableVertexAttribArray = GLPROC(PFENABLE_VERTEX_ATTRIB_ARRAY, "glEnableVertexAttribArray"); CHK(p_EnableVertexAttribArray,"glEnableVertexAttribArray");
    p_VertexAttribPointer  = GLPROC(PFVERTEX_ATTRIB_POINTER,   "glVertexAttribPointer");   CHK(p_VertexAttribPointer,"glVertexAttribPointer");
    p_GetUniformLocation   = GLPROC(PFGET_UNIFORM_LOCATION,    "glGetUniformLocation");    CHK(p_GetUniformLocation,"glGetUniformLocation");
    p_Uniform1i            = GLPROC(PFUNIFORM1I,               "glUniform1i");             CHK(p_Uniform1i,"glUniform1i");
    p_Uniform1f            = GLPROC(PFUNIFORM1F,               "glUniform1f");             CHK(p_Uniform1f,"glUniform1f");
    p_ActiveTexture        = GLPROC(PFACTIVE_TEXTURE,          "glActiveTexture");         CHK(p_ActiveTexture,"glActiveTexture");
#undef CHK
    return 1;
}

/* ---- GL 3.3 constants ---- */
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER   0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS  0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS     0x8B82
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER    0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW     0x88B4
#endif
#ifndef GL_R32F
#define GL_R32F            0x822E
#endif
#ifndef GL_RED
#define GL_RED             0x1903
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0        0x84C0
#endif

/* ---- configuration ---- */
static const char  *g_feed      = "soapy.IQ-info";
static int          g_fft_n     = 2048;
static int          g_width     = 1400;
static int          g_height    = 900;
static int          g_tex_h     = 2048;   /* ring depth in rows; controls how much history is visible */
static float        g_dbmin     = -90.0f;
static float        g_dbmax     = -20.0f;
static const float  g_spec_frac = 0.22f;  /* fraction of window height for spectrum pane */

/* ---- auto-gain: percentile tracker updated in fft_thread ---- */
static struct {
    pthread_mutex_t mu;
    float p05;   /* 5th-percentile dB across recent rows ≈ noise floor   */
    float p95;   /* 95th-percentile dB ≈ peak signal level               */
    int   valid;
} g_auto = { .mu = PTHREAD_MUTEX_INITIALIZER, .p05 = -90.0f, .p95 = -20.0f, .valid = 0 };

/* ---- ring metadata (updated in recv_thread, read in render thread) ---- */
static double          g_center_freq = 0.0;
static double          g_sample_rate = 0.0;
static pthread_mutex_t g_info_mu     = PTHREAD_MUTEX_INITIALIZER;

/* CPU-side copy of the latest FFT row for cursor dB readback */
static float          *g_latest_row_cpu = NULL; /* allocated in main, [g_fft_n] */
static pthread_mutex_t g_latest_mu      = PTHREAD_MUTEX_INITIALIZER;

/* ---- shared IQ ring ---- */
typedef struct { int memfd; phiq_hdr_t *hdr; size_t map_bytes; } wf_ring_t;
static wf_ring_t          g_ring    = { .memfd=-1, .hdr=NULL, .map_bytes=0 };
static pthread_mutex_t    g_ring_mu = PTHREAD_MUTEX_INITIALIZER;
static ph_ring_consumer_t g_ring_cons;

static void ring_close_locked(wf_ring_t *r) {
    if (r->hdr && r->hdr!=MAP_FAILED) munmap(r->hdr, r->map_bytes);
    if (r->memfd>=0) close(r->memfd);
    r->hdr=NULL; r->map_bytes=0; r->memfd=-1;
}

/* ---- row queue (fft thread → render thread) ---- */
#define ROW_Q_DEPTH 512
static float         **g_row_q  = NULL;
static int             g_row_wr = 0;
static int             g_row_rd = 0;
static pthread_mutex_t g_row_mu = PTHREAD_MUTEX_INITIALIZER;

static void row_enqueue(float *row) {
    pthread_mutex_lock(&g_row_mu);
    int next = (g_row_wr+1) % ROW_Q_DEPTH;
    if (next == g_row_rd) {
        free(g_row_q[g_row_rd]);
        g_row_q[g_row_rd] = NULL;
        g_row_rd = (g_row_rd+1) % ROW_Q_DEPTH;
    }
    g_row_q[g_row_wr] = row;
    g_row_wr = next;
    pthread_mutex_unlock(&g_row_mu);
}

static float *row_dequeue_nowait(void) {
    pthread_mutex_lock(&g_row_mu);
    if (g_row_rd == g_row_wr) { pthread_mutex_unlock(&g_row_mu); return NULL; }
    float *r = g_row_q[g_row_rd];
    g_row_q[g_row_rd] = NULL;
    g_row_rd = (g_row_rd+1) % ROW_Q_DEPTH;
    pthread_mutex_unlock(&g_row_mu);
    return r;
}

static _Atomic int g_run = 1;

/* ---- recv thread ---- */
static void *recv_thread(void *arg) {
    (void)arg;
    int fd = ph_connect_retry(PH_SOCK_PATH, 60, 200);
    if (fd<0) { fprintf(stderr,"[waterfall] connect failed\n"); return NULL; }

    char fesc[128], sub[256];
    ph_json_escape_string(g_feed, fesc, sizeof fesc);
    snprintf(sub, sizeof sub, "{\"type\":\"subscribe\",\"feed\":\"%s\"}", fesc);
    send_frame_json(fd, sub, strlen(sub));
    fprintf(stderr,"[waterfall] subscribed to %s\n", g_feed);

    char js[POC_MAX_JSON];
    while (atomic_load(&g_run)) {
        int infd=-1; size_t nfds=1;
        int got=recv_frame_json_with_fds(fd,js,sizeof js,&infd,&nfds,300);
        if (got<=0) continue;

        char type[32]={0}, feed[128]={0};
        json_get_type(js,type,sizeof type);
        json_get_string(js,"feed",feed,sizeof feed);

        if (strcmp(type,"publish")==0 && strcmp(feed,g_feed)==0 && nfds==1 && infd>=0) {
            struct stat st;
            if (fstat(infd,&st)==0 && st.st_size>(off_t)sizeof(phiq_hdr_t)) {
                void *base=mmap(NULL,(size_t)st.st_size,PROT_READ|PROT_WRITE,MAP_SHARED,infd,0);
                if (base && base!=MAP_FAILED) {
                    pthread_mutex_lock(&g_ring_mu);
                    ring_close_locked(&g_ring);
                    g_ring.memfd=infd; infd=-1;
                    g_ring.hdr=(phiq_hdr_t*)base;
                    g_ring.map_bytes=(size_t)st.st_size;
                    ph_iq_ring_consumer_init_live(&g_ring_cons,g_ring.hdr);
                    double cf=g_ring.hdr->center_freq, fs=g_ring.hdr->sample_rate;
                    pthread_mutex_unlock(&g_ring_mu);
                    pthread_mutex_lock(&g_info_mu);
                    g_center_freq=cf; g_sample_rate=fs;
                    pthread_mutex_unlock(&g_info_mu);
                    fprintf(stderr,"[waterfall] ring mapped: cf=%.0f fs=%.0f bps=%u\n",
                            cf, fs, g_ring.hdr->bytes_per_samp);
                }
            }
        }
        if (infd>=0) close(infd);
    }
    close(fd);
    return NULL;
}

/* ---- FFT thread ---- */
static float *g_hann=NULL, *g_fft_work=NULL;

static void build_hann(int N) {
    free(g_hann);
    g_hann=(float*)malloc((size_t)N*sizeof(float));
    if (!g_hann) return;
    for (int k=0;k<N;k++)
        g_hann[k]=0.5f*(1.0f-cosf((float)(2.0*M_PI*k/(N-1))));
}

static int cmp_float_asc(const void *a, const void *b) {
    float fa=*(const float*)a, fb=*(const float*)b;
    return (fa>fb)-(fa<fb);
}

static void *fft_thread(void *arg) {
    (void)arg;
    int N=g_fft_n;
    build_hann(N);
    g_fft_work=(float*)malloc((size_t)N*2*sizeof(float));
    float *sort_buf=(float*)malloc((size_t)N*sizeof(float));
    if (!g_hann||!g_fft_work||!sort_buf) return NULL;

    float  *accum=(float*)calloc((size_t)N*2,sizeof(float));
    int     accum_n=0;
    uint8_t *raw=NULL; size_t raw_cap=0;
    float   *tmp_f=NULL; size_t tmp_cap=0;
    int      auto_ctr=0;

    while (atomic_load(&g_run)) {
        pthread_mutex_lock(&g_ring_mu);
        phiq_hdr_t *h=g_ring.hdr;
        if (!h||h->capacity==0||h->bytes_per_samp==0) {
            pthread_mutex_unlock(&g_ring_mu); ph_msleep(5); continue;
        }
        size_t want=(size_t)N*h->bytes_per_samp*4;
        if (raw_cap<want) {
            uint8_t *p=(uint8_t*)realloc(raw,want);
            if (!p) { pthread_mutex_unlock(&g_ring_mu); ph_msleep(5); continue; }
            raw=p; raw_cap=want;
        }
        uint64_t lost=0;
        size_t got_b=ph_iq_ring_consume_copy(h,&g_ring_cons,raw,want,&lost);
        uint32_t fmt=h->fmt, bps=h->bytes_per_samp;
        pthread_mutex_unlock(&g_ring_mu);

        if (got_b==0) { ph_msleep(1); continue; }
        size_t nsamp=got_b/bps;
        const float *src=NULL;

        if (fmt==PHIQ_FMT_CF32) {
            src=(const float*)raw;
        } else if (fmt==PHIQ_FMT_CS16) {
            if (tmp_cap<nsamp*2) {
                float *p=(float*)realloc(tmp_f,nsamp*2*sizeof(float));
                if (!p) continue;
                tmp_f=p; tmp_cap=nsamp*2;
            }
            const int16_t *s=(const int16_t*)raw;
            const float sc=1.0f/32768.0f;
            for (size_t i=0;i<nsamp;i++) {
                tmp_f[2*i+0]=(float)s[2*i+0]*sc;
                tmp_f[2*i+1]=(float)s[2*i+1]*sc;
            }
            src=tmp_f;
        } else { continue; }

        for (size_t i=0;i<nsamp&&atomic_load(&g_run);i++) {
            accum[2*accum_n+0]=src[2*i+0];
            accum[2*accum_n+1]=src[2*i+1];
            accum_n++;
            if (accum_n>=N) {
                for (int k=0;k<N;k++) {
                    g_fft_work[2*k+0]=accum[2*k+0]*g_hann[k];
                    g_fft_work[2*k+1]=accum[2*k+1]*g_hann[k];
                }
                ph_fft_cf32(g_fft_work,N,0);
                float *row=(float*)malloc((size_t)N*sizeof(float));
                if (!row) { accum_n=0; continue; }

                /* Store raw dB — normalization done in fragment shader so
                 * changing g_dbmin/g_dbmax instantly recolours all history. */
                int half=N/2;
                for (int k=0;k<N;k++) {
                    int sk=(k+half)%N;
                    float r=g_fft_work[2*sk+0], im=g_fft_work[2*sk+1];
                    row[k]=10.0f*log10f(r*r+im*im+1e-30f);
                }

                /* Update percentile estimates every 40 rows (~34 ms @ 2.4 Msps) */
                if (++auto_ctr % 40 == 0) {
                    memcpy(sort_buf, row, (size_t)N*sizeof(float));
                    qsort(sort_buf, (size_t)N, sizeof(float), cmp_float_asc);
                    float p05=sort_buf[(int)(0.05f*N)];
                    float p95=sort_buf[(int)(0.95f*N)];
                    pthread_mutex_lock(&g_auto.mu);
                    if (!g_auto.valid) {
                        g_auto.p05=p05; g_auto.p95=p95; g_auto.valid=1;
                    } else {
                        g_auto.p05=0.88f*g_auto.p05+0.12f*p05;
                        g_auto.p95=0.88f*g_auto.p95+0.12f*p95;
                    }
                    pthread_mutex_unlock(&g_auto.mu);
                }

                row_enqueue(row);
                accum_n=0;
            }
        }
    }
    free(accum); free(raw); free(tmp_f); free(sort_buf);
    free(g_hann); free(g_fft_work);
    return NULL;
}

/* ---- GLSL ---- */

/* Shared vertex shader: explicit pos (loc=0) + uv (loc=1) */
static const char *VERT=
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main(){\n"
    "    gl_Position=vec4(aPos,0.0,1.0);\n"
    "    vUV=aUV;\n"
    "}\n";

/* Waterfall: ring-buffer texture + inferno colourmap + freq tick marks */
static const char *WF_FRAG=
    "#version 330 core\n"
    "in vec2 vUV; out vec4 C;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_row_off;\n"
    "uniform float u_dbmin;\n"
    "uniform float u_dbmax;\n"
    "vec3 inferno(float t){\n"
    "  t=clamp(t,0.0,1.0);\n"
    "  vec3 c0=vec3(0.00021,0.00016,0.01376);\n"
    "  vec3 c1=vec3(0.10648,0.02819,0.48692);\n"
    "  vec3 c2=vec3(1.37634,0.06176,0.17632);\n"
    "  vec3 c3=vec3(-3.75012,2.16480,-0.34900);\n"
    "  vec3 c4=vec3(3.32534,-3.89258,2.36554);\n"
    "  vec3 c5=vec3(-1.10993,1.61750,-1.91299);\n"
    "  return clamp(c0+t*(c1+t*(c2+t*(c3+t*(c4+t*c5)))),0.0,1.0);\n"
    "}\n"
    "void main(){\n"
    "  float lw=fwidth(vUV.x)*1.5;\n"
    "  if(abs(vUV.x-0.25)<lw||abs(vUV.x-0.5)<lw||abs(vUV.x-0.75)<lw){\n"
    "    C=vec4(1.0,1.0,1.0,1.0); return;\n"
    "  }\n"
    "  float ty=fract(vUV.y+u_row_off);\n"
    "  float raw_db=texture(u_tex,vec2(vUV.x,ty)).r;\n"
    "  float v=clamp((raw_db-u_dbmin)/(u_dbmax-u_dbmin+0.001),0.0,1.0);\n"
    "  C=vec4(inferno(v),1.0);\n"
    "}\n";

/* Spectrum: power vs frequency filled area from the latest texture row */
static const char *SPEC_FRAG=
    "#version 330 core\n"
    "in vec2 vUV; out vec4 C;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_spec_row;\n"
    "uniform float u_dbmin;\n"
    "uniform float u_dbmax;\n"
    "void main(){\n"
    "  float raw_db=texture(u_tex,vec2(vUV.x,u_spec_row)).r;\n"
    "  float norm=clamp((raw_db-u_dbmin)/(u_dbmax-u_dbmin+0.001),0.0,1.0);\n"
    "  float lw=fwidth(vUV.x)*1.5;\n"
    "  bool tick=abs(vUV.x-0.25)<lw||abs(vUV.x-0.5)<lw||abs(vUV.x-0.75)<lw;\n"
    /* dark background */
    "  C=vec4(0.04,0.04,0.07,1.0);\n"
    "  if(vUV.y<=norm){\n"
    "    vec3 col=mix(vec3(0.04,0.28,0.18),vec3(0.15,1.0,0.55),vUV.y/max(norm,0.001));\n"
    "    C=tick?vec4(1.0,1.0,0.3,1.0):vec4(col,1.0);\n"
    "  } else if(vUV.y<=norm+fwidth(vUV.y)*2.5){\n"
    "    C=vec4(0.6,1.0,0.6,1.0);\n"
    "  } else if(tick){\n"
    "    C=vec4(0.22,0.22,0.22,1.0);\n"
    "  }\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s=p_CreateShader(type);
    p_ShaderSource(s,1,&src,NULL);
    p_CompileShader(s);
    GLint ok=0; p_GetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if (!ok) {
        char log[1024]; p_GetShaderInfoLog(s,sizeof log,NULL,log);
        fprintf(stderr,"[waterfall] shader:\n%s\n",log);
        p_DeleteShader(s); return 0;
    }
    return s;
}
static GLuint link_program(const char *vs, const char *fs) {
    GLuint v=compile_shader(GL_VERTEX_SHADER,vs);
    GLuint f=compile_shader(GL_FRAGMENT_SHADER,fs);
    if(!v||!f){p_DeleteShader(v);p_DeleteShader(f);return 0;}
    GLuint p=p_CreateProgram();
    p_AttachShader(p,v); p_AttachShader(p,f); p_LinkProgram(p);
    p_DeleteShader(v); p_DeleteShader(f);
    GLint ok=0; p_GetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){char log[1024];p_GetProgramInfoLog(p,sizeof log,NULL,log);
        fprintf(stderr,"[waterfall] link:\n%s\n",log);p_DeleteProgram(p);return 0;}
    return p;
}

/* Build a VAO+VBO for one quad with explicit (pos.xy, uv.xy) per vertex */
static void make_quad(GLuint *vao, GLuint *vbo, const float verts[24]) {
    p_GenVertexArrays(1,vao); p_BindVertexArray(*vao);
    p_GenBuffers(1,vbo); p_BindBuffer(GL_ARRAY_BUFFER,*vbo);
    p_BufferData(GL_ARRAY_BUFFER,24*sizeof(float),verts,GL_STATIC_DRAW);
    /* location=0: pos (x,y), stride=16, offset=0 */
    p_EnableVertexAttribArray(0);
    p_VertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,(const void*)0);
    /* location=1: uv (x,y), stride=16, offset=8 */
    p_EnableVertexAttribArray(1);
    p_VertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,16,(const void*)8);
    p_BindVertexArray(0);
}

static void apply_auto_gain(void) {
    pthread_mutex_lock(&g_auto.mu);
    if (g_auto.valid) {
        g_dbmin = g_auto.p05 - 3.0f;
        g_dbmax = g_auto.p95 + 3.0f;
        fprintf(stderr,"[waterfall] auto-gain: dbmin=%.1f dbmax=%.1f\n", g_dbmin, g_dbmax);
    }
    pthread_mutex_unlock(&g_auto.mu);
}

int main(int argc, char **argv) {
    for (int i=1;i<argc;i++) {
        if      (!strcmp(argv[i],"--feed")   &&i+1<argc) g_feed   = argv[++i];
        else if (!strcmp(argv[i],"--fft")    &&i+1<argc) g_fft_n  = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--width")  &&i+1<argc) g_width  = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--height") &&i+1<argc) g_height = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--rows")   &&i+1<argc) g_tex_h  = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--dbmin")  &&i+1<argc) g_dbmin  = (float)atof(argv[++i]);
        else if (!strcmp(argv[i],"--dbmax")  &&i+1<argc) g_dbmax  = (float)atof(argv[++i]);
        else if (!strcmp(argv[i],"--help"))  {
            fprintf(stderr,
                "ph-waterfall [--feed F] [--fft N] [--width W] [--height H]\n"
                "             [--rows R] [--dbmin D] [--dbmax D]\n"
                "  --rows R   texture ring depth in FFT rows (default 2048 = ~1.7 s @ 2.4 Msps)\n"
                "Keys: A=auto-gain  +/-=raise/lower dbmax  [/]=lower/raise dbmin  Q/Esc=quit\n");
            return 0;
        } else { fprintf(stderr,"unknown: %s\n",argv[i]); return 1; }
    }
    if (g_fft_n<64||(g_fft_n&(g_fft_n-1))!=0) {
        fprintf(stderr,"--fft must be power of 2 >= 64\n"); return 1;
    }
    if (g_tex_h < g_height) g_tex_h = g_height;

    g_row_q=(float**)calloc(ROW_Q_DEPTH,sizeof(float*));
    if (!g_row_q) { perror("calloc"); return 1; }
    g_latest_row_cpu=(float*)calloc((size_t)g_fft_n,sizeof(float));
    if (!g_latest_row_cpu) { perror("calloc"); return 1; }

    pthread_t recv_thr,fft_thr;
    pthread_create(&recv_thr,NULL,recv_thread,NULL);
    pthread_create(&fft_thr, NULL,fft_thread, NULL);

    if (!glfwInit()) { fprintf(stderr,"glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE,GLFW_FALSE);

    char title[128];
    snprintf(title,sizeof title,"ph-waterfall  %s  FFT %d  %d rows",g_feed,g_fft_n,g_tex_h);
    GLFWwindow *win=glfwCreateWindow(g_width,g_height,title,NULL,NULL);
    if (!win) {
        fprintf(stderr,"glfwCreateWindow failed (no display?)\n");
        glfwTerminate(); atomic_store(&g_run,0);
        pthread_join(recv_thr,NULL); pthread_join(fft_thr,NULL);
        return 1;
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    if (!load_gl3()) { glfwTerminate(); return 1; }
    fprintf(stderr,"[waterfall] OpenGL %s\n",(const char*)glGetString(GL_VERSION));

    /* Build shader programs */
    GLuint wf_prog   = link_program(VERT, WF_FRAG);
    GLuint spec_prog  = link_program(VERT, SPEC_FRAG);
    if (!wf_prog||!spec_prog) { glfwTerminate(); return 1; }

    /* Uniform locations — waterfall */
    GLint wf_u_tex     = p_GetUniformLocation(wf_prog,  "u_tex");
    GLint wf_u_row_off = p_GetUniformLocation(wf_prog,  "u_row_off");
    GLint wf_u_dbmin   = p_GetUniformLocation(wf_prog,  "u_dbmin");
    GLint wf_u_dbmax   = p_GetUniformLocation(wf_prog,  "u_dbmax");

    /* Uniform locations — spectrum */
    GLint sp_u_tex      = p_GetUniformLocation(spec_prog, "u_tex");
    GLint sp_u_spec_row = p_GetUniformLocation(spec_prog, "u_spec_row");
    GLint sp_u_dbmin    = p_GetUniformLocation(spec_prog, "u_dbmin");
    GLint sp_u_dbmax    = p_GetUniformLocation(spec_prog, "u_dbmax");

    /* Layout split.
     * spec_y0 is the NDC y-coordinate of the boundary between spectrum (top)
     * and waterfall (bottom).  NDC runs from -1 (bottom) to +1 (top). */
    float spec_y0 = 1.0f - 2.0f * g_spec_frac;   /* e.g. 0.56 for 22% spec */

    /* Waterfall quad: NDC (-1,-1) to (1, spec_y0); UV (0,0)→(1,1) */
    float wf_verts[24] = {
        -1.0f, -1.0f,    0.0f, 0.0f,
         1.0f, -1.0f,    1.0f, 0.0f,
         1.0f,  spec_y0, 1.0f, 1.0f,
        -1.0f, -1.0f,    0.0f, 0.0f,
         1.0f,  spec_y0, 1.0f, 1.0f,
        -1.0f,  spec_y0, 0.0f, 1.0f,
    };
    /* Spectrum quad: NDC (-1, spec_y0) to (1, 1); UV (0,0)→(1,1) */
    float sp_verts[24] = {
        -1.0f,  spec_y0, 0.0f, 0.0f,
         1.0f,  spec_y0, 1.0f, 0.0f,
         1.0f,  1.0f,    1.0f, 1.0f,
        -1.0f,  spec_y0, 0.0f, 0.0f,
         1.0f,  1.0f,    1.0f, 1.0f,
        -1.0f,  1.0f,    0.0f, 1.0f,
    };
    GLuint wf_vao,wf_vbo, sp_vao,sp_vbo;
    make_quad(&wf_vao,&wf_vbo,wf_verts);
    make_quad(&sp_vao,&sp_vbo,sp_verts);

    /* Ring-buffer texture: width=fft_n, height=tex_h, GL_R32F stores raw dB */
    GLuint tex;
    glGenTextures(1,&tex);
    glBindTexture(GL_TEXTURE_2D,tex);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D,0,GL_R32F,g_fft_n,g_tex_h,0,GL_RED,GL_FLOAT,NULL);

    int   write_row    = 0;
    int   wrote_any    = 0;
    int   auto_applied = 0;
    double first_row_t = -1.0;
    int   title_ctr    = 0;    /* throttle glfwSetWindowTitle calls */
    int   title_has_cf = 0;   /* true once we've shown center freq in title */

    /* Key repeat suppression */
    int prev_A=0, prev_plus=0, prev_minus=0, prev_lbr=0, prev_rbr=0;

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        if (glfwGetKey(win,GLFW_KEY_ESCAPE)==GLFW_PRESS ||
            glfwGetKey(win,GLFW_KEY_Q)     ==GLFW_PRESS) break;

        /* Key handling (edge-triggered) */
        int cur_A    = glfwGetKey(win,GLFW_KEY_A);
        int cur_plus = glfwGetKey(win,GLFW_KEY_EQUAL);   /* = key (same as + without shift) */
        int cur_minus= glfwGetKey(win,GLFW_KEY_MINUS);
        int cur_lbr  = glfwGetKey(win,GLFW_KEY_LEFT_BRACKET);
        int cur_rbr  = glfwGetKey(win,GLFW_KEY_RIGHT_BRACKET);

        if (cur_A && !prev_A)    { apply_auto_gain(); auto_applied=1; }
        if (cur_plus && !prev_plus)  { g_dbmax+=5.0f; fprintf(stderr,"[waterfall] dbmax=%.1f\n",g_dbmax); }
        if (cur_minus&&!prev_minus)  { g_dbmax-=5.0f; fprintf(stderr,"[waterfall] dbmax=%.1f\n",g_dbmax); }
        if (cur_lbr && !prev_lbr)   { g_dbmin-=5.0f; fprintf(stderr,"[waterfall] dbmin=%.1f\n",g_dbmin); }
        if (cur_rbr && !prev_rbr)   { g_dbmin+=5.0f; fprintf(stderr,"[waterfall] dbmin=%.1f\n",g_dbmin); }
        prev_A=cur_A; prev_plus=cur_plus; prev_minus=cur_minus;
        prev_lbr=cur_lbr; prev_rbr=cur_rbr;

        /* Drain row queue — up to 64 rows per frame */
        float *row;
        int up=0;
        while (up<64 && (row=row_dequeue_nowait())!=NULL) {
            glBindTexture(GL_TEXTURE_2D,tex);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,write_row,g_fft_n,1,GL_RED,GL_FLOAT,row);
            pthread_mutex_lock(&g_latest_mu);
            memcpy(g_latest_row_cpu, row, (size_t)g_fft_n*sizeof(float));
            pthread_mutex_unlock(&g_latest_mu);
            free(row);
            write_row=(write_row+1)%g_tex_h;
            if (!wrote_any) {
                wrote_any=1;
                first_row_t=glfwGetTime();
            }
            up++;
        }

        /* Auto-gain 2 s after first data, once */
        if (!auto_applied && wrote_any &&
            glfwGetTime()-first_row_t >= 2.0 &&
            g_auto.valid) {
            apply_auto_gain();
            auto_applied=1;
        }

        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        p_ActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D,tex);

        /* Draw waterfall */
        p_UseProgram(wf_prog);
        p_Uniform1i(wf_u_tex,0);
        p_Uniform1f(wf_u_row_off,(float)write_row/(float)g_tex_h);
        p_Uniform1f(wf_u_dbmin,g_dbmin);
        p_Uniform1f(wf_u_dbmax,g_dbmax);
        p_BindVertexArray(wf_vao);
        glDrawArrays(GL_TRIANGLES,0,6);

        /* Draw spectrum (only once we have at least one row) */
        if (wrote_any) {
            int latest=(write_row-1+g_tex_h)%g_tex_h;
            float spec_row=(float)latest/(float)g_tex_h;
            p_UseProgram(spec_prog);
            p_Uniform1i(sp_u_tex,0);
            p_Uniform1f(sp_u_spec_row,spec_row);
            p_Uniform1f(sp_u_dbmin,g_dbmin);
            p_Uniform1f(sp_u_dbmax,g_dbmax);
            p_BindVertexArray(sp_vao);
            glDrawArrays(GL_TRIANGLES,0,6);
        }

        p_BindVertexArray(0);

        /* Update window title ~10 Hz with cursor position or ring metadata */
        if ((++title_ctr % 6) == 0) {
            pthread_mutex_lock(&g_info_mu);
            double cf=g_center_freq, fs=g_sample_rate;
            pthread_mutex_unlock(&g_info_mu);

            char ttl[256];
            double cx_d, cy_d;
            glfwGetCursorPos(win, &cx_d, &cy_d);
            int in_win = (cx_d>=0 && cx_d<g_width && cy_d>=0 && cy_d<g_height);

            if (in_win && fs>0.0) {
                double freq_x = cx_d / (double)g_width;
                double freq_hz = cf + (freq_x - 0.5) * fs;
                float db = 0.0f;
                pthread_mutex_lock(&g_latest_mu);
                int bin = (int)(freq_x * g_fft_n);
                if (bin<0) bin=0;
                if (bin>=g_fft_n) bin=g_fft_n-1;
                db = g_latest_row_cpu[bin];
                pthread_mutex_unlock(&g_latest_mu);
                snprintf(ttl, sizeof ttl,
                    "ph-waterfall | %.4f MHz | %.1f dB  (BW %.3f MHz)",
                    freq_hz/1e6, db, fs/1e6);
            } else if (fs > 0.0) {
                snprintf(ttl, sizeof ttl,
                    "ph-waterfall | CF %.4f MHz  BW %.3f MHz | FFT %d",
                    cf/1e6, fs/1e6, g_fft_n);
                title_has_cf = 1;
            } else if (!title_has_cf) {
                snprintf(ttl, sizeof ttl,
                    "ph-waterfall  %s  FFT %d  %d rows", g_feed, g_fft_n, g_tex_h);
            } else {
                ttl[0] = '\0';
            }
            if (ttl[0]) glfwSetWindowTitle(win, ttl);
        }

        glfwSwapBuffers(win);
    }

    atomic_store(&g_run,0);
    pthread_join(recv_thr,NULL);
    pthread_join(fft_thr, NULL);

    float *r;
    while ((r=row_dequeue_nowait())!=NULL) free(r);
    free(g_row_q);
    free(g_latest_row_cpu); g_latest_row_cpu=NULL;
    pthread_mutex_lock(&g_ring_mu); ring_close_locked(&g_ring); pthread_mutex_unlock(&g_ring_mu);

    glDeleteTextures(1,&tex);
    p_DeleteBuffers(1,&wf_vbo);  p_DeleteVertexArrays(1,&wf_vao);
    p_DeleteBuffers(1,&sp_vbo);  p_DeleteVertexArrays(1,&sp_vao);
    p_DeleteProgram(wf_prog);
    p_DeleteProgram(spec_prog);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
