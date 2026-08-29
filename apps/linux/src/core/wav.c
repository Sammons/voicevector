#include "core/wav.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static void put32(uint8_t *p, uint32_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }

void vv_wav_header(uint8_t h[VV_WAV_HEADER_BYTES], int sample_rate, int channels, uint32_t data_bytes) {
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);
    put16(h + 20, 1);                                   /* PCM */
    put16(h + 22, (uint16_t)channels);
    put32(h + 24, (uint32_t)sample_rate);
    put32(h + 28, (uint32_t)(sample_rate * channels * 2));
    put16(h + 32, (uint16_t)(channels * 2));
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
}

bool vv_wav_open(VvWavWriter *w, const char *path, int sample_rate, int channels) {
    memset(w, 0, sizeof *w);
    w->file = fopen(path, "wb");
    if (!w->file) return false;
    w->path = g_strdup(path);
    w->sample_rate = sample_rate;
    w->channels = channels;
    uint8_t header[VV_WAV_HEADER_BYTES];
    vv_wav_header(header, sample_rate, channels, 0);
    fwrite(header, 1, sizeof header, w->file);
    return true;
}

void vv_wav_append(VvWavWriter *w, const int16_t *samples, size_t count) {
    if (!w->file || count == 0) return;
    /* Little-endian on every Linux target we build for. */
    fwrite(samples, 2, count, w->file);
    fflush(w->file);
    w->data_bytes += (uint32_t)(count * 2);
}

double vv_wav_finalize(VvWavWriter *w) {
    if (!w->file) return 0;
    uint8_t header[VV_WAV_HEADER_BYTES];
    vv_wav_header(header, w->sample_rate, w->channels, w->data_bytes);
    fseek(w->file, 0, SEEK_SET);
    fwrite(header, 1, sizeof header, w->file);
    fclose(w->file);
    w->file = NULL;
    double seconds = w->data_bytes / (double)(2 * w->channels * w->sample_rate);
    g_free(w->path); w->path = NULL;
    return seconds;
}

void vv_wav_abandon(VvWavWriter *w) {
    if (w->file) fclose(w->file);
    w->file = NULL;
    g_free(w->path); w->path = NULL;
}

GBytes *vv_wav_slice(const char *path, uint32_t from_byte, uint32_t to_byte, int sample_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t want = to_byte > from_byte ? to_byte - from_byte : 0;
    uint8_t *wav = g_malloc(VV_WAV_HEADER_BYTES + want);
    fseek(f, VV_WAV_HEADER_BYTES + from_byte, SEEK_SET);
    size_t got = want ? fread(wav + VV_WAV_HEADER_BYTES, 1, want, f) : 0;
    fclose(f);
    vv_wav_header(wav, sample_rate, 1, (uint32_t)got);
    return g_bytes_new_take(wav, VV_WAV_HEADER_BYTES + got);
}

GBytes *vv_wav_silent(double seconds, int sample_rate) {
    uint32_t data_bytes = (uint32_t)(sample_rate * seconds) * 2;
    uint8_t *wav = g_malloc0(VV_WAV_HEADER_BYTES + data_bytes);
    vv_wav_header(wav, sample_rate, 1, data_bytes);
    return g_bytes_new_take(wav, VV_WAV_HEADER_BYTES + data_bytes);
}

GBytes *vv_wav_chime(bool rising) {
    const int rate = 44100;
    const double dur = 0.18;
    uint32_t frames = (uint32_t)(rate * dur);
    uint8_t *wav = g_malloc(VV_WAV_HEADER_BYTES + frames * 2);
    vv_wav_header(wav, rate, 1, frames * 2);
    int16_t *s = (int16_t *)(wav + VV_WAV_HEADER_BYTES);
    double f0 = rising ? 660 : 880, f1 = rising ? 880 : 660;
    for (uint32_t i = 0; i < frames; i++) {
        double t = i / (double)rate;
        double f = i < frames / 2 ? f0 : f1;
        double env = fmin(1.0, t / 0.01) * fmin(1.0, (dur - t) / 0.03);
        s[i] = (int16_t)(sin(2 * M_PI * f * t) * 0.35 * env * 32767);
    }
    return g_bytes_new_take(wav, VV_WAV_HEADER_BYTES + frames * 2);
}
