// audio_song.c — the gift key's show sings (2026-11-06, the owner: "如果连续多次按压，gugu 将变成歌手表演一段歌曲").
// A small synthesiser plays a song written as notes in content/songs/<name>.json (public-domain melodies: the
// melody, a chord per bar or half bar, the meter, a style) or one it makes up ("improvise": a pentatonic tune in AABA
// over a four-chord loop). Gugu's "coo" sings the melody (a soft tone that scoops up into each note, a little vibrato)
// over a plucked bass, a quiet arpeggio and, for the lively styles, drums; 16 kHz mono, rendered as the speaker
// drains so a song never fills the playback ring. on_beat fires on the song's task when each beat is heard (the
// output's latency allowed for), so the stage's lights and the dance land on the music.
#define LOG_TAG "audio"
#include "audio_internal.h"
#include "foundation/log.h"
#include "foundation/clock.h"
#include "foundation/mem.h"
#include "foundation/task.h"
#include "foundation/atomic_file.h"
#include "cJSON.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define SR AUDIO_SAMPLE_RATE
#define MAX_NOTES 200
#define MAX_CHORDS 64
#define AHEAD_MS 1200                 // rendered this far ahead of the speaker

typedef enum { STYLE_POP, STYLE_WALTZ, STYLE_LULLABY } style_t;
typedef struct { float beat, len, freq; } note_t;                      // freq 0: a rest
typedef struct { float beat, len; int root; bool minor, seventh; } chord_t;   // root: semitones above C
typedef struct {
    char title[48];
    float tempo, pickup, beats;       // beats per minute; beats before the first bar; the song's length in beats
    int meter;                        // beats per bar
    style_t style;
    note_t mel[MAX_NOTES]; int n;
    chord_t ch[MAX_CHORDS]; int nch;
} song_t;

static song_t *s_song;                // PSRAM, one at a time
static atomic_bool s_playing, s_stop;
static audio_beat_cb s_beat_cb; static void *s_beat_user;
static char s_dir[128] = "content/songs";
static char s_title[48];

void audio_song_set_dir(const char *dir) { snprintf(s_dir, sizeof s_dir, "%s", dir); }

// ---- the score ----
static int semitone(char letter) { static const int S[7] = { 9, 11, 0, 2, 4, 5, 7 }; return letter >= 'A' && letter <= 'G' ? S[letter - 'A'] : -1; }
static float note_freq(const char *t, const char **end)   // "C4", "F#5", "Bb3"; 0 for "r"
{
    const char *p = t;
    if (*p == 'r' || *p == 'R') { *end = p + 1; return 0; }
    int s = semitone((char)toupper((unsigned char)*p++));
    if (s < 0) { *end = t; return -1; }
    if (*p == '#') { s++; p++; } else if (*p == 'b') { s--; p++; }
    int octave = isdigit((unsigned char)*p) ? *p++ - '0' : 4;
    *end = p;
    int midi = 12 * (octave + 1) + s;
    return 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
}
static bool parse_melody(song_t *g, const char *m)
{
    float beat = 0;
    for (const char *p = m; *p;) {
        while (*p == ' ' || *p == '|' || *p == '\n') p++;
        if (!*p) break;
        const char *e; float f = note_freq(p, &e);
        if (f < 0) return false;
        float len = 1;
        if (*e == ':') { len = strtof(e + 1, (char **)&e); if (len <= 0) return false; }
        if (g->n < MAX_NOTES) g->mel[g->n++] = (note_t){ beat, len, f };
        beat += len; p = e;
    }
    g->beats = beat;
    return g->n > 0;
}
static bool parse_chords(song_t *g, const char *c)   // "C | F C | G7 C": one or two chords a bar
{
    float bar = g->pickup;
    for (const char *p = c; *p && g->nch < MAX_CHORDS;) {
        while (*p == ' ') p++;
        const char *e = p; while (*e && *e != '|') e++;
        char tok[2][8]; int k = 0;
        for (const char *q = p; q < e && k < 2;) {
            while (q < e && *q == ' ') q++;
            if (q >= e) break;
            int n = 0; while (q < e && *q != ' ' && n < 7) tok[k][n++] = *q++;
            tok[k++][n] = '\0';
        }
        for (int i = 0; i < k && g->nch < MAX_CHORDS; i++) {
            int s = semitone((char)toupper((unsigned char)tok[i][0]));
            if (s < 0) return false;
            const char *r = tok[i] + 1;
            if (*r == '#') { s++; r++; } else if (*r == 'b') { s--; r++; }
            float len = (float)g->meter / (float)k;
            g->ch[g->nch++] = (chord_t){ bar + len * (float)i, len, (s + 12) % 12, r[0] == 'm', strchr(r, '7') != NULL };
        }
        bar += (float)g->meter;
        p = *e ? e + 1 : e;
    }
    return g->nch > 0;
}
static bool load_song(song_t *g, const char *name)
{
    char path[200]; snprintf(path, sizeof path, "%s/%s.json", s_dir, name);
    char *txt = file_read_all(path, NULL);
    if (!txt) return false;
    cJSON *j = cJSON_Parse(txt); mem_release(txt);
    if (!j) return false;
    memset(g, 0, sizeof *g);
    const cJSON *t = cJSON_GetObjectItem(j, "title"), *tempo = cJSON_GetObjectItem(j, "tempo"), *meter = cJSON_GetObjectItem(j, "meter"),
                *pickup = cJSON_GetObjectItem(j, "pickup"), *style = cJSON_GetObjectItem(j, "style"), *mel = cJSON_GetObjectItem(j, "melody"), *ch = cJSON_GetObjectItem(j, "chords");
    snprintf(g->title, sizeof g->title, "%s", cJSON_IsString(t) ? t->valuestring : name);
    g->tempo = cJSON_IsNumber(tempo) ? (float)tempo->valuedouble : 110;
    g->meter = cJSON_IsNumber(meter) ? meter->valueint : 4;
    g->pickup = cJSON_IsNumber(pickup) ? (float)pickup->valuedouble : 0;
    g->style = cJSON_IsString(style) && !strcmp(style->valuestring, "lullaby") ? STYLE_LULLABY : g->meter == 3 ? STYLE_WALTZ : STYLE_POP;
    bool ok = cJSON_IsString(mel) && parse_melody(g, mel->valuestring) && cJSON_IsString(ch) && parse_chords(g, ch->valuestring) && g->tempo >= 40 && g->tempo <= 200 && g->meter >= 2 && g->meter <= 6;
    cJSON_Delete(j);
    return ok;
}

