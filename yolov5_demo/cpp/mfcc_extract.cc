// MFCC feature extraction — matches librosa.feature.mfcc(y, sr=16000, n_mfcc=40, n_fft=480, hop_length=160)

#include "mfcc_extract.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// ── constants ──────────────────────────────────────
#define PI 3.14159265358979323846f
#define N_FFT_BINS (MFCC_FFT_PAD / 2 + 1)  // 257

// ── pre-computed tables (lazily initialized) ──────
static float hamming_window[MFCC_N_FFT];
static float mel_filterbank[MFCC_N_MELS][N_FFT_BINS];
static float dct_matrix[MFCC_N_MFCC][MFCC_N_MELS];
static int   tables_init = 0;

// ── radix-2 complex FFT (in-place, decimation-in-time) ───
static void fft_c2c(float* real, float* imag, int n) {
    // bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = real[i], ti = imag[i];
            real[i] = real[j]; imag[i] = imag[j];
            real[j] = tr;      imag[j] = ti;
        }
    }
    // butterfly
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * PI / (float)len;
        float w_r = cosf(ang), w_i = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cur_r = 1.0f, cur_i = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                int a = i + j, b = i + j + len / 2;
                float u_r = real[a], u_i = imag[a];
                float v_r = real[b] * cur_r - imag[b] * cur_i;
                float v_i = real[b] * cur_i + imag[b] * cur_r;
                real[a] = u_r + v_r; imag[a] = u_i + v_i;
                real[b] = u_r - v_r; imag[b] = u_i - v_i;
                float t = cur_r * w_r - cur_i * w_i;
                cur_i = cur_r * w_i + cur_i * w_r;
                cur_r = t;
            }
        }
    }
}

// ── initialize tables ──────────────────────────────
static void init_tables() {
    if (tables_init) return;
    tables_init = 1;

    // Hamming window: 0.54 - 0.46 * cos(2*pi*n / (N-1))
    for (int i = 0; i < MFCC_N_FFT; i++)
        hamming_window[i] = 0.54f - 0.46f * cosf(2.0f * PI * i / (float)(MFCC_N_FFT - 1));

    // Mel filterbank: 40 triangular filters, 0–8000 Hz, 257 bins
    float mel_low  = 1125.0f * logf(1.0f + 0.0f / 700.0f);
    float mel_high = 1125.0f * logf(1.0f + 8000.0f / 700.0f);
    float mel_step = (mel_high - mel_low) / (float)(MFCC_N_MELS + 1);

    float mel_centers[MFCC_N_MELS + 2];
    for (int m = 0; m < MFCC_N_MELS + 2; m++) {
        float mel = mel_low + mel_step * m;
        mel_centers[m] = 700.0f * (expf(mel / 1125.0f) - 1.0f);  // mel → Hz
    }

    memset(mel_filterbank, 0, sizeof(mel_filterbank));
    for (int m = 1; m <= MFCC_N_MELS; m++) {
        float f_left  = mel_centers[m - 1];
        float f_center = mel_centers[m];
        float f_right = mel_centers[m + 1];

        for (int k = 0; k < N_FFT_BINS; k++) {
            float freq = (float)k * MFCC_SR / (float)MFCC_FFT_PAD;
            if (freq >= f_left && freq <= f_center)
                mel_filterbank[m - 1][k] = (freq - f_left) / (f_center - f_left);
            else if (freq > f_center && freq <= f_right)
                mel_filterbank[m - 1][k] = (f_right - freq) / (f_right - f_center);
        }
    }

    // DCT type-II: dct[i][j] = cos(pi * i * (j + 0.5) / n_mels)
    for (int i = 0; i < MFCC_N_MFCC; i++)
        for (int j = 0; j < MFCC_N_MELS; j++)
            dct_matrix[i][j] = cosf(PI * (float)i * ((float)j + 0.5f) / (float)MFCC_N_MELS);
}

// ── main extraction ────────────────────────────────
void mfcc_extract(const int16_t* samples, int num_samples, float* mfcc_out) {
    init_tables();

    // Need at least 1 full second
    int total = num_samples;
    if (total < MFCC_N_FFT + (MFCC_N_FRAMES - 1) * MFCC_HOP_LEN) return;

    // process each frame
    float fft_real[MFCC_FFT_PAD], fft_imag[MFCC_FFT_PAD];
    int frame_offset = 0;

    for (int t = 0; t < MFCC_N_FRAMES; t++, frame_offset += MFCC_HOP_LEN) {
        // window + zero-pad
        memset(fft_real, 0, sizeof(fft_real));
        memset(fft_imag, 0, sizeof(fft_imag));
        for (int i = 0; i < MFCC_N_FFT; i++) {
            fft_real[i] = (float)samples[frame_offset + i] * hamming_window[i];
        }

        // FFT
        fft_c2c(fft_real, fft_imag, MFCC_FFT_PAD);

        // power spectrum
        float spec[N_FFT_BINS];
        for (int k = 0; k < N_FFT_BINS; k++) {
            spec[k] = (fft_real[k] * fft_real[k] + fft_imag[k] * fft_imag[k])
                      / (float)(MFCC_FFT_PAD * MFCC_FFT_PAD);
            if (spec[k] < 1e-10f) spec[k] = 1e-10f;
        }

        // mel filterbank
        float mel_energies[MFCC_N_MELS];
        for (int m = 0; m < MFCC_N_MELS; m++) {
            mel_energies[m] = 0;
            for (int k = 0; k < N_FFT_BINS; k++)
                mel_energies[m] += mel_filterbank[m][k] * spec[k];
        }

        // log
        for (int m = 0; m < MFCC_N_MELS; m++)
            mel_energies[m] = logf(mel_energies[m]);

        // DCT
        for (int i = 0; i < MFCC_N_MFCC; i++) {
            float sum = 0;
            for (int j = 0; j < MFCC_N_MELS; j++)
                sum += dct_matrix[i][j] * mel_energies[j];
            mfcc_out[i * MFCC_N_FRAMES + t] = sum;  // row-major: mfcc[n_mfcc][n_frames]
        }
    }
}

// ── normalize: (x - mean) / std ────────────────────
void mfcc_normalize(float* mfcc, int total) {
    float mean = 0, var = 0;
    for (int i = 0; i < total; i++) mean += mfcc[i];
    mean /= (float)total;
    for (int i = 0; i < total; i++) {
        float d = mfcc[i] - mean;
        var += d * d;
    }
    var = sqrtf(var / (float)total) + 1e-6f;
    for (int i = 0; i < total; i++) mfcc[i] = (mfcc[i] - mean) / var;
}

// ── float → int8 ───────────────────────────────────
void mfcc_quantize_int8(const float* mfcc, int total, int8_t* out) {
    for (int i = 0; i < total; i++) {
        float v = mfcc[i] * 42.0f;  // [-3,3] → ~[-127,127]
        if (v > 127) v = 127;
        if (v < -128) v = -128;
        out[i] = (int8_t)v;
    }
}
