// mind_trip.c — the words around the trip card: the block a call prompt cites, the "where" sentence for
// block C, and the request that asks the model to plan the next trip.
#define LOG_TAG "mind"
#include "mind/mind.h"
#include "mind_internal.h"
#include "world/world.h"
#include "foundation/log.h"
#include "foundation/mem.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

char *mind_trip_tpl, *mind_recap_tpl;   // content/prompts/trip_card.md and trip_recap.md, loaded by mind_prompt_load

static const char *kind_zh(world_kind_t k)
{
    switch (k) { case WORLD_KIND_VACATION: return "度假"; case WORLD_KIND_VISIT: return "探亲"; default: return "送信"; }
}

static const char *tod_zh(world_tod_t t)
{
    switch (t) { case WORLD_TOD_SUNRISE: return "刚日出"; case WORLD_TOD_DAY: return "白天"; case WORLD_TOD_DUSK: return "黄昏"; default: return "夜里"; }
}

#define PUT(...) do { int w = snprintf(dst + o, o < cap ? cap - o : 0, __VA_ARGS__); if (w > 0) o += (size_t)w; } while (0)

// The trip part of block B. Empty when there is no card.
void mind_trip_block(char *dst, size_t cap)
{
    size_t o = 0;
    dst[0] = '\0';
    world_state_t st; world_state(&st);
    const world_trip_t *t = world_trip_lock();
    if (!t) return;
    PUT("【这一趟】");
    if (st.phase == WORLD_TRIP_PLANNED) PUT("你%s要出发去%s（%s）%s。", st.day_index == -1 ? "明天" : "过几天", t->place.name, t->place.country, kind_zh(t->kind));
    else if (st.phase == WORLD_TRIP_AWAY) PUT("你正在%s（%s）%s，今天是第 %d 天，还要睡 %d 觉才回家。", t->place.name, t->place.country, kind_zh(t->kind), st.day_index + 1, st.nights_left);
    else PUT("你刚从%s%s回来。", t->place.name, kind_zh(t->kind));
    PUT("为什么去：%s", t->reason);
    if (t->prev_link[0]) PUT("这趟和上一趟的关系：%s", t->prev_link);
    if (t->recipient[0]) PUT("收件人 / 要见的人：%s。", t->recipient);
    if (t->letter[0]) PUT("信里写的是：%s", t->letter);
    if (st.phase == WORLD_TRIP_AWAY && st.day_index < t->days_count && t->days[st.day_index][0]) PUT("\n今天的安排：%s", t->days[st.day_index]);
    PUT("\n你知道的关于%s的事（只能讲这些，别的真实世界的事不要编；孩子问起你不知道的，就说\"咕，这个我得去看看\"）：\n", t->place.name);
    for (int i = 0; i < t->fact_count; i++) PUT("- %s\n", t->facts[i]);
    if (t->friend_name[0]) PUT("那里的朋友：%s（%s）。", t->friend_name, t->friend_animal);
    if (t->souvenir[0]) PUT("准备带回家的纪念品：%s。", t->souvenir);
    for (int i = 0; i < t->hook_count; i++) PUT("%s%s", i ? "；" : "你记着的约定：", t->hooks[i]);
    if (t->hook_count) PUT("。");
    if (st.phase == WORLD_TRIP_DONE && t->recap[0]) PUT("\n这趟的回顾：%s", t->recap);
    world_trip_unlock();
}

// Block C's "where" when the caller leaves it to the world.
void mind_where_sentence(char *dst, size_t cap)
{
    size_t o = 0;
    dst[0] = '\0';
    world_state_t st; world_state(&st);
    if (st.away) {
        const world_trip_t *t = world_trip_lock();
        PUT("在%s，当地%s，%d 点 %02d 分。你在用对讲机和孩子说话，声音从很远的地方传过来", t ? t->place.name : "外地", tod_zh(st.tod), st.local_hour, st.local_min);
        if (t) world_trip_unlock();
    } else PUT("在家里，刚被叫到洞口聊天。窗外%s", tod_zh(st.tod));
}

static const char *season_zh(int month)
{
    if (month >= 3 && month <= 5) return "春天";
    if (month >= 6 && month <= 8) return "夏天";
    if (month >= 9 && month <= 11) return "秋天";
    return "冬天";
}

