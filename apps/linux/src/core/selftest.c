/* Built-in test suite (no framework) — mirrors the macOS and Windows
 * self-tests: JSON, tap gestures, byte-identical storage rendering, tolerant
 * config, cleanup/review prompts vs shared/prompts, WAV, webhook payload. */
#include "core/json.h"
#include "core/tap.h"
#include "core/wav.h"
#include "core/library.h"
#include "core/config.h"
#include "core/cleanup.h"
#include "core/webhook.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <linux/input-event-codes.h>

static int count, failed;
static void expect(bool ok, const char *label) {
    count++;
    if (!ok) { failed++; printf("  x %s\n", label); }
}
static bool streq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

static void test_json(void) {
    VvJson *o = vv_json_object();
    vv_json_object_set(o, "s", vv_json_string("he\"llo\nworld"));
    vv_json_object_set(o, "n", vv_json_number(12.5));
    vv_json_object_set(o, "i", vv_json_number(42));
    vv_json_object_set(o, "b", vv_json_bool(true));
    vv_json_object_set(o, "z", vv_json_null());
    VvJson *arr = vv_json_array(); vv_json_array_add(arr, vv_json_number(1)); vv_json_array_add(arr, vv_json_string("two")); vv_json_array_add(arr, vv_json_bool(false));
    vv_json_object_set(o, "arr", arr);
    VvJson *inner = vv_json_object(); vv_json_object_set(inner, "k", vv_json_string("v")); vv_json_object_set(o, "obj", inner);
    char *text = vv_json_write(o, 2);
    char *err = NULL;
    VvJson *round = vv_json_parse(text, &err);
    expect(round != NULL, "json: pretty output parses");
    expect(streq(vv_json_get_string(round, "s", ""), "he\"llo\nworld"), "json: string escaping round trip");
    expect(vv_json_get_number(round, "n", 0) == 12.5, "json: double round trip");
    expect(vv_json_get_number(round, "i", 0) == 42, "json: integer round trip");
    expect(vv_json_get_bool(round, "b", false), "json: bool round trip");
    expect(vv_json_object_get(round, "z") && vv_json_object_get(round, "z")->type == VV_JSON_NULL, "json: null round trip");
    expect(vv_json_array_length(vv_json_get_array(round, "arr")) == 3, "json: array round trip");
    expect(streq(vv_json_get_string(vv_json_get_object(round, "obj"), "k", ""), "v"), "json: nested object");
    vv_json_free(round); vv_json_free(o); g_free(text);
    VvJson *u = vv_json_parse("{\"u\":\"\\u0041\\t\"}", NULL);
    expect(u && streq(vv_json_get_string(u, "u", ""), "A\t"), "json: unicode escape");
    vv_json_free(u);
    VvJson *sci = vv_json_parse("-1.5e2", NULL);
    expect(sci && sci->number == -150.0, "json: scientific notation");
    vv_json_free(sci);
    VvJson *bad = vv_json_parse("{\"a\":}", &err);
    expect(bad == NULL && err != NULL, "json: malformed input errors");
    g_free(err);
}

