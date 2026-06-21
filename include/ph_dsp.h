#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ph_dsp_nco_f32 {
    float c, s;
    float rc, rs;
    uint32_t renorm;
} ph_dsp_nco_f32_t;

void ph_dsp_nco_f32_init(ph_dsp_nco_f32_t *nco, double fs_hz, double freq_hz, double phase_rad);
void ph_dsp_nco_f32_set_freq(ph_dsp_nco_f32_t *nco, double fs_hz, double freq_hz);
void ph_dsp_nco_f32_next(ph_dsp_nco_f32_t *nco, float *c, float *s);

/* In-place radix-2 DIT complex FFT.
   buf: interleaved CF32 [re,im,...], N elements, N must be a power of 2.
   inverse: 0 = forward DFT, non-zero = inverse DFT (not normalised). */
void ph_fft_cf32(float *buf, int N, int inverse);

#ifdef __cplusplus
}
#endif
