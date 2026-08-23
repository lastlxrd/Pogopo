-- Minimal, game-oriented replacements for the Playdate CoreLibs used by
-- PDSnake and Celeste Classic. Pixel work stays in the native C++ backend;
-- object, sprite, tilemap and input orchestration remains ordinary Lua 5.4.

Object = Object or {}
Object.__index = Object
function Object:init() end
setmetatable(Object, {
	__call = function(cls, ...)
		local value = setmetatable({}, cls)
		if value.init then value:init(...) end
		return value
	end,
})

function class(name)
	local cls = {}
	cls.__index = cls
	local mt = {
		__call = function(class_table, ...)
			local value = setmetatable({}, class_table)
			if value.init then value:init(...) end
			return value
		end,
	}
	setmetatable(cls, mt)
	_G[name] = cls
	return {
		extends = function(parent)
			if type(parent) == "string" then parent = _G[parent] end
			parent = parent or Object
			cls.super = parent
			mt.__index = parent
			setmetatable(cls, mt)
			return cls
		end,
	}
end

local gfx = playdate.graphics
local sprites = {}
local collision_sprites = {}
local wall_cells = {}
local background_callback = nil
local sprite_sequence = 0
local sprites_dirty = false
local collision_sprites_dirty = false
local query_sequence = 0

local Sprite = {}
Sprite.__index = Sprite
setmetatable(Sprite, {
	__call = function(cls, image)
		local value = setmetatable({}, cls)
		value:init(image)
		return value
	end,
})

function Sprite:init(image)
	self.image = image
	self.tilemap = nil
	self.x, self.y = 0, 0
	self.centerX, self.centerY = 0.5, 0.5
	self.width, self.height = 0, 0
	if image then self.width, self.height = image:getSize() end
	self.zIndex = 0
	self.visible = true
	self.added = false
	self._updatesEnabled = true
	self._collisionsEnabled = true
	self.imageDrawMode = gfx.kDrawModeCopy
	self.flip = gfx.kImageUnflipped
	self.xScale, self.yScale = 1, 1
	self.rotation = 0
	self.clipRect = nil
	self.groups = {}
	self.collidesWithGroups = {}
	self.tag = 0
	self._sequence = 0
end

function Sprite.new(image) return Sprite(image) end

function Sprite:setImage(image, flip, xScale, yScale)
	if self.image ~= image then
		self.image = image
		if image then
			local width, height = image:getSize()
			if self.width == 0 then self.width = width end
			if self.height == 0 then self.height = height end
		end
	end
	self.flip = flip or gfx.kImageUnflipped
	self.xScale = xScale or 1
	self.yScale = yScale or self.xScale
	if image and self.width == 0 then
		local width, height = image:getSize()
		self.width, self.height = width, height
	end
end
function Sprite:getImage() return self.image end
function Sprite:setTilemap(tilemap)
	self.tilemap = tilemap
	if tilemap and tilemap.getPixelSize then
		self.width, self.height = tilemap:getPixelSize()
	end
end
function Sprite:setSize(width, height) self.width, self.height = width, height end
function Sprite:getSize() return self.width, self.height end
function Sprite:setCenter(x, y) self.centerX, self.centerY = x, y end
function Sprite:getCenter() return self.centerX, self.centerY end
function Sprite:moveTo(x, y) self.x, self.y = x, y end
function Sprite:moveBy(x, y) self.x, self.y = self.x + x, self.y + y end
function Sprite:getPosition() return self.x, self.y end
function Sprite:setRotation(value) self.rotation = value or 0 end
function Sprite:getRotation() return self.rotation end
function Sprite:setClipRect(x, y, width, height)
	if type(x) == "table" then
		self.clipRect = {x=x.x or 0, y=x.y or 0,
			width=x.width or x.w or 0, height=x.height or x.h or 0}
	else
		self.clipRect = {x=x or 0, y=y or 0,
			width=width or 0, height=height or 0}
	end
end
function Sprite:clearClipRect() self.clipRect = nil end
function Sprite:setZIndex(value)
	if self.zIndex ~= value then
		self.zIndex = value
		sprites_dirty = true
	end
