-- Minimal status text without dwmblocks: the time, refreshed every 30s.
-- Install: copy to ~/.config/ewm/plugins/clock.lua
local ewm = require("ewm")

local function update()
	ewm.setstatus(os.date("%a %d %b %H:%M"))
end

ewm.on("startup", update)
ewm.timer(30, update, true)
update() -- also on reload, when startup has already happened
