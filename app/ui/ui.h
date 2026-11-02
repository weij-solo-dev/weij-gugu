// ui.h — LVGL scenes and the expression animation table. The UI owns the LVGL task; other modules
// change what is shown through this API and never touch LVGL objects directly.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef enum {   // order matches ui_emotion_sprites (generated): the 8 emotions + puff + wave
    UI_EMO_NEUTRAL = 0, UI_EMO_HAPPY, UI_EMO_LAUGHING, UI_EMO_SURPRISED, UI_EMO_CURIOUS,
    UI_EMO_SHY, UI_EMO_SAD, UI_EMO_SLEEPY, UI_EMO_PUFF, UI_EMO_WAVE, UI_EMO_COUNT
} ui_emotion_t;

typedef enum { UI_SCENE_HOME, UI_SCENE_AWAY } ui_scene_t;

esp_err_t ui_init(void);
// Serialise access to LVGL from other tasks.
void ui_lock(void);
void ui_unlock(void);

void ui_show_scene(ui_scene_t scene, bool night);
void ui_set_emotion(ui_emotion_t e);
ui_emotion_t ui_get_emotion(void);

// ---- animation layer (ui_anim.c) ----
typedef enum { UI_ACT_NOD, UI_ACT_WAVE, UI_ACT_COO, UI_ACT_WHISTLE, UI_ACT_PUFF } ui_action_t;
void ui_play_action(ui_action_t a);          // short one-shot animation on top of the current emotion
void ui_set_mouth(uint8_t level);            // 0–255 output loudness → beak open/closed while speaking
void ui_set_listening(bool on);              // thin pulsing ring while the key is held
void ui_set_idle(bool on);                   // blink + breathe when nobody is talking to Gugu
