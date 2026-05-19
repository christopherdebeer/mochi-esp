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
        "battery.cpp"
        "sleep_gesture.cpp"
        "sprite_cache.cpp"
        "voice.cpp"
        "voice/voice_https.c"
        "voice/openai_signaling.c"
        "voice/voice_peer.c"
        "voice/voice_diag.c"
        "voice/peer_log_shim.c"
        "i2c_bus.cpp"
        "../vendor/waveshare-eink/epaper_driver_bsp.cpp"
        "../vendor/waveshare-eink/i2c_bsp.cpp"
        "../vendor/waveshare-eink/ft6336_bsp.cpp"
    INCLUDE_DIRS
        "."
        "voice"
        "../vendor/waveshare-eink"
    EMBED_FILES
        "assets/splash.bin"
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
        espressif__esp_peer
        log
)

# Force-link the esp_log/esp_log_timestamp shim into the final
# image. Without `-u`, the prebuilt `libpeer_default.a` (which
# references these symbols) is processed by ld AFTER libmain.a,
# so a non-forced reference loses to archive search order. The
# shim itself lives in voice/peer_log_shim.c. See
# firmware/components/README § "Path choice" for context.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u esp_log"
)
