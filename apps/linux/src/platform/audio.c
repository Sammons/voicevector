#include "platform/audio.h"
#include "core/wav.h"
#include "core/log.h"
#include <glib/gstdio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <math.h>
#include <string.h>

struct VvRecorder {
    GMutex lock;
    GThread *thread;
    pa_simple *pa;
    volatile bool stopping;        /* end the capture thread */
    VvWavWriter writer;
    bool writing;
    gint64 started_at_us;
    float level;
    uint32_t tail_start_byte;
    bool chunking;
    VvSegmentFn on_segment; gpointer segment_user;
    int segment_index;
    double last_voiced_at;
    bool voiced_in_segment;
    float noise_floor;
    bool warm_after, warm_always;
    guint idle_stop_source;
};

#define VOICE_RMS_CEILING 0.015f
#define VOICE_RMS_FLOOR 0.001f
#define SILENCE_CUT_AFTER 2.0
#define MIN_SEGMENT_SECONDS 5.0
#define WARM_IDLE_SECONDS 15

float vv_display_level(float rms) {
    double db = 20 * log10(fmax(rms, 1e-6));
    double normalized = fmin(1, fmax(0, (db + 60) / 45));
    return (float)(ceil(normalized * 3) / 3);
}

static double now_seconds(void) { return g_get_monotonic_time() / 1e6; }

static bool is_voiced(VvRecorder *r, float rms) {
    if (rms < r->noise_floor) r->noise_floor = rms; else r->noise_floor = fminf(rms, r->noise_floor * 1.02f);
    return rms > VOICE_RMS_CEILING || (rms > VOICE_RMS_FLOOR && rms > r->noise_floor * 3);
}

static void process(VvRecorder *r, const int16_t *samples, size_t count) {
    double sum = 0;
    for (size_t i = 0; i < count; i++) { double v = samples[i] / 32768.0; sum += v * v; }
    float rms = count ? (float)sqrt(sum / count) : 0;
    g_mutex_lock(&r->lock);
    bool voiced = is_voiced(r, rms);
    r->level = fmaxf(vv_display_level(rms), r->level * 0.7f);
    bool writing = r->writing;
    if (writing) vv_wav_append(&r->writer, samples, count);
    VvSegmentFn fn = NULL; GBytes *slice = NULL; int index = 0;
    if (writing && r->chunking) {
        double now = now_seconds();
        if (voiced) { r->last_voiced_at = now; r->voiced_in_segment = true; }
        else if (r->voiced_in_segment && now - r->last_voiced_at >= SILENCE_CUT_AFTER) {
            uint32_t current = r->writer.data_bytes;
            double seconds = (current - r->tail_start_byte) / (double)(2 * VV_WAV_SAMPLE_RATE);
            if (seconds >= MIN_SEGMENT_SECONDS) {
                slice = vv_wav_slice(r->writer.path, r->tail_start_byte, current, VV_WAV_SAMPLE_RATE);
                if (slice) { fn = r->on_segment; index = r->segment_index++; r->tail_start_byte = current; r->voiced_in_segment = false; }
            }
        }
    }
    g_mutex_unlock(&r->lock);
    if (slice) { if (fn) fn(slice, index, r->segment_user); g_bytes_unref(slice); }
}

static gpointer capture_thread(gpointer data) {
    VvRecorder *r = data;
    int16_t buf[1600];   /* 100 ms */
    while (!r->stopping) {
        int err = 0;
        if (pa_simple_read(r->pa, buf, sizeof buf, &err) < 0) {
            vv_log_error("Audio read failed: %s", pa_strerror(err));
            break;
        }
        process(r, buf, G_N_ELEMENTS(buf));
    }
    return NULL;
}

static bool open_device(VvRecorder *r, char **error) {
    if (r->pa) return true;
    pa_sample_spec spec = { .format = PA_SAMPLE_S16LE, .rate = VV_WAV_SAMPLE_RATE, .channels = 1 };
    pa_buffer_attr attr = { .maxlength = (uint32_t)-1, .tlength = (uint32_t)-1, .prebuf = (uint32_t)-1, .minreq = (uint32_t)-1, .fragsize = 3200 };
    int err = 0;
    r->pa = pa_simple_new(NULL, "VoiceVector", PA_STREAM_RECORD, NULL, "Dictation", &spec, NULL, &attr, &err);
    if (!r->pa) { *error = g_strdup_printf("Could not open the microphone: %s", pa_strerror(err)); return false; }
    r->stopping = false;
    r->thread = g_thread_new("vv-capture", capture_thread, r);
    return true;
}

static void close_device(VvRecorder *r) {
    if (!r->pa) return;
    r->stopping = true;
    if (r->thread) { g_thread_join(r->thread); r->thread = NULL; }
    pa_simple_free(r->pa);
    r->pa = NULL;
}

static gboolean idle_stop(gpointer data) {
    VvRecorder *r = data;
    r->idle_stop_source = 0;
    g_mutex_lock(&r->lock); bool writing = r->writing; g_mutex_unlock(&r->lock);
    if (!writing && !r->warm_always) close_device(r);
    return G_SOURCE_REMOVE;
}

