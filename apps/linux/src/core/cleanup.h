/* Cleanup + review prompt assembly and policy resolution. Prompt text comes
 * from shared/prompts/ at build time (build/prompts.h). */
#pragma once
#include <glib.h>
#include <stdbool.h>
#include "core/config.h"

const char *vv_cleanup_default_prompt(VvCleanupMode mode);
const char *vv_review_prompt(void);
const char *vv_screenshot_note(void);

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
