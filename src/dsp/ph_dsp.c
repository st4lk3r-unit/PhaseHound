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
