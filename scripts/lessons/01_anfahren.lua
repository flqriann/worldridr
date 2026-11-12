-- Fahrstunde 1: Anfahren und Anhalten
local function manual() return not car.automatic and not car.auto_clutch end

return {
  title = "1 · Anfahren und Anhalten",
  steps = {
    { text = "Anschnallen: Taste G (Lenkrad: rechter Stick-Knopf).",
      done = function() return car.belt end },
    { text = function()
        if manual() then return "Kupplung ganz durchtreten und halten. Dann den Motor starten: I gedrückt halten (Lenkrad: Y)." end
        return "Motor starten: I gedrückt halten (Lenkrad: Y)."
      end,
      done = function() return car.engine_on end },
    { text = function()
        if car.automatic then return "Bremse treten und mit Shift (rechte Schaltwippe) auf D schalten." end
        if manual() then return "Kupplung getreten lassen und den 1. Gang einlegen: Shift oder rechte Schaltwippe." end
        return "1. Gang einlegen: Shift (die Kupplung übernimmt mit Tastatur das Spiel)."
      end,
      done = function() return (car.automatic and car.selector == 3) or (not car.automatic and car.gear == 1) end },
    { text = "Handbremse lösen: Leertaste (Lenkrad: A).",
      done = function() return not car.handbrake end },
    { text = function()
        if manual() then return "Etwas Gas geben (ca. 1500 U/min) und die Kupplung LANGSAM kommen lassen, bis das Auto rollt. Zu schnell = abgewürgt." end
        return "Sanft Gas geben und auf 15 km/h beschleunigen."
      end,
      done = function() return car.speed_kmh > 14 end },
    { text = "Geradeaus weiterfahren - 40 Meter.",
      done = function(c) return car.odo - c.odo > 40 end },
    { text = function()
        if manual() then return "Anhalten: Gas weg, bremsen und kurz vor dem Stillstand die Kupplung treten." end
        return "Anhalten: Gas weg und sanft bis zum Stillstand bremsen."
      end,
      done = function() return car.speed_kmh < 0.5 and car.engine_on end },
    { text = function()
        if car.automatic then return "Auf P schalten (Strg / linke Wippe) und die Handbremse anziehen." end
        return "Leerlauf einlegen (Strg / linke Wippe bis N) und die Handbremse anziehen."
      end,
      done = function() return car.handbrake and ((car.automatic and car.selector == 0) or (not car.automatic and car.gear == 0)) end },
  },
}
