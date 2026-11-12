-- Fahrstunde 3: Abbiegen mit Blinker
return {
  title = "3 · Abbiegen und Blinken",
  steps = {
    { text = "Losfahren. Vor jedem Abbiegen: Spiegel, Blinker (Q links / E rechts), Tempo verringern, dann lenken.",
      done = function() return car.speed_kmh > 10 end },
    { text = function(c) return string.format("Biege zweimal RECHTS ab - mit Blinker. (%d/2)", c.turns_right) end,
      done = function(c) return c.turns_right >= 2 end },
    { text = function(c) return string.format("Biege zweimal LINKS ab - mit Blinker, Gegenverkehr und Fahrbahnmitte beachten. (%d/2)", c.turns_left) end,
      done = function(c) return c.turns_left >= 2 end },
    { text = "Rechts blinken und am Fahrbahnrand anhalten.",
      done = function() return car.speed_kmh < 0.5 and car.lane_offset > 0 and (car.blinker == 1 or car.handbrake) end },
  },
}
