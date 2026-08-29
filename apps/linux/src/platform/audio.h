/* Microphone capture on PulseAudio/PipeWire (libpulse-simple resamples to
 * 16 kHz mono s16 for us) → WAV on disk. Publishes a quantized level for the
 * HUD, silence-gap segments for streamed transcription, and honours the warm
 * policy (device stays open between takes). */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct VvRecorder VvRecorder;
/* Called on the recorder thread with a standalone WAV of one segment. */
typedef void (*VvSegmentFn)(GBytes *wav, int index, gpointer user);

VvRecorder *vv_recorder_new(void);
void vv_recorder_free(VvRecorder *r);
bool vv_recorder_start(VvRecorder *r, const char *wav_path, bool chunking, VvSegmentFn on_segment, gpointer user, char **error);
/* Returns duration in seconds. */
double vv_recorder_stop(VvRecorder *r);
void vv_recorder_discard(VvRecorder *r);
bool vv_recorder_is_recording(const VvRecorder *r);
float vv_recorder_level(const VvRecorder *r);          /* 0…1, 3 steps */
double vv_recorder_elapsed(const VvRecorder *r);
uint32_t vv_recorder_tail_start_byte(const VvRecorder *r);
void vv_recorder_set_warm_policy(VvRecorder *r, bool after_recording, bool always);
void vv_recorder_apply_warm_policy(VvRecorder *r);

/* HUD level on the fixed, quantized scale shared with the other apps. */
float vv_display_level(float rms);
/* Chimes / test tones. */
void vv_audio_play_wav(GBytes *wav);
bool vv_audio_input_available(void);