// A short song is sung twice (a show runs half a minute or more), the second time a whole step higher, as pop songs do
// (a lullaby stays in its key).
#define TWICE_UNDER_S 24.0f
static void sing_twice(song_t *g)
{
    if (g->beats * 60.0f / g->tempo >= TWICE_UNDER_S) return;
    float lift = g->style == STYLE_LULLABY ? 1.0f : powf(2.0f, 2.0f / 12.0f), off = g->beats;
    int n = g->n, nch = g->nch;
    for (int i = 0; i < n && g->n < MAX_NOTES; i++) { note_t x = g->mel[i]; x.beat += off; x.freq *= lift; g->mel[g->n++] = x; }
    for (int i = 0; i < nch && g->nch < MAX_CHORDS; i++) { chord_t c = g->ch[i]; c.beat += off; if (lift > 1.0f) c.root = (c.root + 2) % 12; g->ch[g->nch++] = c; }
    g->beats *= 2;
}

// ---- making one up: C pentatonic over C G Am F (or C F G C), AABA, eight bars ----
static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void) { uint32_t x = s_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return s_rng = x ? x : 0x2545F491u; }
static void improvise(song_t *g)
{
    static const int SCALE[] = { 0, 2, 4, 7, 9, 12, 14, 16 };          // C D E G A C' D' E'
    static const float RHYTHMS[][5] = { { 1, 1, 1, 1, 0 }, { 1, 1, 2, 0, 0 }, { 0.5f, 0.5f, 1, 2, 0 }, { 2, 1, 1, 0, 0 }, { 1, 0.5f, 0.5f, 2, 0 }, { 0.5f, 0.5f, 0.5f, 0.5f, 2 } };
    static const int LOOPS[][4] = { { 0, 7, 9, 5 }, { 0, 5, 7, 0 }, { 0, 9, 5, 7 } };   // C G Am F · C F G C · C Am F G
    memset(g, 0, sizeof *g);
    snprintf(g->title, sizeof g->title, "improvised");
    g->tempo = 104 + (float)(rnd() % 21); g->meter = 4; g->style = STYLE_POP;
    const int *loop = LOOPS[rnd() % 3];
    for (int bar = 0; bar < 8; bar++) { int r = loop[bar % 4]; g->ch[g->nch++] = (chord_t){ (float)bar * 4, 4, r, r == 9, false }; }
    note_t a[16], b[16]; int na = 0, nb = 0;                              // two-bar phrases A and B
    for (int phrase = 0; phrase < 2; phrase++) {
        note_t *out = phrase ? b : a; int *n = phrase ? &nb : &na;
        int deg = phrase ? 4 + (int)(rnd() % 3) : 2 + (int)(rnd() % 3);
        for (int bar = 0; bar < 2; bar++) {
            const float *rh = RHYTHMS[rnd() % 6]; float beat = (float)bar * 4;
            for (int i = 0; i < 5 && rh[i] > 0; i++) {
                int step = (int)(rnd() % 5) - 2; deg += step; if (deg < 0) deg = 1; if (deg > 7) deg = 6;
                out[(*n)++] = (note_t){ beat, rh[i] * 0.95f, 261.63f * powf(2.0f, (float)SCALE[deg] / 12.0f) };
                beat += rh[i];
            }
        }
    }
    const note_t *form[4] = { a, a, b, a }; const int fn[4] = { na, na, nb, na };
    for (int part = 0; part < 4; part++)
        for (int i = 0; i < fn[part] && g->n < MAX_NOTES; i++) { note_t x = form[part][i]; x.beat += (float)part * 8; g->mel[g->n++] = x; }
    if (g->n) { note_t *last = &g->mel[g->n - 1]; last->freq = 261.63f; last->len = 32 - last->beat; }   // home on C, held to the end
    if (g->n > 1 && fn[0] > 1) { note_t *v = &g->mel[fn[0] * 2 - 1]; v->freq = 392.0f; }               // A' answers A on G
    g->beats = 32;
}

