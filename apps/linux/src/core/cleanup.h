/* Cleanup + review prompt assembly and policy resolution. Prompt text comes
 * from shared/prompts/ at build time (build/prompts.h). */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include "core/config.h"

const char *vv_cleanup_default_prompt(VvCleanupMode mode);
const char *vv_review_prompt(void);
const char *vv_router_prompt(void);
const char *vv_screenshot_note(void);
const char *vv_routing_prefix_note(void);

/* One display's capture plus the caption sent as a text part before it. */
typedef struct { GBytes *jpeg; char *caption; } VvScreenshot;
VvScreenshot *vv_screenshot_new(GBytes *jpeg, char *caption_take);
void vv_screenshot_free(VvScreenshot *s);
/* "Display i of n — ACTIVE: …" ; active_known=false ⇒ says the active display is unknown. */
char *vv_screenshot_caption(int index, int total, bool active_known, bool active, bool outlined);

/* The screenshots taken for one dictation: one JPEG per display, the active
 * display (where the text is inserted) first when known. Persisted beside the
 * entry as <id>-screen-N.jpg (docs/storage-format.md). Reference counted. */
typedef struct {
    GPtrArray *images;     /* GBytes* */
    int active_index;      /* 0-based, -1 when not knowable */
    bool outlined;         /* target window outlined in red in the active image */
    int refs;
} VvScreenshotSet;
VvScreenshotSet *vv_screenshot_set_new(void);
VvScreenshotSet *vv_screenshot_set_ref(VvScreenshotSet *s);
void vv_screenshot_set_unref(VvScreenshotSet *s);
/* Captioned attachments for the chat call: GPtrArray of VvScreenshot (free func set). */
GPtrArray *vv_screenshot_set_attachments(const VvScreenshotSet *s);

/* AI routing (docs/multi-machine.md). */
/* The router's user message: the draft, the spoken request, and a single
 * numbered destination list (options[0] = current focus, then 1…N). */
char *vv_router_message(const char *draft, const char *spoken, GPtrArray *options /* char* */);
/* Extracts {"target": N} from a router reply; false when unparseable (sets *out). */
bool vv_router_parse_target(const char *reply, int *target_out);
bool vv_router_target_valid(int target, int count);
/* Corrective steer when the last answer was unparseable or out of range. */
char *vv_router_correction(const char *reply, int count);

GPtrArray *vv_parse_vocabulary(const char *raw);                 /* char* */
char *vv_merge_vocabulary(const char *global, const char *extra);
/* Custom prompt if set, else the built-in for the mode (no vocabulary). */
char *vv_cleanup_system_prompt_base(const VvCleanupConfig *cfg);
/* …plus the vocabulary line. */
char *vv_cleanup_system_prompt(const VvCleanupConfig *cfg);
char *vv_review_system_prompt(const char *vocabulary);
char *vv_wrap_transcript(const char *raw);
char *vv_review_message(const char *draft, const char *instruction);
/* Strip code fences / echoed delimiters / whitespace; empty ⇒ fallback. */
char *vv_post_process(const char *reply, const char *fallback);

typedef struct {
    bool enabled;
    VvProvider *provider;     /* borrowed */
    VvProvider *stt;          /* borrowed */
    VvCleanupConfig config;   /* owned strings; free with vv_effective_clear */
} VvEffective;

VvCleanupMode vv_effective_mode(const VvProfile *profile, const VvConfig *config);
VvEffective vv_effective(const VvProfile *profile, const VvConfig *config);
void vv_effective_clear(VvEffective *e);

/* Single-pass activates when both stages point at the same provider AND model. */
bool vv_single_pass_eligible(const VvProvider *stt, const VvProvider *cleanup, VvCleanupMode mode);
