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
| Minimap-Zoom | N | |
| Fahrstunden-Menü | F1, dann Ziffer | |
| Lenkrad-Diagnose, VR neu zentrieren, Screenshot | F3, F5, P | |

Mit Tastatur kuppelt das Spiel automatisch. Sobald ein Lenkrad erkannt wird, gilt die echte Kupplung:
zu schnell kommen lassen = Motor abgewürgt. Einstellungen (Automatik, Achsen, Force Feedback) in `config.lua`.

**Esc** öffnet das Menü (Fahrstunden, Ort wechseln, Studio, Mehrspieler, Getriebe, Kamera).
Im Cockpit stehen Tempo, Drehzahl, Gang, Blinker und Tempolimit im 3D-Armaturenbrett, nicht in der Oberfläche.

## Weltkarte – Taste M oder Menü

Satellitenkarte mit Ortsnamen: ziehen zum Verschieben, Mausrad zum Zoomen, „Suchen“ springt zu einem Ortsnamen.
Ein Klick setzt das Ziel, Doppelklick, Enter oder „Hierhin teleportieren“ setzt das Auto dort auf die nächste Straße.


## Navigation und Minimap

Unten rechts zeigt eine **Minimap** (Fahrtrichtung oben, Straßen nach Klasse eingefärbt, Gebäudeumrisse). **N** schaltet den
Zoom um (150 m, 300 m, 700 m, aus). In der Weltkarte (**M**) einen Ort anklicken und **Navigation** wählen (oder nach einem Ort
suchen): die Route wird über OSRM (OpenStreetMap-Routing) berechnet und als blaues Band auf der Straße, in der Minimap und in
der Weltkarte angezeigt. Oben erscheinen Abbiegehinweis mit Entfernung, Restweg und Ankunftszeit; verlässt du die Route, wird
automatisch neu berechnet. Beenden: Esc-Menü → „Navigation beenden“. Ohne Internet gibt es nur eine Luftlinie zum Ziel.
Zum Testen: `run.bat --dest 48.2085 16.3721`.

## Autos als .obj

Alle `.obj`-Modelle in `assets/cars` (mit `.mtl` und Farbtextur) werden geladen: Der Spieler fährt das in `config.lua`
(`car_model = "sedan"`) gewählte Auto, der Verkehr und die Mitspieler nutzen alle vorhandenen Modelle in wechselnden Farben.
Im Esc-Menü schaltet „Auto: …“ durch (letzter Eintrag „Eigenbau“ = das selbst erzeugte Modell).
Ein Modell wird automatisch ausgerichtet und skaliert. Räder müssen eigene Gruppen heißen, z. B. `wheel-front-left`,
`wheel-front-right`, `wheel-back-left`, `wheel-back-right` (dann drehen und lenken sie); die häufigste bunte Fläche gilt als
Lack und wird für Verkehrsautos umgefärbt. Mitgeliefert sind sechs Autos aus dem **Car Kit von Kenney** (CC0, siehe
`assets/cars/LICENSE-Kenney-CC0.txt`); weitere findest du auf kenney.nl oder opengameart.org. Im Cockpit bleibt das
eigene Armaturenbrett, außen siehst du das `.obj`-Auto.

## Gebäude und Straßen

Gebäude entstehen aus dem echten Grundriss mit Höhe (`height`, `building:levels`), Farben (`building:colour`, Material) und
Dachform (`roof:shape`: Sattel-, Walm-, Zelt-, Pult- und Kuppeldach). Fehlen Tags, werden Dachform und -höhe nach Gebäudeart und
Grundriss geschätzt; die Dachfarbe wird aus dem Luftbild abgelesen. Straßen sind geglättet (gerundete Kurven, Kreuzungen
bleiben exakt), haben Gehsteige, weiche Markierungen und folgen einem glatt interpolierten Höhenmodell.

## Hauptmenü, Einstellungen und Leistung

Das Spiel startet mit einem Hauptmenü (Spielen, Fahrstunden, Weltkarte, Mehrspieler, Einstellungen). **Esc** öffnet es jederzeit als Pause.
Die Oberfläche ist absichtlich ruhig: im Cockpit zeigt nur das Armaturenbrett etwas, außen ein kleines Tempofeld und ein kleines
Tempolimit. Fehlerpunkte erscheinen nur in Fahrstunden oder wenn „Fehlerpunkte“ in den Einstellungen eingeschaltet ist.

Für schwache Rechner: **Einstellungen → Grafik: Niedrig** (oder `quality = 0` in `config.lua`). Das senkt die Renderauflösung (70 %),
schaltet Spiegel und Detailrauschen ab, verkürzt die Sichtweite, reduziert Verkehr und Gelände und blendet alles Unsichtbare aus.
Einzeln einstellbar sind Auflösung, Spiegel, Sichtweite, Verkehr und Kantenglättung (diese gilt erst nach einem Neustart).
Alles wird in `settings.cfg` gespeichert.