// ---- the voices ----
static float s_sine[1024];
static inline float sine(float phase) { return s_sine[(int)(phase * 1024.0f) & 1023]; }   // phase in turns
static inline float pulse(float phase, float width) { return phase - floorf(phase) < width ? 0.8f : -0.8f; }
static float chord_freq(const chord_t *c, int which, int octave)   // which: 0 root, 1 third, 2 fifth, 3 octave
{
    static const int OFF_MAJ[4] = { 0, 4, 7, 12 }, OFF_MIN[4] = { 0, 3, 7, 12 };
    int midi = 12 * (octave + 1) + c->root + (c->minor ? OFF_MIN : OFF_MAJ)[which & 3];
    if (which == 3 && c->seventh) midi -= 2;
    return 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
}
static uint32_t s_noise = 12345;
static inline float noise(void) { s_noise = s_noise * 1664525u + 1013904223u; return (float)(int32_t)s_noise / 2147483648.0f; }

typedef struct { int note, chord; float ph_lead, ph_bass, ph_arp, last_noise; } voices_t;

static void render(const song_t *g, voices_t *v, int16_t *out, size_t n, uint64_t first)
{
    const float spb = 60.0f / g->tempo;                                  // seconds per beat
    for (size_t i = 0; i < n; i++) {
        float t = (float)(first + i) / SR, beat = t / spb;
        float mix = 0;
        // Gugu's coo on the melody
        while (v->note < g->n && beat >= g->mel[v->note].beat + g->mel[v->note].len) v->note++;
        if (v->note < g->n && beat >= g->mel[v->note].beat && g->mel[v->note].freq > 0) {
            const note_t *m = &g->mel[v->note];
            float in = (beat - m->beat) * spb, left = (m->beat + m->len - beat) * spb;
            float scoop = in < 0.04f ? powf(2.0f, -0.035f * (1.0f - in / 0.04f)) : 1.0f;          // up into the note
            float vib = in > 0.15f ? 1.0f + 0.006f * sine(t * 5.5f) : 1.0f;
            v->ph_lead += m->freq * scoop * vib / SR; v->ph_lead -= floorf(v->ph_lead);
            float env = fminf(1.0f, in / 0.015f) * fminf(1.0f, left / 0.06f) * (in < 0.12f ? 1.0f : 0.85f + 0.15f * expf(-(in - 0.12f) * 3.0f));
            mix += 0.42f * env * (sine(v->ph_lead) + 0.28f * sine(v->ph_lead * 2) + 0.08f * sine(v->ph_lead * 3));
        }
        // the chord of the moment
        while (v->chord + 1 < g->nch && beat >= g->ch[v->chord + 1].beat) v->chord++;
        if (g->nch && beat >= g->pickup && beat < g->beats) {
            const chord_t *c = &g->ch[v->chord];
            float in_beat = beat - floorf(beat), since = in_beat * spb;
            int beat_in_bar = ((int)floorf(beat - g->pickup)) % g->meter;
            bool bass_on = g->style == STYLE_LULLABY ? beat_in_bar == 0 : (g->meter == 3 ? beat_in_bar == 0 : (beat_in_bar % 2) == 0);
            float bass_since = bass_on ? since : since + spb * (float)(g->meter == 3 ? beat_in_bar : beat_in_bar % 2);
            float bf = chord_freq(c, beat_in_bar == 2 && g->meter == 4 ? 2 : 0, 2);
            v->ph_bass += bf / SR; v->ph_bass -= floorf(v->ph_bass);
            mix += (g->style == STYLE_LULLABY ? 0.16f : 0.22f) * expf(-bass_since * 3.0f) * pulse(v->ph_bass, 0.3f);
            float eighth = beat * 2 - floorf(beat * 2), esince = eighth * spb / 2;      // the arpeggio, in eighths
            int step = ((int)floorf(beat * 2)) % 4;
            v->ph_arp += chord_freq(c, step == 3 ? 1 : step, 5) / SR; v->ph_arp -= floorf(v->ph_arp);
            mix += (g->style == STYLE_LULLABY ? 0.10f : 0.07f) * expf(-esince * (g->style == STYLE_LULLABY ? 6.0f : 14.0f)) * (g->style == STYLE_LULLABY ? sine(v->ph_arp) : pulse(v->ph_arp, 0.125f));
            if (g->style != STYLE_LULLABY) {                                              // drums
                bool kick = beat_in_bar == 0 || (g->meter == 4 && beat_in_bar == 2), snare = g->meter == 4 && (beat_in_bar == 1 || beat_in_bar == 3);
                if (kick && since < 0.12f) mix += 0.55f * expf(-since * 28.0f) * sine((150.0f - 600.0f * since) * since);
                if (snare && since < 0.12f) mix += 0.22f * expf(-since * 30.0f) * noise();
                if (esince < 0.03f && !(g->meter == 3 && beat_in_bar == 0)) { float nz = noise(); mix += 0.07f * expf(-esince * 120.0f) * (nz - v->last_noise); v->last_noise = nz; }
            }
        }
        mix = mix / (1.0f + fabsf(mix) * 0.6f);                                           // soft limit
        out[i] = (int16_t)(mix * 21000.0f);
    }
}

