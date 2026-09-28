-- dwmblocks status bar: starts it, and forwards clicks on its blocks.
-- Install: copy to ~/.config/ewm/plugins/dwmblocks.lua
-- For instant volume updates, add "; pkill -RTMIN+1 dwmblocks" to your
-- volume key commands (block signal 1).
local ewm = require("ewm")

local function have(name)
	for dir in (os.getenv("PATH") or ""):gmatch("[^:]+") do
		local f = io.open(dir .. "/" .. name)
		if f then
			f:close()
			return true
		end
	end
	return false
end

if not have("dwmblocks") then
	return
end

-- clicks on a block send SIGRTMIN+<block signal> to this process, with the
-- mouse button as value
ewm.set { statusbar = "dwmblocks" }
for b = 1, 5 do
	ewm.button("status", "", b, ewm.sigstatusbar, b)
end

ewm.autostart("pkill -x dwmblocks; exec dwmblocks")
