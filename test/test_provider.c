#include "minitest.h"
#include "provider/provider.h"
#include "provider/provider_wire.h"
#include "provider/provider_internal.h"
#include "foundation/mem.h"
#include "store/store.h"
#include <stdatomic.h>
#include "foundation/strbuf.h"
#include "foundation/task.h"
#include "foundation/clock.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

#define TEST_DATA GUGU_REPO_ROOT "/host/build/test_data"

static char acc[1024]; static int done_calls; static char stop[32]; static provider_usage_t usage; static provider_error_t last_err;
static void d_delta(const char *t, size_t n, void *u) { strncat(acc, t, n); }
static void d_done(const char *sr, const provider_usage_t *us, void *u) { done_calls++; snprintf(stop, 32, "%s", sr); usage = *us; }
static void d_err(const provider_error_t *e, void *u) { last_err = *e; }

TEST(test_wire_anthropic_build)
{
    chat_block_t sys[2] = { { "persona", true }, { "today", false } };
    chat_message_t msgs[2] = { { "user", "你好 \"咕咕\"" }, { "assistant", "你好呀" } };
    chat_request_t req = { .system = sys, .system_count = 2, .messages = msgs, .message_count = 2, .effort = CHAT_EFFORT_LOW, .max_tokens = 120, .temperature = 0.7f };
    strbuf_t b; strbuf_init(&b);
    CHECK(wire_anthropic_build(&b, "MiniMax-M3.1-Flash", &req, "\"output_config\":{\"effort\":\"low\"}"));
    cJSON *j = cJSON_Parse(b.s);
    CHECK(j != NULL);
    CHECK_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(j, "model")), "MiniMax-M3.1-Flash");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(j, "stream")));
    cJSON *s = cJSON_GetObjectItem(j, "system");
    CHECK_EQ_INT(cJSON_GetArraySize(s), 2);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(s, 0), "cache_control") != NULL);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(s, 1), "cache_control") == NULL);
    cJSON *m = cJSON_GetObjectItem(j, "messages");
    CHECK_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(m, 0), "content")), "你好 \"咕咕\"");
    CHECK_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "output_config"), "effort")), "low");
    cJSON_Delete(j); strbuf_free(&b);
}

TEST(test_wire_anthropic_stream)
{
    acc[0] = 0; done_calls = 0; stop[0] = 0; memset(&usage, 0, sizeof usage);
    chat_events_t ev = { d_delta, d_done, d_err }; wire_anthropic_t w; wire_anthropic_init(&w, &ev, NULL);
    wire_anthropic_event(&w, "message_start", "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":80,\"cache_read_input_tokens\":128}}}");
    wire_anthropic_event(&w, "content_block_start", "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}");
    wire_anthropic_event(&w, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"hmm\"}}");
    wire_anthropic_event(&w, "content_block_start", "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    wire_anthropic_event(&w, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"[happy] 你好\"}}");
    wire_anthropic_event(&w, "ping", "{\"type\":\"ping\"}");
    wire_anthropic_event(&w, "content_block_delta", "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"呀！\"}}");
    wire_anthropic_event(&w, "message_delta", "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":21}}");
    wire_anthropic_event(&w, "message_stop", "{\"type\":\"message_stop\"}");
    CHECK_EQ_STR(acc, "[happy] 你好呀！");
    CHECK_EQ_INT(done_calls, 1); CHECK_EQ_STR(stop, "end_turn");
    CHECK_EQ_INT(usage.in_tokens, 80); CHECK_EQ_INT(usage.out_tokens, 21); CHECK_EQ_INT(usage.cached_tokens, 128);
    // http error classification with MiniMax's "(2013)" suffix
    wire_anthropic_t w2; wire_anthropic_init(&w2, &ev, NULL); memset(&last_err, 0, sizeof last_err);
    wire_anthropic_http_error(&w2, 400, "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"message\":\"invalid params (2013)\"}}");
    CHECK_EQ_INT(last_err.http_status, 400); CHECK_EQ_INT(last_err.api_code, 2013); CHECK(last_err.kind == PERR_OTHER);
    wire_anthropic_t w3; wire_anthropic_init(&w3, &ev, NULL);
    wire_anthropic_http_error(&w3, 401, "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid api key\"}}");
    CHECK(last_err.kind == PERR_AUTH);
}

