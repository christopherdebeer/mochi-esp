# CMake definition for the `main` component.
#
# Add new source files to SRCS as the firmware grows. For larger
# subsystems (display driver, audio, networking) prefer separate
# components in `firmware/components/` rather than piling everything
# into `main`.
#
# The Waveshare e-paper driver lives in `firmware/vendor/waveshare-eink/`
# as a frozen vendor snapshot; we compile its sources directly into
# `main` rather than wrapping it in a component. See
# `vendor/waveshare-eink/README.md` for why and when to revisit.

# Embedded binary assets. splash.mpk — the random-cell boot splash pack
# with text/pet zones (design/20) — is OPTIONAL: it's produced by
# scripts/refresh-splash.sh fetching /devsprite/pack/spash-bundle-a. When
# absent the build still works and the splash falls back to splash.bin.
set(MOCHI_EMBED_FILES
    "assets/splash.bin"
    "assets/scenes_a.mpk"
    "assets/pet_a.mpk"
)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/assets/splash.mpk")
    list(APPEND MOCHI_EMBED_FILES "assets/splash.mpk")
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "font8x8.cpp"
        "epd_ui.cpp"
        "nvs_creds.cpp"
        "wifi_prov.cpp"
        "wifi_sta.cpp"
        "sprite_fetch.cpp"
        "touch.cpp"
        "rtc.cpp"
        "shtc3.cpp"
        "pair_creds.cpp"
        "openai_key.cpp"
        "device_pair.cpp"
        "factory_reset.cpp"
        "compositor.cpp"
        "ui_dialog.cpp"
        "lvgl_port.cpp"
        "dev_menu.cpp"
        "battery.cpp"
        "sleep_gesture.cpp"
        "power.cpp"
        "sprite_cache.cpp"
        "pack_cache.cpp"
        "device_diag.c"
        "ota_update.cpp"
        "ota_channel.c"
        "key_portal.cpp"
        "decay.c"
        "engagement.c"
        "mood.c"
        "thought.cpp"
        "event_log.c"
        "time_sync.c"
        "pet_sync.cpp"
        "scene_pack.c"
        "pet_pack.c"
        "imagine.c"
        "consolidate.c"
        "model_prefs.c"
        "voice.cpp"
        "voice/voice_https.c"
        "voice/openai_signaling.c"
        "voice/voice_peer.c"
        "voice/voice_diag.c"
        "voice/voice_tools.c"
        "voice/voice_mic.c"
        "voice/voice_aec.c"
        "voice/peer_log_shim.c"
        "i2c_bus.cpp"
        "../vendor/waveshare-eink/epaper_driver_bsp.cpp"
        "../vendor/waveshare-eink/i2c_bsp.cpp"
        "../vendor/waveshare-eink/ft6336_bsp.cpp"
        "../vendor/qrcodegen/qrcodegen.c"
    INCLUDE_DIRS
        "."
        "voice"
        "../vendor/waveshare-eink"
        "../vendor/qrcodegen"
    EMBED_FILES
        ${MOCHI_EMBED_FILES}
    REQUIRES
        driver
        esp_timer
        esp_psram
        spi_flash
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_http_client
        esp-tls
        esp_adc
        lwip
        joltwallet__littlefs
        codec_board
        json
        app_update
        esp_https_ota
        espressif__esp_peer
        espressif__esp-sr
        lvgl__lvgl
        mbedtls
        log
        esp_hw_support
        esp_pm
)

# Optional splash pack (design/20): when assets/splash.mpk is present
# (produced by scripts/refresh-splash.sh) the boot splash renders a random
# cell + text/pet zones; otherwise it falls back to splash.bin.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/assets/splash.mpk")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MOCHI_HAVE_SPLASH_MPK=1)
endif()

# Force-link the esp_log/esp_log_timestamp shim into the final
# image. Without `-u`, the prebuilt `libpeer_default.a` (which
# references these symbols) is processed by ld AFTER libmain.a,
# so a non-forced reference loses to archive search order. The
# shim itself lives in voice/peer_log_shim.c. See
# firmware/components/README § "Path choice" for context.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u esp_log"
)
