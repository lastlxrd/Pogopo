scenes.stats = {}

local layout <const> = layout
local stats <const> = stats
local gfx <const> = playdate.graphics

function scenes.stats.update()
	if playdate.buttonJustPressed(playdate.kButtonA) and playdate.buttonJustPressed(playdate.kButtonB) then
		sfx.play(sfx.death)
		stats.reset()
		scenes.stats.draw()
		return
	end

	if playdate.buttonJustPressed(playdate.kButtonB) then
		sfx.play(sfx.select)
		scenes.switchTo(scenes.mainMenu)
		return
	end
end

function scenes.stats.init()
	stats.load()
	scenes.stats.draw()
end

function scenes.stats.draw()
	gfx.clear()

	gfx.setFont(fonts.medium)

	gfx.drawText("Stats", layout.xPad, 10);

	gfx.setFont(fonts.small)


	gfx.drawText("High-Score: " .. stats.highScore, layout.xPad, 60)
	gfx.drawText("Plays: " .. stats.plays, layout.xPad, 88)
	gfx.drawText("Apples Eaten: " .. stats.applesEaten, layout.xPad, 116)

	icons.drawAButton(layout.xPad, 160)
	gfx.drawText("+", layout.xPad + 32, 162)
	icons.drawBButton(layout.xPad + 48, 160)
	gfx.drawText("to reset stats", layout.xPad + 82, 162)

	icons.drawBButton(layout.xPad, 200)
	gfx.drawText("ack", layout.xPad + 29, 202)
end

function scenes.stats.denit()
end