TEST(test_preset_minimax)
{
    CHECK(provider_preset_load_dir(GUGU_REPO_ROOT "/content/providers") == ESP_OK);
    const provider_preset_t *p = provider_preset_get("minimax");
    CHECK(p != NULL);
    if (!p) return;
    CHECK_EQ_STR(p->chat_model, "MiniMax-M3");
    CHECK_EQ_STR(p->asr_url, "https://api.minimax.cn/v1/speech_to_text");
    CHECK_EQ_STR(p->tts_url, "wss://api.minimax.cn/ws/v1/t2a_v2_bidi");
    CHECK_EQ_STR(p->lang_boost[2], "Chinese,Yue");
    CHECK_EQ_STR(p->chat_extra[0], "\"thinking\":{\"type\":\"disabled\"}");
    CHECK(p->tts_bidi && p->image && !p->asr_stream);
    CHECK_EQ_STR(p->default_voice, "Cantonese_CuteGirl");
}

// The family's choices live in settings and NVS; the presets say what each vendor needs. provider_configure() turns
// them into role assignments, regions, voices and credentials, naming no vendor in code.
TEST(test_provider_configure)
{
    store_init(TEST_DATA, GUGU_REPO_ROOT "/content");
    CHECK(provider_init(GUGU_REPO_ROOT "/content/providers") == ESP_OK);
    const provider_preset_t *q = provider_preset_get("qwen_realtime"), *v = provider_preset_get("volc");
    CHECK(q && !strcmp(q->cred_store[0], "qwen_api_key"));          // the realtime recognizer shares the 百炼 key
    CHECK(v && !strcmp(v->role_cred[1], "ark_api_key"));             // Volcengine's chat uses the Ark key (slot 1 = CHAT)
    CHECK(v && !strcmp(v->role_cred[2], "api_key"));                 // its voice the speech key (slot 2 = TTS)
    int declared = 0;
    for (int i = 0; store_secret_name(i); i++) declared += !strcmp(store_secret_name(i), "volc_ark_api_key");
    CHECK_EQ_INT(declared, 1);                                       // the presets declared their secrets to the store
    char old_q[256] = ""; bool had_q = store_secret_get("qwen_api_key", old_q, sizeof old_q) == ESP_OK;
    char old_a[256] = ""; bool had_a = store_secret_get("volc_ark_api_key", old_a, sizeof old_a) == ESP_OK;
    store_setting_set_str("provider_tts", "qwen"); store_setting_set_str("provider_chat", "volc");
    store_setting_set_str("region_minimax", "overseas");
    store_setting_set_str("voice_minimax_yue", "Cantonese_GentleLady");
    store_secret_set("qwen_api_key", "sk-test-qwen-0123456789"); store_secret_set("volc_ark_api_key", "");
    provider_configure();
    CHECK_EQ_STR(provider_for(PROVIDER_ROLE_TTS)->id, "qwen");
    CHECK_EQ_STR(provider_for(PROVIDER_ROLE_CHAT)->id, "volc");
    CHECK_EQ_STR(provider_for(PROVIDER_ROLE_ASR)->id, "minimax");    // unset: the preset with the lowest "order" that plays it
    CHECK_EQ_STR(provider_preset_get("minimax")->asr_url, "https://api.minimax.io/v1/speech_to_text");   // the overseas region
    CHECK(provider_credential("qwen_realtime", "api_key") && !strcmp(provider_credential("qwen_realtime", "api_key"), "sk-test-qwen-0123456789"));
    char why[96];
    CHECK(provider_role_ready(PROVIDER_ROLE_TTS, why, sizeof why));
    CHECK(!provider_role_ready(PROVIDER_ROLE_CHAT, why, sizeof why) && strstr(why, "ark_api_key") != NULL);
    // a voice: the caller's, then the family's pick for that vendor and language, then the preset's per language, then "*"
    const provider_preset_t *m = provider_preset_get("minimax");
    CHECK_EQ_STR(provider_voice(m, LANG_YUE, NULL), "Cantonese_GentleLady");
    CHECK_EQ_STR(provider_voice(m, LANG_YUE, "Cantonese_KindWoman"), "Cantonese_KindWoman");
    CHECK_EQ_STR(provider_voice(m, LANG_ZH, NULL), "lovely_girl");
    CHECK_EQ_STR(provider_voice(m, LANG_EN, NULL), "Cute_Elf");
    store_setting_set_str("provider_tts", ""); store_setting_set_str("provider_chat", "");
    store_setting_set_str("region_minimax", ""); store_setting_set_str("voice_minimax_yue", "");
    store_secret_set("qwen_api_key", had_q ? old_q : ""); store_secret_set("volc_ark_api_key", had_a ? old_a : "");
    provider_configure();
    CHECK_EQ_STR(provider_for(PROVIDER_ROLE_TTS)->id, "minimax");
    CHECK_EQ_STR(provider_preset_get("minimax")->asr_url, "https://api.minimax.cn/v1/speech_to_text");
    CHECK_EQ_STR(provider_voice(m, LANG_YUE, NULL), "Cantonese_CuteGirl");
}

