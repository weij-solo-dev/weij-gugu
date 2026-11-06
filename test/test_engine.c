// Offline end-to-end turn through the cascade engine with the fake vendor: event order, segment /
// emotion pairing, timing fields (every stage stamped, in order, from the moment the key was let go), dialogue memory.
#include "minitest.h"
#include "engine/engine.h"
#include "provider/provider.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/atomic_file.h"
#include "store/store.h"
#include <string.h>

static int ev_count[16]; static int seg_emotions[8]; static int seg_n; static size_t audio_samples; static int done_segments; static char done_reply[512]; static engine_timing_t done_t; static int ready, errors;
static int story_phase[16], story_n;             // ENGINE_EV_STORY phases in order
static char seg_said[48][96]; static int seg_said_n;   // every sentence handed to the voice, in order
static volatile uint32_t s_emitted_ms, s_played_ms; static volatile bool s_play_all;
static uint32_t backlog(void) { return s_play_all || s_played_ms >= s_emitted_ms ? 0 : s_emitted_ms - s_played_ms; }
static void on_ev(const engine_event_t *ev, void *u)
{
    ev_count[ev->kind]++;
    switch (ev->kind) {
    case ENGINE_EV_STORY: if (story_n < 16) story_phase[story_n++] = ev->u.story.phase; break;
    case ENGINE_EV_READY: ready++; break;
    case ENGINE_EV_SEGMENT_START:
        if (seg_n < 8) seg_emotions[seg_n++] = ev->u.segment.emotion;
        if (ev->u.segment.text && seg_said_n < 48) snprintf(seg_said[seg_said_n++], sizeof seg_said[0], "%s", ev->u.segment.text);
        break;
    case ENGINE_EV_AUDIO: audio_samples += ev->u.audio.samples; s_emitted_ms += (uint32_t)(ev->u.audio.samples * 1000 / 16000); break;
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

void provider_fake_script(const char *asr_text, const char *chat_reply);
void provider_fake_script_queue(const char *chat_reply);
void provider_fake_script_clear(void);
static const char *PART1 = "[happy] 从前有一只小恐龙，叫嘟嘟。[curious] 它想去看大海。[surprised] 走着走着，前面有一条大河！[scared] 河水哗啦啦地流。[curious] 嘟嘟站在河边想啊想。[curious] 你猜，嘟嘟会怎么过河？";
static const char *PART2 = "[happy] 嘟嘟想到了大象。[laughing] 大象用长鼻子把它卷了起来。[curious] 可是河太宽了。[curious] 你猜它们怎么办？";
static const char *PART3 = "[happy] 一只大乌龟游了过来。[happy] 嘟嘟坐在乌龟背上过了河。[happy] 故事讲完啦。";
static engine_t *story_engine(void)
{
    provider_init(GUGU_REPO_ROOT "/content/providers");
    provider_assign(PROVIDER_ROLE_ASR, "fake"); provider_assign(PROVIDER_ROLE_CHAT, "fake"); provider_assign(PROVIDER_ROLE_TTS, "fake");
    mind_config_t mc = { .content_dir = GUGU_REPO_ROOT "/content", .child_name = "小满" };
    mind_init(&mc);
    memset(ev_count, 0, sizeof ev_count); seg_n = 0; audio_samples = 0; ready = 0; errors = 0; story_n = 0; seg_said_n = 0;
    s_emitted_ms = s_played_ms = 0; s_play_all = true;
    engine_t *e = engine_cascade_create(on_ev, NULL);
    engine_set_backlog_probe(e, backlog);
    engine_start_call(e, LANG_ZH, "在家里");
    for (int i = 0; i < 100 && !ready; i++) clock_sleep_ms(20);
    return e;
}
static void say(engine_t *e, const char *words)
{
    provider_fake_script(words, NULL);
    int16_t frame[320] = {0};
    for (int i = 0; i < 50; i++) engine_on_audio(e, frame, 320);
    engine_on_release(e, NULL);
}
static bool wait_for(int phases, int ms) { for (int i = 0; i < ms / 20 && story_n < phases && !errors; i++) clock_sleep_ms(20); return story_n >= phases; }

// Stories (2026-11-06, the owner: "小朋友喜欢听长故事"): asking for one tells it in three parts, each its own request.
// The first two end on a question and the engine waits; the child's answer (the next turn) or engine_story_next (no
// answer came) goes on; the third ends it, and the whole story lands on the shelf.
TEST(test_engine_story)
{
    engine_t *e = story_engine();
    CHECK_EQ_INT(ready, 1);
    provider_fake_script_clear();
    provider_fake_script_queue(PART1); provider_fake_script_queue(PART2); provider_fake_script_queue(PART3);
    say(e, "给我讲个恐龙的故事");
    CHECK(wait_for(2, 6000));
    CHECK_EQ_INT(errors, 0);
    CHECK(story_n >= 2 && story_phase[0] == ENGINE_STORY_BEGIN && story_phase[1] == ENGINE_STORY_ASK);
    CHECK_EQ_INT(ev_count[ENGINE_EV_TURN_DONE], 1);
    CHECK_EQ_INT(seg_said_n, 6);                                 // part one, sentence by sentence
    CHECK(engine_story_on(e));
    say(e, "大象来帮忙");                                         // the child answers the question
    CHECK(wait_for(3, 6000));
    CHECK(story_n >= 3 && story_phase[2] == ENGINE_STORY_ASK);
    chat_message_t m[12]; size_t n = mind_dialog_messages(m, 12);
    CHECK(n >= 2 && strstr(m[n - 2].text, "大象来帮忙") != NULL);   // the answer went to the model before part two
    CHECK(engine_story_next(e) == ESP_OK);                       // no answer to the second question: on with it
    CHECK(wait_for(4, 6000));
    CHECK(story_n >= 4 && story_phase[3] == ENGINE_STORY_END);
    CHECK(!engine_story_on(e));
    CHECK_EQ_INT(ev_count[ENGINE_EV_TURN_DONE], 3);
    char names[2][40]; char path[200]; char *saved = NULL;
    if (mind_story_shelf(names, 2) > 0) { snprintf(path, sizeof path, "%s/stories/%s", store_data_dir(), names[0]); saved = file_read_all(path, NULL); }
    CHECK(saved && strstr(saved, "从前有一只小恐龙") && strstr(saved, "大象用长鼻子") && strstr(saved, "故事讲完啦"));
    CHECK(saved && strstr(saved, "[surprised] 走着走着") != NULL);    // each sentence's face kept
    mem_release(saved);
    int shelf_before = mind_story_shelf(names, 2) > 0 ? 1 : 0, before_said = seg_said_n; story_n = 0;
    memset(ev_count, 0, sizeof ev_count);
    say(e, "再讲一遍");                                           // the same story again, from the shelf: no model, no new copy
    CHECK(wait_for(2, 6000));
    CHECK(story_n >= 2 && story_phase[0] == ENGINE_STORY_BEGIN && story_phase[1] == ENGINE_STORY_END);
    CHECK(seg_said_n - before_said == 6 + 4 + 3);                 // every sentence of the three parts
    char names2[2][40];
    CHECK(shelf_before == 1 && mind_story_shelf(names2, 2) > 0 && !strcmp(names2[0], names[0]));
    engine_end_call(e); clock_sleep_ms(100); engine_destroy(e);
}

// The voice is fed only as fast as the speaker plays (a part is longer than the 30 s playback ring), and a child who
// breaks in is answered first; then the story goes on from the sentence that was sounding.
TEST(test_engine_story_paces_and_resumes)
{
    engine_t *e = story_engine();
    s_play_all = false;                                          // nothing is heard: the speaker's queue only grows
    provider_fake_script_clear();
    provider_fake_script_queue(PART1);
    provider_fake_script_queue("[happy] 因为大海可大了。[curious] 我们接着讲吧。");
    provider_fake_script_queue(PART2);
    say(e, "讲个故事吧");
    for (int i = 0; i < 25; i++) clock_sleep_ms(20);
    CHECK(seg_said_n >= 2 && seg_said_n <= 3);                   // 8 s of voice waiting: no more sentences go out
    CHECK_EQ_INT(ev_count[ENGINE_EV_TURN_DONE], 0);
    engine_story_pause(e, 1);                                    // the child presses while sentence 1 sounds
    for (int i = 0; i < 100 && engine_busy(e); i++) clock_sleep_ms(20);
    CHECK(!engine_busy(e));
    CHECK(engine_story_on(e));
    s_play_all = true; int before = seg_said_n;
    say(e, "它为什么想看大海");
    CHECK(wait_for(2, 6000));
    CHECK(story_n >= 2 && story_phase[1] == ENGINE_STORY_ASK);
    CHECK(seg_said_n - before == 2 + 5);                         // the answer, then sentences 1–5 of part one
    if (seg_said_n - before == 7) {
        CHECK(strstr(seg_said[before], "大海可大了") != NULL);
        CHECK(strstr(seg_said[before + 2], "它想去看大海") != NULL);   // the sentence it broke into, from its start
        CHECK(strstr(seg_said[before + 6], "你猜") != NULL);
    }
    engine_end_call(e); clock_sleep_ms(100); engine_destroy(e);
}

void test_engine(void) { RUN(test_engine_fake_turn); RUN(test_engine_tts_recovers); RUN(test_engine_story); RUN(test_engine_story_paces_and_resumes); }
