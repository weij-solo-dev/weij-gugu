// Live vendor tests: run only with GUGU_LIVE=1 and the vendor key in the environment. They spend
// real money (fractions of a cent) and record latency, which is the point.
#include "minitest.h"
#include "provider/provider.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/wav.h"
#include "foundation/atomic_file.h"
#include "life/life.h"
#include "world/world.h"
#include "mind/mind.h"
#include "store/store.h"
#include "art/art.h"
#include <stdlib.h>
#include <time.h>
#include <stdio.h>

static char acc[2048]; static int64_t t_first_delta, t_send; static int done_calls; static provider_error_t err_seen; static int err_calls;
static void l_delta(const char *t, size_t n, void *u) { if (!acc[0]) t_first_delta = clock_us(); strncat(acc, t, n); }
static void l_done(const char *sr, const provider_usage_t *us, void *u) { done_calls++; }
static void l_err(const provider_error_t *e, void *u) { err_seen = *e; err_calls++; printf("  vendor error: kind %d http %d api %d: %s\n", e->kind, e->http_status, e->api_code, e->message); }

static int16_t *l_pcm; static size_t l_samples, l_cap; static int64_t t_first_audio, t_flushed; static int l_sent_starts;
static void l_audio(const int16_t *pcm, size_t n, void *u) { if (!l_samples) t_first_audio = clock_us(); if (l_samples + n <= l_cap) { memcpy(l_pcm + l_samples, pcm, n * 2); l_samples += n; } }
static void l_flushed(const provider_usage_t *us, void *u) { t_flushed = clock_us(); }
static void l_sstart(const char *t, void *u) { l_sent_starts++; }

static char asr_text[512]; static lang_t asr_lang; static int64_t t_asr_done;
static void l_final(const char *t, lang_t l, const provider_usage_t *u, void *x) { snprintf(asr_text, sizeof asr_text, "%s", t); asr_lang = l; t_asr_done = clock_us(); }

static bool live_ready(void)
{
    const char *live = getenv("GUGU_LIVE"), *key = getenv("GUGU_MINIMAX_API_KEY");
    if (!live || !key) { printf("  (skipped: set GUGU_LIVE=1 and GUGU_MINIMAX_API_KEY)\n"); return false; }
    provider_init(GUGU_REPO_ROOT "/content/providers");
    provider_set_credential("minimax", "api_key", key);
    return true;
}

TEST(test_live_minimax_chat_stream)
{
    if (!live_ready()) return;
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_CHAT);
    chat_events_t ev = { l_delta, l_done, l_err };
    chat_session_t *c; acc[0] = 0; done_calls = 0; err_calls = 0;
    int64_t t0 = clock_us();
    CHECK(v->chat_open(&ev, NULL, &c) == ESP_OK);
    int64_t t_open = clock_us();
    chat_block_t sys[1] = { { "你是咕咕，一只住在孩子窗台鸽子屋里的小信鸽。用孩子能懂的话，一句话回答，不超过 20 个字。每句话前面加一个方括号表情标签，词表：[neutral] [happy] [laughing] [surprised] [curious] [shy] [sad] [sleepy]。", true } };
    chat_message_t m = { "user", "咕咕你今天去哪里玩了呀？" };
    chat_request_t req = { .system = sys, .system_count = 1, .messages = &m, .message_count = 1, .effort = CHAT_EFFORT_LOW, .max_tokens = 80 };
    for (int round = 0; round < 2; round++) {
        acc[0] = 0; t_send = clock_us();
        v->chat_send(c, &req);
        printf("  round %d: first delta %lld ms after send, reply: %s\n", round + 1, (long long)((t_first_delta - t_send) / 1000), acc);
    }
    v->chat_close(c);
    CHECK(acc[0] != 0); CHECK_EQ_INT(done_calls, 2); CHECK_EQ_INT(err_calls, 0);
    printf("  open+prewarm %lld ms\n", (long long)((t_open - t0) / 1000));
}