end
function Sprite:getZIndex() return self.zIndex end
function Sprite:setVisible(value) self.visible = value == true end
function Sprite:isVisible() return self.visible end
function Sprite:setUpdatesEnabled(value) self._updatesEnabled = value == true end
function Sprite:updatesEnabled() return self._updatesEnabled end
function Sprite:setImageDrawMode(value) self.imageDrawMode = value end
function Sprite:setTag(value) self.tag = math.max(0, math.min(255, value or 0)) end
function Sprite:getTag() return self.tag end
function Sprite:setGroups(value) self.groups = value or {} end
function Sprite:setCollidesWithGroups(value) self.collidesWithGroups = value or {} end
function Sprite:setCollisionsEnabled(value) self._collisionsEnabled = value == true end
function Sprite:collisionsEnabled() return self._collisionsEnabled end

function Sprite:setCollideRect(x, y, width, height)
	if type(x) == "table" then
		self.collideRect = {x=x.x or 0, y=x.y or 0,
			width=x.width or x.w or 0, height=x.height or x.h or 0}
	else
		self.collideRect = {x=x or 0, y=y or 0, width=width or 0, height=height or 0}
	end
	if not self._wall and not self._collision_listed then
		self._collision_listed = true
		table.insert(collision_sprites, self)
	end
end
function Sprite:clearCollideRect()
	self.collideRect = nil
	collision_sprites_dirty = true
end

function Sprite:add()
	if self.added then return self end
	self.added = true
	if not self._listed then
		sprite_sequence = sprite_sequence + 1
		self._sequence = sprite_sequence
		self._listed = true
		table.insert(sprites, self)
	end
	if self.collideRect and not self._wall and not self._collision_listed then
		self._collision_listed = true
		table.insert(collision_sprites, self)
	end
	sprites_dirty = true
	return self
end

function Sprite:remove()
	if self.added then
		self.added = false
		sprites_dirty = true
		if self._collision_listed then collision_sprites_dirty = true end
	end
	return self
end

