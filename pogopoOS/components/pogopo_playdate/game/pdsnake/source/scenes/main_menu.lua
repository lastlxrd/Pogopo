scenes.mainMenu = {}

local mainMenu <const> = scenes.mainMenu

local gfx <const> = playdate.graphics

local meta <const> = meta
local sfx <const> = sfx
local fonts <const> = fonts
local layout <const> = layout
local scenes <const> = scenes

local drawStart = nil
local snakeImage = nil
local currentOption = 1
local hs = nil

local menuOptions = {
	{ title = "Play", scene = "gameplay" },
	{ title = "Stats", scene = "stats" },
	{ title = "More", scene = "more" },
}

local function menuYPos(i)
	return 68 + (i * 24)
end

function mainMenu.update()
	if playdate.buttonJustPressed(playdate.kButtonA) then
		sfx.play(sfx.select)
		local sceneKey = menuOptions[currentOption].scene
		local selectedScene = scenes[sceneKey]
		scenes.switchTo(selectedScene)
		return
	end

	if playdate.buttonJustPressed(playdate.kButtonUp) then
		sfx.play(sfx.click)
		currentOption -= 1
		if currentOption < 1 then
			currentOption = #menuOptions
		end
	end

	if playdate.buttonJustPressed(playdate.kButtonDown) then
		sfx.play(sfx.click)
		currentOption += 1
		if currentOption > #menuOptions then
			currentOption = 1
		end
	end

	gfx.clear()
	snakeImage:draw(layout.xPad, 8)

	gfx.setFont(fonts.medium)

	gfx.drawText(meta.name, layout.xPad + 38, 10);

	gfx.setFont(fonts.small)

	for i, option in ipairs(menuOptions) do
		gfx.drawText(option.title, layout.xPad + 20, menuYPos(i));
	end

	gfx.fillRect(math.sin(playdate.getCurrentTimeMilliseconds() / 140) + 16, menuYPos(currentOption) + 6, 8, 8, 0)

	if hs > 0 then
		gfx.drawText("High-Score: " .. hs, 240, 16);
	end

	gfx.drawText("by " .. meta.author, layout.xPad, screen.height - 34);
end

function mainMenu.init()
	drawStart = true
	stats.load()
	hs = stats.highScore

	snakeImage = gfx.image.new("sprites/snake.png")
	assert(snakeImage)
end

function mainMenu.denit()
	hs = nil
	snakeImage = nil
end