TEST(test_live_minimax_tts_bidi_then_asr)
{
    if (!live_ready()) return;
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_TTS);
    l_cap = 16000 * 30; l_pcm = mem_alloc_big(l_cap * 2); l_samples = 0; l_sent_starts = 0; err_calls = 0;
    tts_params_t tp = { .lang = LANG_ZH, .sample_rate = 16000, .speed = 1.0f };
    tts_events_t te = { l_sstart, l_audio, NULL, l_flushed, l_err };
    tts_session_t *t;
    int64_t t0 = clock_us();
    esp_err_t e = v->tts_open(&tp, &te, NULL, &t);
    CHECK(e == ESP_OK);
    if (e != ESP_OK) return;
    int64_t t_open = clock_us();
    const char *seg = "你好呀，我是咕咕，今天我去了公园。";
    v->tts_text(t, seg, strlen(seg)); v->tts_flush(t);
    int64_t t_send1 = clock_us();
    for (int i = 0; i < 100 && t_flushed < t_send1; i++) clock_sleep_ms(100);
    size_t s1 = l_samples; int64_t first1 = t_first_audio;
    const char *seg2 = "你今天开心吗？";
    int64_t t_send2 = clock_us(); size_t before2 = l_samples; t_flushed = 0;
    v->tts_text(t, seg2, strlen(seg2)); v->tts_flush(t);
    for (int i = 0; i < 100 && t_flushed < t_send2; i++) clock_sleep_ms(100);
    v->tts_close(t);
    printf("  tts open %lld ms; seg1 first audio %lld ms, flushed %lld ms, %u samples; seg2 +%u samples; sentence_starts %d\n",
           (long long)((t_open - t0) / 1000), (long long)((first1 - t_send1) / 1000), (long long)((t_send2 - t_send1) / 1000),
           (unsigned)s1, (unsigned)(l_samples - before2), l_sent_starts);
    CHECK(l_samples > 16000 * 2); CHECK_EQ_INT(err_calls, 0);
    // keep the audio as a fixture for the replay harness and listen tests
    uint8_t hdr[WAV_HEADER_BYTES]; wav_write_header(hdr, 16000, 1, (uint32_t)(l_samples * 2));
    FILE *f = fopen(GUGU_REPO_ROOT "/test/fixtures/live_tts_zh.wav", "wb");
    if (f) { fwrite(hdr, 1, sizeof hdr, f); fwrite(l_pcm, 2, l_samples, f); fclose(f); }
    // round trip through ASR
    v = provider_for(PROVIDER_ROLE_ASR);
    asr_session_t *a; asr_params_t ap = { .sample_rate = 16000 }; asr_events_t ae = { NULL, l_final, l_err };
    CHECK(v->asr_open(&ap, &ae, NULL, &a) == ESP_OK);
    v->asr_feed(a, l_pcm, s1);
    int64_t t_rel = clock_us(); asr_text[0] = 0;
    v->asr_finish(a); v->asr_close(a);
    printf("  asr: %lld ms for %u ms audio -> \"%s\" (lang %s)\n", (long long)((t_asr_done - t_rel) / 1000), (unsigned)(s1 / 16), asr_text, lang_code(asr_lang));
    CHECK(strstr(asr_text, "你好") != NULL);
    mem_release(l_pcm);
}

