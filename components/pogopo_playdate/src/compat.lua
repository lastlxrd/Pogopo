-- Game-oriented Playdate/CoreLib compatibility for PogoDate. Pixel work stays
-- in the native C++ backend; geometry, object, sprite, tilemap, input and
-- utility orchestration remain ordinary Lua 5.4 to keep the API extensible.

Object = Object or {}
Object.__index = Object
function Object:init() end

local function objectIsA(value, candidate)
	if type(candidate) == "string" then candidate = _G[candidate] end
	if type(candidate) ~= "table" then return false end
	local current = getmetatable(value)
	while type(current) == "table" do
		if current == candidate then return true end
		current = rawget(current, "super")
	end
	return false
end

function Object:isa(candidate) return objectIsA(self, candidate) end
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

-- CoreLibs/utilities table additions.  Newer Noble Engine releases use
-- table.deepcopy() while constructing their settings defaults, before the
-- first game frame is allowed to run.  Keep these helpers general instead of
-- special-casing Noble or a package name.  The memo table makes deepcopy safe
-- for cyclic tables and preserves shared references between nested values.
function table.indexOfElement(source, element)
	for index, value in ipairs(source) do
		if value == element then return index end
	end
	return nil
end

function table.getsize(source)
	local arrayCount = #source
	local hashCount = 0
	for key in pairs(source) do
		if type(key) ~= "number" or key < 1 or key > arrayCount or key % 1 ~= 0 then
			hashCount = hashCount + 1
		end
	end
	return arrayCount, hashCount
end

function table.create(arrayCount, hashCount)
	-- Lua does not expose its table preallocation hint to Lua code.  Returning
	-- an ordinary empty table preserves the public behavior; the counts are
	-- accepted so games can use the same call signature as on Playdate.
	return {}
end

function table.shallowcopy(source, destination)
	destination = destination or {}
	for key, value in pairs(source) do destination[key] = value end
	return destination
end

local function deepcopyValue(value, memo)
	if type(value) ~= "table" then return value end
	local existing = memo[value]
	if existing then return existing end
	local copy = {}
	memo[value] = copy
	for key, nested in pairs(value) do
		copy[deepcopyValue(key, memo)] = deepcopyValue(nested, memo)
	end
	return setmetatable(copy, getmetatable(value))
end

function table.deepcopy(source)
	return deepcopyValue(source, {})
end

local gfx = playdate.graphics
local sprites = {}
local collision_sprites = {}
local wall_cells = {}
local background_callback = nil
local always_redraw = true
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
	self._sizeExplicit = false
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
	self.groupMask = 0
	self.collidesWithGroupsMask = 0
	self.tag = 0
	self._sequence = 0
end

function Sprite:isa(candidate) return objectIsA(self, candidate) end

function Sprite.new(image) return Sprite(image) end

function Sprite:setImage(image, flip, xScale, yScale)
	if self.image ~= image then
		self.image = image
		if image and not self._sizeExplicit then
			local width, height = image:getSize()
			self.width, self.height = width, height
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
function Sprite:setSize(width, height)
	self.width, self.height = width, height
	self._sizeExplicit = true
end
function Sprite:getSize() return self.width, self.height end
function Sprite:setCenter(x, y) self.centerX, self.centerY = x, y end
function Sprite:getCenter() return self.centerX, self.centerY end
function Sprite:moveTo(x, y) self.x, self.y = x, y end
function Sprite:moveBy(x, y) self.x, self.y = self.x + x, self.y + y end
function Sprite:getPosition() return self.x, self.y end
function Sprite:setRotation(value) self.rotation = value or 0 end
function Sprite:getRotation() return self.rotation end
function Sprite:setScale(xScale, yScale)
	self.xScale = xScale or 1
	self.yScale = yScale or self.xScale
end
function Sprite:getScale() return self.xScale or 1, self.yScale or self.xScale or 1 end
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
function Sprite:setImageFlip(value, flipCollideRect)
	local nextFlip = value or gfx.kImageUnflipped
	if flipCollideRect and self.collideRect then
		local changed = (self.flip or gfx.kImageUnflipped) ~ nextFlip
		if (changed & gfx.kImageFlippedX) ~= 0 then
			self.collideRect.x = self.width - self.collideRect.x -
				self.collideRect.width
		end
		if (changed & gfx.kImageFlippedY) ~= 0 then
			self.collideRect.y = self.height - self.collideRect.y -
				self.collideRect.height
		end
	end
	self.flip = nextFlip
end
function Sprite:getImageFlip() return self.flip or gfx.kImageUnflipped end
function Sprite:setTag(value) self.tag = math.max(0, math.min(255, value or 0)) end
function Sprite:getTag() return self.tag end
local function maskFromGroups(value)
	if value == nil then return 0 end
	if type(value) == "number" then
		local group = math.floor(value)
		if group < 1 or group > 32 then return 0 end
		return 1 << (group - 1)
	end
	local mask = 0
	for i=1,#value do
		local group = math.floor(value[i] or 0)
		if group >= 1 and group <= 32 then
			mask = mask | (1 << (group - 1))
		end
	end
	return mask
end
function Sprite:setGroups(value)
	self.groups = value or {}
	self.groupMask = maskFromGroups(value)
end
function Sprite:setCollidesWithGroups(value)
	self.collidesWithGroups = value or {}
	self.collidesWithGroupsMask = maskFromGroups(value)
end
function Sprite:setGroupMask(mask) self.groupMask = math.tointeger(mask) or 0 end
function Sprite:getGroupMask() return self.groupMask or 0 end
function Sprite:resetGroupMask() self.groupMask, self.groups = 0, {} end
function Sprite:setCollidesWithGroupsMask(mask)
	self.collidesWithGroupsMask = math.tointeger(mask) or 0
end
function Sprite:getCollidesWithGroupsMask()
	return self.collidesWithGroupsMask or 0
end
function Sprite:resetCollidesWithGroupsMask()
	self.collidesWithGroupsMask, self.collidesWithGroups = 0, {}