static char fake_text[256]; static int fake_audio_samples; static int fake_flushed;
static void fa_final(const char *t, lang_t l, const provider_usage_t *u, void *x) { snprintf(fake_text, 256, "%s", t); }
static void fa_err(const provider_error_t *e, void *x) { }
static void ft_audio(const int16_t *pcm, size_t n, void *x) { fake_audio_samples += (int)n; }
static void ft_flushed(const provider_usage_t *u, void *x) { fake_flushed++; }

TEST(test_fake_vendor_roundtrip)
{
    CHECK(provider_init(GUGU_REPO_ROOT "/content/providers") == ESP_OK);
    CHECK(provider_assign(PROVIDER_ROLE_ASR, "fake") == ESP_OK);
    CHECK(provider_assign(PROVIDER_ROLE_CHAT, "fake") == ESP_OK);
    CHECK(provider_assign(PROVIDER_ROLE_TTS, "fake") == ESP_OK);
    CHECK(provider_assign(PROVIDER_ROLE_S2S, "fake") == ESP_ERR_NOT_SUPPORTED);
    const provider_vtable_t *v = provider_for(PROVIDER_ROLE_ASR);
    asr_session_t *a; asr_params_t ap = { .sample_rate = 16000 }; asr_events_t ae = { NULL, fa_final, fa_err };
    CHECK(v->asr_open(&ap, &ae, NULL, &a) == ESP_OK);
    int16_t z[160] = {0}; v->asr_feed(a, z, 160); v->asr_finish(a); v->asr_close(a);
    CHECK(strstr(fake_text, "鸭子") != NULL);
    acc[0] = 0; done_calls = 0;
    chat_session_t *c; chat_events_t ce = { d_delta, d_done, d_err };
    v = provider_for(PROVIDER_ROLE_CHAT);
    CHECK(v->chat_open(&ce, NULL, &c) == ESP_OK);
    chat_message_t m = { "user", fake_text }; chat_request_t req = { .messages = &m, .message_count = 1 };
    v->chat_send(c, &req); v->chat_close(c);
    CHECK(strstr(acc, "[happy]") == acc); CHECK_EQ_INT(done_calls, 1);
    tts_session_t *t; tts_params_t tp = { .lang = LANG_ZH, .sample_rate = 16000 }; tts_events_t te = { NULL, ft_audio, NULL, ft_flushed, fa_err };
    v = provider_for(PROVIDER_ROLE_TTS);
    CHECK(v->tts_open(&tp, &te, NULL, &t) == ESP_OK);
    v->tts_text(t, "你好呀。", 12); v->tts_flush(t); v->tts_close(t);
    CHECK(fake_audio_samples > 10000 && fake_flushed == 1);   // 4 characters ≈ 0.97 s of tone
}