MEM_BIG_BSS static char s_req_task[8192], s_req_user[3072];

esp_err_t mind_trip_request_build(mind_trip_request_t *out, const char *kind_hint, const char *place_hint, const char *depart_date, int nights_hint, const char *recent_places, const char *journey)
{
    if (!mind_trip_tpl) return ESP_ERR_NOT_FOUND;
    world_place_t home; world_home(&home);
    const char *const pairs[] = { "home_city", home.name, "child_name", mind_child_name(), NULL };
    mind_fill(s_req_task, sizeof s_req_task, mind_trip_tpl, pairs);
    if (journey && *journey) { size_t o = strlen(s_req_task); snprintf(s_req_task + o, sizeof s_req_task - o, "\n\n%s", journey); }
    time_t now = world_now(); struct tm tm; localtime_r(&now, &tm);
    static const char *const WD[] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };
    const char *kind_zh_hint = !kind_hint ? "由你决定" : !strcmp(kind_hint, "vacation") ? "vacation（度假）" : !strcmp(kind_hint, "visit") ? "visit（探亲）" : "deliver（送信）";
    char nights[40];
    if (nights_hint >= 1 && nights_hint <= WORLD_MAX_NIGHTS) snprintf(nights, sizeof nights, "住 %d 晚", nights_hint); else snprintf(nights, sizeof nights, "住 1 到 3 晚，由你决定");
    snprintf(s_req_user, sizeof s_req_user,
             "今天是%04d年%d月%d日，%s，%s。%s家在%s。出发日期 %s，%s。出门类型：%s。地点提示：%s。最近去过：%s。请写这一趟的趟卡，只输出 JSON。",
             (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100, tm.tm_mday % 100, WD[tm.tm_wday], season_zh(tm.tm_mon + 1),
             mind_child_name(), home.name, depart_date ? depart_date : "明天", nights, kind_zh_hint,
             place_hint && *place_hint ? place_hint : "由你决定（全世界都可以，和最近去过的换个国家或大洲）", recent_places && *recent_places ? recent_places : "还没出过门");
    out->count = 2;
    out->blocks[0] = (chat_block_t){ mind_block_a(), true };
    out->blocks[1] = (chat_block_t){ s_req_task, false };
    out->user = s_req_user;
    return ESP_OK;
}

MEM_BIG_BSS static char s_recap_task[2048], s_recap_user[4096];

esp_err_t mind_recap_request_build(mind_trip_request_t *out, const char *days_text)
{
    if (!mind_recap_tpl) return ESP_ERR_NOT_FOUND;
    const char *const pairs[] = { "child_name", mind_child_name(), NULL };
    mind_fill(s_recap_task, sizeof s_recap_task, mind_recap_tpl, pairs);
    const world_trip_t *t = world_trip_lock();
    if (!t) return ESP_ERR_NOT_FOUND;
    size_t o = 0; char *dst = s_recap_user; size_t cap = sizeof s_recap_user;
    PUT("【这一趟】%s去%s（%s）%s，%s到%s，住 %d 晚。为什么去：%s", kind_zh(t->kind), t->place.name, t->place.country, t->place.landmark, t->depart_date, t->return_date, t->nights, t->reason);
    if (t->prev_link[0]) PUT("和上一趟的关系：%s", t->prev_link);
    if (t->recipient[0]) PUT("收件人 / 要见的人：%s。", t->recipient);
    if (t->letter[0]) PUT("信里写的是：%s", t->letter);
    if (t->friend_name[0]) PUT("那里的朋友：%s（%s）。", t->friend_name, t->friend_animal);
    if (t->souvenir[0]) PUT("带回的纪念品：%s。", t->souvenir);
    for (int i = 0; i < t->hook_count; i++) PUT("%s%s", i ? "；" : "出发时记着的线头：", t->hooks[i]);
    if (t->hook_count) PUT("。");
    PUT("\n【每天】\n%s", days_text && *days_text ? days_text : "（没有记下每天的事。）\n");
    PUT("请写回顾，只输出 JSON。");
    world_trip_unlock();
    out->count = 2;
    out->blocks[0] = (chat_block_t){ mind_block_a(), true };
    out->blocks[1] = (chat_block_t){ s_recap_task, false };
    out->user = s_recap_user;
    return ESP_OK;
}