## Polizei

Wer an roten Ampeln oder Stoppschildern vorbeifährt, einen Unfall baut oder deutlich zu schnell fährt, fällt der Polizei auf.
Eine Streife in der Nähe (oder ein neu losgeschicktes Fahrzeug) schaltet Blaulicht und Sirene ein und ordnet an anzuhalten.
Hältst du an, gibt es eine kurze Verkehrskontrolle. Fährst du weiter, beginnt die Verfolgung; sie endet, wenn du anhältst oder
die Polizei abhängst. In Fahrstunden und bei eingeschalteten Fehlerpunkten zählt eine ignorierte Anweisung als Fehler.
Abschalten: `police = false` in `config.lua` oder im Menü.

## Server und Mehrspieler

- **Spiel hosten** im Menü (Mehrspieler) oder `run.bat --host`: ein Spieler hostet, die anderen treten bei.
- **Eigenständiger Server** ohne Fenster: `server.bat --name "Mein Server" --port 27015 --max 8` (verteilt Positionen und Chat,
  braucht keine Grafik und kein Internet für Karten). Den UDP-Port freigeben.
- Im Menü findet **LAN-Server suchen** Spiele im lokalen Netz, beigetretene Adressen werden in `servers.txt` gemerkt.
- **Eingabetaste** öffnet den Chat, Beitritte und Abgänge meldet der Server. Namensschilder erscheinen nur in der Nähe.

## Startort, Fußgänger, Spiegel, Kartenfragen

- **Startort:** Standard (`start = "auto"` in `config.lua`) ist dein ungefährer Standort per IP-Adresse (Stadtgenauigkeit). Dafür wird
  beim Start der Dienst ipwho.is gefragt, das Ergebnis liegt 7 Tage in `cache/ip_location.json`. Wer das nicht möchte, trägt einen
  Ortsnamen oder Koordinaten ein. Das Auto startet auf einer befahrbaren Straße, nicht in Durchgängen oder vor Wänden.
- **Fußgänger** laufen auf den Gehsteigen und überqueren an Kreuzungen die Fahrbahn; KI-Autos halten dafür an. Wer einen Fußgänger
  anfährt, bekommt eine Meldung, einen Fehler (10 Punkte, falls Fehlerpunkte aktiv) und die Polizei wird aufmerksam.
  Anzahl: Menü → Einstellungen → Fußgänger.
- **Spiegel:** Die Seitenspiegel zeigen jetzt auch dein eigenes Auto (Flanke und Heck), wie in echt.
- **Polizei sichtbar:** Eine Verfolgerin startet hinter dir; oben steht ihr Abstand („180 m hinter dir“), auf der Minimap blinkt sie
  blau/rot (am Rand als Pfeil), im Spiegel siehst du das Blaulicht.
- **Kartenfragen:** Wechselt auf einer Straße die Spurzahl (z. B. von 2 auf 3), ist unklar, welche Angabe stimmt. Wenn du stehst,
  fragt das Spiel einmal nach (höchstens alle 25 Sekunden). Deine Antwort gilt dauerhaft für diese Straßenabschnitte und steht in
  `maps/map_overrides.json` (Datei löschen = Fragen wieder zulassen). „So lassen“ wird ebenfalls gespeichert.

## Autokatalog und eigene Modelle

Menü → **Autokatalog**: Liste aller Autos in `assets/cars` (plus „Eigenbau“), 3D-Vorschau zum Drehen mit der Maus, 15 Lackfarben
oder die Originalfarbe. Auswahl und Farbe werden gespeichert (`settings.cfg`). Eigene `.obj`-Autos einfach nach `assets/cars` legen,
auch ohne `.mtl` (Farben werden dann aus den Materialnamen abgeleitet, z. B. `crimson_paint`, `dark_grey`, `tire_black`). Räder dürfen aus
mehreren Teilen bestehen (`tire_FL`, `rim_FL`, `spoke0_FL` …), Leuchten (`ruby_emissive`, `amber_emissive`, `headlight_lens`) reagieren
auf Bremse, Blinker, Licht und Rückwärtsgang. Das Fußgängermodell liegt in `assets/pedestrians/pedestrian_walk.obj`.

## Tank, Tankstellen, Supermärkte, Moped und Motorrad

- **Tank und Geld:** Der Tank leert sich mit der Fahrt (Anzeige: Balken im Display bzw. unter dem Tempo, rot bei wenig Sprit); bei leerem Tank
  geht der Motor aus. **K** navigiert zur nächsten Tankstelle. Tankstand und Geld werden gespeichert.
