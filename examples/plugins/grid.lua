-- Grid layout: tiles windows in a near-square grid.
-- Install: copy to ~/.config/ewm/plugins/grid.lua, then Mod4+y selects it.
local ewm = require("ewm")

ewm.layout("grid", "###", function(area, n)
	local cols = math.ceil(math.sqrt(n))
	local rows = math.ceil(n / cols)
	local w, h = area.w // cols, area.h // rows
	local boxes = {}
	for i = 0, n - 1 do
		local col, row = i % cols, i // cols
		boxes[#boxes + 1] = {
			x = area.x + col * w + area.gap,
			y = area.y + row * h + area.gap,
			w = w - 2 * area.gap,
			h = h - 2 * area.gap,
		}
	end
	return boxes
end)

ewm.key("Mod4", "y", ewm.setlayout, "grid")
