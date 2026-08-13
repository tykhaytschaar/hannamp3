# Boot splash frame-ek: az assets/boot/frame_NNN.jpg-ket a binárisba ágyazzuk
# (EMBED_FILES). Az embed-sorrend lényegtelen — a szimbólumnevek a basename-ből
# jönnek (_binary_frame_NNN_jpg_start/_end), a lejátszási sorrendet a
# boot_splash.c s_frames[] táblája rögzíti.
file(GLOB BOOT_SPLASH_FRAMES "${CMAKE_CURRENT_SOURCE_DIR}/../assets/boot/frame_*.jpg")

idf_component_register(
    SRCS
        "main.c"
        "sd.c"
        "audio.c"
        "ui.c"
        "io.c"
        "player.c"
        "cli.c"
        "boot_splash.c"
        "cover_art.c"
        "usb_msc.c"
        "game.c"
        "gb.c"
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
        esp_app_format   # esp_app_get_elf_sha256 — save state build-azonosító
        esp_lcd
        esp_adc
        esp_timer
        fatfs
        sdmmc
        nvs_flash
        esp_new_jpeg
        esp_tinyusb
    # Album art placeholder: a Now Playing cover-helye, ha egy trackhez nincs
    # (vagy nem dekódolható) album art. Lásd cover_art.c.
    EMBED_FILES ${BOOT_SPLASH_FRAMES}
                "${CMAKE_CURRENT_SOURCE_DIR}/../assets/cover_placeholder.jpg"
)