static void test_tap(void) {
    VvTap t; VvTapAct a;
    vv_tap_init(&t, VV_TAP_DOUBLE);
    expect(vv_tap_key_down(&t, 0, &a) == 1 && a == VV_ACT_START_RECORDING, "hold: down starts recording");
    expect(vv_tap_key_up(&t, 1.0, &a) == 1 && a == VV_ACT_COMMIT, "hold: long release commits");
    expect(t.phase == VV_PHASE_IDLE, "hold: back to idle");

    vv_tap_init(&t, VV_TAP_DOUBLE);
    expect(vv_tap_key_down(&t, 0, &a) == 1 && a == VV_ACT_START_RECORDING, "double: first down records");
    expect(vv_tap_key_up(&t, 0.1, &a) == 0, "double: first short tap waits");
    expect(vv_tap_key_down(&t, 0.2, &a) == 0, "double: second tap continues");
    expect(vv_tap_key_up(&t, 0.3, &a) == 0, "double: latched after second tap");
    expect(t.phase == VV_PHASE_LATCHED, "double: latched phase");
    expect(vv_tap_key_down(&t, 5.0, &a) == 1 && a == VV_ACT_COMMIT, "double: stop tap commits");
    expect(vv_tap_key_up(&t, 5.1, &a) == 0, "double: drain up");
    expect(t.phase == VV_PHASE_IDLE, "double: idle after stop");

    vv_tap_init(&t, VV_TAP_DOUBLE);
    vv_tap_key_down(&t, 0, &a); vv_tap_key_up(&t, 0.1, &a);
    expect(vv_tap_pending_deadline(&t) >= 0, "stray: deadline pending");
    expect(vv_tap_expire(&t, 0.6, &a) == 1 && a == VV_ACT_DISCARD, "stray: expiry discards");

    vv_tap_init(&t, VV_TAP_SINGLE);
    expect(vv_tap_key_down(&t, 0, &a) == 1 && a == VV_ACT_START_RECORDING, "single: down records");
    expect(vv_tap_key_up(&t, 0.1, &a) == 0, "single: quick release latches");
    expect(t.phase == VV_PHASE_LATCHED, "single: latched");
    expect(vv_tap_key_down(&t, 2.0, &a) == 1 && a == VV_ACT_COMMIT, "single: next tap commits");

    vv_tap_init(&t, VV_TAP_DOUBLE);
    vv_tap_key_down(&t, 0, &a); vv_tap_key_up(&t, 0.1, &a); vv_tap_key_down(&t, 0.2, &a);
    expect(vv_tap_key_up(&t, 1.5, &a) == 1 && a == VV_ACT_COMMIT, "double+hold: commits on release");

    vv_tap_init(&t, VV_TAP_DOUBLE);
    vv_tap_key_down(&t, 0, &a);
    expect(vv_tap_cancel(&t, &a) == 1 && a == VV_ACT_DISCARD, "cancel discards");
}

static void test_markdown(void) {
    VvEntry *e = vv_entry_new();
    g_free(e->id); e->id = g_strdup("20260825-120000");
    GDateTime *dt = g_date_time_new_utc(2026, 8, 25, 12, 0, 0);
    e->date_unix = g_date_time_to_unix(dt); g_date_time_unref(dt);
    e->duration = 12.4;
    g_free(e->stt_label); e->stt_label = g_strdup("ElevenLabs/scribe_v2");
    g_free(e->cleanup_label); e->cleanup_label = g_strdup("Fireworks/gpt-oss-20b");
    g_free(e->cleaned); e->cleaned = g_strdup("Hello world.\n\n- bullet one\n- bullet two");
    g_free(e->raw); e->raw = g_strdup("um hello world uh bullet one bullet two");
    char *md = vv_library_render(e);
    const char *expected = "---\ndate: 2026-08-25T12:00:00Z\nduration: 12.4\naudio: 20260825-120000.wav\nstt: ElevenLabs/scribe_v2\ncleanup: Fireworks/gpt-oss-20b\nstatus: complete\n---\n\nHello world.\n\n- bullet one\n- bullet two\n\n## Raw transcript\n\num hello world uh bullet one bullet two\n";
    expect(streq(md, expected), "markdown: byte-identical to macOS/Windows rendering");
    VvEntry *p = vv_library_parse(md, e->id, e->folder);
    expect(streq(p->cleaned, e->cleaned), "markdown: cleaned round trip");
    expect(streq(p->raw, e->raw), "markdown: raw round trip");
    expect(fabs(p->duration - 12.4) < 0.01, "markdown: duration");
    expect(streq(p->stt_label, e->stt_label), "markdown: stt label");
    expect(p->date_unix == e->date_unix, "markdown: date");
    vv_entry_free(p); g_free(md); vv_entry_free(e);
    VvEntry *bare = vv_library_parse("---\ndate: 2026-08-25T12:00:00Z\nstatus: complete\n---\n\nJust text\n", "x", "Inbox");
    expect(streq(bare->cleaned, "Just text") && streq(bare->raw, "Just text"), "markdown: bare body");
    vv_entry_free(bare);
}

