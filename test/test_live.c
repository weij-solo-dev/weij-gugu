// Live vendor tests: run only with GUGU_LIVE=1 and the vendor key in the environment. They spend
// real money (fractions of a cent) and record latency, which is the point.
#include "minitest.h"
#include "provider/provider.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/wav.h"
#include "foundation/atomic_file.h"
#include <stdlib.h>
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

void test_live(void)
{
    RUN(test_live_minimax_chat_stream);
    RUN(test_live_minimax_tts_bidi_then_asr);
}
