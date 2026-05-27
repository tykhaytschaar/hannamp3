#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    AUDIO_STATE_STOPPED,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED,
    AUDIO_STATE_FINISHED,   // a fájl véget ért magától
} audio_state_t;

typedef struct {
    audio_state_t state;
    uint32_t position_ms;     // becsült lejátszott pozíció
    uint32_t duration_ms;     // becsült teljes hossz (CBR esetén pontos)
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  volume;          // 0..100
} audio_status_t;

// I2S + Helix init, decode task indítása. Csak egyszer hívd.
void audio_init(void);

// Új fájl indítása. Ha már szól valami, leállítja előbb.
void audio_play(const char *path);

void audio_stop(void);
void audio_pause(void);
void audio_resume(void);

void audio_set_volume(uint8_t vol);
uint8_t audio_get_volume(void);

void audio_get_status(audio_status_t *out);
