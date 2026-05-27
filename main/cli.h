#pragma once

// UART0-on (a "COMM" porton, ahol a logot is látod) fogadja a parancsokat.
// Formátum:  #parancs$param#   — a $param opcionális (#parancs# is jó).
//
// Parancsok:
//   #play#            — lejátszás indítása / szünetelt folytatása
//   #pause#           — szüneteltetés
//   #stop#            — leállítás (fájl bezárás, pozíció elvész)
//   #next#            — következő szám
//   #prev#            — előző szám
//   #menu#            — SD újraolvasás
//   #vol$up#          — hangerő +2%
//   #vol$down#        — hangerő −2%
//   #vol$max#         — hangerő 100%
//   #vol$off#         — hangerő 0%
//   #vol$<0-100>#     — hangerő konkrét százalékra (pl. #vol$50#)
void cli_init(void);
