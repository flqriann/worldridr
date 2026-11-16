// Lua side of the scripting API. Runs once per Lua state before any user
// script. Talks to JS only through the __B_* bridge functions (primitive
// values + small arrays) – see LuaRuntime.ts for the JS half.

export const PRELUDE = String.raw`
local B_get, B_set, B_call, B_new, B_connect, B_kind, B_log, B_enums, B_clock =
  __B_get, __B_set, __B_call, __B_new, __B_connect, __B_kind, __B_log, __B_enums, __B_clock
__B_get, __B_set, __B_call, __B_new, __B_connect, __B_kind, __B_log, __B_enums, __B_clock = nil

local WAIT, EVENTWAIT = {}, {}
local now = 0
local currentScript = nil          -- script id of the running thread
local scriptOf = setmetatable({}, { __mode = "k" })  -- thread -> script id
local resumeStart = 0
local TIMEOUT = 5

-- ===================================================================
-- Datatypes
-- ===================================================================
local function typeerr(what, got) error(what .. " expected, got " .. typeof(got), 3) end

---------------- Vector3
local V3MT, V3 = {}, {}
local function v3(x, y, z) return setmetatable({ X = x or 0, Y = y or 0, Z = z or 0 }, V3MT) end
local function isV3(v) return getmetatable(v) == V3MT end
V3MT.__index = function(v, k)
  if k == "Magnitude" then return math.sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z)
  elseif k == "Unit" then local m = math.sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); if m == 0 then return v3(0, 0, 0) end; return v3(v.X / m, v.Y / m, v.Z / m)
  elseif k == "x" then return v.X elseif k == "y" then return v.Y elseif k == "z" then return v.Z end
  return V3[k]
end
V3MT.__newindex = function(_, k) error(tostring(k) .. " cannot be assigned to", 2) end
V3MT.__add = function(a, b) if not isV3(a) or not isV3(b) then typeerr("Vector3", isV3(a) and b or a) end return v3(a.X + b.X, a.Y + b.Y, a.Z + b.Z) end
V3MT.__sub = function(a, b) if not isV3(a) or not isV3(b) then typeerr("Vector3", isV3(a) and b or a) end return v3(a.X - b.X, a.Y - b.Y, a.Z - b.Z) end
V3MT.__mul = function(a, b)
  if type(a) == "number" then return v3(a * b.X, a * b.Y, a * b.Z) end
  if type(b) == "number" then return v3(a.X * b, a.Y * b, a.Z * b) end
  return v3(a.X * b.X, a.Y * b.Y, a.Z * b.Z)
end
V3MT.__div = function(a, b)
  if type(b) == "number" then return v3(a.X / b, a.Y / b, a.Z / b) end
  if type(a) == "number" then return v3(a / b.X, a / b.Y, a / b.Z) end
  return v3(a.X / b.X, a.Y / b.Y, a.Z / b.Z)
end
V3MT.__unm = function(a) return v3(-a.X, -a.Y, -a.Z) end
V3MT.__eq = function(a, b) return a.X == b.X and a.Y == b.Y and a.Z == b.Z end
local function fmt(n) local s = string.format("%.4f", n):gsub("0+$", ""):gsub("%.$", ""); if s == "-0" then return "0" end return s end
V3MT.__tostring = function(v) return fmt(v.X) .. ", " .. fmt(v.Y) .. ", " .. fmt(v.Z) end
function V3.Dot(a, b) return a.X * b.X + a.Y * b.Y + a.Z * b.Z end
function V3.Cross(a, b) return v3(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X) end
function V3.Lerp(a, b, t) return v3(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t) end
function V3.FuzzyEq(a, b, eps) eps = eps or 1e-5; return math.abs(a.X - b.X) <= eps and math.abs(a.Y - b.Y) <= eps and math.abs(a.Z - b.Z) <= eps end
function V3.Abs(a) return v3(math.abs(a.X), math.abs(a.Y), math.abs(a.Z)) end
function V3.Floor(a) return v3(math.floor(a.X), math.floor(a.Y), math.floor(a.Z)) end
function V3.Max(a, b) return v3(math.max(a.X, b.X), math.max(a.Y, b.Y), math.max(a.Z, b.Z)) end
function V3.Min(a, b) return v3(math.min(a.X, b.X), math.min(a.Y, b.Y), math.min(a.Z, b.Z)) end
Vector3 = {
  new = function(x, y, z) return v3(tonumber(x) or 0, tonumber(y) or 0, tonumber(z) or 0) end,
  zero = v3(0, 0, 0), one = v3(1, 1, 1), xAxis = v3(1, 0, 0), yAxis = v3(0, 1, 0), zAxis = v3(0, 0, 1),
}

---------------- Vector2
local V2MT, V2 = {}, {}
local function v2(x, y) return setmetatable({ X = x or 0, Y = y or 0 }, V2MT) end
V2MT.__index = function(v, k)
  if k == "Magnitude" then return math.sqrt(v.X * v.X + v.Y * v.Y)
  elseif k == "Unit" then local m = math.sqrt(v.X * v.X + v.Y * v.Y); if m == 0 then return v2(0, 0) end return v2(v.X / m, v.Y / m) end
  return V2[k]
end
V2MT.__add = function(a, b) return v2(a.X + b.X, a.Y + b.Y) end
V2MT.__sub = function(a, b) return v2(a.X - b.X, a.Y - b.Y) end
V2MT.__mul = function(a, b) if type(a) == "number" then return v2(a * b.X, a * b.Y) end if type(b) == "number" then return v2(a.X * b, a.Y * b) end return v2(a.X * b.X, a.Y * b.Y) end
V2MT.__eq = function(a, b) return a.X == b.X and a.Y == b.Y end
V2MT.__tostring = function(v) return fmt(v.X) .. ", " .. fmt(v.Y) end
function V2.Dot(a, b) return a.X * b.X + a.Y * b.Y end
function V2.Lerp(a, b, t) return v2(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t) end
Vector2 = { new = function(x, y) return v2(x or 0, y or 0) end, zero = v2(0, 0), one = v2(1, 1) }

---------------- Color3
local C3MT, C3 = {}, {}
local function c3(r, g, b) return setmetatable({ R = r or 0, G = g or 0, B = b or 0 }, C3MT) end
local function isC3(v) return getmetatable(v) == C3MT end
C3MT.__index = C3
C3MT.__eq = function(a, b) return a.R == b.R and a.G == b.G and a.B == b.B end
C3MT.__tostring = function(c) return fmt(c.R) .. ", " .. fmt(c.G) .. ", " .. fmt(c.B) end
function C3.Lerp(a, b, t) return c3(a.R + (b.R - a.R) * t, a.G + (b.G - a.G) * t, a.B + (b.B - a.B) * t) end
function C3.ToHex(c) return string.format("%02x%02x%02x", math.floor(c.R * 255 + 0.5), math.floor(c.G * 255 + 0.5), math.floor(c.B * 255 + 0.5)) end
function C3.ToHSV(c)
  local r, g, b = c.R, c.G, c.B
  local mx, mn = math.max(r, g, b), math.min(r, g, b)
  local h, s, v = 0, 0, mx
  local d = mx - mn
  if mx > 0 then s = d / mx end
  if d > 0 then
    if mx == r then h = (g - b) / d % 6 elseif mx == g then h = (b - r) / d + 2 else h = (r - g) / d + 4 end
    h = h / 6
  end
  return h, s, v
end
local function hsv(h, s, v)
  h = (h % 1) * 6
  local i = math.floor(h); local f = h - i
  local p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
  if i == 0 then return c3(v, t, p) elseif i == 1 then return c3(q, v, p) elseif i == 2 then return c3(p, v, t)
  elseif i == 3 then return c3(p, q, v) elseif i == 4 then return c3(t, p, v) else return c3(v, p, q) end
end
Color3 = {
  new = function(r, g, b) return c3(r or 0, g or 0, b or 0) end,
  fromRGB = function(r, g, b) return c3((r or 0) / 255, (g or 0) / 255, (b or 0) / 255) end,
  fromHSV = hsv,
  fromHex = function(h) h = tostring(h):gsub("#", ""); return c3(tonumber(h:sub(1, 2), 16) / 255, tonumber(h:sub(3, 4), 16) / 255, tonumber(h:sub(5, 6), 16) / 255) end,
}
BrickColor = { new = function(name) error("BrickColor is not supported – use Color3", 2) end }

---------------- UDim / UDim2
local UDMT, U2MT = {}, {}
local function udim(s, o) return setmetatable({ Scale = s or 0, Offset = o or 0 }, UDMT) end
UDMT.__tostring = function(u) return fmt(u.Scale) .. ", " .. fmt(u.Offset) end
UDMT.__add = function(a, b) return udim(a.Scale + b.Scale, a.Offset + b.Offset) end
UDMT.__eq = function(a, b) return a.Scale == b.Scale and a.Offset == b.Offset end
local function udim2(xs, xo, ys, yo) return setmetatable({ X = udim(xs, xo), Y = udim(ys, yo) }, U2MT) end
local function isU2(v) return getmetatable(v) == U2MT end
U2MT.__index = function(u, k)
  if k == "Width" then return u.X elseif k == "Height" then return u.Y end
  if k == "Lerp" then return function(a, b, t) return udim2(a.X.Scale + (b.X.Scale - a.X.Scale) * t, a.X.Offset + (b.X.Offset - a.X.Offset) * t, a.Y.Scale + (b.Y.Scale - a.Y.Scale) * t, a.Y.Offset + (b.Y.Offset - a.Y.Offset) * t) end end
end
U2MT.__add = function(a, b) return udim2(a.X.Scale + b.X.Scale, a.X.Offset + b.X.Offset, a.Y.Scale + b.Y.Scale, a.Y.Offset + b.Y.Offset) end
U2MT.__sub = function(a, b) return udim2(a.X.Scale - b.X.Scale, a.X.Offset - b.X.Offset, a.Y.Scale - b.Y.Scale, a.Y.Offset - b.Y.Offset) end
U2MT.__eq = function(a, b) return a.X == b.X and a.Y == b.Y end
U2MT.__tostring = function(u) return "{" .. tostring(u.X) .. "}, {" .. tostring(u.Y) .. "}" end
UDim = { new = udim }
UDim2 = {
  new = function(xs, xo, ys, yo)
    if getmetatable(xs) == UDMT then return udim2(xs.Scale, xs.Offset, xo.Scale, xo.Offset) end
    return udim2(xs or 0, xo or 0, ys or 0, yo or 0)
  end,
  fromScale = function(x, y) return udim2(x or 0, 0, y or 0, 0) end,
  fromOffset = function(x, y) return udim2(0, x or 0, 0, y or 0) end,
}

---------------- CFrame (position + row-major rotation matrix)
local CFMT, CF = {}, {}
local function cf(x, y, z, r00, r01, r02, r10, r11, r12, r20, r21, r22)
  return setmetatable({ x = x, y = y, z = z, r00 or 1, r01 or 0, r02 or 0, r10 or 0, r11 or 1, r12 or 0, r20 or 0, r21 or 0, r22 or 1 }, CFMT)
end
local function isCF(v) return getmetatable(v) == CFMT end
CFMT.__index = function(c, k)
  if k == "Position" or k == "p" then return v3(c.x, c.y, c.z)
  elseif k == "X" then return c.x elseif k == "Y" then return c.y elseif k == "Z" then return c.z
  elseif k == "LookVector" then return v3(-c[3], -c[6], -c[9])
  elseif k == "RightVector" then return v3(c[1], c[4], c[7])
  elseif k == "UpVector" then return v3(c[2], c[5], c[8])
  elseif k == "Rotation" then return cf(0, 0, 0, c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9]) end
  return CF[k]
end
local function mulRot(a, b)
  local r = {}
  for i = 0, 2 do for j = 0, 2 do
    r[i * 3 + j + 1] = a[i * 3 + 1] * b[j + 1] + a[i * 3 + 2] * b[j + 4] + a[i * 3 + 3] * b[j + 7]
  end end
  return r
end
local function rotVec(c, x, y, z) return c[1] * x + c[2] * y + c[3] * z, c[4] * x + c[5] * y + c[6] * z, c[7] * x + c[8] * y + c[9] * z end
CFMT.__mul = function(a, b)
  if isCF(b) then
    local r = mulRot(a, b)
    local x, y, z = rotVec(a, b.x, b.y, b.z)
    return cf(a.x + x, a.y + y, a.z + z, table.unpack(r))
  elseif isV3(b) then
    local x, y, z = rotVec(a, b.X, b.Y, b.Z)
    return v3(a.x + x, a.y + y, a.z + z)
  end
  error("CFrame can only be multiplied by a CFrame or Vector3", 2)
end
CFMT.__add = function(a, b) return cf(a.x + b.X, a.y + b.Y, a.z + b.Z, a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]) end
CFMT.__sub = function(a, b) return cf(a.x - b.X, a.y - b.Y, a.z - b.Z, a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]) end
CFMT.__eq = function(a, b) if a.x ~= b.x or a.y ~= b.y or a.z ~= b.z then return false end for i = 1, 9 do if a[i] ~= b[i] then return false end end return true end
CFMT.__tostring = function(c) local t = { fmt(c.x), fmt(c.y), fmt(c.z) } for i = 1, 9 do t[#t + 1] = fmt(c[i]) end return table.concat(t, ", ") end
function CF.Inverse(c)
  local r = { c[1], c[4], c[7], c[2], c[5], c[8], c[3], c[6], c[9] }
  local x, y, z = r[1] * c.x + r[2] * c.y + r[3] * c.z, r[4] * c.x + r[5] * c.y + r[6] * c.z, r[7] * c.x + r[8] * c.y + r[9] * c.z
  return cf(-x, -y, -z, table.unpack(r))
end
function CF.PointToWorldSpace(c, v) return c * v end
function CF.PointToObjectSpace(c, v) return CF.Inverse(c) * v end
function CF.VectorToWorldSpace(c, v) return v3(rotVec(c, v.X, v.Y, v.Z)) end
function CF.VectorToObjectSpace(c, v) return v3(rotVec(CF.Inverse(c), v.X, v.Y, v.Z)) end
function CF.ToWorldSpace(c, o) return c * o end
function CF.ToObjectSpace(c, o) return CF.Inverse(c) * o end
function CF.GetComponents(c) return c.x, c.y, c.z, c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9] end
CF.components = CF.GetComponents
function CF.ToEulerAnglesYXZ(c)
  -- R = Ry * Rx * Rz
  local rx = math.asin(math.max(-1, math.min(1, -c[6])))
  local ry, rz
  if math.abs(c[6]) < 0.9999999 then ry = math.atan(c[3], c[9]); rz = math.atan(c[4], c[5])
  else ry = math.atan(-c[7], c[1]); rz = 0 end
  return rx, ry, rz
end
CF.ToOrientation = CF.ToEulerAnglesYXZ
function CF.ToEulerAnglesXYZ(c)
  -- R = Rx * Ry * Rz
  local ry = math.asin(math.max(-1, math.min(1, c[3])))
  local rx, rz
  if math.abs(c[3]) < 0.9999999 then rx = math.atan(-c[6], c[9]); rz = math.atan(-c[2], c[1])
  else rx = math.atan(c[8], c[5]); rz = 0 end
  return rx, ry, rz
end
local function rotX(a) local c, s = math.cos(a), math.sin(a); return { 1, 0, 0, 0, c, -s, 0, s, c } end
local function rotY(a) local c, s = math.cos(a), math.sin(a); return { c, 0, s, 0, 1, 0, -s, 0, c } end
local function rotZ(a) local c, s = math.cos(a), math.sin(a); return { c, -s, 0, s, c, 0, 0, 0, 1 } end
local function fromRot(r, x, y, z) return cf(x or 0, y or 0, z or 0, table.unpack(r)) end
local function quatFrom(c)
  local t = c[1] + c[5] + c[9]
  local w, x, y, z
  if t > 0 then local s = math.sqrt(t + 1) * 2; w = 0.25 * s; x = (c[8] - c[6]) / s; y = (c[3] - c[7]) / s; z = (c[4] - c[2]) / s
  elseif c[1] > c[5] and c[1] > c[9] then local s = math.sqrt(1 + c[1] - c[5] - c[9]) * 2; w = (c[8] - c[6]) / s; x = 0.25 * s; y = (c[2] + c[4]) / s; z = (c[3] + c[7]) / s
  elseif c[5] > c[9] then local s = math.sqrt(1 + c[5] - c[1] - c[9]) * 2; w = (c[3] - c[7]) / s; x = (c[2] + c[4]) / s; y = 0.25 * s; z = (c[6] + c[8]) / s
  else local s = math.sqrt(1 + c[9] - c[1] - c[5]) * 2; w = (c[4] - c[2]) / s; x = (c[3] + c[7]) / s; y = (c[6] + c[8]) / s; z = 0.25 * s end
  return w, x, y, z
end
local function rotFromQuat(w, x, y, z)
  return { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
           2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
           2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) }
end
function CF.Lerp(a, b, t)
  local aw, ax, ay, az = quatFrom(a)
  local bw, bx, by, bz = quatFrom(b)
  local d = aw * bw + ax * bx + ay * by + az * bz
  if d < 0 then bw, bx, by, bz, d = -bw, -bx, -by, -bz, -d end
  local w, x, y, z
  if d > 0.9995 then w, x, y, z = aw + (bw - aw) * t, ax + (bx - ax) * t, ay + (by - ay) * t, az + (bz - az) * t
  else
    local th = math.acos(d); local s = math.sin(th)
    local f1, f2 = math.sin((1 - t) * th) / s, math.sin(t * th) / s
    w, x, y, z = aw * f1 + bw * f2, ax * f1 + bx * f2, ay * f1 + by * f2, az * f1 + bz * f2
  end
  local m = math.sqrt(w * w + x * x + y * y + z * z)
  return fromRot(rotFromQuat(w / m, x / m, y / m, z / m), a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t)
end
local function lookAt(pos, target, up)
  up = up or v3(0, 1, 0)
  local f = (target - pos).Unit
  if f.Magnitude == 0 then return cf(pos.X, pos.Y, pos.Z) end
  local r = f:Cross(up)
  if r.Magnitude < 1e-6 then r = f:Cross(v3(0, 0, 1)) end
  r = r.Unit
  local u = r:Cross(f).Unit
  return cf(pos.X, pos.Y, pos.Z, r.X, u.X, -f.X, r.Y, u.Y, -f.Y, r.Z, u.Z, -f.Z)
end
CFrame = {
  new = function(a, b, c, ...)
    if a == nil then return cf(0, 0, 0) end
    if isV3(a) then
      if isV3(b) then return lookAt(a, b) end
      return cf(a.X, a.Y, a.Z)
    end
    if select("#", ...) >= 9 then return cf(a, b, c, ...) end
    return cf(a or 0, b or 0, c or 0)
  end,
  lookAt = lookAt,
  Angles = function(rx, ry, rz) return fromRot(mulRot(mulRot(rotX(rx or 0), rotY(ry or 0)), rotZ(rz or 0))) end,
  fromEulerAnglesYXZ = function(rx, ry, rz) return fromRot(mulRot(mulRot(rotY(ry or 0), rotX(rx or 0)), rotZ(rz or 0))) end,
  fromAxisAngle = function(axis, a)
    local u = axis.Unit; local s = math.sin(a / 2)
    return fromRot(rotFromQuat(math.cos(a / 2), u.X * s, u.Y * s, u.Z * s))
  end,
  identity = cf(0, 0, 0),
}
CFrame.fromEulerAnglesXYZ = CFrame.Angles
CFrame.fromOrientation = CFrame.fromEulerAnglesYXZ

---------------- Enum
local ENUMMT = { __tostring = function(e) return "Enum." .. e.EnumType .. "." .. e.Name end }
local enumItems = {}
Enum = {}
do
  local raw = B_enums()
  for typeName, names in pairs(raw) do
    local t = {}
    local list = {}
    for i, n in ipairs(names) do
      local item = setmetatable({ Name = n, Value = i - 1, EnumType = typeName }, ENUMMT)
      t[n] = item
      list[#list + 1] = item
      enumItems[typeName .. "." .. n] = item
    end
    t.GetEnumItems = function() return list end
    Enum[typeName] = setmetatable(t, { __index = function(_, k) error(tostring(k) .. " is not a valid member of Enum." .. typeName, 2) end })
  end
end
local function isEnum(v) return getmetatable(v) == ENUMMT end

TweenInfo = {
  new = function(time, style, dir, repeatCount, reverses, delayTime)
    return { Time = time or 1, EasingStyle = style or Enum.EasingStyle.Quad, EasingDirection = dir or Enum.EasingDirection.Out,
             RepeatCount = repeatCount or 0, Reverses = reverses or false, DelayTime = delayTime or 0, __tweeninfo = true }
  end,
}
RaycastParams = { new = function() return { FilterDescendantsInstances = {}, FilterType = Enum.RaycastFilterType.Exclude, IgnoreWater = true, __raycastparams = true } end }

-- ===================================================================
-- Instances
-- ===================================================================
local idOf = setmetatable({}, { __mode = "k" })
local classOf = setmetatable({}, { __mode = "k" })
local proxies = setmetatable({}, { __mode = "v" })
local InstanceMT = {}
local kinds = {}
local overrides = {}     -- "ClassName.Method" or ".Method" -> function
local methodCache = {}

local function wrap(id, cls)
  local p = proxies[id]
  if p then return p end
  p = setmetatable({}, InstanceMT)
  idOf[p] = id
  classOf[p] = cls
  proxies[id] = p
  return p
end
local function isInstance(v) return idOf[v] ~= nil end

local encode, decode

encode = function(v)
  local t = type(v)
  if t ~= "table" then return v end
  local id = idOf[v]
  if id then return { "I", id } end
  local mt = getmetatable(v)
  if mt == V3MT then return { "V3", v.X, v.Y, v.Z }
  elseif mt == C3MT then return { "C3", v.R, v.G, v.B }
  elseif mt == U2MT then return { "U2", v.X.Scale, v.X.Offset, v.Y.Scale, v.Y.Offset }
  elseif mt == CFMT then return { "CF", v.x, v.y, v.z, v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9] }
  elseif mt == ENUMMT then return { "E", v.EnumType, v.Name }
  elseif mt == V2MT then return { "V2", v.X, v.Y } end
  local n, count = #v, 0
  for _ in pairs(v) do count = count + 1 end
  if count == n then
    local arr = { "T" }
    for i = 1, n do arr[#arr + 1] = encode(v[i]) end
    return arr
  end
  local dict = { "D" }
  for k, val in pairs(v) do
    local tk = type(k)
    if tk == "string" or tk == "number" then dict[#dict + 1] = k; dict[#dict + 1] = encode(val) end
  end
  return dict
end

decode = function(v)
  if type(v) ~= "table" then return v end
  local tag = v[1]
  if tag == "I" then return wrap(v[2], v[3])
  elseif tag == "V3" then return v3(v[2], v[3], v[4])
  elseif tag == "C3" then return c3(v[2], v[3], v[4])
  elseif tag == "U2" then return udim2(v[2], v[3], v[4], v[5])
  elseif tag == "CF" then return cf(v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13])
  elseif tag == "E" then return enumItems[v[2] .. "." .. v[3]] or v[3]
  elseif tag == "V2" then return v2(v[2], v[3])
  elseif tag == "N" then return nil
  elseif tag == "IO" then
    return { KeyCode = decode(v[2]), UserInputType = decode(v[3]), Position = decode(v[4]), UserInputState = decode(v[5]) }
  elseif tag == "T" then local out = {} for i = 2, #v do out[#out + 1] = decode(v[i]) end return out
  elseif tag == "D" then local out = {} for i = 2, #v, 2 do out[v[i]] = decode(v[i + 1]) end return out
  elseif tag == "RR" then
    return { Instance = decode(v[2]), Position = decode(v[3]), Normal = decode(v[4]), Material = decode(v[5]), Distance = v[6] }
  end
  return v
end

local function kindOf(cls, key)
  local ck = kinds[cls]
  if not ck then ck = {}; kinds[cls] = ck end
  local k = ck[key]
  if k == nil then k = B_kind(cls, key) or ""; ck[key] = k end
  return k
end

local getSignal
local asyncWait = {}

local function awaitAsync(token)
  local co = coroutine.running()
  if not scriptOf[co] then error("This call yields and can only be used from a script thread", 3) end
  asyncWait[token] = co
  local ok, value = coroutine.yield(EVENTWAIT)
  if not ok then error(value, 3) end
  return value
end

local function method(name)
  local m = methodCache[name]
  if m then return m end
  m = function(self, ...)
    local id = idOf[self]
    if not id then error("Expected ':' not '.' calling member function " .. name, 2) end
    local n = select("#", ...)
    local args = { ... }
    for i = 1, n do args[i] = encode(args[i]) end
    local r = B_call(id, name, table.unpack(args, 1, n))
    if type(r) == "table" and r[1] == "ASYNC" then return awaitAsync(r[2]) end
    if type(r) == "table" and r[1] == "M" then
      local out = {}
      for i = 2, #r do out[i - 1] = decode(r[i]) end
      return table.unpack(out, 1, #r - 1)
    end
    return decode(r)
  end
  methodCache[name] = m
  return m
end

InstanceMT.__index = function(self, key)
  local cls = classOf[self]
  local k = kindOf(cls, key)
  if k == "m" then return overrides[cls .. "." .. key] or overrides["." .. key] or method(key)
  elseif k == "e" then return getSignal(idOf[self], key) end
  return decode(B_get(idOf[self], key))
end
InstanceMT.__newindex = function(self, key, value) B_set(idOf[self], key, encode(value)) end
InstanceMT.__tostring = function(self) return B_get(idOf[self], "Name") end

function typeof(v)
  local t = type(v)
  if t ~= "table" then return t end
  if idOf[v] then return "Instance" end
  local mt = getmetatable(v)
  if mt == V3MT then return "Vector3" elseif mt == V2MT then return "Vector2" elseif mt == C3MT then return "Color3"
  elseif mt == U2MT then return "UDim2" elseif mt == UDMT then return "UDim" elseif mt == CFMT then return "CFrame"
  elseif mt == ENUMMT then return "EnumItem" end
  if rawget(v, "__signal") then return "RBXScriptSignal" end
  if rawget(v, "__connection") then return "RBXScriptConnection" end
  return "table"
end

Instance = {
  new = function(cls, parent)
    local id = B_new(cls)
    local p = wrap(id, cls)
    if parent ~= nil then p.Parent = parent end
    return p
  end,
}

-- ===================================================================
-- Scheduler
-- ===================================================================
local queue = {}
local function report(err, co)
  local sid = scriptOf[co] or currentScript
  local tb = debug.traceback(co, tostring(err))
  local line = tonumber(tostring(err):match(":(%d+):"))
  B_log("error", (tb:gsub("\n%s*%[C%]: in %?", "")), sid, line)
end

local function hook()
  if B_clock() - resumeStart > TIMEOUT then
    debug.sethook()
    error("Script timeout: exhausted allowed execution time (" .. TIMEOUT .. " s) – add task.wait() to long loops", 2)
  end
end

local function resume(co, ...)
  if coroutine.status(co) ~= "suspended" then return end
  local prev = currentScript
  currentScript = scriptOf[co]
  resumeStart = B_clock()
  debug.sethook(co, hook, "", 200000)
  local ok, a, b = coroutine.resume(co, ...)
  currentScript = prev
  if not ok then report(a, co); return end
  if coroutine.status(co) == "suspended" then
    if a == WAIT then queue[#queue + 1] = { co = co, at = now + (b or 0), start = now }
    elseif a == EVENTWAIT then -- resumed by a signal
    else queue[#queue + 1] = { co = co, at = now, start = now } end
  end
end

local function spawnThread(fn, sid, ...)
  local co = type(fn) == "thread" and fn or coroutine.create(fn)
  scriptOf[co] = sid or currentScript
  resume(co, ...)
  return co
end

local function yieldWait(t)
  local co = coroutine.running()
  if not scriptOf[co] then error("wait() can only be called from a script thread", 2) end
  return coroutine.yield(WAIT, t)
end

task = {
  wait = function(t) local start = now; yieldWait(math.max(tonumber(t) or 0, 0)); return now - start end,
  spawn = function(fn, ...) return spawnThread(fn, currentScript, ...) end,
  defer = function(fn, ...)
    local co = type(fn) == "thread" and fn or coroutine.create(fn)
    scriptOf[co] = currentScript
    local args = table.pack(...)
    queue[#queue + 1] = { co = co, at = now, start = now, args = args }
    return co
  end,
  delay = function(t, fn, ...)
    local co = coroutine.create(fn)
    scriptOf[co] = currentScript
    queue[#queue + 1] = { co = co, at = now + (tonumber(t) or 0), start = now, args = table.pack(...) }
    return co
  end,
  cancel = function(co)
    for i = #queue, 1, -1 do if queue[i].co == co then table.remove(queue, i) end end
    if coroutine.status(co) == "suspended" then coroutine.close(co) end
  end,
}
wait = function(t) local start = now; yieldWait(math.max(tonumber(t) or 0.03, 0.03)); return now - start, now end
delay = task.delay
spawn = task.defer
function tick() return B_clock() end
function time() return now end
elapsedTime = time

local function runQueue()
  local due = {}
  local keep = {}
  for _, e in ipairs(queue) do if e.at <= now then due[#due + 1] = e else keep[#keep + 1] = e end end
  queue = keep
  table.sort(due, function(a, b) return a.at < b.at end)
  for _, e in ipairs(due) do
    if e.args then resume(e.co, table.unpack(e.args, 1, e.args.n)) else resume(e.co, now - e.start) end
  end
end

-- ===================================================================
-- Signals
-- ===================================================================
local SignalMT, ConnMT = {}, {}
SignalMT.__index = SignalMT
ConnMT.__index = ConnMT
function ConnMT:Disconnect()
  self.Connected = false
  local list = self.signal.conns
  for i = #list, 1, -1 do if list[i] == self then table.remove(list, i) end end
end

local function newSignal(onFirstConnect)
  return setmetatable({ __signal = true, conns = {}, waiting = {}, onFirst = onFirstConnect }, SignalMT)
end
function SignalMT:Connect(fn)
  if type(fn) ~= "function" then error("Connect expects a function", 2) end
  if self.onFirst then self.onFirst(); self.onFirst = nil end
  local c = setmetatable({ __connection = true, fn = fn, sid = currentScript, Connected = true, signal = self }, ConnMT)
  self.conns[#self.conns + 1] = c
  return c
end
function SignalMT:Once(fn)
  local c
  c = self:Connect(function(...) c:Disconnect(); fn(...) end)
  return c
end
function SignalMT:Wait()
  if self.onFirst then self.onFirst(); self.onFirst = nil end
  local co = coroutine.running()
  self.waiting[#self.waiting + 1] = co
  return coroutine.yield(EVENTWAIT)
end
function SignalMT:Fire(...)
  for _, c in ipairs({ table.unpack(self.conns) }) do
    if c.Connected then spawnThread(c.fn, c.sid, ...) end
  end
  local w = self.waiting
  self.waiting = {}
  for _, co in ipairs(w) do resume(co, ...) end
end
SignalMT.connect = SignalMT.Connect
SignalMT.wait = SignalMT.Wait

local signals = {}
getSignal = function(id, event)
  local key = id .. "\0" .. event
  local s = signals[key]
  if not s then
    s = newSignal(function() B_connect(id, event) end)
    signals[key] = s
  end
  return s
end

-- ===================================================================
-- Method overrides implemented in Lua
-- ===================================================================
overrides[".WaitForChild"] = function(self, name, timeout)
  local start = now
  local warned = false
  while true do
    local c = self:FindFirstChild(name)
    if c then return c end
    if timeout and now - start >= timeout then return nil end
    if not warned and not timeout and now - start > 5 then
      warned = true
      B_log("warn", "Infinite yield possible on '" .. self:GetFullName() .. ":WaitForChild(\"" .. tostring(name) .. "\")'", currentScript)
    end
    task.wait()
  end
end

local pcSignals = {}
overrides[".GetPropertyChangedSignal"] = function(self, prop)
  local id = idOf[self]
  local key = id .. "\0" .. prop
  local s = pcSignals[key]
  if not s then
    s = newSignal()
    pcSignals[key] = s
    getSignal(id, "Changed"):Connect(function(p) if p == prop then s:Fire() end end)
  end
  return s
end

local function easing(style, dir, t)
  local function base(x)
    if style == "Linear" then return x
    elseif style == "Sine" then return 1 - math.cos(x * math.pi / 2)
    elseif style == "Quad" then return x * x
    elseif style == "Cubic" then return x * x * x
    elseif style == "Quart" then return x ^ 4
    elseif style == "Quint" then return x ^ 5
    elseif style == "Exponential" then return x == 0 and 0 or 2 ^ (10 * x - 10)
    elseif style == "Circular" then return 1 - math.sqrt(1 - x * x)
    elseif style == "Back" then local s = 1.70158; return x * x * ((s + 1) * x - s)
    elseif style == "Elastic" then if x == 0 or x == 1 then return x end return -(2 ^ (10 * x - 10)) * math.sin((x * 10 - 10.75) * (2 * math.pi) / 3)
    elseif style == "Bounce" then
      local function out(y)
        if y < 1 / 2.75 then return 7.5625 * y * y elseif y < 2 / 2.75 then y = y - 1.5 / 2.75; return 7.5625 * y * y + 0.75
        elseif y < 2.5 / 2.75 then y = y - 2.25 / 2.75; return 7.5625 * y * y + 0.9375 else y = y - 2.625 / 2.75; return 7.5625 * y * y + 0.984375 end
      end
      return 1 - out(1 - x)
    end
    return x
  end
  if dir == "In" then return base(t)
  elseif dir == "Out" then return 1 - base(1 - t)
  else if t < 0.5 then return base(t * 2) / 2 else return 1 - base((1 - t) * 2) / 2 end end
end

local function lerpValue(a, b, t)
  if type(a) == "number" then return a + (b - a) * t end
  local mt = getmetatable(a)
  if mt == V3MT then return a:Lerp(b, t) elseif mt == C3MT then return a:Lerp(b, t)
  elseif mt == U2MT then return a:Lerp(b, t) elseif mt == CFMT then return a:Lerp(b, t) end
  return t >= 1 and b or a
end

local tweens = {}
local TweenMT = {}
TweenMT.__index = TweenMT
function TweenMT:Play()
  if self.PlaybackState == "Playing" then return end
  if self.PlaybackState ~= "Paused" then
    self.starts = {}
    for k in pairs(self.goals) do self.starts[k] = self.Instance[k] end
    self.t = -self.info.DelayTime
    self.iter = 0
    self.forward = true
  end
  self.PlaybackState = "Playing"
  tweens[self] = true
end
function TweenMT:Pause() if self.PlaybackState == "Playing" then self.PlaybackState = "Paused"; tweens[self] = nil end end
function TweenMT:Cancel() self.PlaybackState = "Cancelled"; tweens[self] = nil; self.Completed:Fire(Enum.PlaybackState.Cancelled) end
local function stepTween(tw, dt)
  tw.t = tw.t + dt
  local info = tw.info
  local alpha = info.Time <= 0 and 1 or math.max(0, math.min(1, tw.t / info.Time))
  local e = easing(info.EasingStyle.Name, info.EasingDirection.Name, alpha)
  if not tw.forward then e = 1 - e end
  local ok, err = pcall(function() for k, goal in pairs(tw.goals) do tw.Instance[k] = lerpValue(tw.starts[k], goal, e) end end)
  if not ok then tweens[tw] = nil; B_log("error", "Tween failed: " .. tostring(err), tw.sid); return end
  if alpha >= 1 then
    if info.Reverses and tw.forward then tw.forward = false; tw.t = 0; return end
    tw.iter = tw.iter + 1
    if info.RepeatCount < 0 or tw.iter <= info.RepeatCount then tw.forward = true; tw.t = 0; return end
    tweens[tw] = nil
    tw.PlaybackState = "Completed"
    tw.Completed:Fire(Enum.PlaybackState.Completed)
  end
end
overrides["TweenService.Create"] = function(self, inst, info, goals)
  if not isInstance(inst) then error("TweenService:Create expects an Instance", 2) end
  if type(info) ~= "table" or not info.__tweeninfo then error("TweenService:Create expects a TweenInfo", 2) end
  if type(goals) ~= "table" then error("TweenService:Create expects a table of goals", 2) end
  return setmetatable({ Instance = inst, info = info, goals = goals, PlaybackState = "Begin", Completed = newSignal(), sid = currentScript }, TweenMT)
end
overrides["DataStore.UpdateAsync"] = function(self, key, fn)
  if type(fn) ~= "function" then error("UpdateAsync expects (key, function(oldValue) return newValue end)", 2) end
  local old = self:GetAsync(key)
  local new = fn(old)
  if new == nil then return old end
  self:SetAsync(key, new)
  return new
end
overrides["Debris.AddItem"] = function(self, inst, life)
  task.delay(tonumber(life) or 10, function() if inst.Parent ~= nil then inst:Destroy() end end)
end
overrides["Workspace.Raycast"] = function(self, origin, direction, params)
  if not isV3(origin) or not isV3(direction) then error("Raycast expects (Vector3 origin, Vector3 direction)", 2) end
  local filter, mode = { "T" }, "Exclude"
  if params then
    for _, i in ipairs(params.FilterDescendantsInstances or {}) do if idOf[i] then filter[#filter + 1] = { "I", idOf[i] } end end
    local ft = params.FilterType
    mode = (ft == Enum.RaycastFilterType.Include or ft == Enum.RaycastFilterType.Whitelist) and "Include" or "Exclude"
  end
  return decode(B_call(idOf[self], "Raycast", encode(origin), encode(direction), filter, mode))
end

-- ===================================================================
-- Script loading, globals
-- ===================================================================
local SHARED = {}
shared = {}

local function lineOf(level) local info = debug.getinfo(level, "l"); return info and info.currentline or nil end
local function joinArgs(...)
  local n = select("#", ...)
  local t = {}
  for i = 1, n do t[i] = tostring((select(i, ...))) end
  return table.concat(t, " ")
end
print = function(...) B_log("print", joinArgs(...), currentScript, lineOf(3)) end
warn = function(...) B_log("warn", joinArgs(...), currentScript, lineOf(3)) end

local game = wrap(__GAME_ID, "DataModel")
local envMT = { __index = _G }
local function makeEnv(scriptProxy)
  return setmetatable({ script = scriptProxy, game = game, workspace = game.Workspace, Workspace = game.Workspace, _G = SHARED }, envMT)
end

local moduleCache = {}
local loading = {}
function require(m)
  local id = idOf[m]
  if not id then error("require expects a ModuleScript instance", 2) end
  if moduleCache[id] ~= nil then return moduleCache[id] end
  if loading[id] then error("Recursive require detected for " .. m:GetFullName(), 2) end
  local r = B_call(id, "__moduleSource")
  local src, name = r[2], r[3]
  local f, err = load(src, "=" .. name, "t", makeEnv(m))
  if not f then error("Syntax error in " .. name .. ": " .. err, 2) end
  loading[id] = true
  local ok, res = pcall(f)
  loading[id] = nil
  if not ok then error("Requested module experienced an error while loading: " .. tostring(res), 2) end
  if res == nil then error("Module code did not return exactly one value: " .. name, 2) end
  moduleCache[id] = res
  return res
end

function __runScript(id, cls, name, src)
  local p = wrap(id, cls)
  local f, err = load(src, "=" .. name, "t", makeEnv(p))
  if not f then
    B_log("error", "Syntax error: " .. err, id, tonumber(tostring(err):match(":(%d+):")))
    return false
  end
  spawnThread(f, id)
  return true
end

function __exec(src)
  local f, err = load(src, "=CommandBar", "t", makeEnv(nil))
  if not f then B_log("error", err, nil); return false end
  local co = coroutine.create(f)
  scriptOf[co] = "commandbar"
  resume(co)
  return true
end

function __step(dt, t, events)
  now = t
  for tw in pairs(tweens) do stepTween(tw, dt) end
  if events then
    for i = 1, #events do
      local e = events[i]
      if e[1] == "__async" then
        local co = asyncWait[e[2]]
        asyncWait[e[2]] = nil
        if co then resume(co, e[3], decode(e[4])) end
      end
      local s = e[1] ~= "__async" and signals[e[1] .. "\0" .. e[2]]
      if s then
        local n = #e
        local args = {}
        for k = 3, n do args[k - 2] = decode(e[k]) end
        s:Fire(table.unpack(args, 1, n - 2))
      end
    end
  end
  runQueue()
end

function __stopScript(id)
  for i = #queue, 1, -1 do if scriptOf[queue[i].co] == id then table.remove(queue, i) end end
  for _, s in pairs(signals) do
    for i = #s.conns, 1, -1 do if s.conns[i].sid == id then s.conns[i].Connected = false; table.remove(s.conns, i) end end
  end
end

function __forget(id) proxies[id] = nil end
`;