- **Tankstellen** (aus OpenStreetMap, mit Dach, Zapfsäulen und Markenfarbe): anhalten, Motor aus (I), **J** zum Tanken, bezahlt wird pro Liter.
- **Supermärkte** (Werbesäule in Markenfarbe): anhalten und **J** nimmt einen **Lieferauftrag** an. Das Ziel liegt 1,2–2,8 km entfernt, die
  Navigation startet automatisch. Bezahlt wird nach Strecke, Pünktlichkeit und Fahrstil (Unfälle ziehen ab); davon bezahlst du den Sprit.
- **Moped und Motorrad:** im Autokatalog „Derbi Senda (Moped)“ (50 ccm, 6 Gänge, ca. 50 km/h) und „Motorrad 125“ (ca. 100 km/h). Eigenes
  Modell mit Fahrer, neigt sich in Kurven; im Cockpit sitzt du hinter dem Lenker. Auf der Straße fahren auch KI-Mopeds und -Motorräder.
- **Kreisverkehre** sind ein eigenes Modell: glatter Ring, Mittelinsel mit Bordstein (für Fahrzeuge ein Hindernis), Fußgänger laufen nur außen.
- Entwicklungsbau: Läuft das Spiel noch, ist `worldridr.exe` gesperrt. Mit `set WR_SUFFIX=_dev` vor `build.bat` entsteht `build\worldridr_dev.exe`.

## Zu Fuß, Auto sperren, Tastenbelegung, Motor

- **Aussteigen/Einsteigen: O** (Auto muss stehen). Zu Fuß: WASD (relativ zur Kamera), Shift rennt, rechte Maustaste dreht die Kamera. **Y** sperrt
  das Auto zu bzw. auf (Warnblinker-Quittung, funktioniert bis 30 m); ein abgesperrtes Auto lässt sich nicht betreten.
- **Angefahren werden:** Fahrzeuge (und du als Fußgänger) werden bei einem Aufprall zu Ragdolls mit Verlet-Physik: sie fliegen, überschlagen sich und
  rutschen aus. Als Fußgänger wird dir danach schwarz, du stehst an der Stelle wieder auf.
- **Tastenbelegung:** Einstellungen → „Tasten belegen“: Aktion anklicken, neue Taste drücken, „Alle auf Standard“ setzt zurück.
- **Motor:** Einstellungen → Motor Benzin/Diesel. Der Diesel hat viel Drehmoment im Leerlauf und rollt nur mit der Kupplung los; der Benziner hat einen
  Leerlaufregler mit begrenzter Reserve und würgt bei zu schnellem Einkuppeln verzögert ab. Die Kupplung hat einen Schleifpunkt.
- **Polizei:** Sie reagiert nur auf Verstöße, die eine Streife in der Nähe (≤ 55–80 m, freie Sicht) wirklich sehen konnte. Dann fährt sie unauffällig
  hinterher (kein Blaulicht), überholt irgendwann mit Blaulicht und fährt rechts ran, um dich zum Anhalten zu bringen.
- **Insassen:** Fahrer (und teils Beifahrer) sitzen in den Autos und wackeln mit Beschleunigung und Kurven; dein Fahrer dreht den Kopf mit der
  Kamera bzw. dem VR-Headset. **Bäume, Laternen, Kreuzungsflächen und Hangschummerung** beleben Straßen und Gelände.
## Verkehr

KI-Fahrzeuge fahren auf dem echten Straßennetz und auf selbst gebauten Straßen. Sie halten Abstand, halten an
roten Ampeln und Stoppschildern, blinken beim Abbiegen und beachten den Vorrang (höherrangige Straße, sonst
rechts vor links). Nimmst du einem Fahrzeug den Vorrang, gibt es Fehlerpunkte; ein Zusammenstoß zählt als Unfall.
Anzahl in `config.lua` (`traffic = 22`, 0 = aus) oder im Menü ein-/ausschalten.

## Studio (Editor) – F2 oder Menü

Aufbau wie in Roblox Studio / Blender: links **Einfügen** und **Explorer** (Objektliste), rechts **Eigenschaften**,
oben Speichern, Rückgängig/Wiederholen, Werkzeuge, Raster und der Schalter **Bearbeiten (Tab)**.

