meta = {
	name = playdate.metadata.name,
	version = playdate.metadata.version,
	build = playdate.metadata.buildNumber,
	author = playdate.metadata.author,
	isDebug = string.find(playdate.metadata.version, "-dev") ~= nil,
}

meta.versionAndBuild = meta.version .. " (" .. meta.build .. ")"

