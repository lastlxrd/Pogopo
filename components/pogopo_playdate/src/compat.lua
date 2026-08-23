-- Minimal, game-oriented replacements for the Playdate CoreLibs used by
-- PDSnake and Celeste Classic. Pixel work stays in the native C++ backend;
-- object, sprite, tilemap and input orchestration remains ordinary Lua 5.4.

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
					item.image:drawScaled(x, y, item.xScale, item.yScale)
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

function Sprite:checkCollisions(goalX, goalY)
	local previousX, previousY = self.x, self.y
	if not self.added or not self._collisionsEnabled or not self.collideRect then
		return goalX, goalY, {}, 0
	end
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
	local previousX, previousY = self.x, self.y
	local actualX, actualY, collisions, length = self:checkCollisions(goalX, goalY)
	local moveX, moveY = goalX - previousX, goalY - previousY
	for i=1,length do
		local collision = collisions[i]
		local sx, sy, sw, sh = collision.spriteRect.x, collision.spriteRect.y,
			collision.spriteRect.width, collision.spriteRect.height
		local ox, oy, ow, oh = collision.otherRect.x, collision.otherRect.y,
			collision.otherRect.width, collision.otherRect.height
		local previousLeft = sx - moveX
		local previousTop = sy - moveY
		local normalX, normalY = 0, 0
		if moveX > 0 and previousLeft + sw <= ox then normalX = -1
		elseif moveX < 0 and previousLeft >= ox + ow then normalX = 1 end
		if moveY > 0 and previousTop + sh <= oy then normalY = -1
		elseif moveY < 0 and previousTop >= oy + oh then normalY = 1 end
		if normalX ~= 0 and normalY ~= 0 then
			local penetrationX = normalX < 0 and (sx + sw - ox) or (ox + ow - sx)
			local penetrationY = normalY < 0 and (sy + sh - oy) or (oy + oh - sy)
			if penetrationX < penetrationY then normalY = 0 else normalX = 0 end
		elseif normalX == 0 and normalY == 0 then
			local left, right = sx + sw - ox, ox + ow - sx
			local top, bottom = sy + sh - oy, oy + oh - sy
			local minimum = math.min(left, right, top, bottom)
			if minimum == left then normalX = -1
			elseif minimum == right then normalX = 1
			elseif minimum == top then normalY = -1
			else normalY = 1 end
		end
		local response = self.collisionResponse and
			self:collisionResponse(collision.other) or Sprite.kCollisionTypeFreeze
		collision.normal = {x=normalX, y=normalY, dx=normalX, dy=normalY}
		collision.move = {x=moveX, y=moveY}
		collision.touch = {x=actualX, y=actualY}
		collision.type = response
		collision.overlaps = false
		collision.ti = 0
		if response ~= Sprite.kCollisionTypeOverlap then
			if response == Sprite.kCollisionTypeFreeze then
				actualX, actualY = previousX, previousY
			elseif normalX ~= 0 then
				local desiredLeft = normalX < 0 and (ox - sw) or (ox + ow)
				actualX = actualX + desiredLeft - sx
			elseif normalY ~= 0 then
				local desiredTop = normalY < 0 and (oy - sh) or (oy + oh)
				actualY = actualY + desiredTop - sy
			end
		end
	end
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
function Rect.fast_intersection(ax, ay, aw, ah, bx, by, bw, bh)
	local left = math.max(ax, bx)
	local top = math.max(ay, by)
	local right = math.min(ax + aw, bx + bw)
	local bottom = math.min(ay + ah, by + bh)
	return left, top, math.max(0, right - left), math.max(0, bottom - top)
end

local Vector2D = {}
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

playdate.geometry = {rect=Rect, vector2D=Vector2D}

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