static void test_library_files(void) {
    char *root = g_build_filename(g_get_tmp_dir(), "vv-test-XXXXXX", NULL);
    root = g_mkdtemp(root);
    VvLibrary *lib = vv_library_new(root);
    GPtrArray *names = vv_library_folder_names(lib);
    expect(names->len == 1 && streq(g_ptr_array_index(names, 0), "Inbox"), "library: Inbox auto-created");
    g_ptr_array_unref(names);
    vv_library_create_folder(lib, "Work Notes");
    names = vv_library_folder_names(lib);
    expect(names->len == 2 && streq(g_ptr_array_index(names, 1), "Work Notes"), "library: folder created");
    g_ptr_array_unref(names);
    char *id = NULL, *audio = NULL;
    vv_library_new_slot(lib, "Inbox", &id, &audio);
    VvEntry *e = vv_entry_new();
    g_free(e->id); e->id = g_strdup(id);
    e->date_unix = g_get_real_time() / G_USEC_PER_SEC; e->duration = 1;
    g_free(e->cleaned); e->cleaned = g_strdup("hi"); g_free(e->raw); e->raw = g_strdup("hi");
    vv_library_save(lib, e);
    GPtrArray *ids = vv_library_entry_ids(lib, "Inbox");
    expect(ids->len == 1, "library: entry saved");
    g_ptr_array_unref(ids);
    VvEntry *got = vv_library_get_entry(lib, "Inbox", id);
    expect(got && streq(got->cleaned, "hi"), "library: entry listed");
    vv_entry_free(got);
    g_free(e->cleaned); e->cleaned = g_strdup("updated");
    vv_library_save(lib, e);
    got = vv_library_get_entry(lib, "Inbox", id);
    expect(got && streq(got->cleaned, "updated"), "library: entry updated");
    vv_entry_free(got);
    vv_library_delete(lib, e);
    ids = vv_library_entry_ids(lib, "Inbox");
    expect(ids->len == 0, "library: entry deleted");
    g_ptr_array_unref(ids);
    vv_entry_free(e); g_free(id); g_free(audio); vv_library_free(lib);
    char *rm = g_strdup_printf("rm -rf '%s'", root);
    if (system(rm)) {}
    g_free(rm); g_free(root);
}

static void test_config(void) {
    VvConfig *c = vv_config_new();
    g_ptr_array_add(c->providers, vv_provider_preset(VV_KIND_ELEVENLABS));
    g_ptr_array_add(c->providers, vv_provider_preset(VV_KIND_VERCEL_GATEWAY));
    c->stt_provider_id = g_strdup(((VvProvider *)g_ptr_array_index(c->providers, 0))->id);
    c->cleanup.provider_id = g_strdup(((VvProvider *)g_ptr_array_index(c->providers, 1))->id);
    g_free(c->cleanup.custom_prompt); c->cleanup.custom_prompt = g_strdup("My prompt");
    VvWebhook *h = g_new0(VvWebhook, 1); h->url = g_strdup("https://x.test/h"); h->include_audio = true; h->enabled = true;
    g_hash_table_insert(c->folder_webhooks, g_strdup("Inbox"), h);
    VvJson *j = vv_config_to_json(c);
    char *text = vv_json_write(j, 2);
    VvJson *back = vv_json_parse(text, NULL);
    VvConfig *d = vv_config_from_json(back);
    expect(d->providers->len == 2, "config: providers round trip");
    expect(streq(d->stt_provider_id, c->stt_provider_id), "config: stt provider id");
    expect(streq(d->cleanup.provider_id, c->cleanup.provider_id), "config: cleanup provider id");
    expect(streq(d->cleanup.custom_prompt, "My prompt"), "config: custom prompt");
    VvWebhook *dh = g_hash_table_lookup(d->folder_webhooks, "Inbox");
    expect(dh && dh->include_audio, "config: webhook round trip");
    expect(((VvProvider *)g_ptr_array_index(d->providers, 0))->kind == VV_KIND_ELEVENLABS, "config: kind wire names");
    vv_config_free(d); vv_json_free(back); g_free(text); vv_json_free(j); vv_config_free(c);

    VvJson *pj = vv_json_parse("{\"cleanup\":{\"mode\":\"light\"},\"unknown\":1}", NULL);
    VvConfig *partial = vv_config_from_json(pj);
    expect(partial->cleanup.mode == VV_CLEANUP_LIGHT, "config: tolerant partial decode");
    expect(partial->play_sounds && partial->chunked_transcription, "config: defaults kept");
    vv_config_free(partial); vv_json_free(pj);
    expect(!vv_kind_supports_transcription(VV_KIND_FIREWORKS), "config: fireworks is chat-only");
    expect(!vv_kind_supports_transcription(VV_KIND_CEREBRAS), "config: cerebras is chat-only");
    expect(vv_kind_supports_transcription(VV_KIND_VERCEL_GATEWAY) && vv_kind_supports_chat(VV_KIND_VERCEL_GATEWAY), "config: gateway does both");
    expect(vv_kind_supports_vocabulary(VV_KIND_ELEVENLABS) && vv_kind_supports_vocabulary(VV_KIND_OPENAI_COMPATIBLE)
           && !vv_kind_supports_vocabulary(VV_KIND_VERCEL_GATEWAY), "providers: vocabulary support matches the STT calls that send it");
}

