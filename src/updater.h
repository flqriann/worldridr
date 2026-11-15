#pragma once
// Fragt GitHub, ob es eine neuere Version von worldridr (Programm, Karten, Assets) gibt, und laesst den Spieler entscheiden.
#include "game.h"

namespace updater {
void start();                  // beim Programmstart: Pruefung im Hintergrund (wiederholt sich alle 10 Minuten)
void draw(Ui& ui, bool allowed); // jeden Frame; zeichnet die Rueckfrage, wenn eine neue Version bereitsteht (allowed = gerade nicht fahren/editieren)
}
