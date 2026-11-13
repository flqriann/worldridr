-- Regelpruefung wie in der Fahrschule: vergibt Fehlerpunkte ueber sim.fault(text, punkte).
local R = { on_turn = nil }

local timers, cooldown, samples, last_sample_odo, turn_block

local function angle_diff(a, b) return (a - b + 540) % 360 - 180 end

function R.reset()
  timers = { speed = 0, offroad = 0, wrong = 0, oncoming = 0, belt = 0, handbrake = 0, rev = 0 }
  cooldown = {}
  samples = {}
  last_sample_odo = nil
  turn_block = 0
end

local function fault(key, text, points, wait)
  if not sim.faults_on() then return end
  local now = sim.time()
  if cooldown[key] and now < cooldown[key] then return end
  cooldown[key] = now + (wait or 10)
  sim.fault(text, points)
  sim.message(text, 3)
end

-- laeuft eine Bedingung laenger als "limit" Sekunden, gibt es den Fehler
local function timed(key, cond, dt, limit)
  if cond then timers[key] = timers[key] + dt else timers[key] = 0 end
  return timers[key] > limit
end

function R.event(name)
  if name == "stall" then fault("stall", "Motor abgewürgt", 1, 1)
  elseif name == "grind" then fault("grind", "Schalten ohne Kupplung", 1, 2)
  elseif name == "red_light" then fault("red", "Rote Ampel überfahren", 5, 3)
  elseif name == "stop_sign" then fault("stop", "Stoppschild: nicht angehalten", 4, 3)
  elseif name == "priority" then fault("priority", "Vorrang missachtet", 4, 5)
  elseif name == "crash" then fault("crash", "Unfall", 5, 3)
  elseif name == "pedestrian" then fault("ped", "Fußgänger angefahren", 10, 5)
  elseif name == "police_ignored" then fault("police", "Polizeianweisung missachtet", 5, 30)
  end
end

function R.update(dt)
  if not car.speed_kmh then return end
  local v = car.speed_kmh

  if car.speed_limit > 0 and timed("speed", v > car.speed_limit + 6, dt, 2) then
    fault("speed", string.format("Zu schnell: %d bei erlaubten %d km/h", math.floor(v), car.speed_limit), v > car.speed_limit + 25 and 4 or 2, 12)
  end
  if timed("belt", v > 6 and not car.belt, dt, 1) then fault("belt", "Nicht angeschnallt (Taste G)", 2, 60) end
  if timed("handbrake", v > 12 and car.handbrake, dt, 1.5) then fault("handbrake", "Mit angezogener Handbremse gefahren", 1, 30) end
  if timed("offroad", v > 5 and not car.on_road, dt, 1.2) then fault("offroad", "Fahrbahn verlassen", 3, 10) end
  if timed("wrong", car.wrong_way, dt, 2) then fault("wrong", "Einbahn in falscher Richtung", 5, 15) end
  if timed("oncoming", car.two_way_marked and car.on_road and v > 15 and car.lane_offset < -0.7, dt, 3) then
    fault("oncoming", "Auf der Gegenfahrbahn gefahren", 3, 12)
  end
  if timed("rev", car.rpm > 5200 and not car.automatic, dt, 3) then fault("rev", "Zu hohe Drehzahl - früher hochschalten", 1, 20) end

  -- Abbiegen erkennen: Kursaenderung > 55 Grad innerhalb von 30 m. Blinker muss vorher gesetzt gewesen sein.
  local odo = car.odo
  if not last_sample_odo or odo - last_sample_odo >= 2 then
    last_sample_odo = odo
    samples[#samples + 1] = { odo = odo, heading = car.heading, blink = car.blinker }
    while #samples > 0 and odo - samples[1].odo > 45 do table.remove(samples, 1) end
    if odo >= turn_block and v < 60 then
      for i = #samples, 1, -1 do
        local s = samples[i]
        if odo - s.odo > 30 then break end
        local d = angle_diff(car.heading, s.heading)
        if math.abs(d) > 55 then
          local dir = d > 0 and 1 or -1
          local ok = false
          for _, b in ipairs(samples) do if b.blink == dir or b.blink == 2 then ok = true end end
          if car.on_road then
            if not ok then fault("blinker", dir > 0 and "Rechts abgebogen ohne Blinker" or "Links abgebogen ohne Blinker", 2, 1) end
            if R.on_turn then R.on_turn(dir, ok) end
          end
          turn_block = odo + 25
          samples = {}
          break
        end
      end
    end
  end
end

R.reset()
return R