// The planner against the real model: one trip card, printed in full so the owner can judge it, validated by the same code the device runs.
TEST(test_live_trip_card)
{
    if (!live_ready()) return;
    store_init(GUGU_REPO_ROOT "/host/build/test_data", GUGU_REPO_ROOT "/content");
    world_init(GUGU_REPO_ROOT "/host/build/test_data");
    mind_config_t cfg = { .content_dir = GUGU_REPO_ROOT "/content", .child_name = "小满", .child_age_months = 42, .family_langs = { LANG_ZH, LANG_YUE, LANG_EN } };
    mind_init(&cfg);
    world_trip_clear();
    const char *place = getenv("GUGU_TRIP_PLACE");
    char why[240]; life_plan_t p = { .kind_hint = getenv("GUGU_TRIP_KIND") ? getenv("GUGU_TRIP_KIND") : "deliver", .place_hint = place, .nights = 2 };
    int64_t t0 = clock_us();
    esp_err_t err = life_plan_trip(&p, why, sizeof why);
    printf("  plan: %s in %lld ms: %s\n", esp_err_to_name(err), (long long)((clock_us() - t0) / 1000), why);
    CHECK(err == ESP_OK);
    const world_trip_t *t = world_trip_lock();
    if (t) {
        printf("  %s to %s, %s (%s) lat %.3f lon %.3f tz %+d min, %s..%s\n  reason: %s\n  recipient: %s\n  letter: %s\n", world_kind_name(t->kind), t->place.name,
               t->place.landmark, t->place.country, t->place.lat, t->place.lon, t->place.tz_offset_min, t->depart_date, t->return_date, t->reason, t->recipient, t->letter);
        for (int i = 0; i < t->days_count; i++) printf("  day %d: %s\n", i + 1, t->days[i]);
        for (int i = 0; i < t->fact_count; i++) printf("  fact: %s\n", t->facts[i]);
        for (int i = 0; i < t->days_count && t->postcards[i][0]; i++) printf("  postcard %d: %s\n", i + 1, t->postcards[i]);
        printf("  scene: %s\n  objects:", t->scene_prompt);
        for (int i = 0; i < t->object_count; i++) printf(" %s", t->objects[i]);
        printf("\n  friend %s (%s), souvenir %s\n", t->friend_name, t->friend_animal, t->souvenir);
        CHECK(t->fact_count >= 5); CHECK_EQ_INT(t->days_count, t->nights + 1); CHECK(t->postcards[0][0] != 0); CHECK(t->object_count >= 2); CHECK(t->scene_prompt[0] != 0);
        CHECK(t->place.lat != 0 && t->place.lon != 0);
        world_trip_unlock();
    }
    world_trip_clear();
}

// Two trips in a row against the real model: the recap of the first, then a second card that is asked with the journey.
TEST(test_live_story_chain)
{
    if (!live_ready()) return;
    store_init(GUGU_REPO_ROOT "/host/build/test_data", GUGU_REPO_ROOT "/content");
    world_init(GUGU_REPO_ROOT "/host/build/test_data");
    mind_config_t cfg = { .content_dir = GUGU_REPO_ROOT "/content", .child_name = "小满", .child_age_months = 42, .family_langs = { LANG_ZH, LANG_YUE, LANG_EN } };
    mind_init(&cfg);
    char p[300]; snprintf(p, sizeof p, "%s/memory/journey.jsonl", GUGU_REPO_ROOT "/host/build/test_data"); remove(p);
    world_trip_clear(); world_clock_reset();
    char why[240]; life_plan_t pl = { .kind_hint = "deliver", .nights = 1 };
    CHECK(life_plan_trip(&pl, why, sizeof why) == ESP_OK); printf("  trip 1: %s\n", why);
    const world_trip_t *t = world_trip_lock(); if (!t) return;
    char depart[11], ret[11]; snprintf(depart, 11, "%s", t->depart_date); snprintf(ret, 11, "%s", t->return_date);
    printf("  hooks:"); for (int i = 0; i < t->hook_count; i++) printf(" [%s]", t->hooks[i]); printf("\n");
    world_trip_unlock();
    struct tm tm = { 0 }; int y, m, d;
    sscanf(ret, "%d-%d-%d", &y, &m, &d); tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d; tm.tm_hour = 18; tm.tm_isdst = -1;
    world_clock_set(mktime(&tm));
    CHECK(life_diary_now(why, sizeof why) == ESP_OK); printf("  diary: %s\n", why);
    int64_t t0 = clock_us();
    CHECK(life_recap_now(why, sizeof why) == ESP_OK); printf("  recap (%lld ms): %s\n", (long long)((clock_us() - t0) / 1000), why);
    t = world_trip_lock(); if (t) { printf("  recap text: %s\n  open hooks:", t->recap); for (int i = 0; i < t->hook_count; i++) printf(" [%s]", t->hooks[i]); printf("\n"); world_trip_unlock(); }
    tm.tm_mday += 1; tm.tm_hour = 3; world_clock_set(mktime(&tm));
    life_plan_t p2 = { 0 };
    CHECK(life_plan_trip(&p2, why, sizeof why) == ESP_OK); printf("  trip 2: %s\n", why);
    t = world_trip_lock(); if (t) { printf("  prev_link: %s\n", t->prev_link); CHECK(t->prev_link[0] != 0); world_trip_unlock(); }
    static char buf[4096]; mind_journey_recent(buf, sizeof buf, 3); printf("  journey block:\n%s", buf);
    world_trip_clear(); world_clock_reset();
}