TEST(test_wire_openai)
{
    chat_block_t sys[1] = { { "persona", true } };
    chat_message_t msgs[1] = { { "user", "hi" } };
    chat_request_t req = { .system = sys, .system_count = 1, .messages = msgs, .message_count = 1, .max_tokens = 50 };
    strbuf_t b; strbuf_init(&b);
    CHECK(wire_openai_build(&b, "qwen-plus", &req, "\"enable_thinking\":false", "max_tokens", true));
    cJSON *j = cJSON_Parse(b.s); CHECK(j != NULL);
    cJSON *m = cJSON_GetObjectItem(j, "messages");
    CHECK_EQ_INT(cJSON_GetArraySize(m), 2);
    CHECK_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(m, 0), "role")), "system");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(j, "enable_thinking")));
    cJSON_Delete(j); strbuf_free(&b);
    acc[0] = 0; done_calls = 0;
    chat_events_t ev = { d_delta, d_done, d_err }; wire_openai_t w; wire_openai_init(&w, &ev, NULL);
    wire_openai_event(&w, "", "{\"choices\":[{\"delta\":{\"content\":\"[happy] 你好\"},\"finish_reason\":null}]}");
    wire_openai_event(&w, "", "{\"choices\":[{\"delta\":{\"content\":\"呀\"},\"finish_reason\":\"stop\"}]}");
    wire_openai_event(&w, "", "{\"choices\":[],\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":9,\"prompt_tokens_details\":{\"cached_tokens\":100}}}");
    wire_openai_event(&w, "", "[DONE]");
    CHECK_EQ_STR(acc, "[happy] 你好呀"); CHECK_EQ_INT(done_calls, 1); CHECK_EQ_STR(stop, "stop");
    CHECK_EQ_INT(usage.in_tokens, 120); CHECK_EQ_INT(usage.cached_tokens, 100);
    provider_preset_load_dir(GUGU_REPO_ROOT "/content/providers");
    CHECK(provider_preset_get("qwen") != NULL && provider_preset_get("qwen_realtime") != NULL);
}

TEST(test_volc_frames)
{
    strbuf_t b; strbuf_init(&b);
    // client frame with event + session: StartSession
    size_t n = volc_frame_build(&b, 0x1, 0x4, 0x1, 100, "sess-1", 0, "{\"a\":1}", 7);
    const uint8_t *d = (const uint8_t *)b.s;
    CHECK_EQ_INT(n, 4 + 4 + 4 + 6 + 4 + 7);
    CHECK(d[0] == 0x11 && d[1] == 0x14 && d[2] == 0x10 && d[3] == 0);
    CHECK(d[4] == 0 && d[5] == 0 && d[6] == 0 && d[7] == 100);          // event big-endian
    CHECK(d[11] == 6 && !memcmp(d + 12, "sess-1", 6));                 // session length + id
    CHECK(d[21] == 7);                                                  // payload size
    volc_frame_t f;
    CHECK(volc_frame_parse(d, n, &f));
    CHECK_EQ_INT(f.event, 100); CHECK_EQ_INT(f.n, 7); CHECK(!memcmp(f.payload, "{\"a\":1}", 7));
    // audio-only with negative (last) sequence
    int16_t pcm[4] = { 1, 2, 3, 4 };
    n = volc_frame_build(&b, 0x2, 0x3, 0x0, 0, NULL, -5, pcm, sizeof pcm);
    d = (const uint8_t *)b.s;
    CHECK(d[1] == 0x23 && d[2] == 0x00);
    CHECK(volc_frame_parse(d, n, &f)); CHECK_EQ_INT(f.seq, -5); CHECK_EQ_INT(f.n, 8); CHECK_EQ_INT(f.flags, 0x3);
    // error frame
    const uint8_t err[] = { 0x11, 0xF0, 0x10, 0x00, 0x02, 0xAE, 0xA5, 0x41, 0x00, 0x00, 0x00, 0x03, 'b', 'a', 'd' };
    CHECK(volc_frame_parse(err, sizeof err, &f)); CHECK_EQ_INT(f.type, 0xF); CHECK_EQ_INT(f.err_code, 45000001u); CHECK_EQ_INT(f.n, 3);
    strbuf_free(&b);
    provider_preset_load_dir(GUGU_REPO_ROOT "/content/providers");
    CHECK(provider_preset_get("volc") != NULL);
}

