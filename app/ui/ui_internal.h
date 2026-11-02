// ui_internal.h — private contract between ui.c (LVGL objects, pre-scaled images, the display) and
// ui_anim.c (timers that decide which sprite and which overlays are shown). Not for other modules.
#pragma once
#include "ui/ui.h"
#include "ui_sprites.h"
#include "lvgl.h"

#define GUGU_BASE_X (34 * UI_SPRITE_SCALE + 1)   // the pigeon's home position in screen pixels
#define GUGU_BASE_Y (46 * UI_SPRITE_SCALE + 1)

extern lv_obj_t *ui_gugu_img;                    // the pigeon: a pre-scaled base sprite; overlays are its children

void ui_anim_init(lv_obj_t *scr);                // creates the ring and timers, shows the first sprite
void ui_anim_emotion_changed(ui_emotion_t e);

// Services ui.c offers the animation layer. Call with the UI lock held or from an LVGL timer.
void ui_show_base(const lv_image_dsc_t *base);  // switch the pigeon's base sprite (scaled once into PSRAM)
void ui_show_beak(const ui_patch_t *p);         // beak-open overlay; NULL hides it
void ui_show_eyes(const ui_patch_t *p);         // eyes-closed overlay; NULL hides it
