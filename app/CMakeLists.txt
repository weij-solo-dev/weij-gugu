# gugu application component. Flat modules under this directory; app.c assembles them.
# Platform backends are picked by file suffix: *_esp.c on the device, *_host.c in the simulator.
file(GLOB_RECURSE GUGU_SRCS "${CMAKE_CURRENT_LIST_DIR}/*.c")   # no CONFIGURE_DEPENDS: IDF evaluates this file in script mode first; run `idf.py reconfigure` after adding files
if(IDF_TARGET STREQUAL "linux")
    list(FILTER GUGU_SRCS EXCLUDE REGEX ".*_esp\\.c$")
    set(GUGU_REQUIRES esp_timer esp_event json)
    set(GUGU_PRIV_REQUIRES lvgl__lvgl)
else()
    list(FILTER GUGU_SRCS EXCLUDE REGEX ".*_host\\.c$")
    set(GUGU_REQUIRES driver esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_i2s esp_lcd esp_timer esp_event
        nvs_flash esp_wifi esp_netif lwip esp_http_client esp_http_server esp-tls json vfs esp_psram esp_partition app_update esp_driver_usb_serial_jtag mbedtls espcoredump esp_app_format)
    set(GUGU_PRIV_REQUIRES lvgl__lvgl espressif__esp_lcd_co5300 espressif__esp_codec_dev joltwallet__littlefs
        espressif__esp_websocket_client espressif__bmi270)
endif()
idf_component_register(
    SRCS ${GUGU_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${GUGU_REQUIRES}
    PRIV_REQUIRES ${GUGU_PRIV_REQUIRES}
)

if(NOT IDF_TARGET STREQUAL "linux")
    # Read-only content image (prompts, presets, place packs, sfx, parent web) → `content` partition.
    littlefs_create_partition_image(content "${CMAKE_CURRENT_LIST_DIR}/../content" FLASH_IN_PROJECT)
endif()