// OpenAI and Gemini: what the presets say, what a request looks like, how the replies read (fixtures in the shapes of
// the vendors' own docs, checked 2026-11-04). Live calls go through test_live.c once keys exist.
TEST(test_vendors_openai_gemini)
{
    CHECK(provider_preset_load_dir(GUGU_REPO_ROOT "/content/providers") == ESP_OK);
    const provider_preset_t *o = provider_preset_get("openai"), *g = provider_preset_get("gemini");
    CHECK(o && g); if (!o || !g) return;
    CHECK_EQ_STR(o->chat_max_field, "max_completion_tokens"); CHECK(!o->chat_temperature); CHECK_EQ_INT(o->tts_rate, 24000);
    CHECK(strstr(o->lang_boost[2], "香港") != NULL);                  // the voice is told Hong Kong Cantonese
    CHECK_EQ_STR(o->chat_url, "https://api.openai.com/v1/chat/completions");
    CHECK_EQ_STR(g->chat_url, "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions");
    CHECK_EQ_STR(g->chat_max_field, "max_tokens"); CHECK(!g->chat_temperature); CHECK_EQ_INT(g->tts_rate, 0);
    CHECK_EQ_STR(provider_voice(o, LANG_YUE, NULL), "marin"); CHECK_EQ_STR(provider_voice(g, LANG_EN, NULL), "Leda");
    strbuf_t b; strbuf_init(&b);                                      // a reasoning model: the newer field, no temperature, reasoning off
    chat_block_t sys = { "persona", true }; chat_message_t m = { "user", "hi" };
    chat_request_t req = { .system = &sys, .system_count = 1, .messages = &m, .message_count = 1, .max_tokens = 400, .temperature = 0.8f };
    CHECK(wire_openai_build(&b, o->chat_model, &req, o->chat_extra[0], o->chat_max_field, o->chat_temperature));
    CHECK(strstr(b.s, "\"max_completion_tokens\":400") != NULL && strstr(b.s, "temperature") == NULL && strstr(b.s, "\"reasoning_effort\":\"none\"") != NULL);
    cJSON *jb = cJSON_Parse(b.s); CHECK(jb != NULL); cJSON_Delete(jb); strbuf_free(&b);
    char t[256]; lang_t l = LANG_UNKNOWN;
    CHECK(provider_openai_transcript("{\"text\":\"咕咕你喺邊度？\",\"languages\":[{\"code\":\"yue\"}],\"usage\":{\"type\":\"duration\",\"seconds\":2}}", t, sizeof t, &l));
    CHECK_EQ_STR(t, "咕咕你喺邊度？"); CHECK_EQ_INT(l, LANG_YUE);
    CHECK(provider_openai_transcript("{\"text\":\"hello gugu\",\"languages\":[]}", t, sizeof t, &l)); CHECK_EQ_INT(l, LANG_EN);   // unsure: guessed
    CHECK(!provider_openai_transcript("{\"error\":{\"message\":\"Incorrect API key provided\"}}", t, sizeof t, &l));
    const char *inter = "{\"id\":\"i1\",\"steps\":[{\"type\":\"user_input\",\"content\":[{\"type\":\"text\",\"text\":\"ignored\"}]},"
                        "{\"type\":\"model_output\",\"content\":[{\"type\":\"text\",\"text\":\"你今日去咗邊度玩？\"}]}]}";
    CHECK(provider_gemini_text(inter, t, sizeof t)); CHECK_EQ_STR(t, "你今日去咗邊度玩？");
    CHECK(provider_gemini_text("{\"steps\":[]}", t, sizeof t)); CHECK_EQ_STR(t, "");          // nothing heard
    CHECK(!provider_gemini_text("{\"error\":{\"code\":\"authentication\",\"message\":\"bad key\"}}", t, sizeof t));
    size_t n = 0;
    uint8_t *img = provider_gemini_image("{\"steps\":[{\"type\":\"model_output\",\"content\":[{\"type\":\"image\",\"mime_type\":\"image/jpeg\",\"data\":\"/9j/4AAQ\"}]}]}", &n);
    CHECK(img && n == 6 && img[0] == 0xFF && img[1] == 0xD8); mem_release(img);
    img = provider_json_b64("{\"created\":1,\"data\":[{\"b64_json\":\"/9j/4AAQ\"}],\"output_format\":\"jpeg\"}", NULL, "b64_json", &n);
    CHECK(img && n == 6 && img[0] == 0xFF); mem_release(img);
    uint8_t pcm[64]; uint32_t rate = 0; char err[96];
    CHECK_EQ_INT(provider_gemini_tts_event("step.delta", "{\"index\":1,\"delta\":{\"type\":\"audio\",\"data\":\"AAECAw==\",\"mime_type\":\"audio/l16\",\"sample_rate\":16000,\"channels\":1}}",
                                           pcm, sizeof pcm, &rate, err, sizeof err), 4);
    CHECK_EQ_INT(rate, 16000); CHECK(pcm[3] == 3);
    CHECK_EQ_INT(provider_gemini_tts_event("error", "{\"error\":{\"code\":\"safety\",\"message\":\"blocked\"},\"event_type\":\"error\"}", pcm, sizeof pcm, &rate, err, sizeof err), 0);
    CHECK_EQ_STR(err, "blocked");
    CHECK_EQ_INT(provider_gemini_tts_event("step.start", "{\"index\":1,\"step\":{\"type\":\"model_output\"}}", pcm, sizeof pcm, &rate, err, sizeof err), 0);
    CHECK_EQ_STR(err, "");
}