function Sprite:update()
	-- Base-instance update is intentionally empty. A call with no receiver is
	-- the static Playdate sprite frame update.
	if self ~= nil then return end
	gfx._beginFrame()
	if background_callback then background_callback(0, 0, 400, 240) end
	if sprites_dirty then
		local compacted = {}
		for i=1,#sprites do
			local item = sprites[i]
			if item.added then
				compacted[#compacted + 1] = item
			else
				item._listed = false
			end
		end
		sprites = compacted
		table.sort(sprites, function(a, b)
			local az, bz = a.zIndex or 0, b.zIndex or 0
			if az == bz then return (a._sequence or 0) < (b._sequence or 0) end
			return az < bz
		end)
		sprites_dirty = false
	end
	for i=1,#sprites do
		local item = sprites[i]
		if item.added and item._updatesEnabled then
			local update = item.update
			if update and update ~= Sprite.update then update(item) end
		end
	end
	for i=1,#sprites do
		local item = sprites[i]
		if item.added and item.visible and (item.image or item.tilemap) then
			local previous = gfx._getImageDrawMode()
			gfx.setImageDrawMode(item.imageDrawMode)
			if item.clipRect then
				gfx.setClipRect(item.clipRect.x, item.clipRect.y,
					item.clipRect.width, item.clipRect.height)
			end
			if item.tilemap then
				local x = math.floor(item.x - item.width * item.centerX)
				local y = math.floor(item.y - item.height * item.centerY)
				item.tilemap:draw(x, y)
			elseif item.rotation ~= 0 then
				item.image:drawRotated(item.x, item.y, item.rotation,
					item.xScale, item.yScale)
			else
				local x = math.floor(item.x - item.width * item.centerX)
				local y = math.floor(item.y - item.height * item.centerY)
				if item.xScale ~= 1 or item.yScale ~= 1 then
					-- PogoDate's native scaled path is integer-only; animation
					-- libraries normally use 1x, while rotated sprites retain
					-- independent fractional scales through drawRotated above.
					item.image:drawRotated(item.x, item.y, 0,
						item.xScale, item.yScale)
				else
					item.image:draw(x, y, item.flip)
				end
			end
			if item.clipRect then gfx.clearClipRect() end
			gfx.setImageDrawMode(previous)
		end
	end
end

function Sprite.performOnAllSprites(callback)
	for i=1,#sprites do if sprites[i].added then callback(sprites[i]) end end
end

function Sprite.removeAll()
	for i=1,#sprites do
		sprites[i].added = false
		sprites[i]._listed = false
	end
	sprites = {}
	for i=1,#collision_sprites do
		collision_sprites[i]._collision_listed = false
	end
	collision_sprites = {}
	wall_cells = {}
	sprites_dirty = false
	collision_sprites_dirty = false
end

function Sprite.setBackgroundDrawingCallback(callback) background_callback = callback end
function Sprite.redrawBackground()
	if background_callback then background_callback(0, 0, 400, 240) end
end

local function spriteBounds(item)
	local left = item.x - item.width * item.centerX
	local top = item.y - item.height * item.centerY
	local rect = item.collideRect
	if rect then
		return left + rect.x, top + rect.y, rect.width, rect.height
	end
	return left, top, item.width, item.height
end

local function overlaps(ax, ay, aw, ah, bx, by, bw, bh)
	return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah
end

local function hasGroup(groups, wanted)
	if groups == nil then return false end
	if type(groups) == "number" then return groups == wanted end
	for i=1,#groups do if groups[i] == wanted then return true end end
	return false
end

local function groupsEmpty(groups)
	return groups == nil or (type(groups) == "table" and #groups == 0)
end

local function groupsAllow(left, right)
	if groupsEmpty(left.collidesWithGroups) and groupsEmpty(right.collidesWithGroups) then
		return true
	end
	if type(right.groups) == "number" then
		if hasGroup(left.collidesWithGroups, right.groups) then return true end
	else
		for i=1,#right.groups do
			if hasGroup(left.collidesWithGroups, right.groups[i]) then return true end
		end
	end
	if type(left.groups) == "number" then
		if hasGroup(right.collidesWithGroups, left.groups) then return true end
	else
		for i=1,#left.groups do
			if hasGroup(right.collidesWithGroups, left.groups[i]) then return true end
		end
	end
	return false
end

function Sprite:checkCollisions(goalX, goalY)
	local previousX, previousY = self.x, self.y
	self.x, self.y = goalX, goalY
	local sx, sy, sw, sh = spriteBounds(self)
	local collisions = {}
	-- Celeste's rooms contain hundreds of 8 x 8 wall sprites. The Playdate
	-- SDK spatially indexes these; doing the equivalent lookup here avoids a
	-- full Lua scan for every horizontal and vertical player move.
	local candidates = Sprite.querySpritesInRect(sx, sy, sw, sh)
	for i=1,#candidates do
		local other = candidates[i]
		if other ~= self and other.added and other._collisionsEnabled and
			groupsAllow(self, other) and
			other.collideRect and self.collideRect then
			local ox, oy, ow, oh = spriteBounds(other)
			if overlaps(sx,sy,sw,sh,ox,oy,ow,oh) then
				table.insert(collisions, {
					sprite=self, other=other,
					spriteRect={x=sx,y=sy,width=sw,height=sh},
					otherRect={x=ox,y=oy,width=ow,height=oh},
				})
			end
		end
	end
	self.x, self.y = previousX, previousY
	return goalX, goalY, collisions, #collisions
end

function Sprite:moveWithCollisions(goalX, goalY)
	local actualX, actualY, collisions, length = self:checkCollisions(goalX, goalY)
	self:moveTo(actualX, actualY)
	return actualX, actualY, collisions, length
end

function Sprite.querySpritesInRect(x, y, width, height)
	if type(x) == "table" then
		local rect = x
		x, y = rect.x, rect.y
		width, height = rect.width or rect.w, rect.height or rect.h
	end
	local result = {}
	if collision_sprites_dirty then
		local compacted = {}
		for i=1,#collision_sprites do
			local item = collision_sprites[i]
			if item.added and item.collideRect then
				compacted[#compacted + 1] = item
			else
				item._collision_listed = false
			end
		end
		collision_sprites = compacted
		collision_sprites_dirty = false
	end
	query_sequence = query_sequence + 1
	local stamp = query_sequence
	local minx = math.floor((x or 0) / 8)
	local maxx = math.floor(((x or 0) + math.max(0, (width or 0) - 1)) / 8)
	local miny = math.floor((y or 0) / 8)
	local maxy = math.floor(((y or 0) + math.max(0, (height or 0) - 1)) / 8)
	for cy=miny,maxy do
		local grid_row = wall_cells[cy]
		for cx=minx,maxx do
			local cell = grid_row and grid_row[cx]
			if cell then
				for i=1,#cell do
					local item = cell[i]
					if item.added and item._collisionsEnabled and
						item._query_stamp ~= stamp then
						local sx, sy, sw, sh = spriteBounds(item)
						if overlaps(x,y,width,height,sx,sy,sw,sh) then
							item._query_stamp = stamp
							table.insert(result, item)
						end
					end
				end
			end
		end
	end
	for i=1,#collision_sprites do
		local item = collision_sprites[i]
		if item.added and item._collisionsEnabled and item._query_stamp ~= stamp then
			local sx, sy, sw, sh = spriteBounds(item)
			if overlaps(x,y,width,height,sx,sy,sw,sh) then
				item._query_stamp = stamp
				table.insert(result, item)
			end
		end
	end
	return result
end

function Sprite:overlappingSprites()
	if not self.added or not self._collisionsEnabled or not self.collideRect then
		return {}
	end
	local x, y, width, height = spriteBounds(self)
	local candidates = Sprite.querySpritesInRect(x, y, width, height)
	local result = {}
	for i=1,#candidates do
		local other = candidates[i]
		if other ~= self and other.added and other._collisionsEnabled and
			other.collideRect and groupsAllow(self, other) then
			local ox, oy, ow, oh = spriteBounds(other)
			if overlaps(x, y, width, height, ox, oy, ow, oh) then
				result[#result + 1] = other
			end
		end
	end
	return result
end

function Sprite.allOverlappingSprites()
	local result = {}
	for i=1,#collision_sprites do
		local left = collision_sprites[i]
		if left.added and left._collisionsEnabled and left.collideRect then
			local overlapping = left:overlappingSprites()
			for j=1,#overlapping do
				local right = overlapping[j]
				if (left._sequence or 0) < (right._sequence or 0) then
					result[#result + 1] = left
					result[#result + 1] = right
				end
			end
		end
	end
	return result
end

function Sprite.addWallSprites(tilemap, emptyIDs, offsetX, offsetY)
	local empty = {}
	for i=1,#emptyIDs do empty[emptyIDs[i]] = true end
	local result = {}
	offsetX, offsetY = offsetX or 0, offsetY or 0
	local width = tilemap.width or 1
	for index=1,#tilemap.tiles do
		local tile = tilemap.tiles[index]
		if tile and tile > 0 and not empty[tile] then
			local column = (index - 1) % width
			local row = math.floor((index - 1) / width)
			local item = Sprite()
			item._wall = true
			-- Collision walls live only in the spatial grid. They have no image,
			-- so adding hundreds of them to the render list just makes every
			-- frame sort and scan invisible objects.
			item.added = true
			item:setSize(8, 8)
			item:setCollideRect(0, 0, 8, 8)
			item:moveTo(offsetX + column * 8 + 4, offsetY + row * 8 + 4)
			local cell_x = math.floor((offsetX + column * 8) / 8)
			local cell_y = math.floor((offsetY + row * 8) / 8)
			wall_cells[cell_y] = wall_cells[cell_y] or {}
			wall_cells[cell_y][cell_x] = wall_cells[cell_y][cell_x] or {}
			table.insert(wall_cells[cell_y][cell_x], item)
			table.insert(result, item)
		end
	end
	return result
end

Sprite.kCollisionTypeSlide = "slide"
Sprite.kCollisionTypeFreeze = "freeze"
Sprite.kCollisionTypeOverlap = "overlap"
Sprite.kCollisionTypeBounce = "bounce"
gfx.sprite = Sprite

local Tilemap = {}
Tilemap.__index = Tilemap
function Tilemap.new()
	return setmetatable({tiles={}, width=1, height=0, imagetable=nil}, Tilemap)
end
function Tilemap:setTiles(tiles, width)
	self.tiles, self.width = tiles, math.max(1, width or 1)
	self.height = math.ceil(#tiles / self.width)
end
function Tilemap:setImageTable(value) self.imagetable = value end
function Tilemap:getSize() return self.width, self.height end
function Tilemap:getPixelSize()
	local tile = self.imagetable and self.imagetable:getImage(1)
	if not tile then return 0, 0 end
	local width, height = tile:getSize()
	return self.width * width, self.height * height
end
function Tilemap:draw(x, y)
	if not self.imagetable then return end
	local first = self.imagetable:getImage(1)
	local tileWidth, tileHeight = 8, 8
	if first then tileWidth, tileHeight = first:getSize() end
	for index=1,#self.tiles do
		local tile = self.tiles[index]
		if tile and tile > 0 then
			local image = self.imagetable:getImage(tile)
			if image then
				local column = (index - 1) % self.width
				local row = math.floor((index - 1) / self.width)
				image:draw((x or 0) + column * tileWidth,
					(y or 0) + row * tileHeight)
			end
		end
	end
end
gfx.tilemap = Tilemap

local Rect = {}
Rect.__index = Rect
function Rect.new(x, y, width, height)
	return setmetatable({x=x,y=y,width=width,height=height,w=width,h=height}, Rect)
end
function Rect:offsetBy(x, y) return Rect.new(self.x+x, self.y+y, self.width, self.height) end
function Rect:intersects(other)
	return overlaps(self.x,self.y,self.width,self.height,
		other.x,other.y,other.width or other.w,other.height or other.h)
end
playdate.geometry = {rect=Rect}

playdate.inputHandlers = {stack={}}
function playdate.inputHandlers.push(handler, exclusive)
	table.insert(playdate.inputHandlers.stack, {handler=handler, exclusive=exclusive == true})
end
function playdate.inputHandlers.pop() return table.remove(playdate.inputHandlers.stack) end

local buttonNames = {
	{playdate.kButtonLeft, "left"}, {playdate.kButtonRight, "right"},
	{playdate.kButtonUp, "up"}, {playdate.kButtonDown, "down"},
	{playdate.kButtonA, "A"}, {playdate.kButtonB, "B"},
}
function _pogodate_dispatch_input(pressed, released)
	local stack = playdate.inputHandlers.stack
	for index=#stack,1,-1 do
		local entry = stack[index]
		for i=1,#buttonNames do
			local mask, name = buttonNames[i][1], buttonNames[i][2]
			if (pressed & mask) ~= 0 then
				local callback = entry.handler[name .. "ButtonDown"]
				if callback then callback() end
			end
			if (released & mask) ~= 0 then
				local callback = entry.handler[name .. "ButtonUp"]
				if callback then callback() end
			end
		end
		if entry.exclusive then break end
	end
end

local Gridview = {}
Gridview.__index = Gridview
function Gridview.new(cellWidth, cellHeight)
	return setmetatable({cellWidth=cellWidth,cellHeight=cellHeight,rows=1,selected=1,needsdisplay=true}, Gridview)
end
function Gridview:setNumberOfSections() end
function Gridview:setNumberOfColumns() end
function Gridview:setNumberOfRows(value) self.rows = value end
function Gridview:setCellPadding() end
function Gridview:setContentInset() end
function Gridview:setHorizontalDividerHeight() end
function Gridview:addHorizontalDividerAbove() end
function Gridview:getSelectedRow() return self.selected end
function Gridview:setSelection(section, row) self.selected = math.max(1, math.min(self.rows,row)); self.needsdisplay=true end
function Gridview:scrollToCell() end
function Gridview:selectPreviousRow(wrap)
	self.selected = self.selected - 1
	if self.selected < 1 then self.selected = wrap and self.rows or 1 end
	self.needsdisplay = true
end
function Gridview:selectNextRow(wrap)
	self.selected = self.selected + 1
	if self.selected > self.rows then self.selected = wrap and 1 or self.rows end
	self.needsdisplay = true
end
function Gridview:drawInRect(x, y, width, height)
	local rowHeight = self.cellHeight > 0 and self.cellHeight or 8
	for row=1,self.rows do
		local top = y + (row-1) * rowHeight
		if top < y + height and self.drawCell then
			self:drawCell(1, row, 1, row == self.selected, x, top, width, rowHeight)
		end
	end
	self.needsdisplay = false
end
playdate.ui = {gridview=Gridview}

kTextAlignment = {left=0, center=1, right=2}

playdate.math = playdate.math or {}
function playdate.math.lerp(startValue, endValue, amount)
	return startValue + (endValue - startValue) * amount
end

-- CoreLibs/frameTimer semantics: durations are measured in calls to
-- updateTimers(), not milliseconds.  This keeps frame-authored animation and
-- difficulty pacing stable when a package requests a non-default refresh rate.
local frameTimers = {}
local pendingFrameTimers = {}
local updatingFrameTimers = false
local FrameTimer = {}
FrameTimer.__index = FrameTimer

function FrameTimer:pause() self.paused = true end
function FrameTimer:start() self.paused = false end
function FrameTimer:reset()
	self.frame = 0
	self.value = self.startValue
	self._reversed = false
	self._delayRemaining = math.max(0, math.floor(self.delay or 0))
	self.paused = false
	self.removed = false
end
function FrameTimer:remove() self.removed = true end

local function frameTimerLinear(t, b, c, d)
	return b + c * (t / d)
end

local function frameTimerCall(callback, arguments, timer)
	if type(callback) ~= "function" then return end
	local count = arguments and (arguments.n or #arguments) or 0
	if count > 0 then
		callback(table.unpack(arguments, 1, count))
	else
		callback(timer)
	end
end

playdate.frameTimer = {}
function playdate.frameTimer.new(duration, second, ...)
	local arguments = table.pack(...)
	local callback = type(second) == "function" and second or nil
	local startValue = callback and 0 or (tonumber(second) or 0)
	local endValue = callback and 0 or (tonumber(arguments[1]) or 0)
	local easingFunction = not callback and type(arguments[2]) == "function"
		and arguments[2] or frameTimerLinear
	local timer = setmetatable({
		duration=math.max(1, math.floor(duration or 1)),
		startValue=startValue,
		endValue=endValue,
		value=startValue,
		easingFunction=easingFunction,
		frame=0,
		delay=0,
		_reversed=false,
		_completionCallback=callback,
		_callbackArguments=callback and arguments or nil,
		paused=false,
		removed=false,
		repeats=false,
		reverses=false,
		discardOnCompletion=true,
	}, FrameTimer)
	local destination = updatingFrameTimers and pendingFrameTimers or frameTimers
	destination[#destination + 1] = timer
	return timer
end

function playdate.frameTimer.performAfterDelay(delay, callback, ...)
	assert(type(callback) == "function", "frameTimer callback must be a function")
	return playdate.frameTimer.new(delay, callback, ...)
end

function playdate.frameTimer.allTimers()
	local active = {}
	for i=1,#frameTimers do
		if not frameTimers[i].removed then active[#active + 1] = frameTimers[i] end
	end
	return active
end

function playdate.frameTimer.updateTimers()
	local active = {}
	updatingFrameTimers = true
	for i=1,#frameTimers do
		local timer = frameTimers[i]
		if not timer.removed then
			if not timer.paused then
				if timer._delayRemaining == nil then
					timer._delayRemaining = math.max(0, math.floor(timer.delay or 0))
				end
				if timer._delayRemaining > 0 then
					timer._delayRemaining = timer._delayRemaining - 1
				else
					timer.frame = math.min(timer.duration, timer.frame + 1)
					local reversed = timer._reversed == true
					local from = reversed and timer.endValue or timer.startValue
					local to = reversed and timer.startValue or timer.endValue
					local easing = reversed and timer.reverseEasingFunction
						or timer.easingFunction or frameTimerLinear
					timer.value = easing(timer.frame, from, to - from,
						timer.duration, timer.easingAmplitude, timer.easingPeriod)
					frameTimerCall(timer.updateCallback,
						timer._callbackArguments, timer)

					if timer.frame >= timer.duration then
						local cycleComplete = true
						if timer.reverses and not reversed then
							timer._reversed = true
							timer.frame = 0
							cycleComplete = false
						elseif timer.repeats then
							timer._reversed = false
							timer.frame = 0
						else
							if timer.discardOnCompletion == false then
								timer.paused = true
							else
								timer.removed = true
							end
						end

						if cycleComplete then
							frameTimerCall(timer._completionCallback,
								timer._callbackArguments, timer)
							frameTimerCall(timer.timerEndedCallback,
								timer.timerEndedArgs, timer)
						end
					end
				end
			end
			if not timer.removed then active[#active + 1] = timer end
		end
	end
	updatingFrameTimers = false
	for i=1,#pendingFrameTimers do
		if not pendingFrameTimers[i].removed then
			active[#active + 1] = pendingFrameTimers[i]
		end
	end
	pendingFrameTimers = {}
	frameTimers = active
end
