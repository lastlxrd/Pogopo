stats = {
	plays = 0, -- default
	applesEaten = 0, -- default
	totalTimePlayed = 0, -- ms, default
	highScore = 0 -- default
}

function stats.load()
	local save = store.read()

	for k, _ in pairs(stats) do
		if save and save[k] ~= nil then
			if meta.isDebug then
				print("stat set from save:" .. k .. " -> " .. save[k])
			end

			stats[k] = save[k]
		end
	end
end

function stats.save()
	store.mergeAndWrite({
		plays = stats.plays,
		applesEaten = stats.applesEaten,
		totalTimePlayed = stats.totalTimePlayed,
		highScore = stats.highScore
	})
end

function stats.reset()
	stats.plays = 0
	stats.applesEaten = 0
	stats.totalTimePlayed = 0
	stats.highScore = 0
	stats.save()
end

function stats.logPlay()
	stats.plays += 1
end

function stats.addApples(apples)
	stats.applesEaten += apples
end

function stats.addTimePlayed(time)
	stats.totalTimePlayed += time
end

function stats.setHighScore(score)
	if score > stats.highScore then
		stats.highScore = score
	end

	return stats.highScore
end

