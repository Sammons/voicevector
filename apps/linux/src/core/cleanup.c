#include "core/cleanup.h"
#include "prompts.h"
#include <string.h>

static const char *strip_trailing_newline(const char *s, char *buf, size_t n) {
    g_strlcpy(buf, s, n);
    size_t len = strlen(buf);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    return buf;
}

const char *vv_cleanup_default_prompt(VvCleanupMode mode) {
    static char rich[4096], light[4096];
    if (!rich[0]) { strip_trailing_newline(VV_PROMPT_CLEANUP_RICH, rich, sizeof rich); strip_trailing_newline(VV_PROMPT_CLEANUP_LIGHT, light, sizeof light); }
    return mode == VV_CLEANUP_RICH ? rich : light;
}

const char *vv_review_prompt(void) {
    static char review[4096];
    if (!review[0]) strip_trailing_newline(VV_PROMPT_REVIEW, review, sizeof review);
    return review;
}

const char *vv_screenshot_note(void) {
    return "Screenshots of the user's displays are attached for context (names, terms, tone), each preceded by a caption saying which display is active — the one the text will be inserted into; never copy content from them.";
}

VvScreenshot *vv_screenshot_new(GBytes *jpeg, char *caption_take) {
    VvScreenshot *s = g_new0(VvScreenshot, 1);
    s->jpeg = g_bytes_ref(jpeg); s->caption = caption_take;
    return s;
}

void vv_screenshot_free(VvScreenshot *s) {
    if (!s) return;
    g_bytes_unref(s->jpeg); g_free(s->caption); g_free(s);
}

VvScreenshotSet *vv_screenshot_set_new(void) {
    VvScreenshotSet *s = g_new0(VvScreenshotSet, 1);
    s->images = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
    s->active_index = -1; s->refs = 1;
    return s;
}

VvScreenshotSet *vv_screenshot_set_ref(VvScreenshotSet *s) { if (s) s->refs++; return s; }

void vv_screenshot_set_unref(VvScreenshotSet *s) {
    if (!s || --s->refs > 0) return;
    g_ptr_array_unref(s->images); g_free(s);
}

GPtrArray *vv_screenshot_set_attachments(const VvScreenshotSet *s) {
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)vv_screenshot_free);
    for (guint i = 0; s && i < s->images->len; i++) {
        bool active = s->active_index == (int)i;
        g_ptr_array_add(out, vv_screenshot_new(g_ptr_array_index(s->images, i),
                        vv_screenshot_caption((int)i + 1, (int)s->images->len, s->active_index >= 0, active, active && s->outlined)));
    }
    return out;
}

char *vv_screenshot_caption(int index, int total, bool active_known, bool active, bool outlined) {
    GString *t = g_string_new(NULL);
    g_string_append_printf(t, "Display %d of %d", index, total);
    if (!active_known) g_string_append(t, " (which display is active is not known on this desktop)");
    else if (active) {
        g_string_append(t, " — ACTIVE: the dictated text will be inserted here");
        if (outlined) g_string_append(t, "; the target window is outlined in red");
    }
    g_string_append_c(t, '.');
    return g_string_free(t, FALSE);
}

GPtrArray *vv_parse_vocabulary(const char *raw) {
    GPtrArray *terms = g_ptr_array_new_with_free_func(g_free);
    if (!raw) return terms;
    char **parts = g_strsplit_set(raw, ",\n\r", -1);
    for (char **p = parts; *p; p++) {
        char *t = g_strstrip(g_strdup(*p));
        if (*t) g_ptr_array_add(terms, t); else g_free(t);
    }
    g_strfreev(parts);
    return terms;
}

char *vv_merge_vocabulary(const char *global, const char *extra) {
    char *g = g_strstrip(g_strdup(global ? global : ""));
    char *e = g_strstrip(g_strdup(extra ? extra : ""));
    char *out;
    if (!*e) out = g_strdup(g);
    else if (!*g) out = g_strdup(e);
    else out = g_strconcat(g, ", ", e, NULL);
    g_free(g); g_free(e);
    return out;
}

static char *join_terms(GPtrArray *terms) {
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < terms->len; i++) { if (i) g_string_append(s, ", "); g_string_append(s, g_ptr_array_index(terms, i)); }
    return g_string_free(s, FALSE);
}

char *vv_cleanup_system_prompt_base(const VvCleanupConfig *cfg) {
    char *custom = g_strstrip(g_strdup(cfg->custom_prompt ? cfg->custom_prompt : ""));
    if (*custom) return custom;
    g_free(custom);
    return g_strdup(vv_cleanup_default_prompt(cfg->mode));
}