end
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
	-- Native Playdate sprite storage is initialized before a Lua subclass'
	-- init() runs.  A valid subclass therefore does not have to call
	-- super.init() just to receive the default visibility/update flags.
	-- Preserve that behavior for framework and game-defined sprite classes.
	if self.visible == nil then self.visible = true end
	if self._updatesEnabled == nil then self._updatesEnabled = true end
	if self._collisionsEnabled == nil then self._collisionsEnabled = true end
	if self.x == nil then self.x = 0 end
	if self.y == nil then self.y = 0 end
	if self.centerX == nil then self.centerX = 0.5 end
	if self.centerY == nil then self.centerY = 0.5 end
	if self.width == nil then self.width = 0 end
	if self.height == nil then self.height = 0 end
	if self._sizeExplicit == nil then self._sizeExplicit = false end
	if self.zIndex == nil then self.zIndex = 0 end
	if self.imageDrawMode == nil then self.imageDrawMode = gfx.kDrawModeCopy end
	if self.flip == nil then self.flip = gfx.kImageUnflipped end
	if self.xScale == nil then self.xScale = 1 end
	if self.yScale == nil then self.yScale = self.xScale end
	if self.rotation == nil then self.rotation = 0 end
	if self.groups == nil then self.groups = {} end
	if self.collidesWithGroups == nil then self.collidesWithGroups = {} end
	if self.groupMask == nil then self.groupMask = maskFromGroups(self.groups) end
	if self.collidesWithGroupsMask == nil then
		self.collidesWithGroupsMask = maskFromGroups(self.collidesWithGroups)
	end
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
				if item.xScale ~= 1 or item.yScale ~= 1 then
					-- A zero-degree scale does not need the general inverse
					-- rotation rasterizer. Keep the same centre semantics while
					-- using the much cheaper native nearest-neighbour path.
					local x = math.floor(item.x -
						item.width * item.xScale * item.centerX)
					local y = math.floor(item.y -
						item.height * item.yScale * item.centerY)
					-- The public image API has no flip argument, but sprite:setImage
					-- combines flip and scale.  The native compatibility method accepts
					-- this private fifth argument so scaled AnimatedSprite states keep
					-- their facing direction.
					item.image:drawScaled(x, y, item.xScale, item.yScale,
						item.flip)
				else
					local x = math.floor(item.x - item.width * item.centerX)
					local y = math.floor(item.y - item.height * item.centerY)
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
function Sprite.setAlwaysRedraw(value) always_redraw = value == true end
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

local function groupsAllow(left, right)
	local collides = left.collidesWithGroupsMask
	if collides == nil then collides = maskFromGroups(left.collidesWithGroups) end
	local groups = right.groupMask
	if groups == nil then groups = maskFromGroups(right.groups) end
	return (collides == 0 and groups == 0) or ((collides & groups) ~= 0)
end

local function sweptRect(sx, sy, sw, sh, moveX, moveY, ox, oy, ow, oh)
	if overlaps(sx, sy, sw, sh, ox, oy, ow, oh) then
		local left = sx + sw - ox
		local right = ox + ow - sx
		local top = sy + sh - oy
		local bottom = oy + oh - sy
		local minimum = math.min(left, right, top, bottom)
		local normalX, normalY = 0, 0
		if minimum == left then normalX = -1
		elseif minimum == right then normalX = 1
		elseif minimum == top then normalY = -1
		else normalY = 1 end
		return 0, normalX, normalY, true
	end

	local xEntry, xExit
	if moveX > 0 then
		xEntry = (ox - (sx + sw)) / moveX
		xExit = ((ox + ow) - sx) / moveX
	elseif moveX < 0 then
		xEntry = ((ox + ow) - sx) / moveX
		xExit = (ox - (sx + sw)) / moveX
	elseif sx + sw <= ox or sx >= ox + ow then
		return nil
	else
		xEntry, xExit = -math.huge, math.huge
	end

	local yEntry, yExit
	if moveY > 0 then
		yEntry = (oy - (sy + sh)) / moveY
		yExit = ((oy + oh) - sy) / moveY
	elseif moveY < 0 then
		yEntry = ((oy + oh) - sy) / moveY
		yExit = (oy - (sy + sh)) / moveY
	elseif sy + sh <= oy or sy >= oy + oh then
		return nil
	else
		yEntry, yExit = -math.huge, math.huge
	end

	local entry = math.max(xEntry, yEntry)
	local exit = math.min(xExit, yExit)
	if entry > exit or entry < 0 or entry > 1 then return nil end
	local normalX, normalY = 0, 0
	if xEntry > yEntry then normalX = moveX > 0 and -1 or 1
	else normalY = moveY > 0 and -1 or 1 end
	return entry, normalX, normalY, false
end