static void test_profiles(void) {
    VvJson *lj = vv_json_parse("{\"hotkey\":{\"keyCode\":123,\"modifiers\":3,\"isModifierOnly\":false}}", NULL);
    VvConfig *legacy = vv_config_from_json(lj);
    expect(legacy->profiles->len == 1, "profiles: legacy migrates to one profile");
    const VvHotkey *hk = vv_config_primary_hotkey(legacy);
    expect(hk->key_code == 123 && hk->modifiers == 3 && !hk->is_modifier_only, "profiles: legacy hotkey preserved");
    vv_config_free(legacy); vv_json_free(lj);

    VvConfig *c = vv_config_new();
    g_ptr_array_add(c->providers, vv_provider_preset(VV_KIND_CEREBRAS));
    VvProvider *cerebras = g_ptr_array_index(c->providers, 0);
    VvProfile *raw = vv_profile_new();
    g_free(raw->name); raw->name = g_strdup("Raw");
    raw->hotkey.key_code = KEY_F13; raw->hotkey.is_modifier_only = false;
    raw->has_cleanup_mode = true; raw->cleanup_mode = VV_CLEANUP_OFF;
    raw->cleanup_provider_id = g_strdup(cerebras->id);
    g_free(raw->custom_prompt); raw->custom_prompt = g_strdup("Terse.");
    g_ptr_array_add(c->profiles, raw);
    VvJson *j = vv_config_to_json(c);
    char *text = vv_json_write(j, 2);
    VvJson *back = vv_json_parse(text, NULL);
    VvConfig *d = vv_config_from_json(back);
    expect(d->profiles->len == 2, "profiles: round trip count");
    VvProfile *r = g_ptr_array_index(d->profiles, 1);
    expect(streq(r->id, raw->id), "profiles: id round trip");
    expect(streq(r->name, "Raw") && r->has_cleanup_mode && r->cleanup_mode == VV_CLEANUP_OFF, "profiles: fields round trip");
    expect(streq(r->cleanup_provider_id, cerebras->id), "profiles: provider id round trip");
    expect(streq(r->custom_prompt, "Terse.") && r->hotkey.key_code == KEY_F13, "profiles: prompt + hotkey round trip");
    VvJson *rj = vv_json_array_get(vv_json_get_array(back, "dictationProfiles"), 1);
    expect(streq(vv_json_get_string(rj, "cleanupMode", ""), "off") && !vv_json_get_bool(rj, "cleanupEnabled", true),
           "profiles: off mode round-trips and mirrors legacy cleanupEnabled");

    VvEffective eff = vv_effective(r, d);
    expect(!eff.enabled, "profiles: off mode disables cleanup");
    vv_effective_clear(&eff);
    VvProfile *with = vv_profile_new(); with->cleanup_provider_id = g_strdup(cerebras->id);
    eff = vv_effective(with, d);
    expect(eff.enabled && eff.provider && streq(eff.provider->id, cerebras->id), "profiles: profile provider overrides global");
    vv_effective_clear(&eff);
    VvProfile *prompt = vv_profile_new(); g_free(prompt->custom_prompt); prompt->custom_prompt = g_strdup("Override.");
    eff = vv_effective(prompt, d);
    expect(streq(eff.config.custom_prompt, "Override."), "profiles: profile prompt overrides global");
    vv_effective_clear(&eff);
    VvProfile *plain = vv_profile_new();
    eff = vv_effective(plain, d);
    expect(eff.enabled && streq(eff.config.custom_prompt, d->cleanup.custom_prompt), "profiles: default profile inherits globals");
    vv_effective_clear(&eff);
    d->cleanup.mode = VV_CLEANUP_OFF;
    eff = vv_effective(plain, d);
    expect(!eff.enabled, "profiles: legacy profile inherits global mode");
    vv_effective_clear(&eff);
    VvProfile *light = vv_profile_new(); light->has_cleanup_mode = true; light->cleanup_mode = VV_CLEANUP_LIGHT;
    eff = vv_effective(light, d);
    expect(eff.config.mode == VV_CLEANUP_LIGHT, "profiles: explicit mode wins over global off");
    vv_effective_clear(&eff);
    d->cleanup.mode = VV_CLEANUP_RICH;
    VvProfile *disabled = vv_profile_new(); disabled->cleanup_enabled = false;
    eff = vv_effective(disabled, d);
    expect(!eff.enabled, "profiles: legacy cleanupEnabled=false forces raw");
    vv_effective_clear(&eff);

    /* transcriber + vocabulary */
    g_ptr_array_add(d->providers, vv_provider_preset(VV_KIND_ELEVENLABS));
    g_ptr_array_add(d->providers, vv_provider_preset(VV_KIND_VERCEL_GATEWAY));
    VvProvider *sttA = g_ptr_array_index(d->providers, 1), *sttB = g_ptr_array_index(d->providers, 2);
    g_free(d->stt_provider_id); d->stt_provider_id = g_strdup(sttA->id);
    g_free(d->cleanup.vocabulary); d->cleanup.vocabulary = g_strdup("Luna");
    VvProfile *code = vv_profile_new(); code->has_cleanup_mode = true; code->cleanup_mode = VV_CLEANUP_LIGHT;
    code->stt_provider_id = g_strdup(sttB->id); g_free(code->vocabulary); code->vocabulary = g_strdup("OrbStack, SwiftPM");
    eff = vv_effective(code, d);
    expect(eff.stt && streq(eff.stt->id, sttB->id), "profiles: transcriber override resolves");
    expect(streq(eff.config.vocabulary, "Luna, OrbStack, SwiftPM"), "profiles: vocabulary merges global + hotkey");
    vv_effective_clear(&eff);
    eff = vv_effective(plain, d);
    expect(eff.stt && streq(eff.stt->id, sttA->id), "profiles: default transcriber inherited");
    vv_effective_clear(&eff);
    char *m1 = vv_merge_vocabulary("", " a, b "), *m2 = vv_merge_vocabulary("x", "");
    expect(streq(m1, "a, b") && streq(m2, "x"), "profiles: vocabulary merge edge cases");
    g_free(m1); g_free(m2);
    VvJson *cj = vv_profile_to_json(code);
    VvProfile *cr = vv_profile_from_json(cj);
    expect(streq(cr->stt_provider_id, sttB->id) && streq(cr->vocabulary, "OrbStack, SwiftPM"), "profiles: transcriber + vocabulary round trip");
    vv_profile_free(cr); vv_json_free(cj);

    /* review options */
    VvProfile *review = vv_profile_new();
    review->review_before_paste = true; review->screenshot_context = true; review->review_provider_id = vv_uuid_new();
    VvJson *rvj = vv_profile_to_json(review);
    VvProfile *rr = vv_profile_from_json(rvj);
    expect(rr->review_before_paste && rr->screenshot_context && streq(rr->review_provider_id, review->review_provider_id),
           "review: profile options round trip");
    VvJson *empty = vv_json_object();
    VvProfile *pp = vv_profile_from_json(empty);
    expect(!pp->review_before_paste && !pp->screenshot_context, "review: options default off");
    vv_profile_free(pp); vv_json_free(empty); vv_profile_free(rr); vv_json_free(rvj); vv_profile_free(review);

    vv_profile_free(with); vv_profile_free(prompt); vv_profile_free(plain); vv_profile_free(light);
    vv_profile_free(disabled); vv_profile_free(code);
    vv_config_free(d); vv_json_free(back); g_free(text); vv_json_free(j); vv_config_free(c);
}

