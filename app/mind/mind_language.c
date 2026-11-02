// mind_language.c — which language Gugu answers in, and the transcript fixes for systematic ASR slips.
#define LOG_TAG "mind"
#include "mind/mind.h"
#include "mind_internal.h"
#include <string.h>

static lang_t s_family[3];
static lang_t s_last = LANG_ZH;

void mind_language_set_family(const lang_t langs[3])
{
    memcpy(s_family, langs, sizeof s_family);
}

lang_t mind_primary_lang(void)
{
    lang_t only = LANG_UNKNOWN; int n = 0;
    for (int i = 0; i < 3; i++) if (s_family[i] != LANG_UNKNOWN) { only = s_family[i]; n++; }
    return n == 1 ? only : LANG_UNKNOWN;
}

static bool family_allows(lang_t l)
{
    bool any = false;
    for (int i = 0; i < 3; i++) { if (s_family[i] != LANG_UNKNOWN) any = true; if (s_family[i] == l) return true; }
    return !any;   // no family setting → everything allowed
}

lang_t mind_reply_lang(lang_t asr_lang, const char *transcript)
{
    lang_t l = asr_lang;
    if (l == LANG_UNKNOWN && transcript) {
        bool cjk = false, latin = false;
        for (const unsigned char *p = (const unsigned char *)transcript; *p; p++) {
            if (*p >= 0xE4 && *p <= 0xE9) cjk = true;
            else if ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'z') latin = true;
        }
        l = cjk ? LANG_ZH : latin ? LANG_EN : LANG_UNKNOWN;
    }
    if (l == LANG_UNKNOWN || !family_allows(l)) l = s_last;
    s_last = l;
    return l;
}

char *mind_normalize_transcript(char *text)
{
    static const char *const pairs[][2] = { { "姑姑", "咕咕" }, { "咕咕鸟", "咕咕" }, { "Gugu", "咕咕" }, { "goo goo", "Gugu" }, { NULL, NULL } };
    for (int i = 0; pairs[i][0]; i++) {
        size_t fl = strlen(pairs[i][0]), rl = strlen(pairs[i][1]);
        if (rl > fl) continue;                      // in-place only when the fix is not longer
        char *p = text;
        while ((p = strstr(p, pairs[i][0])) != NULL) {
            memcpy(p, pairs[i][1], rl);
            if (rl < fl) memmove(p + rl, p + fl, strlen(p + fl) + 1);
            p += rl;
        }
    }
    return text;
}
