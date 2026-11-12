# worldridr

Fahrschul-Simulator auf echten Karten: Satellitenbilder, Höhenmodell und Straßennetz werden während der Fahrt
für jeden Ort der Welt nachgeladen. C++ (Engine, Physik), Lua (Fahrstunden, Regelprüfung), OpenXR (VR), SDL3 (Lenkrad).

## Bauen und starten

Voraussetzung: Visual Studio Build Tools mit C++-Workload (CMake und Ninja sind enthalten).

```bat
build.bat
run.bat
```

Optionen: `run.bat --vr`, `run.bat --place "Graz, Hauptplatz"`, `run.bat --chase`.

## Steuerung

| Aktion | Tastatur | Logitech G920 |
| --- | --- | --- |
| Lenken / Gas / Bremse | A D / W / S (oder Pfeiltasten) | Lenkrad / Pedale |
| Kupplung | X (nur ohne automatische Kupplung) | Kupplungspedal |
| Gang hoch / runter | Shift / Strg | Schaltwippe rechts / links, optional H-Schaltung |
| Motor starten (halten) / abstellen | I | Y |
| Handbremse | Leertaste | A |
| Blinker links / rechts, Warnblinker | Q / E, F | X / B |
| Gurt, Licht, Hupe | G, L, H | RSB, View, LSB |
| Kamera (Cockpit / außen), Umsehen | V, rechte Maustaste + Maus | Menü-Taste, Steuerkreuz |
| Zurück auf die Straße | R | Xbox-Taste |
| Ort wechseln (Name eingeben) | T | |
| Fahrstunden-Menü | F1, dann Ziffer | |
| Lenkrad-Diagnose, VR neu zentrieren, Screenshot | F3, F5, P | |

Mit Tastatur kuppelt das Spiel automatisch. Sobald ein Lenkrad erkannt wird, gilt die echte Kupplung:
zu schnell kommen lassen = Motor abgewürgt. Einstellungen (Automatik, Achsen, Force Feedback) in `config.lua`.

**Esc** öffnet das Menü (Fahrstunden, Ort wechseln, Studio, Mehrspieler, Getriebe, Kamera).
Im Cockpit stehen Tempo, Drehzahl, Gang, Blinker und Tempolimit im 3D-Armaturenbrett, nicht in der Oberfläche.

## Weltkarte – Taste M oder Menü

Satellitenkarte mit Ortsnamen: ziehen zum Verschieben, Mausrad zum Zoomen, „Suchen“ springt zu einem Ortsnamen.
Ein Klick setzt das Ziel, Doppelklick, Enter oder „Hierhin teleportieren“ setzt das Auto dort auf die nächste Straße.

## Verkehr

KI-Fahrzeuge fahren auf dem echten Straßennetz und auf selbst gebauten Straßen. Sie halten Abstand, halten an
roten Ampeln und Stoppschildern, blinken beim Abbiegen und beachten den Vorrang (höherrangige Straße, sonst
rechts vor links). Nimmst du einem Fahrzeug den Vorrang, gibt es Fehlerpunkte; ein Zusammenstoß zählt als Unfall.
Anzahl in `config.lua` (`traffic = 22`, 0 = aus) oder im Menü ein-/ausschalten.

## Studio (Editor) – F2 oder Menü

| Aktion | Bedienung |
| --- | --- |
| Fliegen | rechte Maustaste halten + Maus, W A S D, Q/E runter/hoch, Shift schnell, Mausrad |
| Einfügen | linke Leiste: Block, Zylinder, Keil (Rampe), Kugel, eigene `.obj`-Modelle aus `assets/models` |
| Auswählen / verschieben | anklicken, am Objekt ziehen (folgt dem Boden) oder an den farbigen Achsen ziehen |
| Werkzeuge | 1 Bewegen, 2 Drehen, 3 Skalieren, 4 Straße; Strg beim Ziehen/Drehen rastet ein |
| Eigenschaften | rechte Leiste: Größe, Farbe, Textur, Kachelgröße, „Fest“ (befahrbar/Hindernis) |
| Texturen | eigene `.png`/`.jpg` nach `assets/textures` legen – sie erscheinen in der Liste |
| Straße bauen | Werkzeug 4, Typ wählen, Punkte klicken, Enter oder Rechtsklick beendet. Kurven entstehen automatisch |
| Kreuzung / Verlängern | Punkt nahe an einen bestehenden Punkt klicken (rastet ein) |
| Brücke / Rampe | Straßenpunkt auswählen, „+ 0,5 m“ oder Bild auf/ab |
| Sonstiges | Strg+D duplizieren, Entf löschen, Strg+Z rückgängig, Strg+S speichern, „Testfahrt“ setzt das Auto an die Blickposition |

Gespeichert wird in `maps/welt.json` mit Geokoordinaten; die Karte wird beim Start automatisch geladen.
Gebaute Straßen haben Tempolimit, Spuren und Gehsteige und zählen für die Regelprüfung wie echte Straßen.
Feste Blöcke und Keile ohne Neigung sind befahrbar (Rampen, Podeste), alles Höhere ist ein Hindernis.

## Mehrspieler

Menü → Mehrspieler → „Spiel hosten“ zeigt die eigene Adresse; die anderen wählen „Beitreten“ und tippen sie ein.
Oder per Kommandozeile: `run.bat --host --name Anna` und `run.bat --join 192.168.0.40 --name Ben`.
Im selben Netzwerk geht das direkt. Über das Internet muss der Host den UDP-Port 27015 freigeben
(oder alle nutzen ein VPN wie Tailscale). Die Autos fahren durcheinander hindurch (keine Kollision zwischen Spielern),
und jeder sieht nur seine eigene `maps/welt.json`.

## VR (Meta Quest über Meta Horizon Link)

1. Meta Horizon Link (PC-App) starten, Quest per Link/Air Link verbinden.
2. In der PC-App unter Einstellungen → Allgemein „Meta Horizon Link als aktive OpenXR-Laufzeit festlegen“.
3. `run.bat --vr` starten, Headset aufsetzen, mit F5 die Sitzposition zentrieren.

## Daten

- Satellitenbilder: Esri World Imagery (Standard) oder Google Map Tiles API (`google_maps_api_key` in `config.lua`)
- Straßen, Gebäude, Ampeln, Tempolimits: OpenStreetMap über die Overpass-API
- Höhen: Mapzen/AWS Terrain Tiles
- Alles wird unter `cache/` zwischengespeichert (Google-Kacheln nicht).

## Eigene Fahrstunden

Eine Fahrstunde ist eine Lua-Tabelle mit `title` und `steps` (siehe `scripts/lessons`). Jeder Schritt hat einen `text`
und eine Funktion `done(ctx)`, die den Zustand in der Tabelle `car` prüft (`speed_kmh`, `rpm`, `gear`, `clutch`,
`blinker`, `on_road`, `speed_limit`, `lane_offset`, ...). Neue Datei in `scripts/main.lua` eintragen.