static char *read_prompt(const char *dir, const char *name) {
    char *path = g_build_filename(dir, name, NULL);
    char *text = NULL;
    g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    if (text) g_strstrip(text);
    return text;
}

static void test_cleanup(void) {
    GPtrArray *terms = vv_parse_vocabulary("Luna, VoiceVector\n OrbStack ,,\n");
    expect(terms->len == 3 && streq(g_ptr_array_index(terms, 0), "Luna") && streq(g_ptr_array_index(terms, 2), "OrbStack"), "cleanup: vocabulary parsing");
    g_ptr_array_unref(terms);
    VvCleanupConfig cfg = { VV_CLEANUP_RICH, NULL, g_strdup(""), g_strdup("") };
    char *sys = vv_cleanup_system_prompt(&cfg);
    expect(streq(sys, vv_cleanup_default_prompt(VV_CLEANUP_RICH)), "cleanup: default prompt used when no custom");
    g_free(sys);
    g_free(cfg.custom_prompt); cfg.custom_prompt = g_strdup("My own prompt.");
    g_free(cfg.vocabulary); cfg.vocabulary = g_strdup("Luna");
    sys = vv_cleanup_system_prompt(&cfg);
    expect(g_str_has_prefix(sys, "My own prompt.") && strstr(sys, "Luna"), "cleanup: custom prompt + vocab");
    g_free(sys); g_free(cfg.custom_prompt); g_free(cfg.vocabulary);
    char *pp = vv_post_process("```md\nHi\n```", "raw");
    expect(streq(pp, "Hi"), "cleanup: fence stripping"); g_free(pp);
    pp = vv_post_process("<transcript>\nHi\n</transcript>", "raw");
    expect(streq(pp, "Hi"), "cleanup: delimiter stripping"); g_free(pp);
    pp = vv_post_process("  ", "raw");
    expect(streq(pp, "raw"), "cleanup: empty reply keeps raw"); g_free(pp);
    char *wrapped = vv_wrap_transcript("x");
    expect(streq(wrapped, "<transcript>\nx\n</transcript>"), "cleanup: transcript wrapping"); g_free(wrapped);
    char *rm = vv_review_message("Hi", "shorter");
    expect(streq(rm, "<draft>\nHi\n</draft>\n<instruction>\nshorter\n</instruction>"), "review: message wraps draft and instruction"); g_free(rm);
    char *cap = vv_screenshot_caption(1, 2, true, true, true);
    expect(streq(cap, "Display 1 of 2 — ACTIVE: the dictated text will be inserted here; the target window is outlined in red."), "screenshot: active caption"); g_free(cap);
    cap = vv_screenshot_caption(2, 2, true, false, false);
    expect(streq(cap, "Display 2 of 2."), "screenshot: plain caption"); g_free(cap);
    pp = vv_post_process("<draft>\nHello\n</draft>", "x");
    expect(streq(pp, "Hello"), "review: echoed draft delimiters stripped"); g_free(pp);

    const char *candidates[] = { "shared/prompts", "../shared/prompts", "../../shared/prompts", "../../../shared/prompts" };
    const char *dir = NULL;
    for (int i = 0; i < 4; i++) {
        char *probe = g_build_filename(candidates[i], "review.txt", NULL);
        bool ok = g_file_test(probe, G_FILE_TEST_EXISTS);
        g_free(probe);
        if (ok) { dir = candidates[i]; break; }
    }
    if (dir) {
        char *rich = read_prompt(dir, "cleanup-rich.txt"), *light = read_prompt(dir, "cleanup-light.txt"), *rev = read_prompt(dir, "review.txt");
        expect(streq(rich, vv_cleanup_default_prompt(VV_CLEANUP_RICH)), "cleanup: rich prompt matches shared/prompts");
        expect(streq(light, vv_cleanup_default_prompt(VV_CLEANUP_LIGHT)), "cleanup: light prompt matches shared/prompts");
        expect(streq(rev, vv_review_prompt()), "review: prompt matches shared/prompts");
        g_free(rich); g_free(light); g_free(rev);
    }

    VvProvider *sp = vv_provider_preset(VV_KIND_VERCEL_GATEWAY);
    g_free(sp->stt_model); sp->stt_model = g_strdup("google/gemini-2.5-flash");
    g_free(sp->chat_model); sp->chat_model = g_strdup("google/gemini-2.5-flash");
    expect(vv_single_pass_eligible(sp, sp, VV_CLEANUP_RICH), "single-pass: same provider+model");
    expect(!vv_single_pass_eligible(sp, sp, VV_CLEANUP_OFF), "single-pass: off mode blocks");
    VvProvider *other = vv_provider_preset(VV_KIND_VERCEL_GATEWAY);
    expect(!vv_single_pass_eligible(sp, other, VV_CLEANUP_RICH), "single-pass: different provider blocks");
    g_free(sp->stt_model); sp->stt_model = g_strdup("openai/whisper-1");
    expect(!vv_single_pass_eligible(sp, sp, VV_CLEANUP_RICH), "single-pass: different models block");
    vv_provider_free(sp); vv_provider_free(other);
}