char *vv_cleanup_system_prompt(const VvCleanupConfig *cfg) {
    char *prompt = vv_cleanup_system_prompt_base(cfg);
    GPtrArray *terms = vv_parse_vocabulary(cfg->vocabulary);
    if (terms->len) {
        char *joined = join_terms(terms);
        char *with = g_strconcat(prompt, "\nVocabulary the speaker uses (prefer these exact spellings when the audio is ambiguous): ", joined, ".", NULL);
        g_free(prompt); g_free(joined);
        prompt = with;
    }
    g_ptr_array_unref(terms);
    return prompt;
}

char *vv_review_system_prompt(const char *vocabulary) {
    char *prompt = g_strdup(vv_review_prompt());
    GPtrArray *terms = vv_parse_vocabulary(vocabulary);
    if (terms->len) {
        char *joined = join_terms(terms);
        char *with = g_strconcat(prompt, "\nVocabulary the speaker uses (prefer these exact spellings): ", joined, ".", NULL);
        g_free(prompt); g_free(joined);
        prompt = with;
    }
    g_ptr_array_unref(terms);
    return prompt;
}

char *vv_wrap_transcript(const char *raw) { return g_strconcat("<transcript>\n", raw, "\n</transcript>", NULL); }
char *vv_review_message(const char *draft, const char *instruction) {
    return g_strconcat("<draft>\n", draft, "\n</draft>\n<instruction>\n", instruction, "\n</instruction>", NULL);
}

char *vv_post_process(const char *reply, const char *fallback) {
    char *cleaned = g_strstrip(g_strdup(reply ? reply : ""));
    const char *tags[] = { "transcript", "draft" };
    for (int i = 0; i < 2; i++) {
        char *open = g_strdup_printf("<%s>", tags[i]);
        char *close = g_strdup_printf("</%s>", tags[i]);
        if (g_str_has_prefix(cleaned, open)) { char *n = g_strdup(cleaned + strlen(open)); g_free(cleaned); cleaned = n; }
        if (g_str_has_suffix(cleaned, close)) cleaned[strlen(cleaned) - strlen(close)] = '\0';
        g_free(open); g_free(close);
    }
    g_strstrip(cleaned);
    if (g_str_has_prefix(cleaned, "```") && g_str_has_suffix(cleaned, "```") && strlen(cleaned) >= 6) {
        const char *nl = strchr(cleaned, '\n');
        if (nl) {
            char *inner = g_strndup(nl + 1, strlen(nl + 1) - 3);
            g_free(cleaned);
            cleaned = g_strstrip(inner);
        }
    }
    if (!*cleaned) { g_free(cleaned); return g_strdup(fallback); }
    return cleaned;
}

VvCleanupMode vv_effective_mode(const VvProfile *profile, const VvConfig *config) {
    if (!profile) return config->cleanup.mode;
    if (profile->has_cleanup_mode) return profile->cleanup_mode;
    return profile->cleanup_enabled ? config->cleanup.mode : VV_CLEANUP_OFF;
}

VvEffective vv_effective(const VvProfile *profile, const VvConfig *config) {
    VvEffective e = {0};
    e.config.mode = vv_effective_mode(profile, config);
    e.config.provider_id = g_strdup(config->cleanup.provider_id);
    e.config.custom_prompt = g_strdup(config->cleanup.custom_prompt);
    e.config.vocabulary = g_strdup(config->cleanup.vocabulary);
    const char *provider_id = config->cleanup.provider_id;
    const char *stt_id = config->stt_provider_id;
    if (profile) {
        if (profile->cleanup_provider_id) provider_id = profile->cleanup_provider_id;
        if (profile->stt_provider_id) stt_id = profile->stt_provider_id;
        char *custom = g_strstrip(g_strdup(profile->custom_prompt));
        if (*custom) { g_free(e.config.custom_prompt); e.config.custom_prompt = g_strdup(profile->custom_prompt); }
        g_free(custom);
        g_free(e.config.vocabulary);
        e.config.vocabulary = vv_merge_vocabulary(config->cleanup.vocabulary, profile->vocabulary);
    }
    e.provider = vv_config_find_provider(config, provider_id);
    e.stt = vv_config_find_provider(config, stt_id);
    e.enabled = e.config.mode != VV_CLEANUP_OFF;
    return e;
}

void vv_effective_clear(VvEffective *e) {
    g_free(e->config.provider_id); g_free(e->config.custom_prompt); g_free(e->config.vocabulary);
    memset(e, 0, sizeof *e);
}

bool vv_single_pass_eligible(const VvProvider *stt, const VvProvider *cleanup, VvCleanupMode mode) {
    return mode != VV_CLEANUP_OFF && stt && cleanup
        && g_ascii_strcasecmp(stt->id, cleanup->id) == 0
        && vv_kind_supports_chat(stt->kind)
        && *stt->chat_model
        && strcmp(stt->stt_model, stt->chat_model) == 0;
}