local function solveCollisions(self, goalX, goalY)
	local previousX, previousY = self.x, self.y
	if not self.added or not self._collisionsEnabled or not self.collideRect then
		return goalX, goalY, {}, 0
	end
	local sx, sy, sw, sh = spriteBounds(self)
	local moveX, moveY = goalX - previousX, goalY - previousY
	local broadX = math.min(sx, sx + moveX)
	local broadY = math.min(sy, sy + moveY)
	local broadWidth = sw + math.abs(moveX)
	local broadHeight = sh + math.abs(moveY)
	local candidates = Sprite.querySpritesInRect(
		broadX, broadY, broadWidth, broadHeight)
	local collisions = {}
	for i=1,#candidates do
		local other = candidates[i]
		if other ~= self and other.added and other._collisionsEnabled and
			other.collideRect and groupsAllow(self, other) then
			local ox, oy, ow, oh = spriteBounds(other)
			local ti, normalX, normalY, startedOverlapping =
				sweptRect(sx, sy, sw, sh, moveX, moveY, ox, oy, ow, oh)
			if ti then
				local touchX = previousX + moveX * ti
				local touchY = previousY + moveY * ti
				local collisionHandler = self.collisionResponse
				local response = Sprite.kCollisionTypeFreeze
				if type(collisionHandler) == "function" then
					response = collisionHandler(self, other) or response
				elseif type(collisionHandler) == "string" then
					-- Playdate also supports assigning the response constant
					-- directly instead of implementing a callback method.
					response = collisionHandler
				end
				collisions[#collisions + 1] = {
					sprite=self, other=other, type=response,
					overlaps=startedOverlapping, ti=ti,
					move={x=moveX * ti, y=moveY * ti},
					normal={x=normalX, y=normalY, dx=normalX, dy=normalY},
					touch={x=touchX, y=touchY},
					spriteRect={x=sx + moveX * ti, y=sy + moveY * ti,
						width=sw, height=sh},
					otherRect={x=ox,y=oy,width=ow,height=oh},
				}
			end
		end
	end
	table.sort(collisions, function(a, b)
		if a.ti == b.ti then
			return (a.other._sequence or 0) < (b.other._sequence or 0)
		end
		return a.ti < b.ti
	end)

	local actualX, actualY = goalX, goalY
	local blockedX, blockedY = false, false
	for i=1,#collisions do
		local collision = collisions[i]
		if collision.type ~= Sprite.kCollisionTypeOverlap then
			local touchX, touchY = collision.touch.x, collision.touch.y
			if collision.overlaps then
				-- Never eject an already-overlapping body to an arbitrary far edge.
				-- That old depenetration is what made Duel fighters visibly teleport.
				actualX, actualY = previousX, previousY
				blockedX, blockedY = true, true
			elseif collision.type == Sprite.kCollisionTypeFreeze then
				actualX, actualY = touchX, touchY
				blockedX, blockedY = true, true
			elseif collision.type == Sprite.kCollisionTypeBounce then
				local remainingX, remainingY = goalX - touchX, goalY - touchY
				local bounceX = touchX +
					(collision.normal.x ~= 0 and -remainingX or remainingX)
				local bounceY = touchY +
					(collision.normal.y ~= 0 and -remainingY or remainingY)
				collision.bounce = {x=bounceX, y=bounceY}
				actualX, actualY = bounceX, bounceY
				blockedX = blockedX or collision.normal.x ~= 0
				blockedY = blockedY or collision.normal.y ~= 0
			else
				local slideX = collision.normal.x ~= 0 and touchX or goalX
				local slideY = collision.normal.y ~= 0 and touchY or goalY
				collision.slide = {x=slideX, y=slideY}
				if collision.normal.x ~= 0 and not blockedX then
					actualX, blockedX = touchX, true
				end
				if collision.normal.y ~= 0 and not blockedY then
					actualY, blockedY = touchY, true
				end
			end
		end
	end
	return actualX, actualY, collisions, #collisions
end

function Sprite:checkCollisions(goalX, goalY)
	return solveCollisions(self, goalX, goalY)
end

