/* Streaming RIFF WAV (16-bit PCM) writer; header sizes patched on finalize.
 * Also in-memory WAVs (silent test clips, chimes) and byte-range slices for
 * streamed segment transcription. Same layout as the other two apps. */
#pragma once
#include <glib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define VV_WAV_SAMPLE_RATE 16000
#define VV_WAV_HEADER_BYTES 44

typedef struct {
    FILE *file;
    char *path;
    int sample_rate;
    int channels;
    uint32_t data_bytes;
} VvWavWriter;

bool vv_wav_open(VvWavWriter *w, const char *path, int sample_rate, int channels);
void vv_wav_append(VvWavWriter *w, const int16_t *samples, size_t count);
/* Patches sizes and closes; returns duration in seconds. */
double vv_wav_finalize(VvWavWriter *w);
void vv_wav_abandon(VvWavWriter *w);

void vv_wav_header(uint8_t out[VV_WAV_HEADER_BYTES], int sample_rate, int channels, uint32_t data_bytes);
/* Standalone WAV from a byte range of a (possibly growing) recording. */
GBytes *vv_wav_slice(const char *path, uint32_t from_byte, uint32_t to_byte, int sample_rate);
GBytes *vv_wav_silent(double seconds, int sample_rate);
/* Short two-tone chime (start: rising, stop: falling). */
GBytes *vv_wav_chime(bool rising);
