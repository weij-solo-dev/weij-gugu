// Offline end-to-end turn through the cascade engine with the fake vendor: event order, segment /
// emotion pairing, timing fields (every stage stamped, in order, from the moment the key was let go), dialogue memory.
#include "minitest.h"
#include "engine/engine.h"
#include "provider/provider.h"
#include "foundation/clock.h"
#include <string.h>

static int ev_count[16]; static int seg_emotions[8]; static int seg_n; static size_t audio_samples; static int done_segments; static char done_reply[512]; static engine_timing_t done_t; static int ready, errors;
static void on_ev(const engine_event_t *ev, void *u)
{
    ev_count[ev->kind]++;
    switch (ev->kind) {
    case ENGINE_EV_READY: ready++; break;
    case ENGINE_EV_SEGMENT_START: if (seg_n < 8) seg_emotions[seg_n++] = ev->u.segment.emotion; break;
    case ENGINE_EV_AUDIO: audio_samples += ev->u.audio.samples; break;
    case ENGINE_EV_TURN_DONE: done_segments = ev->u.done.segments; snprintf(done_reply, sizeof done_reply, "%s", ev->u.done.reply); done_t = ev->u.done.timing; break;
    case ENGINE_EV_ERROR: errors++; break;
    default: break;
    }
}

TEST(test_engine_fake_turn)
{
    provider_init(GUGU_REPO_ROOT "/content/providers");
    provider_assign(PROVIDER_ROLE_ASR, "fake"); provider_assign(PROVIDER_ROLE_CHAT, "fake"); provider_assign(PROVIDER_ROLE_TTS, "fake");
    mind_config_t mc = { .content_dir = GUGU_REPO_ROOT "/content", .child_name = "小满" };
    mind_init(&mc);
    engine_t *e = engine_cascade_create(on_ev, NULL);
    CHECK(e != NULL);
    CHECK(engine_start_call(e, LANG_ZH, "在家里") == ESP_OK);
    for (int i = 0; i < 100 && !ready; i++) clock_sleep_ms(20);
    CHECK_EQ_INT(ready, 1);
    int16_t frame[320] = {0};
    for (int i = 0; i < 50; i++) engine_on_audio(e, frame, 320);
    int64_t before = clock_us();
    CHECK(engine_on_release(e, "小满刚摸了你一下。") == ESP_OK);
    int64_t after = clock_us();
    for (int i = 0; i < 300 && !ev_count[ENGINE_EV_TURN_DONE] && !errors; i++) clock_sleep_ms(20);
    CHECK_EQ_INT(errors, 0);
    CHECK_EQ_INT(ev_count[ENGINE_EV_TRANSCRIPT], 1);
    CHECK_EQ_INT(ev_count[ENGINE_EV_TAG], 3);                  // [happy] [curious] [coo]
    CHECK_EQ_INT(done_segments, 3);
    CHECK_EQ_INT(seg_n, 3);
    CHECK(seg_emotions[0] == TAG_HAPPY && seg_emotions[1] == TAG_CURIOUS && seg_emotions[2] == TAG_CURIOUS);   // coo is not an emotion: keeps curious
    CHECK(audio_samples > 16000 * 3);
    CHECK(strstr(done_reply, "鸭子") != NULL && strstr(done_reply, "[") == NULL);
    CHECK(done_t.t1_transcript >= done_t.t0_release && done_t.t2_first_token >= done_t.t1_transcript && done_t.t4_turn_done >= done_t.t3_first_audio);
    CHECK(done_t.t0_release >= before && done_t.t0_release <= after);    // the turn's clock starts at the release, not when the engine's task gets to it
    CHECK(done_t.t_chat_sent >= done_t.t1_transcript && done_t.t2_first_token >= done_t.t_chat_sent);            // words → request → first word
    CHECK(done_t.t_first_segment >= done_t.t2_first_token && done_t.t3_first_audio >= done_t.t_first_segment);   // first word → first sentence → first sound
    chat_message_t m[12]; size_t n = mind_dialog_messages(m, 12);
    CHECK_EQ_INT(n, 2); CHECK_EQ_STR(m[0].role, "user"); CHECK_EQ_STR(m[1].role, "assistant");
    // second turn reuses the sessions
    memset(ev_count, 0, sizeof ev_count); seg_n = 0;
    for (int i = 0; i < 50; i++) engine_on_audio(e, frame, 320);
    engine_on_release(e, NULL);
    for (int i = 0; i < 300 && !ev_count[ENGINE_EV_TURN_DONE] && !errors; i++) clock_sleep_ms(20);
    CHECK_EQ_INT(ev_count[ENGINE_EV_TURN_DONE], 1);
    CHECK_EQ_INT(mind_dialog_messages(m, 12), 4);
    engine_end_call(e);
    clock_sleep_ms(100);
    CHECK_EQ_INT(mind_dialog_messages(m, 12), 0);              // reset at hang-up
}

void provider_fake_tts_fail(int n);

// The TTS session dies in a turn (a dropped WebSocket): that turn fails, and the next one in the same call answers
// because the engine opened a new session (it once failed every later turn too).
TEST(test_engine_tts_recovers)
{
    provider_init(GUGU_REPO_ROOT "/content/providers");
    provider_assign(PROVIDER_ROLE_ASR, "fake"); provider_assign(PROVIDER_ROLE_CHAT, "fake"); provider_assign(PROVIDER_ROLE_TTS, "fake");
    mind_config_t mc = { .content_dir = GUGU_REPO_ROOT "/content", .child_name = "小满" };
    mind_init(&mc);
    memset(ev_count, 0, sizeof ev_count); seg_n = 0; audio_samples = 0; ready = 0; errors = 0;
    engine_t *e = engine_cascade_create(on_ev, NULL);
    CHECK(e != NULL);
    CHECK(engine_start_call(e, LANG_ZH, "在家里") == ESP_OK);
    for (int i = 0; i < 100 && !ready; i++) clock_sleep_ms(20);
    CHECK_EQ_INT(ready, 1);
    int16_t frame[320] = {0};
    provider_fake_tts_fail(1);
    for (int i = 0; i < 50; i++) engine_on_audio(e, frame, 320);
    engine_on_release(e, NULL);
    for (int i = 0; i < 300 && !ev_count[ENGINE_EV_TURN_DONE] && !errors; i++) clock_sleep_ms(20);
    CHECK_EQ_INT(errors, 1); CHECK_EQ_INT(ev_count[ENGINE_EV_TURN_DONE], 0);
    for (int i = 0; i < 50 && engine_busy(e); i++) clock_sleep_ms(20);   // the engine renews the session after the failure
    memset(ev_count, 0, sizeof ev_count); audio_samples = 0; errors = 0;
    for (int i = 0; i < 50; i++) engine_on_audio(e, frame, 320);
    engine_on_release(e, NULL);
    for (int i = 0; i < 300 && !ev_count[ENGINE_EV_TURN_DONE] && !errors; i++) clock_sleep_ms(20);
    CHECK_EQ_INT(errors, 0); CHECK_EQ_INT(ev_count[ENGINE_EV_TURN_DONE], 1);
    CHECK(audio_samples > 16000);
    provider_fake_tts_fail(0);
    engine_end_call(e);
    clock_sleep_ms(100);
}

void test_engine(void) { RUN(test_engine_fake_turn); RUN(test_engine_tts_recovers); }
