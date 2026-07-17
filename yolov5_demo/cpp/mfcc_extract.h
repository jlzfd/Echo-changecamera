#ifndef _MFCC_EXTRACT_H_
#define _MFCC_EXTRACT_H_
#include <stdint.h>

// MFCC parameters matching librosa.feature.mfcc(y, sr=16000, n_mfcc=40, n_fft=480, hop_length=160)
#define MFCC_SR         16000
#define MFCC_N_FFT      480
#define MFCC_HOP_LEN    160
#define MFCC_N_MELS     40
#define MFCC_N_MFCC     40
#define MFCC_N_FRAMES   98    // (16000 - 480) / 160 + 1 = 98
#define MFCC_FFT_PAD    512   // next power of 2 >= 480

// extract MFCC from 16000 samples int16 → 40×98 float (row-major: mfcc[mel_bin][frame])
void mfcc_extract(const int16_t* samples, int num_samples, float* mfcc_out);

// normalize: (x - mean) / std → [-3, 3]
void mfcc_normalize(float* mfcc, int total);

// quantize float MFCC → int8 [-128, 127]
void mfcc_quantize_int8(const float* mfcc, int total, int8_t* out);

#endif
