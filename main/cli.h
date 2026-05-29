#pragma once

// UART0-on (a "COMM" porton, ahol a logot is látod) fogadja a parancsokat.
// Formátum:  ##cmd##                          (payload nélküli parancs)
//            ##cmd$$payload##                 (payload-os parancs)
//
// Parancsok:
//   ##play##            — lejátszás indítása / szünetelt folytatása
//   ##pause##           — szüneteltetés
//   ##stop##            — leállítás (fájl bezárás, pozíció elvész)
//   ##next##            — következő szám / böngészőben: belépés mappába
//   ##prev##            — előző szám / böngészőben: szülő mappa
//   ##menu##            — képernyő-váltás (Now / Library / Settings)
//   ##vol$$up##         — hangerő +2%
//   ##vol$$down##       — hangerő −2%
//   ##vol$$max##        — hangerő 100%
//   ##vol$$off##        — hangerő 0%
//   ##vol$$<0-100>##    — konkrét százalék (pl. ##vol$$50##)
void cli_init(void);
