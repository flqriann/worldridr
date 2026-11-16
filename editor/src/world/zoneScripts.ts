// Script templates for zones in a worldridr map (shop, fuel station, teleporter, message).
// They run in the editor's Play mode; the same source is exported to <map>/scripts/.

export const SHOP_SCRIPT = `-- Shop zone: walk into the zone to open the shop. Edit ITEMS and prices.
local zone = script.Parent
local Players = game:GetService("Players")
local player = Players.LocalPlayer

local ITEMS = {
\t{ name = "Sandwich", price = 5 },
\t{ name = "Energy drink", price = 8 },
\t{ name = "Road map", price = 15 },
}

local gui
local function money() return player:GetAttribute("Money") or 100 end

local function close()
\tif gui then gui:Destroy() gui = nil end
end

local function open()
\tif gui then return end
\tgui = Instance.new("ScreenGui")
\tgui.Name = "Shop"
\tgui.Parent = player:WaitForChild("PlayerGui")

\tlocal frame = Instance.new("Frame")
\tframe.Position = UDim2.new(0.5, -170, 0.5, -150)
\tframe.Size = UDim2.new(0, 340, 0, 60 + #ITEMS * 46)
\tframe.BackgroundColor3 = Color3.fromRGB(28, 30, 36)
\tframe.CornerRadius = 12
\tframe.Parent = gui

\tlocal title = Instance.new("TextLabel")
\ttitle.Size = UDim2.new(1, 0, 0, 40)
\ttitle.BackgroundTransparency = 1
\ttitle.TextColor3 = Color3.fromRGB(255, 255, 255)
\ttitle.Text = "Shop  -  " .. money() .. " EUR"
\ttitle.Parent = frame

\tfor i, item in ipairs(ITEMS) do
\t\tlocal b = Instance.new("TextButton")
\t\tb.Position = UDim2.new(0, 12, 0, 40 + (i - 1) * 46)
\t\tb.Size = UDim2.new(1, -24, 0, 38)
\t\tb.Text = item.name .. "  (" .. item.price .. " EUR)"
\t\tb.CornerRadius = 8
\t\tb.Parent = frame
\t\tb.MouseButton1Click:Connect(function()
\t\t\tif money() >= item.price then
\t\t\t\tplayer:SetAttribute("Money", money() - item.price)
\t\t\t\ttitle.Text = "Bought " .. item.name .. "  -  " .. money() .. " EUR"
\t\t\telse
\t\t\t\ttitle.Text = "Not enough money"
\t\t\tend
\t\tend)
\tend
end

zone.Touched:Connect(function(hit)
\tif hit.Parent == player.Character then open() end
end)
zone.TouchEnded:Connect(function(hit)
\tif hit.Parent == player.Character then close() end
end)
`;

export const FUEL_SCRIPT = `-- Fuel station zone: stand in the zone to refuel (costs money).
local zone = script.Parent
local Players = game:GetService("Players")
local player = Players.LocalPlayer
local PRICE_PER_LITER = 1.7

local inside = false
zone.Touched:Connect(function(hit)
\tif hit.Parent ~= player.Character or inside then return end
\tinside = true
\twhile inside do
\t\tlocal money = player:GetAttribute("Money") or 100
\t\tlocal fuel = player:GetAttribute("Fuel") or 20
\t\tif money < PRICE_PER_LITER or fuel >= 60 then break end
\t\tplayer:SetAttribute("Money", money - PRICE_PER_LITER)
\t\tplayer:SetAttribute("Fuel", fuel + 1)
\t\ttask.wait(0.2)
\tend
end)
zone.TouchEnded:Connect(function(hit)
\tif hit.Parent == player.Character then inside = false end
end)
`;

export const TELEPORT_SCRIPT = `-- Teleporter: moves the player to the part named in the attribute "Target" (default: SpawnLocation).
local zone = script.Parent
local Players = game:GetService("Players")
local player = Players.LocalPlayer

zone.Touched:Connect(function(hit)
\tif hit.Parent ~= player.Character then return end
\tlocal target = workspace:FindFirstChild(zone:GetAttribute("Target") or "SpawnLocation")
\tlocal root = player.Character:FindFirstChild("HumanoidRootPart")
\tif target and root then
\t\troot.Position = target.Position + Vector3.new(0, 3, 0)
\tend
end)
`;

export const MESSAGE_SCRIPT = `-- Message zone: shows a text while the player stands inside (e.g. "Speed limit 30"). Edit TEXT.
local zone = script.Parent
local Players = game:GetService("Players")
local player = Players.LocalPlayer
local TEXT = "Welcome!"

local gui
zone.Touched:Connect(function(hit)
\tif hit.Parent ~= player.Character or gui then return end
\tgui = Instance.new("ScreenGui")
\tgui.Parent = player:WaitForChild("PlayerGui")
\tlocal label = Instance.new("TextLabel")
\tlabel.Position = UDim2.new(0.5, -200, 0, 24)
\tlabel.Size = UDim2.new(0, 400, 0, 44)
\tlabel.Text = TEXT
\tlabel.CornerRadius = 10
\tlabel.BackgroundColor3 = Color3.fromRGB(28, 30, 36)
\tlabel.TextColor3 = Color3.fromRGB(255, 255, 255)
\tlabel.Parent = gui
end)
zone.TouchEnded:Connect(function(hit)
\tif hit.Parent == player.Character and gui then gui:Destroy() gui = nil end
end)
`;
