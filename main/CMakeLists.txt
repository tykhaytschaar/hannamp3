idf_component_register(
    SRCS
        "main.c"
        "sd.c"
        "audio.c"
        "ui.c"
        "io.c"
        "player.c"
        "cli.c"
        "fonts/mp3_inter_12.c"
        "fonts/mp3_inter_14.c"
        "fonts/mp3_inter_18.c"
        "fonts/mp3_inter_24.c"
    INCLUDE_DIRS "." "fonts"
    PRIV_INCLUDE_DIRS
        # Helix MP3 internal headers (FrameHeader / SideInfo / IMDCTInfo / ...
        # méretek a track-váltáskori in-place dekóder reset-hez).
        "../managed_components/chmorgan__esp-libhelix-mp3/libhelix-mp3/real"
    REQUIRES
        driver
        esp_lcd
        esp_adc
        esp_timer
        fatfs
        sdmmc
        nvs_flash
)
