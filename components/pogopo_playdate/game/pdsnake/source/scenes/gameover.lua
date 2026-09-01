scenes.gameover = {}

local layout <const> = layout
local gameover <const> = scenes.gameover
local gfx <const> = playdate.graphics
local fonts <const> = fonts

local score = 0
local highScore = 0
local isNewHigh = false

function gameover.setScore(_score, _highScore, _isNewHigh)
	score = _score
	highScore = _highScore
	isNewHigh = _isNewHigh
end

function gameover.init()
end

function gameover.denit()
	score = 0
	highScore = 0
	isNewHigh = false
end

local function drawGameOver()
	gfx.clear()

	gfx.setFont(fonts.medium)
	gfx.drawText("Game Over", layout.xPad, 10);

	gfx.setFont(fonts.small)
	gfx.drawText("Your Score: " .. score, layout.xPad, 60);

	if isNewHigh then
		gfx.drawText("New high-score!", layout.xPad, 88);
	else
		gfx.drawText("High-Score: " .. highScore, layout.xPad, 88);
	end

	icons.drawAButton(layout.xPad, 160)
	gfx.drawText("Play again", layout.xPad + 32, 162)

	icons.drawBButton(layout.xPad, 200)
	gfx.drawText("ack to Main Menu", layout.xPad + 29, 202)
end

function gameover.update()
	if playdate.buttonJustPressed(playdate.kButtonA) then
		sfx.play(sfx.select)
		scenes.switchTo(scenes.gameplay)
		return
	end

	if playdate.buttonJustPressed(playdate.kButtonB) then
		sfx.play(sfx.select)
		scenes.switchTo(scenes.mainMenu)
		return
	end

	drawGameOver()
end

