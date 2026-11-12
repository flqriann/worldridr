-- Fahrstunde 4: Pruefungsfahrt - 2 km mit hoechstens 5 Fehlerpunkten
local LIMIT, DIST = 5, 2000

return {
  title = "4 · Prüfungsfahrt",
  steps = {
    { text = "Prüfungsfahrt: 2 km fahren, mindestens zweimal abbiegen, höchstens 5 Fehlerpunkte. Los, sobald du bereit bist.",
      done = function() return car.speed_kmh > 5 end },
    { text = function(c)
        local left = math.max(0, DIST - (car.odo - c.odo))
        return string.format("Noch %d m · Abbiegen mit Blinker %d/2 · Ampeln, Stoppschilder und Tempolimit beachten.", left, c.turns_left + c.turns_right)
      end,
      done = function(c)
        if sim.points() > LIMIT and not c.failed then
          c.failed = true
          sim.message("Prüfung nicht bestanden - zu viele Fehlerpunkte. F1 für einen neuen Versuch.", 10)
        end
        return car.odo - c.odo >= DIST and (c.turns_left + c.turns_right) >= 2
      end },
    { text = "Geschafft - rechts ranfahren, anhalten und Handbremse anziehen.",
      done = function() return car.speed_kmh < 0.5 and car.handbrake end },
  },
}
