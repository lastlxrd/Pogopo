settings = {
	playSfx = true, -- default
	batterySaver = false, -- default
	darkMode = false, -- default
}

function settings.load()
	local save = store.read()

	if save then
		if save.playSfx ~= nil then
			settings.playSfx = save.playSfx
		end

		if save.batterySaver ~= nil then
			settings.batterySaver = save.batterySaver
		end

		if save.darkMode ~= nil then
			settings.darkMode = save.darkMode
		end
	end

	settings.setFrameRate()
	settings.setColorMode()

	return settings
end

function settings.save()
	store.mergeAndWrite({
		playSfx = settings.playSfx,
		batterySaver = settings.batterySaver,
		darkMode = settings.darkMode,
	})
end

function settings.toggleSfx()
	settings.playSfx = not settings.playSfx
	settings.save()
end

function settings.toggleBatterySaver()
	settings.batterySaver = not settings.batterySaver
	settings.save()
	settings.setFrameRate()
end

function settings.setFrameRate()
	if settings.batterySaver then
		playdate.display.setRefreshRate(25)
	else
		playdate.display.setRefreshRate(50)
	end
end

function settings.toggleDarkMode()
	settings.darkMode = not settings.darkMode
	settings.save()
	settings.setColorMode()
end


function settings.setColorMode()
	playdate.display.setInverted(settings.darkMode)
end

