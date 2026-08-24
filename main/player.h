#pragma once

#include "io.h"   // btn_event_t

// Beolvassa az SD-ről a tracket, megnyitja az UI-t,
// regisztrálja a gomb és akku callbackeket,
// elindít egy state-machine taskot ami az audio státuszt
// és az UI frissítését csinálja.
void player_start(void);

// Egy button event-et lekezel: ezt hívja a gomb-callback és a CLI is.
void player_handle_button(btn_event_t evt);

// Háttérvilágítás fényerő (0–100%): alkalmazza (LEDC PWM) és NVS-be menti.
// A Settings-edit és a CLI `bl` parancs is ezt hívja → perzisztens.
void player_set_backlight(uint8_t pct);

// Hangerő (0–100%): alkalmazza (audio + UI) és NVS-be menti. A Settings volume
// slider release-e hívja; boot-kor a player_start olvassa vissza (default 70).
void player_set_volume(uint8_t vol);

// Settings tap-cycle sorok (touch): léptetés/váltás + NVS-mentés.
void player_cycle_idle_timeout(void);   // "Display off" sor
void player_toggle_sleep(void);         // "Sleep" sor

// Azonnali kikapcsolás (Rendszer oldal gombja): DAC mute + deep sleep, ébredés
// az Up gombbal. Nem tér vissza.
void player_power_off(void);
void player_cycle_album_end(void);      // "Album end" sor (Stop/Repeat/Next album)

// Now Playing transport gombok (touch). A 4 akció a fizikai gombok útjára
// képződik le; a STOP új (audio_stop + UI reset). Alvó kijelzőn csak ébreszt.
typedef enum {
    PLAYER_ACTION_PREV,
    PLAYER_ACTION_PLAY_PAUSE,
    PLAYER_ACTION_STOP,
    PLAYER_ACTION_NEXT,
} player_action_t;
void player_do_action(player_action_t a);

// Egy track lejátszása album-indexszel (Now Playing track-lista tap). Ha az
// idx már a játszó track és megy → no-op. Alvó kijelzőn csak ébreszt.
void player_play_index(int idx);

// Library tap: az adott entry aktiválása (mappa → belép; m3u → playlist-load +
// play + Now Playing; fájl → album-load + play + Now Playing). A ".." parent
// sor a player_browser_up()-ot hívja. Mind alvó kijelzőn csak ébreszt.
void player_browser_tap(int idx);
void player_browser_up(void);

// Library "Play all" sor (m3u nélküli mappák): a böngészett mappa fájljainak
// betöltése név-sorrendben + play az elsőtől + Now Playing. Alvón csak ébreszt.
void player_browser_play_all(void);

// Game Boy ROM indítása (a Játékok oldal listája és a Library .gb/.gbc sora
// hívja). A zenét leállítja; a játékból kilépve nem folytatja.
void player_launch_game(const char *path);
