sfx = {}

function sfx.load(path)
	return playdate.sound.sampleplayer.new(path)
end

function sfx.play(sound)
	if settings.playSfx then
		sound:play()
	end
end

sfx.apple = sfx.load("sounds/apple.wav")
sfx.death = sfx.load("sounds/death.wav")
sfx.click = sfx.load("sounds/click.wav")
sfx.select = sfx.apple

sfx.apple:setVolume(0.5)
sfx.death:setVolume(0.5)
sfx.click:setVolume(0.5)

