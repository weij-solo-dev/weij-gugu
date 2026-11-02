/* LVGL configuration for the gugu host simulator. Mirrors the device's sdkconfig LVGL options. */
#ifndef LV_CONF_H
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB
#define LV_USE_OS   LV_OS_NONE
#define LV_DEF_REFR_PERIOD  16
#define LV_DPI_DEF 130
#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16
#define LV_USE_TJPGD 1
#define LV_USE_FS_STDIO 1
#define LV_FS_STDIO_LETTER 'A'
#define LV_FS_STDIO_PATH ""
#define LV_FS_STDIO_CACHE_SIZE 0
#define LV_USE_SNAPSHOT 1
#define LV_USE_DEMO_WIDGETS 0
#define LV_BUILD_EXAMPLES 0
#endif