// One real picture through the image role, then through the device's own import.
TEST(test_live_image)
{
    if (!live_ready()) return;
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_IMAGE);
    CHECK(v && v->image_generate);
    if (!v || !v->image_generate) return;
    const char *out = GUGU_REPO_ROOT "/host/build/test_data/art/live_backdrop.jpg";
    image_params_t p = { .prompt = "the Great Pyramids of Giza and the Sphinx in a sandy desert, simple flat colours, bold shapes, children's picture-book illustration, no people, no animals, no text", .aspect = "1:1", .out_path = out };
    provider_usage_t u = { 0 }; provider_error_t e = { 0 };
    int64_t t0 = clock_us();
    esp_err_t r = v->image_generate(&p, &u, &e);
    printf("  image: %s in %lld ms (%s)\n", esp_err_to_name(r), (long long)((clock_us() - t0) / 1000), e.message);
    CHECK(r == ESP_OK); CHECK_EQ_INT(u.images, 1);
    art_image_t img;
    CHECK(art_import_backdrop(out, ART_GROUND_SAND, &img) == ESP_OK);
    printf("  imported: %d colours, sky mask %08lx\n", img.ncolors, (unsigned long)img.sky_mask);
    CHECK(img.sky_mask != 0);
    art_free(&img);
}

