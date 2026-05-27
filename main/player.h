#pragma once

#include "io.h"   // btn_event_t

// Beolvassa az SD-ről a tracket, megnyitja az UI-t,
// regisztrálja a gomb és akku callbackeket,
// elindít egy state-machine taskot ami az audio státuszt
// és az UI frissítését csinálja.
void player_start(void);

// Egy button event-et lekezel: ezt hívja a gomb-callback és a CLI is.
void player_handle_button(btn_event_t evt);
