scenes.more = {}

local more <const> = scenes.more
local gfx <const> = playdate.graphics
local layout <const> = layout
local qrCode = nil

function more.update()
	if playdate.buttonJustPressed(playdate.kButtonB) then
		sfx.play(sfx.select)
		scenes.switchTo(scenes.mainMenu)
		return
	end
end

function more.init()
	qrCode = playdate.graphics.image.new("sprites/qrcode.png")
	assert(qrCode, "Failed to load QR code image")

	more.draw()
end

function more.draw()
	gfx.clear()

	gfx.setFont(fonts.medium)

	gfx.drawText("More", layout.xPad, 10);

	gfx.setFont(fonts.small)

	qrCode:draw(layout.xPad - 2, 60)

	gfx.drawText("Find more of my games at\nbrettchalupa.itch.io", 112, 80)

	gfx.drawText("PDSnake is dedicated to the public domain.", layout.xPad, 160)

	icons.drawBButton(layout.xPad, 200)
	gfx.drawText("ack", layout.xPad + 29, 202)
	gfx.drawText(meta.version, screen.width - 40, 200);
end

function more.denit()
	qrCode = nil
end

