#include "minitest.h"
#include "provider/provider.h"
#include "provider/provider_wire.h"
#include "provider/provider_internal.h"
#include "foundation/mem.h"
#include "cJSON.h"
#include <stdlib.h>

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
    CHECK(wire_openai_build(&b, "qwen-plus", &req, "\"enable_thinking\":false"));
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

void test_provider(void)
{
    RUN(test_volc_frames);
    RUN(test_wire_openai);
    RUN(test_wire_anthropic_build);
    RUN(test_wire_anthropic_stream);
    RUN(test_preset_minimax);
    RUN(test_fake_vendor_roundtrip);
}