VvRecorder *vv_recorder_new(void) {
    VvRecorder *r = g_new0(VvRecorder, 1);
    g_mutex_init(&r->lock);
    r->noise_floor = 0.001f;
    r->warm_after = true;
    return r;
}

void vv_recorder_free(VvRecorder *r) {
    if (!r) return;
    if (r->idle_stop_source) g_source_remove(r->idle_stop_source);
    vv_recorder_discard(r);
    close_device(r);
    g_mutex_clear(&r->lock);
    g_free(r);
}

bool vv_recorder_start(VvRecorder *r, const char *wav_path, bool chunking, VvSegmentFn on_segment, gpointer user, char **error) {
    g_mutex_lock(&r->lock);
    if (r->writing) { g_mutex_unlock(&r->lock); return true; }
    g_mutex_unlock(&r->lock);
    if (r->idle_stop_source) { g_source_remove(r->idle_stop_source); r->idle_stop_source = 0; }
    if (!open_device(r, error)) return false;
    g_mutex_lock(&r->lock);
    if (!vv_wav_open(&r->writer, wav_path, VV_WAV_SAMPLE_RATE, 1)) { g_mutex_unlock(&r->lock); *error = g_strdup("Could not create the recording file."); return false; }
    r->writing = true;
    r->started_at_us = g_get_monotonic_time();
    r->tail_start_byte = 0; r->segment_index = 0; r->voiced_in_segment = false; r->last_voiced_at = now_seconds();
    r->noise_floor = 0.001f;
    r->chunking = chunking; r->on_segment = on_segment; r->segment_user = user;
    g_mutex_unlock(&r->lock);
    return true;
}

double vv_recorder_stop(VvRecorder *r) {
    g_mutex_lock(&r->lock);
    if (!r->writing) { g_mutex_unlock(&r->lock); return 0; }
    r->writing = false;
    double duration = vv_wav_finalize(&r->writer);
    r->level = 0;
    g_mutex_unlock(&r->lock);
    if (r->warm_always) { /* device stays open */ }
    else if (r->warm_after) r->idle_stop_source = g_timeout_add_seconds(WARM_IDLE_SECONDS, idle_stop, r);
    else close_device(r);
    return duration;
}

void vv_recorder_discard(VvRecorder *r) {
    g_mutex_lock(&r->lock);
    char *path = r->writing ? g_strdup(r->writer.path) : NULL;
    g_mutex_unlock(&r->lock);
    if (!path) return;
    vv_recorder_stop(r);
    g_unlink(path);
    g_free(path);
}

bool vv_recorder_is_recording(const VvRecorder *r) { return r->writing; }
float vv_recorder_level(const VvRecorder *r) { return r->level; }
double vv_recorder_elapsed(const VvRecorder *r) { return r->writing ? (g_get_monotonic_time() - r->started_at_us) / 1e6 : 0; }
uint32_t vv_recorder_tail_start_byte(const VvRecorder *r) { return r->tail_start_byte; }

void vv_recorder_set_warm_policy(VvRecorder *r, bool after, bool always) { r->warm_after = after; r->warm_always = always; }

void vv_recorder_apply_warm_policy(VvRecorder *r) {
    if (r->writing) return;
    if (r->warm_always) { char *e = NULL; if (!open_device(r, &e)) { vv_log_error("Mic warm-up failed: %s", e); g_free(e); } }
    else if (!r->warm_after) { if (r->idle_stop_source) { g_source_remove(r->idle_stop_source); r->idle_stop_source = 0; } close_device(r); }
}

static gpointer play_thread(gpointer data) {
    GBytes *wav = data;
    gsize n; const guchar *bytes = g_bytes_get_data(wav, &n);
    if (n > 44) {
        pa_sample_spec spec = { .format = PA_SAMPLE_S16LE, .rate = 44100, .channels = 1 };
        int err = 0;
        pa_simple *out = pa_simple_new(NULL, "VoiceVector", PA_STREAM_PLAYBACK, NULL, "Chime", &spec, NULL, NULL, &err);
        if (out) { pa_simple_write(out, bytes + 44, n - 44, &err); pa_simple_drain(out, &err); pa_simple_free(out); }
    }
    g_bytes_unref(wav);
    return NULL;
}

void vv_audio_play_wav(GBytes *wav) {
    GThread *t = g_thread_new("vv-chime", play_thread, g_bytes_ref(wav));
    g_thread_unref(t);
}

bool vv_audio_input_available(void) {
    pa_sample_spec spec = { .format = PA_SAMPLE_S16LE, .rate = VV_WAV_SAMPLE_RATE, .channels = 1 };
    int err = 0;
    pa_simple *pa = pa_simple_new(NULL, "VoiceVector", PA_STREAM_RECORD, NULL, "Probe", &spec, NULL, NULL, &err);
    if (!pa) return false;
    pa_simple_free(pa);
    return true;
}
