-- Fahrstunde 2: Hoch- und Runterschalten
return {
  title = "2 · Schalten",
  steps = {
    { text = "Anschnallen, Motor starten und im 1. Gang anfahren.",
      done = function() return car.belt and car.engine_on and car.speed_kmh > 8 end },
    { text = function()
        if car.automatic then return "Mit Automatik: einfach auf 30 km/h beschleunigen." end
        return "Bei etwa 2500 U/min: Gas weg, Kupplung treten, 2. Gang, Kupplung kommen lassen, wieder Gas."
      end,
      done = function() return (car.automatic and car.speed_kmh > 28) or car.gear == 2 end },
    { text = function()
        if car.automatic then return "Weiter auf 45 km/h beschleunigen. Tempolimit beachten!" end
        return "Weiter beschleunigen und in den 3. Gang schalten. Tempolimit beachten!"
      end,
      done = function() return (car.automatic and car.speed_kmh > 43) or (car.gear == 3 and car.speed_kmh > 30) end },
    { text = "Tempo 150 Meter lang zwischen 35 und 50 km/h halten.",
      enter = function(c) c.hold = car.odo end,
      done = function(c)
        if car.speed_kmh < 35 or car.speed_kmh > 50 then c.hold = car.odo end
        return car.odo - c.hold > 150
      end },
    { text = function()
        if car.automatic then return "Auf unter 20 km/h abbremsen." end
        return "Abbremsen und in den 2. Gang zurückschalten (unter 30 km/h)."
      end,
      done = function() return (car.automatic and car.speed_kmh < 20) or (car.gear == 2 and car.speed_kmh < 32) end },
    { text = "Am rechten Fahrbahnrand anhalten, Leerlauf (bzw. P) und Handbremse.",
      done = function() return car.speed_kmh < 0.5 and car.handbrake and car.lane_offset > 0 end },
  },
}