| Aktion | Bedienung |
| --- | --- |
| Kamera | rechte Maustaste halten + W A S D, Q/E (Shift schnell); **mittlere Taste** kreist um die Auswahl, Shift+mittlere Taste verschiebt; Mausrad zoomt zum Mauszeiger; **F** springt auf die Auswahl |
| Einfügen | Block, Zylinder, Keil, Kugel, **Treppe, Dach, Pyramide, Kegel, Platte, L-Winkel** (frei bearbeitbare Meshes) und eigene `.obj`-Modelle aus `assets/models` |
| Auswählen / verschieben | anklicken (auch in der Explorer-Liste), am Objekt ziehen oder an den farbigen Pfeilen ziehen |
| Werkzeuge | 1 Bewegen, 2 Drehen, 3 Skalieren, 4 Straße |
| Raster | Knopf „Raster“ schaltet 0,25 / 0,5 / 1 / 2 m / aus; Strg beim Ziehen kehrt es um. Drehen rastet in 15°-Schritten ein |
| Eigenschaften | Position X/Höhe/Z, Drehung, Größe mit −/+ (Schrittweite = Raster), „Fest“, Duplizieren, Löschen, Textur durchschalten |
| Farben | RGB-Regler, 12 Grundfarben, **„+ Merken“** legt eigene Farben an (werden in der Karte gespeichert) |

### Mesh bearbeiten (Tab)

Mit **Tab** (oder „Bearbeiten“) wird das gewählte Objekt zum Mesh; Block, Zylinder, Keil und Kugel werden dabei umgewandelt.
Mit **1 / 2 / 3** wählst du Punkte, Kanten oder Flächen.

| Taste | Wirkung |
| --- | --- |
| Klick, Shift+Klick, Rahmen ziehen | auswählen / mehrere / Bereich |
| Pfeil am Auswahlzentrum ziehen | entlang einer Achse verschieben |
| **G / R / S** | verschieben, drehen, skalieren (Blender-Stil); **X / Y / Z** sperrt die Achse, **N** = Flächennormale, Zahl tippen = exakter Wert, Klick/Enter bestätigt, Esc/Rechtsklick bricht ab |
| **E** | ausgewählte Flächen extrudieren (danach direkt entlang der Normale ziehen) |
| **U** | Fläche teilen, **X / Entf** löschen, **F** Fläche aus gewählten Punkten, **V** Flächen umdrehen, **A** alles / nichts |
| Farbe | gilt für die gewählten Flächen (ohne Auswahl für das ganze Objekt) – jede Fläche hat ihre eigene Farbe |

Straßen: Werkzeug 4, Typ wählen, Punkte klicken, Enter oder Rechtsklick beendet; Punkt nahe an einen vorhandenen Punkt klicken
rastet ein (Kreuzung/Verlängern); Brücke/Rampe über „+ 0,5 m“ oder Bild auf/ab am Straßenpunkt.
Sonstiges: Strg+D duplizieren, Entf löschen, Strg+Z / Strg+Y rückgängig / wiederholen, Strg+S speichern,
„Testfahrt“ setzt das Auto an die Blickposition. Eigene Texturen (`.png`/`.jpg`) nach `assets/textures`.

Gespeichert wird in `maps/welt.json` mit Geokoordinaten (inklusive Mesh-Flächen und Farben); die Karte wird beim Start geladen.
Gebaute Straßen haben Tempolimit, Spuren und Gehsteige und zählen für die Regelprüfung wie echte Straßen.
Feste Objekte sind Hindernis und Untergrund: Rampen, Podeste und **Treppen** (Mesh) kann man befahren bzw. zu Fuß hochgehen.

## Kollisionen, Wasser, Straßenhöhe

- **Kollisionen:** Spieler, KI-Autos und Mitspieler sind Rechtecke mit Masse, Drehimpuls und Reibung. Ein Treffer am Heck oder in
  die Flanke dreht das andere Auto (PIT-Manöver gehen); getroffene KI-Autos fahren als freie Körper weiter, schleudern,
  stoßen weitere Autos an und reihen sich danach wieder in den Verkehr ein.
- **Wasser:** Seen, Flüsse, Kanäle und Bäche kommen aus OpenStreetMap und werden mit Himmelsspiegelung gezeichnet.
- **Straßenhöhe (Studio, Taste 5):** Straße anklicken und „Boden / Unterführung“ oder ±1 m wählen. Für Unterführungen, die das Höhenmodell
  als Brücke zeigt. Gespeichert in `maps/map_overrides.json`.

## Bäume und Wälder

Straßenbäume und Wälder nutzen die Modelle `assets/trees/tree.glb` (Laubbaum) und `assets/trees/pine.glb` (Kiefer); eigene `.glb`
mit Materialfarben lassen sich einfach austauschen. Wald entsteht dort, wo das Satellitenbild dunkel und grün ist (nicht auf
Straßen und Gebäuden). Nah wird das volle Modell gezeichnet, weiter weg eine einfache Version (Instanced Rendering).

## Mehrspieler (Details)

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
