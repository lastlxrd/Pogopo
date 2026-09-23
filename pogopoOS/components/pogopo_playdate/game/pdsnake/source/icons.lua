local gfx <const> = playdate.graphics

icons = {}

local function drawButton(char, x, y, xOffset, yOffset)
	gfx.setFont(fonts.small)
	gfx.setLineWidth(2)
	gfx.drawCircleInRect(x, y, 25, 25)
	gfx.drawText(char, x + xOffset, y + yOffset)
end

function icons.drawAButton(x, y)
	drawButton("A", x, y, 6, 3)
end

function icons.drawBButton(x, y)
	drawButton("B", x, y, 8, 3)
end