// Any vendor, the same round trip: one chat reply, one sentence of voice, that voice through the recognizer. The keys
// come the way the device reads them (each preset's secrets; on the host GUGU_<SECRET> from the environment), and a
// vendor without its keys is skipped. GUGU_LIVE_IMAGE=1 also draws one picture through it.
static void live_vendor(const char *asr_id, const char *chat_id, const char *tts_id, lang_t lang, const char *line, const char *expect)
{
    if (!getenv("GUGU_LIVE")) { printf("  (skipped: set GUGU_LIVE=1)\n"); return; }
    store_init(GUGU_REPO_ROOT "/host/build/test_data", GUGU_REPO_ROOT "/content");
    provider_init(GUGU_REPO_ROOT "/content/providers");
    provider_configure();
    provider_assign(PROVIDER_ROLE_ASR, asr_id); provider_assign(PROVIDER_ROLE_CHAT, chat_id); provider_assign(PROVIDER_ROLE_TTS, tts_id);
    char why[96];
    if (!provider_role_ready(PROVIDER_ROLE_CHAT, why, sizeof why) || !provider_role_ready(PROVIDER_ROLE_TTS, why, sizeof why) || !provider_role_ready(PROVIDER_ROLE_ASR, why, sizeof why)) {
        printf("  (skipped: %s)\n", why); return;
    }
    // chat
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_CHAT);
    chat_events_t ev = { l_delta, l_done, l_err };
    chat_session_t *c; acc[0] = 0; done_calls = 0; err_calls = 0;
    CHECK(v->chat_open(&ev, NULL, &c) == ESP_OK);
    chat_block_t sys[1] = { { "你是咕咕，一只住在孩子窗台鸽子屋里的小信鸽。一句话回答，不超过 20 个字。每句话前面加一个方括号表情标签，例如 [happy]。", true } };
    chat_message_t m = { "user", "咕咕你今天去哪里玩了呀？" };
    chat_request_t req = { .system = sys, .system_count = 1, .messages = &m, .message_count = 1, .effort = CHAT_EFFORT_LOW, .max_tokens = 120 };
    t_send = clock_us(); v->chat_send(c, &req); v->chat_close(c);
    printf("  chat %s: first delta %lld ms, reply: %s\n", chat_id, (long long)((t_first_delta - t_send) / 1000), acc);
    CHECK(acc[0] != 0); CHECK_EQ_INT(done_calls, 1); CHECK_EQ_INT(err_calls, 0);
    // voice
    v = provider_for(PROVIDER_ROLE_TTS);
    l_cap = 16000 * 30; l_pcm = mem_alloc_big(l_cap * 2); l_samples = 0; t_flushed = 0; err_calls = 0;
    tts_params_t tp = { .lang = lang, .sample_rate = 16000, .speed = 1.0f };
    tts_events_t te = { l_sstart, l_audio, NULL, l_flushed, l_err };
    tts_session_t *t;
    int64_t t0 = clock_us();
    CHECK(v->tts_open(&tp, &te, NULL, &t) == ESP_OK);
    int64_t t_sent = clock_us();
    v->tts_text(t, line, strlen(line)); v->tts_flush(t);
    for (int i = 0; i < 300 && t_flushed < t_sent && !err_calls; i++) clock_sleep_ms(100);
    v->tts_close(t);
    printf("  voice %s: open %lld ms, first audio %lld ms, %u ms of voice\n", tts_id, (long long)((t_sent - t0) / 1000),
           (long long)((t_first_audio - t_sent) / 1000), (unsigned)(l_samples / 16));
    CHECK(l_samples > 16000); CHECK_EQ_INT(err_calls, 0);
    // the voice through the recognizer
    v = provider_for(PROVIDER_ROLE_ASR);
    asr_session_t *a; asr_params_t ap = { .sample_rate = 16000 }; asr_events_t ae = { NULL, l_final, l_err };
    CHECK(v->asr_open(&ap, &ae, NULL, &a) == ESP_OK);
    v->asr_feed(a, l_pcm, l_samples);
    int64_t t_rel = clock_us(); asr_text[0] = 0;
    v->asr_finish(a); v->asr_close(a);
    printf("  asr %s: %lld ms -> \"%s\" (lang %s)\n", asr_id, (long long)((t_asr_done - t_rel) / 1000), asr_text, lang_code(asr_lang));
    CHECK(strstr(asr_text, expect) != NULL);
    mem_release(l_pcm);
    if (getenv("GUGU_LIVE_IMAGE")) {
        provider_assign(PROVIDER_ROLE_IMAGE, chat_id);
        const provider_vtable_t *iv = provider_for(PROVIDER_ROLE_IMAGE);
        if (iv && iv->image_generate && !strcmp(iv->id, chat_id)) {
            char out[200]; snprintf(out, sizeof out, GUGU_REPO_ROOT "/host/build/test_data/art/live_%s.jpg", chat_id);
            image_params_t ip = { .prompt = "a red lighthouse on a green hill by the sea, simple flat colours, children's picture-book illustration, no people, no text", .aspect = "1:1", .out_path = out };
            provider_usage_t u = { 0 }; provider_error_t e = { 0 };
            int64_t ti = clock_us();
            esp_err_t r = iv->image_generate(&ip, &u, &e);
            printf("  image %s: %s in %lld ms (%s)\n", chat_id, esp_err_to_name(r), (long long)((clock_us() - ti) / 1000), e.message);
            art_image_t img;
            CHECK(r == ESP_OK && art_import_backdrop(out, ART_GROUND_GRASS, &img) == ESP_OK);
            if (r == ESP_OK) art_free(&img);
        }
    }
}
TEST(test_live_openai) { live_vendor("openai", "openai", "openai", LANG_YUE, "你好呀，我係咕咕，一隻識送信嘅鴿仔。", "咕咕"); }
TEST(test_live_gemini) { live_vendor("gemini", "gemini", "gemini", LANG_YUE, "你好呀，我係咕咕，一隻識送信嘅鴿仔。", "咕咕"); }
TEST(test_live_qwen)   { live_vendor("qwen_realtime", "qwen", "qwen", LANG_YUE, "你好呀，我係咕咕，一隻識送信嘅鴿仔。", "咕咕"); }
TEST(test_live_volc)   { live_vendor("volc", "volc", "volc", LANG_ZH, "你好呀，我是咕咕，一只会送信的小鸽子。", "咕咕"); }

void test_live(void)
{
    RUN(test_live_image);
    RUN(test_live_minimax_chat_stream);
    RUN(test_live_minimax_tts_bidi_then_asr);
    RUN(test_live_trip_card);
    RUN(test_live_story_chain);
    RUN(test_live_openai);
    RUN(test_live_gemini);
    RUN(test_live_qwen);
    RUN(test_live_volc);
}