// The shared HTTP voice: segments are spoken in order on the worker, each flushed once; 24 kHz comes out at 16 kHz;
// a cancel cuts the segment in flight and drops the queue, and nothing of theirs reaches the engine afterwards.
static signal_t s_hold; static atomic_int s_ht_flushed, s_ht_errors, s_ht_samples; static char s_ht_order[64];
static esp_err_t ht_speak(http_tts_t *t, const char *text, provider_error_t *err)
{
    if (!strcmp(text, "hold")) { signal_wait(s_hold, 5000); return ESP_OK; }   // a slow request, cut short by abort()
    if (!strcmp(text, "bad")) { provider_error_set(err, PERR_NETWORK, 0, 0, "dropped"); return ESP_FAIL; }
    strncat(s_ht_order, text, sizeof s_ht_order - strlen(s_ht_order) - 1);
    static uint8_t pcm[2400 * 2];
    http_tts_pcm(t, pcm, 1001, 24000); http_tts_pcm(t, pcm + 1001, sizeof pcm - 1001, 24000);   // an odd split
    return ESP_OK;
}
static void ht_abort(void *v) { (void)v; signal_set(s_hold); }
static void ht_audio(const int16_t *pcm, size_t n, void *u) { (void)pcm; (void)u; atomic_fetch_add(&s_ht_samples, (int)n); }
static void ht_flushed(const provider_usage_t *u, void *x) { (void)u; (void)x; atomic_fetch_add(&s_ht_flushed, 1); }
static void ht_error(const provider_error_t *e, void *x) { (void)e; (void)x; atomic_fetch_add(&s_ht_errors, 1); }
TEST(test_http_tts_worker)
{
    s_hold = signal_create(); s_ht_order[0] = 0; s_ht_flushed = 0; s_ht_errors = 0; s_ht_samples = 0;
    tts_params_t p = { .lang = LANG_ZH, .sample_rate = 16000 };
    tts_events_t ev = { NULL, ht_audio, NULL, ht_flushed, ht_error };
    http_tts_t *t = http_tts_create(&p, &ev, NULL, ht_speak, ht_abort, NULL);
    CHECK(t != NULL); if (!t) return;
    http_tts_text(t, "a", 1); http_tts_flush(t); http_tts_text(t, "b", 1); http_tts_flush(t); http_tts_text(t, "c", 1); http_tts_flush(t);
    for (int i = 0; i < 200 && s_ht_flushed < 3; i++) clock_sleep_ms(10);
    CHECK_EQ_INT(s_ht_flushed, 3); CHECK_EQ_STR(s_ht_order, "abc");
    CHECK(s_ht_samples >= 3 * 1599 && s_ht_samples <= 3 * 1600);     // 2400 samples at 24 kHz each: 1600 at 16 kHz
    http_tts_text(t, "hold", 4); http_tts_flush(t); http_tts_text(t, "d", 1); http_tts_flush(t);   // d waits behind the slow one
    clock_sleep_ms(50);
    http_tts_cancel(t);                                               // the engine aborts the turn
    clock_sleep_ms(150);
    CHECK_EQ_INT(s_ht_flushed, 3); CHECK_EQ_STR(s_ht_order, "abc");   // neither the cut one nor the dropped one reported
    http_tts_text(t, "bad", 3); http_tts_flush(t);
    for (int i = 0; i < 200 && !s_ht_errors; i++) clock_sleep_ms(10);
    CHECK_EQ_INT(s_ht_errors, 1);
    http_tts_text(t, "e", 1); http_tts_flush(t);
    for (int i = 0; i < 200 && s_ht_flushed < 4; i++) clock_sleep_ms(10);
    CHECK_EQ_INT(s_ht_flushed, 4); CHECK_EQ_STR(s_ht_order, "abce");
    http_tts_flush(t);                                                // nothing pending: flushed at once
    CHECK_EQ_INT(s_ht_flushed, 5);
    http_tts_destroy(t);
    signal_free(s_hold);
}