function Sprite:moveWithCollisions(goalX, goalY)
	local actualX, actualY, collisions, length =
		solveCollisions(self, goalX, goalY)
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
					if item.added and item.collideRect and
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
		if item.added and item.collideRect and item._query_stamp ~= stamp then
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
		if other ~= self and other.added and
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
					result[#result + 1] = {left, right}
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
	if gfx._drawTilemap then
		gfx._drawTilemap(self.imagetable, self.tiles, self.width, x or 0, y or 0)
		return
	end
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

local Point
local Rect = {}
Rect.__index = Rect
function Rect.new(x, y, width, height)
	return setmetatable({x=x or 0,y=y or 0,width=width or 0,height=height or 0,
		w=width or 0,h=height or 0}, Rect)
end
function Rect:copy() return Rect.new(self.x, self.y, self.width, self.height) end
function Rect:unpack() return self.x, self.y, self.width, self.height end
function Rect:isEmpty() return self.width <= 0 or self.height <= 0 end
function Rect:isEqual(other)
	return other and self.x == other.x and self.y == other.y and
		self.width == (other.width or other.w) and
		self.height == (other.height or other.h)
end
function Rect:offset(x, y)
	self.x, self.y = self.x + (x or 0), self.y + (y or 0)
	return self
end
function Rect:offsetBy(x, y) return self:copy():offset(x, y) end
function Rect:inset(dx, dy)
	dy = dy == nil and dx or dy
	self.x, self.y = self.x + dx, self.y + dy
	self.width, self.height = self.width - 2 * dx, self.height - 2 * dy
	self.w, self.h = self.width, self.height
	return self
end
function Rect:insetBy(dx, dy) return self:copy():inset(dx, dy) end
function Rect:intersects(other)
	return overlaps(self.x,self.y,self.width,self.height,
		other.x,other.y,other.width or other.w,other.height or other.h)
end
function Rect:intersection(other)
	local x, y, w, h = Rect.fast_intersection(self.x, self.y, self.width,
		self.height, other.x, other.y, other.width or other.w,
		other.height or other.h)
	return Rect.new(x, y, w, h)
end
function Rect:union(other)
	local right = math.max(self.x + self.width, other.x + (other.width or other.w))
	local bottom = math.max(self.y + self.height, other.y + (other.height or other.h))
	local x, y = math.min(self.x, other.x), math.min(self.y, other.y)
	return Rect.new(x, y, right - x, bottom - y)
end
function Rect:containsPoint(x, y)
	if type(x) == "table" then x, y = x.x, x.y end
	return x >= self.x and x <= self.x + self.width and
		y >= self.y and y <= self.y + self.height
end
function Rect:containsRect(other)
	return self:containsPoint(other.x, other.y) and self:containsPoint(
		other.x + (other.width or other.w), other.y + (other.height or other.h))
end
function Rect:centerPoint()
	return Point.new(self.x + self.width / 2, self.y + self.height / 2)
end
function Rect.fast_intersection(ax, ay, aw, ah, bx, by, bw, bh)
	local left = math.max(ax, bx)
	local top = math.max(ay, by)
	local right = math.min(ax + aw, bx + bw)
	local bottom = math.min(ay + ah, by + bh)
	return left, top, math.max(0, right - left), math.max(0, bottom - top)
end
function Rect.fast_union(ax, ay, aw, ah, bx, by, bw, bh)
	local x, y = math.min(ax, bx), math.min(ay, by)
	return x, y, math.max(ax + aw, bx + bw) - x,
		math.max(ay + ah, by + bh) - y
end
Rect.__eq = Rect.isEqual
Rect.__tostring = function(value)
	return string.format("(%g, %g, %g, %g)", value.x, value.y,
		value.width, value.height)
end

Point = {}
Point.__index = Point
function Point.new(x, y) return setmetatable({x=x or 0, y=y or 0}, Point) end
function Point:copy() return Point.new(self.x, self.y) end
function Point:unpack() return self.x, self.y end
function Point:offset(x, y)
	self.x, self.y = self.x + (x or 0), self.y + (y or 0)
	return self
end
function Point:offsetBy(x, y) return self:copy():offset(x, y) end
function Point:squaredDistanceToPoint(other)
	local dx, dy = self.x - other.x, self.y - other.y
	return dx * dx + dy * dy
end
function Point:distanceToPoint(other) return math.sqrt(self:squaredDistanceToPoint(other)) end
Point.__add = function(a, b) return Point.new(a.x + b.x, a.y + b.y) end
Point.__sub = function(a, b) return Point.new(a.x - b.x, a.y - b.y) end
Point.__eq = function(a, b) return a.x == b.x and a.y == b.y end
Point.__tostring = function(value) return string.format("(%g, %g)", value.x, value.y) end

local Size = {}
Size.__index = Size
function Size.new(width, height)
	return setmetatable({width=width or 0, height=height or 0,
		w=width or 0, h=height or 0}, Size)
end
function Size:copy() return Size.new(self.width, self.height) end
function Size:unpack() return self.width, self.height end
Size.__eq = function(a, b)
	return a.width == (b.width or b.w) and a.height == (b.height or b.h)
end
Size.__tostring = function(value)
	return string.format("(%g, %g)", value.width, value.height)
end

local function segmentIntersection(ax, ay, bx, by, cx, cy, dx, dy)
	local rx, ry, sx, sy = bx - ax, by - ay, dx - cx, dy - cy
	local denominator = rx * sy - ry * sx
	if math.abs(denominator) < 1e-12 then return nil end
	local qx, qy = cx - ax, cy - ay
	local t = (qx * sy - qy * sx) / denominator
	local u = (qx * ry - qy * rx) / denominator
	if t < 0 or t > 1 or u < 0 or u > 1 then return nil end
	return ax + t * rx, ay + t * ry
end

local Vector2D
local LineSegment = {}
LineSegment.__index = LineSegment
function LineSegment.new(x1, y1, x2, y2)
	if type(x1) == "table" then x1, y1, x2, y2 = x1.x, x1.y, y1.x, y1.y end
	return setmetatable({x1=x1 or 0,y1=y1 or 0,x2=x2 or 0,y2=y2 or 0}, LineSegment)
end
function LineSegment:copy() return LineSegment.new(self.x1,self.y1,self.x2,self.y2) end
function LineSegment:unpack() return self.x1,self.y1,self.x2,self.y2 end
function LineSegment:length()
	local dx, dy = self.x2-self.x1, self.y2-self.y1
	return math.sqrt(dx*dx+dy*dy)
end
function LineSegment:offset(x,y)
	self.x1,self.y1,self.x2,self.y2=self.x1+x,self.y1+y,self.x2+x,self.y2+y
	return self
end
function LineSegment:offsetBy(x,y) return self:copy():offset(x,y) end
function LineSegment:midPoint() return Point.new((self.x1+self.x2)/2,(self.y1+self.y2)/2) end
function LineSegment:pointOnLine(distance, extend)
	local length=self:length()
	if length == 0 then return Point.new(self.x1,self.y1) end
	local t=(distance or 0)/length
	if not extend then t=math.max(0,math.min(1,t)) end
	return Point.new(self.x1+(self.x2-self.x1)*t,self.y1+(self.y2-self.y1)*t)
end
function LineSegment:segmentVector() return Vector2D and Vector2D.new(self.x2-self.x1,self.y2-self.y1) end
function LineSegment:closestPointOnLineToPoint(point)
	local dx,dy=self.x2-self.x1,self.y2-self.y1
	local divisor=dx*dx+dy*dy
	local t=divisor==0 and 0 or ((point.x-self.x1)*dx+(point.y-self.y1)*dy)/divisor
	t=math.max(0,math.min(1,t))
	return Point.new(self.x1+t*dx,self.y1+t*dy)
end
function LineSegment:intersectsLineSegment(other)
	local x,y=segmentIntersection(self.x1,self.y1,self.x2,self.y2,
		other.x1,other.y1,other.x2,other.y2)
	if x then return Point.new(x,y) end
	return nil
end

Vector2D = {}
Vector2D.__index = Vector2D
function Vector2D.new(x, y)
	return setmetatable({x=x or 0, y=y or 0, dx=x or 0, dy=y or 0}, Vector2D)
end
function Vector2D:copy() return Vector2D.new(self.x, self.y) end
function Vector2D:unpack() return self.x, self.y end
function Vector2D:magnitudeSquared() return self.x * self.x + self.y * self.y end
function Vector2D:magnitude() return math.sqrt(self:magnitudeSquared()) end
function Vector2D:normalize()
	local length = self:magnitude()
	if length > 0 then self.x, self.y = self.x / length, self.y / length end
	self.dx, self.dy = self.x, self.y
	return self
end
function Vector2D:normalized() return self:copy():normalize() end
function Vector2D:scale(amount)
	self.x, self.y = self.x * amount, self.y * amount
	self.dx, self.dy = self.x, self.y
	return self
end
function Vector2D:scaledBy(amount) return self:copy():scale(amount) end
function Vector2D:addVector(other)
	self.x, self.y = self.x + other.x, self.y + other.y
	self.dx, self.dy = self.x, self.y
	return self
end
function Vector2D:dotProduct(other) return self.x * other.x + self.y * other.y end
function Vector2D:angleBetween(other)
	local lengths = self:magnitude() * other:magnitude()
	if lengths == 0 then return 0 end
	local cosine = math.max(-1, math.min(1, self:dotProduct(other) / lengths))
	return math.deg(math.acos(cosine))
end
Vector2D.__add = function(a, b) return Vector2D.new(a.x + b.x, a.y + b.y) end
Vector2D.__sub = function(a, b) return Vector2D.new(a.x - b.x, a.y - b.y) end
Vector2D.__unm = function(a) return Vector2D.new(-a.x, -a.y) end
Vector2D.__mul = function(a, b)
	if type(a) == "number" then return b:scaledBy(a) end
	if type(b) == "number" then return a:scaledBy(b) end
	return a:dotProduct(b)
end
Vector2D.__div = function(a, b) return a:scaledBy(1 / b) end

local Polygon = {}
Polygon.__index = Polygon
function Polygon.new(...)
	local args={...}
	if #args == 1 and type(args[1]) == "table" then args=args[1] end
	local points={}
	if #args > 0 and type(args[1]) == "table" then
		for i=1,#args do points[#points+1]=Point.new(args[i].x,args[i].y) end
	else
		for i=1,#args-1,2 do points[#points+1]=Point.new(args[i],args[i+1]) end
	end
	return setmetatable({points=points,_closed=false},Polygon)
end
function Polygon:copy()
	local value=Polygon.new(self.points); value._closed=self._closed; return value
end
function Polygon:close()
	self._closed=true
	local first,last=self.points[1],self.points[#self.points]
	if first and last and (first.x~=last.x or first.y~=last.y) then
		self.points[#self.points+1]=first:copy()
	end
	return self
end
function Polygon:isClosed() return self._closed end
function Polygon:count() return #self.points end
function Polygon:getPointAt(index) return self.points[index] and self.points[index]:copy() end
function Polygon:setPointAt(index,point) self.points[index]=Point.new(point.x,point.y) end
function Polygon:getBounds()
	if #self.points==0 then return Rect.new(0,0,0,0) end
	local left,right,top,bottom=self.points[1].x,self.points[1].x,
		self.points[1].y,self.points[1].y
	for i=2,#self.points do local p=self.points[i]
		left,right=math.min(left,p.x),math.max(right,p.x)
		top,bottom=math.min(top,p.y),math.max(bottom,p.y)
	end
	return Rect.new(left,top,right-left,bottom-top)
end
function Polygon:containsPoint(x,y)
	if type(x)=="table" then x,y=x.x,x.y end
	local inside=false
	local count=#self.points
	for i=1,count do
		local a,b=self.points[i],self.points[i%count+1]
		if ((a.y>y)~=(b.y>y)) and x < (b.x-a.x)*(y-a.y)/(b.y-a.y)+a.x then
			inside=not inside
		end
	end
	return inside
end
function Polygon:length()
	local total=0
	for i=1,#self.points-1 do total=total+self.points[i]:distanceToPoint(self.points[i+1]) end
	if self._closed and #self.points>2 then
		local first,last=self.points[1],self.points[#self.points]
		if first~=last then total=total+last:distanceToPoint(first) end
	end
	return total
end
function Polygon:translate(x,y)
	for i=1,#self.points do self.points[i]:offset(x,y) end
	return self
end
function Polygon:translatedBy(x,y) return self:copy():translate(x,y) end
function Polygon:pointOnPolygon(distance)
	local remaining=math.max(0,distance or 0)
	for i=1,#self.points-1 do
		local segment=LineSegment.new(self.points[i],self.points[i+1])
		local length=segment:length()
		if remaining<=length then return segment:pointOnLine(remaining) end
		remaining=remaining-length
	end
	return self.points[#self.points] and self.points[#self.points]:copy() or Point.new()
end
function Rect:toPolygon()
	return Polygon.new(self.x,self.y,self.x+self.width,self.y,
		self.x+self.width,self.y+self.height,self.x,self.y+self.height):close()
end

local Arc = {}
Arc.__index = Arc
function Arc.new(x,y,radius,startAngle,endAngle,direction)
	return setmetatable({x=x or 0,y=y or 0,radius=radius or 0,
		startAngle=startAngle or 0,endAngle=endAngle or 0,
		clockwise=direction==nil and (endAngle or 0)>=(startAngle or 0) or direction==true},Arc)
end
function Arc:copy() return Arc.new(self.x,self.y,self.radius,self.startAngle,self.endAngle,self.clockwise) end
function Arc:isClockwise() return self.clockwise end
function Arc:setIsClockwise(value) self.clockwise=value==true end
function Arc:length() return math.rad(math.abs(self.endAngle-self.startAngle))*self.radius end
function Arc:pointOnArc(distance,extend)
	local length=self:length()
	local t=length==0 and 0 or (distance or 0)/length
	if not extend then t=math.max(0,math.min(1,t)) end
	local delta=math.abs(self.endAngle-self.startAngle)*(self.clockwise and 1 or -1)
	local angle=math.rad(self.startAngle+delta*t-90)
	return Point.new(self.x+math.cos(angle)*self.radius,self.y+math.sin(angle)*self.radius)
end

local AffineTransform = {}
AffineTransform.__index = AffineTransform
function AffineTransform.new(m11,m12,m21,m22,tx,ty)
	return setmetatable({m11=m11 or 1,m12=m12 or 0,m21=m21 or 0,
		m22=m22 or 1,tx=tx or 0,ty=ty or 0},AffineTransform)
end
function AffineTransform:copy()
	return AffineTransform.new(self.m11,self.m12,self.m21,self.m22,self.tx,self.ty)
end
function AffineTransform:reset()
	self.m11,self.m12,self.m21,self.m22,self.tx,self.ty=1,0,0,1,0,0
	return self
end
function AffineTransform:concat(other)
	local a,b,c,d,tx,ty=self.m11,self.m12,self.m21,self.m22,self.tx,self.ty
	self.m11=a*other.m11+c*other.m12
	self.m12=b*other.m11+d*other.m12
	self.m21=a*other.m21+c*other.m22
	self.m22=b*other.m21+d*other.m22
	self.tx=a*other.tx+c*other.ty+tx
	self.ty=b*other.tx+d*other.ty+ty
	return self
end
function AffineTransform:translate(x,y)
	return self:concat(AffineTransform.new(1,0,0,1,x or 0,y or 0))
end
function AffineTransform:translatedBy(x,y) return self:copy():translate(x,y) end
function AffineTransform:scale(x,y)
	y=y==nil and x or y
	return self:concat(AffineTransform.new(x,0,0,y,0,0))
end
function AffineTransform:scaledBy(x,y) return self:copy():scale(x,y) end
function AffineTransform:rotate(angle,x,y)
	if type(x)=="table" then x,y=x.x,x.y end
	if x or y then self:translate(x or 0,y or 0) end
	local r=math.rad(angle or 0); local c,s=math.cos(r),math.sin(r)
	self:concat(AffineTransform.new(c,s,-s,c,0,0))
	if x or y then self:translate(-(x or 0),-(y or 0)) end
	return self
end
function AffineTransform:rotatedBy(angle,x,y) return self:copy():rotate(angle,x,y) end
function AffineTransform:skew(xAngle,yAngle)
	return self:concat(AffineTransform.new(1,math.tan(math.rad(yAngle or 0)),
		math.tan(math.rad(xAngle or 0)),1,0,0))
end
function AffineTransform:skewedBy(x,y) return self:copy():skew(x,y) end
function AffineTransform:invert()
	local determinant=self.m11*self.m22-self.m12*self.m21
	if math.abs(determinant)<1e-12 then return nil end
	local a,b,c,d,tx,ty=self.m11,self.m12,self.m21,self.m22,self.tx,self.ty
	self.m11,self.m12=d/determinant,-b/determinant
	self.m21,self.m22=-c/determinant,a/determinant
	self.tx,self.ty=(c*ty-d*tx)/determinant,(b*tx-a*ty)/determinant
	return self
end
function AffineTransform:transformXY(x,y)
	return self.m11*x+self.m21*y+self.tx,self.m12*x+self.m22*y+self.ty
end
function AffineTransform:transformPoint(point)
	point.x,point.y=self:transformXY(point.x,point.y); return point
end
function AffineTransform:transformedPoint(point) return self:transformPoint(point:copy()) end
function AffineTransform:transformLineSegment(line)
	local x1,y1=self:transformXY(line.x1,line.y1)
	local x2,y2=self:transformXY(line.x2,line.y2)
	line.x1,line.y1,line.x2,line.y2=x1,y1,x2,y2; return line
end
function AffineTransform:transformedLineSegment(line) return self:transformLineSegment(line:copy()) end
function AffineTransform:transformPolygon(polygon)
	for i=1,#polygon.points do self:transformPoint(polygon.points[i]) end
	return polygon
end
function AffineTransform:transformedPolygon(polygon) return self:transformPolygon(polygon:copy()) end
function AffineTransform:transformAABB(rect)
	local bounds=self:transformedPolygon(rect:toPolygon()):getBounds()
	rect.x,rect.y,rect.width,rect.height=bounds.x,bounds.y,bounds.width,bounds.height
	rect.w,rect.h=rect.width,rect.height
	return rect
end
function AffineTransform:transformedAABB(rect) return self:transformAABB(rect:copy()) end
AffineTransform.__mul=function(a,b)
	if getmetatable(b)==AffineTransform then return a:copy():concat(b) end
	if getmetatable(b)==Point then return a:transformedPoint(b) end
	if getmetatable(b)==Vector2D then
		return Vector2D.new(a.m11*b.x+a.m21*b.y,a.m12*b.x+a.m22*b.y)
	end
	error("unsupported affine transform multiplication",2)
end

playdate.geometry = {rect=Rect, point=Point, size=Size, vector2D=Vector2D,
	lineSegment=LineSegment, polygon=Polygon, arc=Arc,
	affineTransform=AffineTransform}

-- Core graphics primitives which are inexpensive to compose from the native
-- line/rectangle/pixel backend. These accept both numeric vertices and the
-- geometry polygon type used by CoreLibs.
local function polygonCoordinates(...)
	local args={...}
	if type(args[1])=="table" and args[1].points then
		local coordinates={}
		for i=1,#args[1].points do
			coordinates[#coordinates+1]=args[1].points[i].x
			coordinates[#coordinates+1]=args[1].points[i].y
		end
		return coordinates
	end
	return args
end
function gfx.drawPolygon(...)
	local args={...}; local polygon=type(args[1])=="table" and args[1].points and args[1]
	local p=polygonCoordinates(...)
	for i=1,#p-3,2 do gfx.drawLine(p[i],p[i+1],p[i+2],p[i+3]) end
	if #p>=6 and (not polygon or polygon:isClosed()) then
		gfx.drawLine(p[#p-1],p[#p],p[1],p[2])
	end
end
function gfx.fillPolygon(...)
	local args={...}; local polygon=type(args[1])=="table" and args[1].points and args[1]
	if polygon and not polygon:isClosed() then error("polygon must be closed",2) end
	local p=polygonCoordinates(...)
	if #p%2==1 then p[#p]=nil end -- optional fill-rule argument
	if #p<6 then return end
	local minY,maxY=p[2],p[2]
	for i=4,#p,2 do minY,maxY=math.min(minY,p[i]),math.max(maxY,p[i]) end
	for y=math.ceil(minY),math.floor(maxY) do
		local nodes={}
		local j=#p-1
		for i=1,#p-1,2 do
			local xi,yi,xj,yj=p[i],p[i+1],p[j],p[j+1]
			if (yi<y and yj>=y) or (yj<y and yi>=y) then
				nodes[#nodes+1]=xi+(y-yi)/(yj-yi)*(xj-xi)
			end
			j=i
		end
		table.sort(nodes)
		for i=1,#nodes-1,2 do gfx.drawLine(math.ceil(nodes[i]),y,math.floor(nodes[i+1]),y) end
	end
end
function gfx.drawTriangle(x1,y1,x2,y2,x3,y3)
	gfx.drawLine(x1,y1,x2,y2); gfx.drawLine(x2,y2,x3,y3); gfx.drawLine(x3,y3,x1,y1)
end
function gfx.fillTriangle(...) gfx.fillPolygon(...) end
local function ellipseRect(x,y,w,h)
	if type(x)=="table" then return x.x,x.y,x.width or x.w,x.height or x.h end
	return x,y,w,h
end
function gfx.drawEllipseInRect(x,y,w,h,startAngle,endAngle)
	x,y,w,h=ellipseRect(x,y,w,h)
	startAngle,endAngle=startAngle or 0,endAngle or 360
	local steps=math.max(12,math.ceil(math.pi*math.max(w,h)/2))
	local previousX,previousY
	for i=0,steps do
		local angle=math.rad(startAngle+(endAngle-startAngle)*i/steps-90)
		local px=x+w/2+math.cos(angle)*w/2
		local py=y+h/2+math.sin(angle)*h/2
		if previousX then gfx.drawLine(previousX,previousY,px,py) end
		previousX,previousY=px,py
	end
end
function gfx.fillEllipseInRect(x,y,w,h,startAngle,endAngle)
	x,y,w,h=ellipseRect(x,y,w,h)
	if startAngle or endAngle then
		local points={x+w/2,y+h/2}
		local first,last=startAngle or 0,endAngle or 360
		local steps=math.max(12,math.ceil(math.pi*math.max(w,h)/2))
		for i=0,steps do local angle=math.rad(first+(last-first)*i/steps-90)
			points[#points+1]=x+w/2+math.cos(angle)*w/2
			points[#points+1]=y+h/2+math.sin(angle)*h/2
		end
		return gfx.fillPolygon(table.unpack(points))
	end
	for py=math.ceil(y),math.floor(y+h) do
		local normalized=(py-(y+h/2))/(h/2)
		if math.abs(normalized)<=1 then
			local half=w/2*math.sqrt(1-normalized*normalized)
			gfx.drawLine(math.ceil(x+w/2-half),py,math.floor(x+w/2+half),py)
		end
	end
end
gfx.drawEllipse = gfx.drawEllipseInRect
gfx.fillEllipse = gfx.fillEllipseInRect
local lineCapStyle=gfx.kLineCapStyleButt
function gfx.setLineCapStyle(value) lineCapStyle=value end
function gfx.getLineCapStyle() return lineCapStyle end
local polygonFillRule=gfx.kPolygonFillNonZero
function gfx.setPolygonFillRule(value) polygonFillRule=value end
function gfx.getPolygonFillRule() return polygonFillRule end

-- A functional system-menu model. The physical Pogopo shell does not render
-- Playdate's system overlay yet, but games can create, inspect, mutate and
-- remove menu items and invoke their callbacks without nil failures.
local systemMenu={items={}}
local MenuItem={}; MenuItem.__index=MenuItem
function MenuItem:getTitle() return self.title end
function MenuItem:setTitle(value) self.title=tostring(value or "") end
function MenuItem:getValue() return self.value end
function MenuItem:setValue(value)
	self.value=value
	if self.callback then self.callback(value) end
end
function systemMenu:addMenuItem(title,callback)
	local item=setmetatable({title=tostring(title or ""),callback=callback},MenuItem)
	self.items[#self.items+1]=item; return item
end
function systemMenu:addCheckmarkMenuItem(title,checked,callback)
	local item=self:addMenuItem(title,callback); item.value=checked==true; return item
end
function systemMenu:addOptionsMenuItem(title,options,initialValue,callback)
	local item=self:addMenuItem(title,callback); item.options=options or {}
	item.value=initialValue or item.options[1]; return item
end
function systemMenu:removeMenuItem(item)
	for i=#self.items,1,-1 do if self.items[i]==item then table.remove(self.items,i); return end end
end
function systemMenu:removeAllMenuItems() self.items={} end
function playdate.getSystemMenu() return systemMenu end

-- json.decode remains native; the encoder is deliberately pure Lua so it is
-- available to packages and datastore utilities without growing firmware.
json=json or {}
json.null=json.null or setmetatable({}, {__tostring=function() return "null" end})
local function jsonEscape(value)
	return '"'..value:gsub('[%z\1-\31\\"]',function(character)
		local escapes={['"']='\\"',['\\']='\\\\',['\b']='\\b',['\f']='\\f',
			['\n']='\\n',['\r']='\\r',['\t']='\\t'}
		return escapes[character] or string.format('\\u%04x',string.byte(character))
	end)..'"'
end
local function jsonEncodeValue(value,pretty,depth,seen)
	local kind=type(value)
	if value==json.null or kind=="nil" then return "null" end
	if kind=="boolean" then return value and "true" or "false" end
	if kind=="number" then
		if value~=value or value==math.huge or value==-math.huge then return "null" end
		return tostring(value)
	end
	if kind=="string" then return jsonEscape(value) end
	if kind~="table" then error("cannot encode JSON value of type "..kind,2) end
	if seen[value] then error("cannot encode a cyclic table",2) end
	seen[value]=true
	local maximum,count,array=0,0,true
	for key in pairs(value) do
		count=count+1
		if type(key)~="number" or key<1 or key%1~=0 then array=false else maximum=math.max(maximum,key) end
	end
	array=array and maximum==count
	local pieces={}
	local newline=pretty and "\n" or ""
	local separator=pretty and ": " or ":"
	local indent=pretty and string.rep("  ",depth+1) or ""
	if array then
		for i=1,maximum do pieces[i]=indent..jsonEncodeValue(value[i],pretty,depth+1,seen) end
	else
		local keys={}; for key in pairs(value) do keys[#keys+1]=key end
		table.sort(keys,function(a,b) return tostring(a)<tostring(b) end)
		for i=1,#keys do local key=keys[i]
			pieces[i]=indent..jsonEscape(tostring(key))..separator..
				jsonEncodeValue(value[key],pretty,depth+1,seen)
		end
	end
	seen[value]=nil
	local open,close=array and "[" or "{",array and "]" or "}"
	if #pieces==0 then return open..close end
	local tail=pretty and ("\n"..string.rep("  ",depth)) or ""
	return open..newline..table.concat(pieces,","..newline)..tail..close
end
function json.encode(value) return jsonEncodeValue(value,false,0,{}) end
function json.encodePretty(value) return jsonEncodeValue(value,true,0,{}) end
function json.encodeToFile(path,second,third)
	local pretty,value
	if type(second)=="boolean" then pretty,value=second,third else pretty,value=false,second end
	local file=type(path)=="string" and playdate.file.open(path,playdate.file.kFileWrite) or path
	if not file then return false end
	local result=file:write(pretty and json.encodePretty(value) or json.encode(value))
	if type(path)=="string" then file:close() end
	return result~=false
end

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

-- Input handlers are a first-responder stack. For each individual event, the
-- first table that implements the callback receives it. A masking handler
-- stops the search even when it does not implement that callback. The global
-- playdate table is the documented bottom of the stack; Pulp installs its
-- A/B/D-pad callbacks there rather than pushing a separate input handler.
local function dispatchButtonEvent(callbackName)
	local stack = playdate.inputHandlers.stack
	for index=#stack,1,-1 do
		local entry = stack[index]
		if entry then
			local callback = entry.handler and entry.handler[callbackName]
			if callback then
				callback()
				return true
			end
			if entry.exclusive then return true end
		end
	end
	local callback = playdate[callbackName]
	if callback then
		callback()
		return true
	end
	return false
end

function _pogodate_dispatch_input(pressed, released)
	for i=1,#buttonNames do
		local mask, name = buttonNames[i][1], buttonNames[i][2]
		if (pressed & mask) ~= 0 then
			dispatchButtonEvent(name .. "ButtonDown")
		end
		if (released & mask) ~= 0 then
			dispatchButtonEvent(name .. "ButtonUp")
		end
	end
end

-- Crank events use the same responder stack as Playdate. The first handler
-- that implements cranked() receives movement; an exclusive handler masks the
-- handlers below it even when it does not implement the callback. If no stack
-- entry handles movement, the playdate.cranked callback is the final responder.
function _pogodate_dispatch_crank(change, acceleratedChange, dockEvent)
	local consumed = false
	if change ~= 0 then
		local stack = playdate.inputHandlers.stack
		local masked = false
		for index=#stack,1,-1 do
			local entry = stack[index]
			local callback = entry.handler.cranked
			if callback then
				callback(change, acceleratedChange)
				consumed = true
				break
			end
			if entry.exclusive then
				masked = true
				break
			end
		end
		if not consumed and not masked and type(playdate.cranked) == "function" then
			playdate.cranked(change, acceleratedChange)
			consumed = true
		end
	end
	if dockEvent > 0 and type(playdate.crankDocked) == "function" then
		playdate.crankDocked()
	elseif dockEvent < 0 and type(playdate.crankUndocked) == "function" then
		playdate.crankUndocked()
	end
	return consumed
end

local Gridview = {}
Gridview.__index = Gridview
function Gridview.new(cellWidth, cellHeight)
	return setmetatable({
		cellWidth=cellWidth, cellHeight=cellHeight,
		sections=1, rows=1, columns=1,
		selectedSection=1, selectedRow=1, selectedColumn=1,
		paddingLeft=0, paddingRight=0, paddingTop=0, paddingBottom=0,
		needsdisplay=true,
	}, Gridview)
end
function Gridview:setNumberOfSections(value)
	self.sections = math.max(1, value or 1)
	self.selectedSection = math.min(self.selectedSection, self.sections)
end
function Gridview:setNumberOfColumns(value)
	self.columns = math.max(1, value or 1)
	self.selectedColumn = math.min(self.selectedColumn, self.columns)
	self.needsdisplay = true
end
function Gridview:setNumberOfRows(value)
	self.rows = math.max(1, value or 1)
	self.selectedRow = math.min(self.selectedRow, self.rows)
	self.needsdisplay = true
end
function Gridview:setCellPadding(left, right, top, bottom)
	self.paddingLeft = left or 0
	self.paddingRight = right or 0
	self.paddingTop = top or 0
	self.paddingBottom = bottom or 0
end
function Gridview:setContentInset() end
function Gridview:setHorizontalDividerHeight() end
function Gridview:addHorizontalDividerAbove() end
function Gridview:getSelection()
	return self.selectedSection, self.selectedRow, self.selectedColumn
end
function Gridview:getSelectedRow() return self.selectedRow end
function Gridview:getSelectedColumn() return self.selectedColumn end
function Gridview:setSelection(section, row, column)
	if column == nil then
		column = row
		row = section
		section = 1
	end
	self.selectedSection = math.max(1, math.min(self.sections, section or 1))
	self.selectedRow = math.max(1, math.min(self.rows, row or 1))
	self.selectedColumn = math.max(1, math.min(self.columns, column or 1))
	self.needsdisplay = true
end
function Gridview:scrollToCell() end
function Gridview:selectPreviousRow(wrap)
	self.selectedRow = self.selectedRow - 1
	if self.selectedRow < 1 then self.selectedRow = wrap and self.rows or 1 end
	self.needsdisplay = true
end
function Gridview:selectNextRow(wrap)
	self.selectedRow = self.selectedRow + 1
	if self.selectedRow > self.rows then self.selectedRow = wrap and 1 or self.rows end
	self.needsdisplay = true
end
function Gridview:selectPreviousColumn(wrap)
	self.selectedColumn = self.selectedColumn - 1
	if self.selectedColumn < 1 then
		self.selectedColumn = wrap and self.columns or 1
	end
	self.needsdisplay = true
end
function Gridview:selectNextColumn(wrap)
	self.selectedColumn = self.selectedColumn + 1
	if self.selectedColumn > self.columns then
		self.selectedColumn = wrap and 1 or self.columns
	end
	self.needsdisplay = true
end
function Gridview:drawInRect(x, y, width, height)
	local rowHeight = self.cellHeight > 0 and self.cellHeight or 8
	local columnWidth = self.cellWidth > 0 and self.cellWidth or math.floor(width / self.columns)
	for row=1,self.rows do
		for column=1,self.columns do
			local left = x + (column-1) * (columnWidth + self.paddingLeft + self.paddingRight)
			local top = y + (row-1) * (rowHeight + self.paddingTop + self.paddingBottom)
			if left < x + width and top < y + height and self.drawCell then
				local selected = row == self.selectedRow and
					column == self.selectedColumn and self.selectedSection == 1
				self:drawCell(1, row, column, selected,
					left, top, columnWidth, rowHeight)
			end
		end
	end
	self.needsdisplay = false
end
local CrankIndicator = {
	clockwise=true,
	_frame=0,
}
function CrankIndicator:draw(xOffset, yOffset)
	xOffset, yOffset = xOffset or 0, yOffset or 0
	self._frame = (self._frame + 1) % 42
	local x, y = 344 + xOffset, 182 + yOffset
	local centerX, centerY = x + 26, y + 30
	local direction = self.clockwise == false and -1 or 1
	local angle = math.rad((self._frame * direction * 360 / 42) - 90)
	local handleX = math.floor(centerX + math.cos(angle) * 14 + 0.5)
	local handleY = math.floor(centerY + math.sin(angle) * 14 + 0.5)
	playdate.graphics.drawText("CRANK", x + 2, y)
	playdate.graphics.drawCircleAtPoint(centerX, centerY, 16)
	playdate.graphics.drawLine(centerX, centerY, handleX, handleY)
	playdate.graphics.fillCircleAtPoint(handleX, handleY, 3)
end
function CrankIndicator:resetAnimation() self._frame = 0 end
function CrankIndicator:getBounds() return 344, 182, 56, 58 end

playdate.ui = {gridview=Gridview, crankIndicator=CrankIndicator}

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
