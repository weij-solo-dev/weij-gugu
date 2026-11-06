// audio_host.c — SDL2 audio: an output device pulling from the playback ring, a capture device
// pushing microphone frames. Both are optional: without devices the simulator still runs replays.
#define LOG_TAG "audio"
#include "audio/audio.h"
#include "audio_internal.h"
#include "foundation/log.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>
const uint32_t audio_out_latency_ms = 40;   // pulled → heard: SDL's buffer, about two frames

static SDL_AudioDeviceID s_out, s_in;

static void out_cb(void *user, Uint8 *stream, int len)
{
    (void)user;
    audio_pull((int16_t *)stream, (size_t)len / 2);
}

static void in_cb(void *user, Uint8 *stream, int len)
{
    (void)user;
    audio_push_capture((const int16_t *)stream, (size_t)len / 2);
}

esp_err_t audio_init(void)
{
    const char *root = getenv("GUGU_CONTENT_DIR");
    char sfx[200]; snprintf(sfx, sizeof sfx, "%s/sfx", root ? root : GUGU_REPO_ROOT "/content");
    esp_err_t err = audio_common_init(sfx);
    if (err != ESP_OK) return err;
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want = { .freq = AUDIO_SAMPLE_RATE, .format = AUDIO_S16SYS, .channels = 1, .samples = AUDIO_FRAME_SAMPLES, .callback = out_cb }, have;
    s_out = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_out) { SDL_PauseAudioDevice(s_out, 0); LOGI("host audio out: %d Hz", have.freq); }
    else LOGW("no audio output device: %s", SDL_GetError());
    return ESP_OK;
}

esp_err_t audio_platform_capture(bool on)
{
    if (getenv("GUGU_NO_MIC")) return ESP_OK;          // replay-only runs never touch the microphone
    if (on) {
        if (!s_in) {
            SDL_AudioSpec want = { .freq = AUDIO_SAMPLE_RATE, .format = AUDIO_S16SYS, .channels = 1, .samples = AUDIO_FRAME_SAMPLES, .callback = in_cb }, have;
            s_in = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
            if (!s_in) { LOGW("no microphone: %s", SDL_GetError()); return ESP_ERR_NOT_FOUND; }
            LOGI("host microphone open: %d Hz", have.freq);
        }
        SDL_PauseAudioDevice(s_in, 0);
    } else if (s_in) {
        SDL_PauseAudioDevice(s_in, 1);
    }
    return ESP_OK;
}
