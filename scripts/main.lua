-- Einstieg: laedt Regelpruefung und Fahrstunden. Neue Fahrstunden einfach in scripts/lessons ablegen und unten eintragen.
-- Beim Start bleibt die Oberflaeche leer; Fahrstunden werden im Menue (oder mit F1) gewaehlt.
local rules = dofile("scripts/rules.lua")

local lessons = {
  dofile("scripts/lessons/01_anfahren.lua"),
  dofile("scripts/lessons/02_schalten.lua"),
  dofile("scripts/lessons/03_abbiegen.lua"),
  dofile("scripts/lessons/04_pruefung.lua"),
}

local current, step, ctx = nil, 0, nil
local menu = false

local function show_menu()
  menu = true
  local t = "0 = Freies Fahren"
  for i, l in ipairs(lessons) do t = t .. "\n" .. i .. " = " .. l.title end
  sim.title("Fahrstunde wählen (Zifferntaste)")
  sim.instruction(t)
end

local function enter_step()
  local s = current.steps[step]
  ctx = { odo = car.odo or 0, time = sim.time(), points = sim.points(), turns_left = 0, turns_right = 0, stalls = 0 }
  if s.enter then s.enter(ctx) end
end

local function start(i)
  menu = false
  sim.clear_faults()
  rules.reset()
  if i == 0 then
    current = nil
    sim.lesson(false)
    sim.title("")
    sim.instruction("")
    return
  end
  current, step = lessons[i], 1
  sim.lesson(true)
  sim.title(current.title)
  enter_step()
end

rules.on_turn = function(dir, with_blinker)
  if not ctx then return end
  if with_blinker then
    if dir < 0 then ctx.turns_left = ctx.turns_left + 1 else ctx.turns_right = ctx.turns_right + 1 end
  end
end

function on_event(name, arg)
  if name == "key" then
    if arg == "f1" then show_menu() return end
    local n = tonumber(arg)
    if menu and n and (n == 0 or lessons[n]) then start(n) end
    return
  end
  if name == "lesson" then
    -- aus dem Hauptmenue: Fahrstunde direkt starten (0 = Menue zeigen)
    local n = tonumber(arg)
    if n and n > 0 and lessons[n] then start(n) else show_menu() end
    return
  end
  if name == "spawn" then
    rules.reset()
    if not current and not menu then start(0) end
    return
  end
  if name == "stall" and ctx then ctx.stalls = ctx.stalls + 1 end
  rules.event(name)
end

function on_update(dt)
  rules.update(dt)
  if menu or not current then return end
  local s = current.steps[step]
  local text = type(s.text) == "function" and s.text(ctx) or s.text
  sim.instruction(string.format("Schritt %d/%d: %s", step, #current.steps, text))
  if s.done(ctx) then
    step = step + 1
    if step > #current.steps then
      local p = sim.points()
      sim.message(string.format("%s abgeschlossen - %d Fehlerpunkte", current.title, p), 8)
      current = nil
      sim.lesson(false)
      sim.title("")
      sim.instruction("")
    else
      sim.message("Richtig!", 1.5)
      enter_step()
    end
  end
end
