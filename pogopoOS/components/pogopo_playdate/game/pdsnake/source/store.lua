local pdstore <const> = playdate.datastore

store = {}

function store.read()
	local data = pdstore.read()

	if meta.isDebug then
		print("SAVE DATA")
		printTable(data)
	end

	return data
end

function store.write(data)
	pdstore.write(data)
end

function store.mergeAndWrite(data)
	local save = pdstore.read()

	if save == nil then
		save = {}
	end

	for k, v in pairs(data) do
		save[k] = v
	end

	store.write(save)
end