static void test_wav(void) {
    char *path = g_build_filename(g_get_tmp_dir(), "vv-wav-test.wav", NULL);
    VvWavWriter w;
    expect(vv_wav_open(&w, path, VV_WAV_SAMPLE_RATE, 1), "wav: open");
    int16_t samples[16000];
    for (int i = 0; i < 16000; i++) samples[i] = (int16_t)(i % 100);
    vv_wav_append(&w, samples, 16000);
    vv_wav_append(&w, samples, 8000);
    double duration = vv_wav_finalize(&w);
    expect(fabs(duration - 1.5) < 0.001, "wav: duration from data bytes");
    char *data = NULL; gsize n = 0;
    g_file_get_contents(path, &data, &n, NULL);
    expect(n == 44 + 48000 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVEfmt ", 8) == 0, "wav: header + payload size");
    expect(data && (guint32)(guchar)data[40] + ((guint32)(guchar)data[41] << 8) + ((guint32)(guchar)data[42] << 16) == 48000, "wav: data chunk size patched");
    g_free(data);
    GBytes *slice = vv_wav_slice(path, 32000, 48000, VV_WAV_SAMPLE_RATE);
    expect(slice && g_bytes_get_size(slice) == 44 + 16000, "wav: slice has its own header");
    g_bytes_unref(slice);
    GBytes *silent = vv_wav_silent(1.0, VV_WAV_SAMPLE_RATE);
    expect(g_bytes_get_size(silent) == 44 + 32000, "wav: silent clip size");
    g_bytes_unref(silent);
    GBytes *chime = vv_wav_chime(true);
    expect(g_bytes_get_size(chime) > 1000, "wav: chime generated");
    g_bytes_unref(chime);
    g_unlink(path); g_free(path);
}

static void test_webhook(void) {
    VvEntry *e = vv_entry_new();
    g_free(e->id); e->id = g_strdup("20260825-120000");
    GDateTime *dt = g_date_time_new_utc(2026, 8, 25, 12, 0, 0);
    e->date_unix = g_date_time_to_unix(dt); g_date_time_unref(dt);
    e->duration = 3.2;
    g_free(e->cleaned); e->cleaned = g_strdup("Hi");
    g_free(e->raw); e->raw = g_strdup("hi");
    char *payload = vv_webhook_payload(e, "voicevector-linux");
    VvJson *j = vv_json_parse(payload, NULL);
    expect(j && streq(vv_json_get_string(j, "app", ""), "voicevector-linux"), "webhook: app field");
    expect(j && streq(vv_json_get_string(j, "date", ""), "2026-08-25T12:00:00Z"), "webhook: date format");
    expect(j && streq(vv_json_get_string(j, "cleaned", ""), "Hi") && streq(vv_json_get_string(j, "raw", ""), "hi"), "webhook: text fields");
    expect(j && vv_json_get_number(j, "duration", 0) == 3.2, "webhook: duration");
    vv_json_free(j); g_free(payload); vv_entry_free(e);
}

int vv_self_test(void) {
    test_json();
    test_tap();
    test_markdown();
    test_library_files();
    test_config();
    test_profiles();
    test_cleanup();
    test_wav();
    test_webhook();
    if (failed == 0) { printf("SELF-TEST PASSED (%d assertions)\n", count); return 0; }
    printf("SELF-TEST FAILED — %d/%d assertions failed\n", failed, count);
    return 1;
}
