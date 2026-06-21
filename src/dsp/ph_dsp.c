#include "ph_dsp.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void ph_dsp_nco_f32_set_freq(ph_dsp_nco_f32_t *nco, double fs_hz, double freq_hz) {
    if (!nco || !(fs_hz > 0.0)) return;
    double dph = 2.0 * M_PI * freq_hz / fs_hz;
    nco->rc = (float)cos(dph);
    nco->rs = (float)sin(dph);
}

void ph_dsp_nco_f32_init(ph_dsp_nco_f32_t *nco, double fs_hz, double freq_hz, double phase_rad) {
    if (!nco) return;
    nco->c = (float)cos(phase_rad);
    nco->s = (float)sin(phase_rad);
    nco->renorm = 0;
    ph_dsp_nco_f32_set_freq(nco, fs_hz, freq_hz);
}

void ph_dsp_nco_f32_next(ph_dsp_nco_f32_t *nco, float *c, float *s) {
    float nc = nco->c * nco->rc - nco->s * nco->rs;
    float ns = nco->s * nco->rc + nco->c * nco->rs;
    nco->c = nc;
    nco->s = ns;

    /* Cheap drift control; avoids a sqrt every sample. */
    if ((++nco->renorm & 4095u) == 0u) {
        float m2 = nco->c * nco->c + nco->s * nco->s;
        if (m2 > 0.0f) {
            float inv = 1.0f / sqrtf(m2);
            nco->c *= inv;
            nco->s *= inv;
        }
    }

    *c = nco->c;
    *s = nco->s;
}

static void fft_bitrev(float *buf, int N) {
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = buf[2*i];   buf[2*i]   = buf[2*j];   buf[2*j]   = tr;
            float ti = buf[2*i+1]; buf[2*i+1] = buf[2*j+1]; buf[2*j+1] = ti;
        }
    }
}

void ph_fft_cf32(float *buf, int N, int inverse) {
    fft_bitrev(buf, N);
    double sign = inverse ? 1.0 : -1.0;
    for (int s = 1; s < N; s <<= 1) {
        int s2 = s << 1;
        double ang = sign * M_PI / (double)s;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int k = 0; k < N; k += s2) {
            float cr = 1.0f, ci = 0.0f;
            for (int j = 0; j < s; j++) {
                int a = k + j, b = a + s;
                float tr = cr*buf[2*b]   - ci*buf[2*b+1];
                float ti = cr*buf[2*b+1] + ci*buf[2*b];
                buf[2*b]   = buf[2*a]   - tr;
                buf[2*b+1] = buf[2*a+1] - ti;
                buf[2*a]   += tr;
                buf[2*a+1] += ti;
                float nc = cr*wr - ci*wi;
                ci = cr*wi + ci*wr;
                cr = nc;
            }
        }
    }
}