// The pairing page names what to fix from these words: they are part of /api/status, so they do not drift.
TEST(test_provider_names)
{
    CHECK_EQ_STR(provider_role_name(PROVIDER_ROLE_CHAT), "chat");
    CHECK_EQ_STR(provider_role_name(PROVIDER_ROLE_ASR), "asr");
    CHECK_EQ_STR(provider_role_name(PROVIDER_ROLE_TTS), "tts");
    CHECK_EQ_STR(provider_role_name((provider_role_t)0), "");
    CHECK_EQ_STR(provider_role_name(PROVIDER_ROLE_CHAT | PROVIDER_ROLE_TTS), "");     // not one role
    CHECK_EQ_STR(provider_error_kind_name(PERR_NONE), "");
    CHECK_EQ_STR(provider_error_kind_name(PERR_AUTH), "auth");
    CHECK_EQ_STR(provider_error_kind_name(PERR_QUOTA), "quota");
    CHECK_EQ_STR(provider_error_kind_name(PERR_NETWORK), "network");
    CHECK_EQ_STR(provider_error_kind_name(PERR_TIMEOUT), "timeout");
    CHECK_EQ_STR(provider_error_kind_name(PERR_VOICE_NOT_FOUND), "voice");
    CHECK_EQ_STR(provider_error_kind_name(PERR_OTHER), "other");
    CHECK_EQ_STR(provider_error_kind_name((provider_error_kind_t)99), "other");
}

void test_provider(void)
{
    RUN(test_volc_frames);
    RUN(test_wire_openai);
    RUN(test_wire_anthropic_build);
    RUN(test_wire_anthropic_stream);
    RUN(test_preset_minimax);
    RUN(test_provider_configure);
    RUN(test_vendors_openai_gemini);
    RUN(test_http_tts_worker);
    RUN(test_fake_vendor_roundtrip);
    RUN(test_provider_names);
}