// ---- the player ----
static void song_task(void *arg)
{
    (void)arg;
    song_t *g = s_song;
    voices_t v; memset(&v, 0, sizeof v);
    const uint64_t total = (uint64_t)((g->beats * 60.0f / g->tempo + 0.6f) * SR);
    const uint64_t start = audio_written_samples();                     // where the song's first sample lands in the stream
    const uint64_t latency = (uint64_t)audio_out_latency_ms * SR / 1000;
    const float spb = 60.0f / g->tempo;
    int next_beat = 0, beats = (int)ceilf(g->beats);
    int16_t buf[320]; uint64_t done = 0;
    while (!atomic_load(&s_stop)) {
        while (done < total && audio_play_queued_ms() < AHEAD_MS && !atomic_load(&s_stop)) {
            size_t n = total - done < 320 ? (size_t)(total - done) : 320;
            render(g, &v, buf, n, done);
            audio_play_pcm(buf, n);
            done += n;
        }
        uint64_t heard = audio_played_samples();
        while (next_beat < beats && heard >= start + (uint64_t)((float)next_beat * spb * SR) + latency) {
            int b = next_beat++;
            if (s_beat_cb && !atomic_load(&s_stop)) s_beat_cb(b, g->meter, s_beat_user);
        }
        if (done >= total && (next_beat >= beats || heard >= start + total + latency)) break;
        clock_sleep_ms(10);
    }
    LOGI("song over: %s%s", g->title, atomic_load(&s_stop) ? " (stopped)" : "");
    atomic_store(&s_playing, false);
}

esp_err_t audio_song_play(const char *name, audio_beat_cb on_beat, void *user)
{
    if (atomic_load(&s_playing)) return ESP_ERR_INVALID_STATE;
    if (!s_song) s_song = mem_alloc_big(sizeof *s_song);
    if (!s_song) return ESP_ERR_NO_MEM;
    if (!s_sine[256]) for (int i = 0; i < 1024; i++) s_sine[i] = sinf(2.0f * (float)M_PI * (float)i / 1024.0f);
    s_rng ^= (uint32_t)clock_us();
    if (!name || !strcmp(name, "improvise")) improvise(s_song);
    else if (!load_song(s_song, name)) { LOGW("song %s: missing or unreadable", name); return ESP_ERR_NOT_FOUND; }
    sing_twice(s_song);
    snprintf(s_title, sizeof s_title, "%s", s_song->title);
    s_beat_cb = on_beat; s_beat_user = user;
    atomic_store(&s_stop, false); atomic_store(&s_playing, true);
    LOGI("song: %s, %.0f bpm, %d/%d, %.0f beats (%.1f s)", s_song->title, (double)s_song->tempo, s_song->meter, 4, (double)s_song->beats, (double)(s_song->beats * 60.0f / s_song->tempo));
    if (!task_spawn("gugu_audio_song", song_task, NULL, 6144, 4, 1)) { atomic_store(&s_playing, false); return ESP_FAIL; }
    return ESP_OK;
}

void audio_song_stop(void)
{
    if (!atomic_load(&s_playing)) return;
    atomic_store(&s_stop, true);
    for (int i = 0; i < 50 && atomic_load(&s_playing); i++) clock_sleep_ms(10);
    audio_play_flush();
}

bool audio_song_playing(void) { return atomic_load(&s_playing); }
const char *audio_song_title(void) { return s_title; }
