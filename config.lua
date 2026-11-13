-- worldridr Einstellungen

-- Startort: "auto" = dein ungefaehrer Standort per IP-Adresse (Stadtgenauigkeit; fragt dazu einen Geodienst), ein Ortsname
-- (wird per OpenStreetMap gesucht) oder Koordinaten { lat = ..., lon = ... }.
-- Im Spiel jederzeit mit T einen neuen Ort eingeben.
start = "auto"

-- Optional: eigener Google-Maps-Platform-Schluessel (Map Tiles API aktivieren).
-- Leer = Satellitenbilder von Esri World Imagery (kein Schluessel noetig).
google_maps_api_key = ""

transmission = "manual"   -- "manual" (6 Gaenge + Kupplung) oder "automatic"
-- auto_clutch = false    -- weglassen: automatisch (mit Lenkrad/Pedalen aus, mit Tastatur an)

quality = 1               -- 0 niedrig (schwache Rechner), 1 mittel, 2 hoch. Weitere Feineinstellungen im Spiel: Menue > Einstellungen (settings.cfg)
police = true            -- Polizei-KI: haelt dich an oder verfolgt dich bei Verstoessen
fault_points = false     -- Fehlerpunkte auch im freien Fahren zaehlen (in Fahrstunden immer an)
player_name = "Fahrer"   -- Name im Mehrspieler
port = 27015              -- UDP-Port zum Hosten (im Router/Firewall freigeben fuer Spiele ueber das Internet)

vr = false               -- true oder Start mit --vr: VR ueber OpenXR (Meta Horizon Link)
fullscreen = false
width, height = 1600, 900
fov = 62                 -- vertikaler Blickwinkel am Monitor (Grad)
near_zoom = 18            -- Detailstufe der Satellitenbilder am Auto (17 falls Kacheln fehlen, 19 = schaerfer)
volume = 0.6

-- Lenkrad (Standardwerte passen zum Logitech G920 mit G HUB).
-- F3 im Spiel zeigt die rohen Achsen und Tastennummern, falls etwas nicht stimmt.
wheel = {
  steer_axis = 0, throttle_axis = 1, brake_axis = 2, clutch_axis = 3,
  invert_pedals = true,   -- Pedal losgelassen = +1, getreten = -1
  ffb_strength = 0.8,     -- 0 .. 1.5
  ffb_invert = false,     -- true, falls das Lenkrad vom Zentrum WEG drueckt
  -- H-Schalthebel (Driving Force Shifter): im Spiel ueber Esc > "Schalthebel einrichten" anlernen.
  -- Das Ergebnis steht in shifter.cfg und hat Vorrang vor den folgenden Werten.
  h_shifter = false,
  gear_buttons = { 12, 13, 14, 15, 16, 17 }, reverse_button = 11,
  shift_up = 4, shift_down = 5,          -- Schaltwippen rechts / links
  blinker_left = 2, blinker_right = 1,   -- X / B
  handbrake = 0, ignition = 3,           -- A / Y
  seatbelt = 8, horn = 9, lights = 7, camera = 6, reset = 10,
}
car_model = "sedan"   -- Datei aus assets/cars (ohne .obj): sedan, sedan-sports, hatchback-sports, suv, suv-luxury, delivery, police - oder "builtin"
