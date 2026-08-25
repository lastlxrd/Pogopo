-- Game-oriented Playdate/CoreLib compatibility for PogoDate. Pixel work stays
-- in the native C++ backend; geometry, object, sprite, tilemap, input and
-- utility orchestration remain ordinary Lua 5.4 to keep the API extensible.

Object = Object or {}
Object.__index = Object
Object.class = Object
Object.className = "Object"
function Object:init() end
function Object.baseObject() return {} end

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

function Object:isa(candidate)
	if type(candidate)=="string" then candidate=_G[candidate] end
	local current=self
	while type(current)=="table" do
		if current==candidate then return true end
		current=rawget(current,"super")
	end
	return false
end
function Object:tableDump(indent,source)
	indent=indent or 0; source=source or self
	for key,value in pairs(source) do
		if key~="__index" and key~="class" and key~="super" then
			print(string.rep("  ",indent)..tostring(key)..": "..tostring(value))
		end
	end
	local parent=rawget(source,"super")
	if type(parent)=="table" and parent.className~="Object" then
		parent:tableDump(indent+1,parent)
	end
end
function printTable(...)
	local seen={}
	local function render(value,indent)
		if type(value)~="table" then return tostring(value) end
		if seen[value] then return "<table reference>" end
		seen[value]=true
		local rows={"{"}
		for key,nested in pairs(value) do
			rows[#rows+1]=string.rep("\t",indent+1).."["..tostring(key).."] = "..
				render(nested,indent+1).."," 
		end
		rows[#rows+1]=string.rep("\t",indent).."}"
		return table.concat(rows,"\n")
	end
	local values={...}
	for index=1,#values do values[index]=render(values[index],0) end
	print(table.unpack(values))
end
setmetatable(Object, {
	__call = function(cls, ...)
		local value = cls.baseObject()
		setmetatable(value,cls)
		value.super=cls
		cls.init(value,...)
		return value
	end,
})

function class(name,properties,namespace)
	return { extends=function(parent)
		if type(parent)=="string" then parent=_G[parent] end
		parent=parent or Object
		local child=properties or {}
		child.__index=child
		child.class=child
		child.className=name
		child.super=parent
		-- Metamethod lookup does not follow the class inheritance chain.
		-- Match CoreLibs/Object.lua by copying the parent's operators.
		for _,key in ipairs({"__gc","__newindex","__mode","__tostring",
			"__len","__unm","__add","__sub","__mul","__div","__mod",
			"__pow","__concat","__eq","__lt","__le"}) do
			child[key]=parent[key]
		end
		setmetatable(child,{
			__index=parent,
			__call=function(_, ...)
				local value=child.baseObject()
				setmetatable(value,child)
				-- CoreLibs stores the concrete class on each instance. Code
				-- which queues scene instances commonly calls this class later.
				value.super=child
				child.init(value,...)
				return value
			end,
		})
		if namespace~=nil then namespace[name]=child else _G[name]=child end
		-- The SDK's extends() intentionally has no return value.
	end }
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

local spriteBounds
local Sprite = {}
Sprite.__index = Sprite
Sprite.class = Sprite
Sprite.className = "playdate.graphics.sprite"
Sprite.baseObject = Object.baseObject
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
	self.ignoresDrawOffset = false
	self.opaque = false
	self.redrawsOnImageChange = true
	self.alwaysRedraw = false
	self.stencilImage = nil
	self.groups = {}
	self.collidesWithGroups = {}
	self.groupMask = 0
	self.collidesWithGroupsMask = 0
	self.tag = 0
	self._sequence = 0
end

function Sprite:isa(candidate) return objectIsA(self, candidate) end

-- The SDK's base sprite draw hook is a no-op.  A subclass draw callback is
-- used only when the sprite has no image/tilemap; image-backed subclasses
-- such as NobleSprite keep their normal native image rendering.
function Sprite:draw() end

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
function Sprite:getCenterPoint()
	return playdate.geometry.point.new(self.centerX, self.centerY)
end
function Sprite:moveTo(x, y) self.x, self.y = x, y end
function Sprite:moveBy(x, y) self.x, self.y = self.x + x, self.y + y end
function Sprite:getPosition() return self.x, self.y end
function Sprite:setRotation(value, xScale, yScale)
	self.rotation = value or 0
	if xScale ~= nil then
		self.xScale = xScale
		self.yScale = yScale == nil and xScale or yScale
	end
end
function Sprite:getRotation() return self.rotation end
function Sprite:copy()
	local value=Sprite(self.image)
	value.tilemap=self.tilemap
	value.x,value.y=self.x,self.y
	value.centerX,value.centerY=self.centerX,self.centerY
	value.width,value.height=self.width,self.height
	value._sizeExplicit=self._sizeExplicit
	value.zIndex,value.visible=self.zIndex,self.visible
	value._updatesEnabled,value._collisionsEnabled=self._updatesEnabled,self._collisionsEnabled
	value.imageDrawMode,value.flip=self.imageDrawMode,self.flip
	value.xScale,value.yScale,value.rotation=self.xScale,self.yScale,self.rotation
	value.clipRect=self.clipRect and table.shallowcopy(self.clipRect) or nil
	value.groups=table.shallowcopy(self.groups or {})
	value.collidesWithGroups=table.shallowcopy(self.collidesWithGroups or {})
	value.groupMask,value.collidesWithGroupsMask=self.groupMask,self.collidesWithGroupsMask
	value.tag=self.tag
	value.ignoresDrawOffset,value.opaque=self.ignoresDrawOffset,self.opaque
	value.redrawsOnImageChange=self.redrawsOnImageChange
	value.stencilImage=self.stencilImage
	if self.collideRect then value:setCollideRect(self.collideRect) end
	return value
end
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
function Sprite:getClipRect()
	if not self.clipRect then return nil end
	return self.clipRect.x,self.clipRect.y,self.clipRect.width,self.clipRect.height
end
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
function Sprite:setIgnoresDrawOffset(value) self.ignoresDrawOffset=value==true end
function Sprite:getIgnoresDrawOffset() return self.ignoresDrawOffset==true end
function Sprite:setOpaque(value) self.opaque=value==true end
function Sprite:isOpaque() return self.opaque==true end
function Sprite:setRedrawsOnImageChange(value) self.redrawsOnImageChange=value==true end
function Sprite:markDirty() sprites_dirty=true end
function Sprite:setStencilImage(image, tile) self.stencilImage=image; self.stencilTiled=tile==true end
function Sprite:clearStencil() self.stencilImage=nil; self.stencilTiled=false end
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
function Sprite:getCollideRect()
	if not self.collideRect then return nil end
	return playdate.geometry.rect.new(self.collideRect.x,self.collideRect.y,
		self.collideRect.width,self.collideRect.height)
end
function Sprite:getCollideBounds()
	if not self.collideRect then return nil end
	local x,y,w,h=spriteBounds(self)
	return playdate.geometry.rect.new(x,y,w,h)
end
function Sprite:setBounds(x,y,width,height)
	if type(x)=="table" then
		local rect=x; x,y,width,height=rect.x,rect.y,rect.width or rect.w,rect.height or rect.h
	end
	self:setSize(width,height)
	self:moveTo(x+width*self.centerX,y+height*self.centerY)
end
function Sprite:getBounds()
	return self.x-self.width*self.centerX,self.y-self.height*self.centerY,
		self.width,self.height
end
function Sprite:getBoundsRect()
	local x,y,w,h=self:getBounds()
	return playdate.geometry.rect.new(x,y,w,h)
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
Sprite.addSprite=Sprite.add

function Sprite:remove()
	if self.added then
		self.added = false
		sprites_dirty = true
		if self._collision_listed then collision_sprites_dirty = true end
	end
	return self
end
Sprite.removeSprite=Sprite.remove

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
		local customDraw=not item.image and not item.tilemap and
			type(item.draw)=="function" and item.draw~=Sprite.draw
		if item.added and item.visible and (item.image or item.tilemap or customDraw) then
			local previous = gfx._getImageDrawMode()
			gfx.setImageDrawMode(item.imageDrawMode)
			local oldOffsetX,oldOffsetY
			if item.ignoresDrawOffset then
				oldOffsetX,oldOffsetY=gfx.getDrawOffset(); gfx.setDrawOffset(0,0)
			end
			if item.clipRect then
				gfx.setClipRect(item.clipRect.x, item.clipRect.y,
					item.clipRect.width, item.clipRect.height)
			end
			if item.stencilImage then gfx.setStencilImage(item.stencilImage,item.stencilTiled) end
			if customDraw then
				local x,y,w,h=item:getBounds()
				local targetWidth,targetHeight=math.max(1,math.ceil(w)),math.max(1,math.ceil(h))
				local currentWidth,currentHeight=0,0
				if item._customDrawImage then
					currentWidth,currentHeight=item._customDrawImage:getSize()
				end
				if not item._customDrawImage or currentWidth~=targetWidth or
					currentHeight~=targetHeight then
					item._customDrawImage=gfx.image.new(targetWidth,targetHeight,gfx.kColorClear)
				else item._customDrawImage:clear(gfx.kColorClear) end
				local callbackOffsetX,callbackOffsetY=gfx.getDrawOffset()
				gfx.setDrawOffset(0,0); gfx.pushContext(item._customDrawImage)
				item:draw(0,0,w,h)
				gfx.popContext(); gfx.setDrawOffset(callbackOffsetX,callbackOffsetY)
				item._customDrawImage:draw(x,y)
			elseif item.tilemap then
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
			if item.stencilImage then gfx.clearStencil() end
			if item.clipRect then gfx.clearClipRect() end
			if oldOffsetX then gfx.setDrawOffset(oldOffsetX,oldOffsetY) end
			gfx.setImageDrawMode(previous)
		end
	end
end

function Sprite.performOnAllSprites(callback)
	for i=1,#sprites do if sprites[i].added then callback(sprites[i]) end end
end

function Sprite.getAllSprites()
	local result={}
	for i=1,#sprites do if sprites[i].added then result[#result+1]=sprites[i] end end
	return result
end
function Sprite.spriteCount() return #Sprite.getAllSprites() end
function Sprite.removeSprites(items)
	for i=1,#items do if items[i] and items[i].remove then items[i]:remove() end end
end
function Sprite.addSprites(items)
	for i=1,#items do if items[i] and items[i].add then items[i]:add() end end
end
function Sprite.getAlwaysRedraw() return always_redraw end
function Sprite.addDirtyRect() sprites_dirty=true end

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

spriteBounds = function(item)
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
	return result,#result
end

function Sprite.querySpritesAtPoint(x,y)
	if type(x)=="table" then x,y=x.x,x.y end
	local candidates=Sprite.querySpritesInRect(x,y,1,1)
	local result={}
	for i=1,#candidates do
		local sx,sy,sw,sh=spriteBounds(candidates[i])
		if x>=sx and x<=sx+sw and y>=sy and y<=sy+sh then
			result[#result+1]=candidates[i]
		end
	end
	return result,#result
end

local function lineRectInfo(x1,y1,x2,y2,rx,ry,rw,rh)
	local dx,dy=x2-x1,y2-y1
	local near,far=0,1
	local entryX,entryY,exitX,exitY=0,0,0,0
	local function axis(origin,delta,minimum,maximum,isX)
		if math.abs(delta)<1e-12 then return origin>=minimum and origin<=maximum end
		local t1,t2=(minimum-origin)/delta,(maximum-origin)/delta
		local nearNormal,farNormal=-1,1
		if t1>t2 then t1,t2=t2,t1; nearNormal,farNormal=farNormal,nearNormal end
		if t1>near then
			near=t1
			if isX then entryX,entryY=nearNormal,0 else entryX,entryY=0,nearNormal end
		end
		if t2<far then
			far=t2
			if isX then exitX,exitY=farNormal,0 else exitX,exitY=0,farNormal end
		end
		return near<=far
	end
	if not axis(x1,dx,rx,rx+rw,true) or not axis(y1,dy,ry,ry+rh,false) then return nil end
	if far<0 or near>1 then return nil end
	near,far=math.max(0,near),math.min(1,far)
	return near,far,entryX,entryY,exitX,exitY
end

function Sprite.querySpriteInfoAlongLine(x1,y1,x2,y2)
	if type(x1)=="table" then
		local line=x1; x1,y1,x2,y2=line.x1,line.y1,line.x2,line.y2
	end
	local left,top=math.min(x1,x2),math.min(y1,y2)
	local candidates=Sprite.querySpritesInRect(left,top,
		math.max(1,math.abs(x2-x1)),math.max(1,math.abs(y2-y1)))
	local result={}
	for i=1,#candidates do
		local sprite=candidates[i]
		local rx,ry,rw,rh=spriteBounds(sprite)
		local near,far,enx,eny,exn,eyn=lineRectInfo(x1,y1,x2,y2,rx,ry,rw,rh)
		if near then
			result[#result+1]={sprite=sprite,ti1=near,ti2=far,
				entryPoint=playdate.geometry.point.new(
					x1+(x2-x1)*near,y1+(y2-y1)*near),
				exitPoint=playdate.geometry.point.new(
					x1+(x2-x1)*far,y1+(y2-y1)*far),
				entryNormal=playdate.geometry.vector2D.new(enx,eny),
				exitNormal=playdate.geometry.vector2D.new(exn,eyn)}
		end
	end
	table.sort(result,function(a,b) return a.ti1<b.ti1 end)
	return result,#result
end

function Sprite.querySpritesAlongLine(...)
	local info=Sprite.querySpriteInfoAlongLine(...)
	local result={}
	for i=1,#info do result[i]=info[i].sprite end
	return result,#result
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
			local tileWidth,tileHeight=tilemap:getTileSize()
			tileWidth=tileWidth>0 and tileWidth or 8
			tileHeight=tileHeight>0 and tileHeight or 8
			item:setSize(tileWidth, tileHeight)
			item:setCollideRect(0, 0, tileWidth, tileHeight)
			item:moveTo(offsetX + column * tileWidth + tileWidth/2,
				offsetY + row * tileHeight + tileHeight/2)
			local cell_x = math.floor((offsetX + column * tileWidth) / 8)
			local cell_y = math.floor((offsetY + row * tileHeight) / 8)
			wall_cells[cell_y] = wall_cells[cell_y] or {}
			wall_cells[cell_y][cell_x] = wall_cells[cell_y][cell_x] or {}
			table.insert(wall_cells[cell_y][cell_x], item)
			table.insert(result, item)
		end
	end
	return result
end

function Sprite.addEmptyCollisionSprite(x,y,width,height)
	if type(x)=="table" then
		local rect=x; x,y,width,height=rect.x,rect.y,
			rect.width or rect.w,rect.height or rect.h
	end
	local item=Sprite.new(); item:setBounds(x,y,width,height)
	item:setCollideRect(0,0,width,height); item:add(); return item
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
	self.tiles, self.width = table.shallowcopy(tiles), math.max(1, width or 1)
	self.height = math.ceil(#tiles / self.width)
end
function Tilemap:setImageTable(value) self.imagetable = value end
function Tilemap:getImageTable() return self.imagetable end
function Tilemap:setSize(width,height)
	width,height=math.max(1,math.floor(width or 1)),math.max(0,math.floor(height or 0))
	local resized={}; local oldWidth,oldHeight=self.width,self.height
	for y=1,height do for x=1,width do
		resized[(y-1)*width+x]=(x<=oldWidth and y<=oldHeight)
			and (self.tiles[(y-1)*oldWidth+x] or 0) or 0
	end end
	self.tiles,self.width,self.height=resized,width,height
end
function Tilemap:getSize() return self.width, self.height end
function Tilemap:getTiles() return table.shallowcopy(self.tiles),self.width end
function Tilemap:setTileAtPosition(x,y,index)
	x,y=math.floor(x),math.floor(y)
	if x<1 or y<1 or x>self.width or y>self.height then return false end
	self.tiles[(y-1)*self.width+x]=index or 0
	return true
end
function Tilemap:getTileAtPosition(x,y)
	x,y=math.floor(x),math.floor(y)
	if x<1 or y<1 or x>self.width or y>self.height then return nil end
	return self.tiles[(y-1)*self.width+x]
end
function Tilemap:getTileSize()
	local tile=self.imagetable and self.imagetable:getImage(1)
	if not tile then return 0,0 end
	return tile:getSize()
end
function Tilemap:getPixelSize()
	local tile = self.imagetable and self.imagetable:getImage(1)
	if not tile then return 0, 0 end
	local width, height = tile:getSize()
	return self.width * width, self.height * height
end
function Tilemap:draw(x, y, sourceRect, sourceY, sourceWidth, sourceHeight)
	if not self.imagetable then return end
	x,y=x or 0,y or 0
	local previousClip
	if sourceRect then
		local sx,sy,sw,sh
		if type(sourceRect)=="table" then
			sx,sy,sw,sh=sourceRect.x,sourceRect.y,
				sourceRect.width or sourceRect.w,sourceRect.height or sourceRect.h
		else sx,sy,sw,sh=sourceRect,sourceY,sourceWidth,sourceHeight end
		previousClip={gfx.getClipRect()}
		gfx.setClipRect(x+sx,y+sy,sw,sh)
	end
	if gfx._drawTilemap then
		gfx._drawTilemap(self.imagetable, self.tiles, self.width, x, y)
		if previousClip then gfx.setClipRect(table.unpack(previousClip)) end
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
				image:draw(x + column * tileWidth,y + row * tileHeight)
			end
		end
	end
	if previousClip then gfx.setClipRect(table.unpack(previousClip)) end
end
function Tilemap:drawIgnoringOffset(...)
	local ox,oy=gfx.getDrawOffset(); gfx.setDrawOffset(0,0)
	self:draw(...)
	gfx.setDrawOffset(ox,oy)
end
function Tilemap:getCollisionRects(emptyIDs)
	local empty={[0]=true}
	for i=1,#(emptyIDs or {}) do empty[emptyIDs[i]]=true end
	local result,open={},{ }
	for y=1,self.height do
		local rowRuns={}; local x=1
		while x<=self.width do
			if empty[self:getTileAtPosition(x,y) or 0] then x=x+1 else
				local start=x
				repeat x=x+1 until x>self.width or empty[self:getTileAtPosition(x,y) or 0]
				local key=start..":"..(x-start)
				local rect=open[key]
				if rect and rect.y+rect.height==y-1 then rect.height=rect.height+1; rect.h=rect.height
				else rect=playdate.geometry.rect.new(start-1,y-1,x-start,1); result[#result+1]=rect end
				rowRuns[key]=rect
			end
		end
		open=rowRuns
	end
	return result
end
gfx.tilemap = Tilemap

local Point
local Vector2D
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

function Rect:containsRect(other,y,width,height)
	if type(other)~="table" then other=Rect.new(other,y,width,height) end
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
	if right<=left or bottom<=top then return 0,0,0,0 end
	return left, top, right-left, bottom-top
end
function Rect.fast_union(ax, ay, aw, ah, bx, by, bw, bh)
	local x, y = math.min(ax, bx), math.min(ay, by)
	return x, y, math.max(ax + aw, bx + bw) - x,
		math.max(ay + ah, by + bh) - y
end
function Rect:flipRelativeToRect(other,flip)
	if flip==1 or flip==3 then self.x=other.x+other.width-(self.x-other.x)-self.width end
	if flip==2 or flip==3 then self.y=other.y+other.height-(self.y-other.y)-self.height end
	return self
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
Point.__sub = function(a, b) return Vector2D.new(a.x - b.x, a.y - b.y) end
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
	if x then return true,Point.new(x,y) end
	return false,nil
end
function LineSegment.fast_intersection(x1,y1,x2,y2,x3,y3,x4,y4)
	local x,y=segmentIntersection(x1,y1,x2,y2,x3,y3,x4,y4)
	if x then return true,x,y end
	return false
end
function LineSegment:intersectsPolygon(polygon)
	local points={}
	for i=1,#polygon.points-1 do
		local hit,point=self:intersectsLineSegment(LineSegment.new(
			polygon.points[i],polygon.points[i+1]))
		if hit then points[#points+1]=point end
	end
	return #points>0,points
end
function LineSegment:intersectsRect(rect) return self:intersectsPolygon(rect:toPolygon()) end

Vector2D = {}
Vector2D.__index = Vector2D
function Vector2D.new(x, y)
	return setmetatable({x=x or 0, y=y or 0, dx=x or 0, dy=y or 0}, Vector2D)
end
function Vector2D.newPolar(length,angle)
	local radians=math.rad((angle or 0)-90)
	return Vector2D.new(math.cos(radians)*(length or 0),math.sin(radians)*(length or 0))
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
function Vector2D:projectAlong(other)
	local divisor=other:magnitudeSquared()
	local amount=divisor==0 and 0 or self:dotProduct(other)/divisor
	self.x,self.y=other.x*amount,other.y*amount
	self.dx,self.dy=self.x,self.y
	return self
end
function Vector2D:projectedAlong(other) return self:copy():projectAlong(other) end
function Vector2D:leftNormal() return Vector2D.new(self.y,-self.x) end
function Vector2D:rightNormal() return Vector2D.new(-self.y,self.x) end
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
	if #args==1 and type(args[1])=="number" then
		local points={}; for i=1,math.max(0,math.floor(args[1])) do points[i]=Point.new() end
		return setmetatable({points=points,_closed=false},Polygon)
	end
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
function Polygon:setPointAt(index,point,y)
	if type(point)=="table" then self.points[index]=Point.new(point.x,point.y)
	else self.points[index]=Point.new(point,y) end
end
function Polygon:getBounds()
	if #self.points==0 then return 0,0,0,0 end
	local left,right,top,bottom=self.points[1].x,self.points[1].x,
		self.points[1].y,self.points[1].y
	for i=2,#self.points do local p=self.points[i]
		left,right=math.min(left,p.x),math.max(right,p.x)
		top,bottom=math.min(top,p.y),math.max(bottom,p.y)
	end
	return left,top,right-left,bottom-top
end
function Polygon:getBoundsRect() return Rect.new(self:getBounds()) end
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
function Polygon:intersects(other)
	for i=1,#self.points-1 do
		local hit=LineSegment.new(self.points[i],self.points[i+1]):intersectsPolygon(other)
		if hit then return true end
	end
	return (self.points[1] and other:containsPoint(self.points[1])) or
		(other.points[1] and self:containsPoint(other.points[1])) or false
end
Polygon.__mul=function(polygon,transform) return transform:transformedPolygon(polygon) end
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
	local bounds=self:transformedPolygon(rect:toPolygon()):getBoundsRect()
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
Point.__mul=function(point,transform) return transform:transformedPoint(point) end
Point.__concat=function(a,b) return LineSegment.new(a,b) end

playdate.geometry = {rect=Rect, point=Point, size=Size, vector2D=Vector2D,
	lineSegment=LineSegment, polygon=Polygon, arc=Arc,
	affineTransform=AffineTransform,kUnflipped=0,kFlippedX=1,kFlippedY=2,kFlippedXY=3}
function playdate.geometry.squaredDistanceToPoint(x1,y1,x2,y2)
	local dx,dy=x2-x1,y2-y1; return dx*dx+dy*dy
end
function playdate.geometry.distanceToPoint(...) return math.sqrt(playdate.geometry.squaredDistanceToPoint(...)) end

-- Pathfinding is kept in Lua so graphs are ordinary inspectable objects and
-- package code can extend their nodes.  The search is A* with Euclidean
-- distance when nodes have coordinates and Dijkstra behaviour otherwise.
local PathfinderNode={}; PathfinderNode.__index=PathfinderNode
function PathfinderNode.new(id,x,y)
	return setmetatable({id=id,x=x or 0,y=y or 0,connections={}},PathfinderNode)
end
function PathfinderNode:addConnection(other,weight,reciprocal)
	if not other then return false end
	self.connections[other]=tonumber(weight) or 1
	if reciprocal then other.connections[self]=tonumber(weight) or 1 end
	return true
end
function PathfinderNode:addConnections(nodes,weight,reciprocal)
	for i=1,#(nodes or {}) do
		self:addConnection(nodes[i],type(weight)=="table" and weight[i] or weight,reciprocal)
	end
end
function PathfinderNode:removeConnection(other,reciprocal)
	self.connections[other]=nil
	if reciprocal and other then other.connections[self]=nil end
end
function PathfinderNode:connectedNodes()
	local result={}
	for node in pairs(self.connections) do result[#result+1]=node end
	table.sort(result,function(a,b) return tostring(a.id)<tostring(b.id) end)
	return result
end
function PathfinderNode:getID() return self.id end
function PathfinderNode:getPosition() return self.x,self.y end
function PathfinderNode:setPosition(x,y) self.x,self.y=x or 0,y or 0 end
PathfinderNode.setXY=PathfinderNode.setPosition
function PathfinderNode:addConnectionToNodeWithXY(x,y,weight,reciprocal)
	local node=self.graph and self.graph:nodeWithXY(x,y)
	return node and self:addConnection(node,weight,reciprocal) or false
end
function PathfinderNode:removeAllConnections(removeIncoming)
	self.connections={}
	if removeIncoming and self.graph then
		for _,node in ipairs(self.graph.nodeList) do node.connections[self]=nil end
	end
end

local PathfinderGraph={}; PathfinderGraph.__index=PathfinderGraph
function PathfinderGraph.new(nodeCount,coordinates)
	local graph=setmetatable({nodes={},nodeList={}},PathfinderGraph)
	for id=1,math.max(0,math.floor(nodeCount or 0)) do
		local coordinate=coordinates and coordinates[id]
		graph:addNewNode(id,coordinate and coordinate[1] or 0,
			coordinate and coordinate[2] or 0)
	end
	return graph
end
function PathfinderGraph:addNode(node,connectedNodes,weights,reciprocal)
	if not node or node.id==nil then return false end
	local previous=self.nodes[node.id]
	if previous==node then return true end
	if previous then self:removeNodeWithID(node.id) end
	self.nodes[node.id]=node; self.nodeList[#self.nodeList+1]=node; node.graph=self
	if connectedNodes then node:addConnections(connectedNodes,weights,reciprocal) end
	return true
end
function PathfinderGraph:addNewNode(id,x,y,connectedNodes,weights,reciprocal)
	local node=PathfinderNode.new(id,x,y); self:addNode(node)
	if connectedNodes then node:addConnections(connectedNodes,weights,reciprocal) end
	return node
end
PathfinderGraph.addNewNodeWithID=PathfinderGraph.addNewNode
function PathfinderGraph:addNewNodes(count)
	local result={}; local id=1
	for _=1,math.max(0,math.floor(count or 0)) do
		while self.nodes[id] do id=id+1 end
		result[#result+1]=self:addNewNode(id); id=id+1
	end
	return result
end
function PathfinderGraph:addNodes(nodes) for i=1,#(nodes or {}) do self:addNode(nodes[i]) end end
function PathfinderGraph:nodeWithID(id) return self.nodes[id] end
function PathfinderGraph:nodeWithXY(x,y)
	for _,node in ipairs(self.nodeList) do if node.x==x and node.y==y then return node end end
end
function PathfinderGraph:allNodes() return table.shallowcopy(self.nodeList) end
function PathfinderGraph:removeNodeWithID(id)
	local node=self.nodes[id]; if not node then return false end
	self.nodes[id]=nil
	for i=#self.nodeList,1,-1 do
		if self.nodeList[i]==node then table.remove(self.nodeList,i) end
	end
	for _,other in ipairs(self.nodeList) do other.connections[node]=nil end
	node.graph=nil; return node
end
function PathfinderGraph:removeNode(node) return node and self:removeNodeWithID(node.id) end
function PathfinderGraph:removeNodeWithXY(x,y)
	local node=self:nodeWithXY(x,y); return node and self:removeNode(node) or nil
end
function PathfinderGraph:setXYForNodeWithID(id,x,y)
	local node=self.nodes[id]; if not node then return false end
	node:setPosition(x,y); return true
end
function PathfinderGraph:addConnectionToNodeWithID(fromID,toID,weight,reciprocal)
	local from,to=self.nodes[fromID],self.nodes[toID]
	return from and to and from:addConnection(to,weight,reciprocal) or false
end
function PathfinderGraph:addConnections(connections)
	for id,values in pairs(connections or {}) do
		local from=self.nodes[id]
		if from then for i=1,#values,2 do
			local to=self.nodes[values[i]]; if to then from:addConnection(to,values[i+1]) end
		end end
	end
end
function PathfinderGraph:removeAllConnections()
	for _,node in ipairs(self.nodeList) do node.connections={} end
end
function PathfinderGraph:removeAllConnectionsFromNodeWithID(id,removeIncoming)
	local node=self.nodes[id]; if node then node:removeAllConnections(removeIncoming); return true end
	return false
end
local function pathHeuristic(left,right)
	local dx,dy=(left.x or 0)-(right.x or 0),(left.y or 0)-(right.y or 0)
	return math.abs(dx)+math.abs(dy)
end
function PathfinderGraph:findPath(startNode,goalNode,heuristic,findAdjacent)
	if type(startNode)~="table" then startNode=self.nodes[startNode] end
	if type(goalNode)~="table" then goalNode=self.nodes[goalNode] end
	if not startNode or not goalNode then return nil end
	heuristic=heuristic or pathHeuristic
	local open,came,gScore,fScore={startNode},{},{[startNode]=0},{[startNode]=heuristic(startNode,goalNode)}
	local inOpen={[startNode]=true}
	while #open>0 do
		local best=1
		for i=2,#open do if (fScore[open[i]] or math.huge)<(fScore[open[best]] or math.huge) then best=i end end
		local current=table.remove(open,best); inOpen[current]=nil
		local reached=current==goalNode or (findAdjacent and current~=goalNode and
			math.abs((current.x or 0)-(goalNode.x or 0))<=1 and
			math.abs((current.y or 0)-(goalNode.y or 0))<=1)
		if reached then
			local path={current}
			while came[current] do current=came[current]; table.insert(path,1,current) end
			return path
		end
		for neighbour,weight in pairs(current.connections) do
			local candidate=(gScore[current] or math.huge)+(tonumber(weight) or 1)
			if candidate<(gScore[neighbour] or math.huge) then
				came[neighbour]=current; gScore[neighbour]=candidate
				fScore[neighbour]=candidate+heuristic(neighbour,goalNode)
				if not inOpen[neighbour] then open[#open+1]=neighbour; inOpen[neighbour]=true end
			end
		end
	end
	return nil
end
function PathfinderGraph:findPathWithIDs(startID,goalID,heuristic,findAdjacent)
	local path=self:findPath(self.nodes[startID],self.nodes[goalID],heuristic,findAdjacent)
	if not path then return nil end
	local ids={}; for i=1,#path do ids[i]=path[i].id end; return ids
end
function PathfinderGraph.new2DGrid(width,height,allowDiagonals,includedNodes)
	width,height=math.max(0,math.floor(width or 0)),math.max(0,math.floor(height or 0))
	local graph=PathfinderGraph.new()
	for y=1,height do for x=1,width do graph:addNewNode((y-1)*width+x,x,y) end end
	local directions={{1,0,10},{0,1,10}}
	if allowDiagonals then directions[#directions+1]={1,1,14}; directions[#directions+1]={-1,1,14} end
	for y=1,height do for x=1,width do
		local id=(y-1)*width+x
		if not includedNodes or includedNodes[id]==1 then
			for _,direction in ipairs(directions) do
				local nx,ny=x+direction[1],y+direction[2]
				local nid=(ny-1)*width+nx
				if nx>=1 and nx<=width and ny>=1 and ny<=height and
					(not includedNodes or includedNodes[nid]==1) then
					graph.nodes[id]:addConnection(graph.nodes[nid],direction[3],true)
				end
			end
		end
	end end
	return graph
end
playdate.pathfinder={node=PathfinderNode,graph=PathfinderGraph}

-- Low-level images are userdata, but Lua may add real composite operations to
-- their shared method table.  These helpers use the native compositor and
-- therefore preserve transparency, masks and Playdate colour constants.
local imageMethods=debug.getregistry()["PogoDate.Image"]
if imageMethods then
	function imageMethods:getPixel(x,y) return self:sample(x,y) end
	function imageMethods:drawAnchored(x,y,anchorX,anchorY,flip)
		local width,height=self:getSize()
		self:draw(x-width*(anchorX or 0),y-height*(anchorY or 0),flip)
	end
	function imageMethods:getSubImage(x,y,width,height)
		width,height=math.max(0,math.floor(width or 0)),math.max(0,math.floor(height or 0))
		if width==0 or height==0 then return nil end
		local result=gfx.image.new(width,height,gfx.kColorClear)
		local ox,oy=gfx.getDrawOffset(); gfx.setDrawOffset(0,0)
		gfx.pushContext(result); self:draw(-(x or 0),-(y or 0)); gfx.popContext()
		gfx.setDrawOffset(ox,oy); return result
	end
	function imageMethods:rotatedImage(angle,scaleX,scaleY)
		local width,height=self:getSize(); scaleX=scaleX or 1; scaleY=scaleY or scaleX
		local radians=math.rad(angle or 0); local c,s=math.abs(math.cos(radians)),math.abs(math.sin(radians))
		local outWidth=math.max(1,math.ceil(width*math.abs(scaleX)*c+height*math.abs(scaleY)*s))
		local outHeight=math.max(1,math.ceil(width*math.abs(scaleX)*s+height*math.abs(scaleY)*c))
		local result=gfx.image.new(outWidth,outHeight,gfx.kColorClear)
		local ox,oy=gfx.getDrawOffset(); gfx.setDrawOffset(0,0)
		gfx.pushContext(result); self:drawRotated(outWidth/2,outHeight/2,angle or 0,scaleX,scaleY); gfx.popContext()
		gfx.setDrawOffset(ox,oy); return result
	end
	function imageMethods:drawWithMask(x,y,mask,flip)
		gfx.setStencilImage(mask); self:draw(x,y,flip); gfx.clearStencil()
	end
	function imageMethods:transformedImage(transform)
		local width,height=self:getSize()
		local bounds=transform:transformedAABB(Rect.new(0,0,width,height))
		local outWidth,outHeight=math.max(1,math.ceil(bounds.width)),math.max(1,math.ceil(bounds.height))
		local inverse=transform:copy():invert(); if not inverse then return nil end
		local result=gfx.image.new(outWidth,outHeight,gfx.kColorClear)
		local oldColor=gfx.getColor(); local ox,oy=gfx.getDrawOffset(); gfx.setDrawOffset(0,0)
		gfx.pushContext(result)
		local activeColor=nil
		for py=0,outHeight-1 do for px=0,outWidth-1 do
			local sx,sy=inverse:transformXY(px+bounds.x+0.5,py+bounds.y+0.5)
			if sx>=0 and sy>=0 and sx<width and sy<height then
				local color=self:sample(math.floor(sx),math.floor(sy))
				if color~=gfx.kColorClear then
					if activeColor~=color then gfx.setColor(color); activeColor=color end
					gfx.drawPixel(px,py)
				end
			end
		end end
		gfx.popContext(); gfx.setDrawOffset(ox,oy); gfx.setColor(oldColor)
		return result
	end
	function imageMethods:drawWithTransform(transform)
		local bounds=transform:transformedAABB(Rect.new(0,0,self:getSize()))
		local image=self:transformedImage(transform)
		if image then image:draw(bounds.x,bounds.y) end
	end
end

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
gfx.drawPolygon=gfx.drawPolygon or function(...)
	local args={...}; local polygon=type(args[1])=="table" and args[1].points and args[1]
	local p=polygonCoordinates(...)
	for i=1,#p-3,2 do gfx.drawLine(p[i],p[i+1],p[i+2],p[i+3]) end
	if #p>=6 and (not polygon or polygon:isClosed()) then
		gfx.drawLine(p[#p-1],p[#p],p[1],p[2])
	end
end
gfx.fillPolygon=gfx.fillPolygon or function(...)
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
gfx.drawTriangle=gfx.drawTriangle or function(x1,y1,x2,y2,x3,y3)
	gfx.drawLine(x1,y1,x2,y2); gfx.drawLine(x2,y2,x3,y3); gfx.drawLine(x3,y3,x1,y1)
end
gfx.fillTriangle=gfx.fillTriangle or function(...) gfx.fillPolygon(...) end
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
function gfx.drawArc(x,y,radius,startAngle,endAngle)
	if type(x)=="table" then
		local arc=x; x,y,radius,startAngle,endAngle=arc.x,arc.y,arc.radius,
			arc.startAngle,arc.endAngle
	end
	local direction=(endAngle or 0)>=(startAngle or 0) and 1 or -1
	local extent=math.abs((endAngle or 0)-(startAngle or 0))
	local steps=math.max(1,math.ceil(math.rad(extent)*math.max(1,radius or 0)/2))
	local previous
	for index=0,steps do
		local angle=(startAngle or 0)+direction*extent*index/steps
		local radians=math.rad(angle-90)
		local point=Point.new(x+math.cos(radians)*radius,y+math.sin(radians)*radius)
		if previous then gfx.drawLine(previous.x,previous.y,point.x,point.y) end
		previous=point
	end
end
local nativeSetStrokeLocation=gfx.setStrokeLocation
local strokeLocation=gfx.kStrokeCentered
function gfx.setStrokeLocation(value)
	strokeLocation=value or gfx.kStrokeCentered
	return nativeSetStrokeLocation(strokeLocation)
end
function gfx.getStrokeLocation() return strokeLocation end
local lineCapStyle=gfx.kLineCapStyleButt
function gfx.setLineCapStyle(value) lineCapStyle=value end
function gfx.getLineCapStyle() return lineCapStyle end
local polygonFillRule=gfx.kPolygonFillNonZero
function gfx.setPolygonFillRule(value) polygonFillRule=value end
function gfx.getPolygonFillRule() return polygonFillRule end
gfx.setScreenClipRect=gfx.setClipRect
gfx.getScreenClipRect=gfx.getClipRect
gfx.clearScreenClipRect=gfx.clearClipRect
function gfx.imageWithText(text,maxWidth,maxHeight,backgroundColor,leadingAdjustment,
	truncationString,alignment,font)
	text=tostring(text or "")
	if font then gfx.setFont(font) end
	local naturalWidth,naturalHeight=gfx.getTextSize(text)
	maxWidth=math.max(1,math.floor(maxWidth or naturalWidth or 1))
	maxHeight=math.max(1,math.floor(maxHeight or naturalHeight or 1))
	local width=math.max(1,math.min(maxWidth,naturalWidth or maxWidth))
	local height=math.max(1,math.min(maxHeight,naturalHeight or maxHeight))
	local truncated=(naturalWidth or 0)>maxWidth or (naturalHeight or 0)>maxHeight
	local image=gfx.image.new(width,height,backgroundColor or gfx.kColorClear)
	local ox,oy=gfx.getDrawOffset(); gfx.setDrawOffset(0,0); gfx.pushContext(image)
	gfx.drawTextInRect(text,0,0,width,height,leadingAdjustment,truncationString,alignment,font)
	gfx.popContext(); gfx.setDrawOffset(ox,oy)
	return image,truncated
end

function Sprite.spriteWithText(text,maxWidth,maxHeight,...)
	local image,truncated=gfx.imageWithText(text,maxWidth,maxHeight,...)
	return Sprite.new(image),truncated
end

function playdate.display.getRect()
	return Rect.new(0,0,playdate.display.getWidth(),playdate.display.getHeight())
end
gfx.font.kLanguageEnglish="en"
gfx.font.kLanguageJapanese="jp"
function gfx.font.newFamily(fontPaths)
	assert(type(fontPaths)=="table","font.newFamily expects a table")
	local family={}
	for variant,path in pairs(fontPaths) do
		if type(path)=="userdata" then
			family[variant]=path
		elseif path~=nil then
			family[variant]=gfx.font.new(path)
		end
	end
	return family
end
function gfx.getTextSizeForMaxWidth(text,maxWidth,leadingAdjustment,font)
	text=tostring(text or "")
	maxWidth=math.max(1,math.floor(tonumber(maxWidth) or 1))
	local _,singleHeight=gfx.getTextSize("M",font)
	local leading=math.floor(tonumber(leadingAdjustment) or 0)
	local widest,lineWidth,lines=0,0,1
	for token in text:gmatch("[^%s]+%s*") do
		local tokenWidth=gfx.getTextSize(token,font)
		if lineWidth>0 and lineWidth+tokenWidth>maxWidth then
			widest=math.max(widest,lineWidth); lineWidth=0; lines=lines+1
		end
		if tokenWidth>maxWidth then
			for char in token:gmatch("[\1-\127\194-\244][\128-\191]*") do
				local charWidth=gfx.getTextSize(char,font)
				if lineWidth>0 and lineWidth+charWidth>maxWidth then
					widest=math.max(widest,lineWidth); lineWidth=0; lines=lines+1
				end
				lineWidth=lineWidth+charWidth
			end
		else
			lineWidth=lineWidth+tokenWidth
		end
	end
	widest=math.min(maxWidth,math.max(widest,lineWidth))
	return widest,lines*(singleHeight or 0)+math.max(0,lines-1)*leading
end
function playdate.getSystemLanguage() return gfx.font.kLanguageEnglish end
function playdate.shouldDisplay24HourTime() return true end
function playdate.getFlipped() return false end

-- Keyboard lifecycle compatibility. Pogopo does not yet have a shell-owned
-- text-entry overlay, but games can open/close the keyboard, seed/read text
-- and receive the documented lifecycle callbacks instead of stopping on a
-- missing module. Physical text entry remains a future shell feature.
playdate.keyboard=playdate.keyboard or {
	text="",kCapitalizationNormal=0,kCapitalizationWords=1,
	kCapitalizationSentences=2,
}
local keyboardVisible=false
function playdate.keyboard.show(text)
	if text~=nil then playdate.keyboard.text=tostring(text) end
	if type(playdate.keyboard.keyboardWillShowCallback)=="function" then
		playdate.keyboard.keyboardWillShowCallback()
	end
	keyboardVisible=true
	if type(playdate.keyboard.keyboardDidShowCallback)=="function" then
		playdate.keyboard.keyboardDidShowCallback()
	end
end
function playdate.keyboard.hide()
	if not keyboardVisible then return end
	if type(playdate.keyboard.keyboardWillHideCallback)=="function" then
		playdate.keyboard.keyboardWillHideCallback()
	end
	keyboardVisible=false
	if type(playdate.keyboard.keyboardDidHideCallback)=="function" then
		playdate.keyboard.keyboardDidHideCallback()
	end
end
function playdate.keyboard.isVisible() return keyboardVisible end
function playdate.keyboard.left() return 0 end
function playdate.keyboard.width() return 400 end
function playdate.keyboard.setCapitalizationBehavior(value)
	playdate.keyboard.capitalizationBehavior=value or
		playdate.keyboard.kCapitalizationNormal
end

-- Hardware capability endpoints use honest neutral results. They let games
-- feature-detect a missing microphone/headset without nil API failures.
playdate.sound.micinput=playdate.sound.micinput or {}
function playdate.sound.micinput.recordToSample() return false end
function playdate.sound.micinput.stopRecording() end
function playdate.sound.micinput.startListening() return false end
function playdate.sound.micinput.stopListening() end
function playdate.sound.micinput.getLevel() return 0 end
function playdate.sound.micinput.getSource() return nil end
function playdate.sound.getHeadphoneState(callback)
	playdate.sound.headphoneStateChangedCallback=callback
	return false,false
end
function playdate.sound.setOutputsActive(headphones,speaker)
	playdate.sound.headphonesActive=headphones==true
	playdate.sound.speakerActive=speaker~=false
	return true
end

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

-- Scoreboard calls are network-backed on Playdate. Pogopo keeps gameplay
-- deterministic and offline while preserving the asynchronous callback
-- shapes used by Lua games.
playdate.scoreboards = playdate.scoreboards or {}
local function finishScoreboardRequest(callback, result)
	if type(callback) ~= "function" then return end
	playdate.timer.performAfterDelay(1, function() callback(true, result) end)
end
function playdate.scoreboards.addScore(boardID, value, callback)
	local result={boardID=tostring(boardID or ""),value=tonumber(value) or 0,
		rank=0,player={name="Pogopo"}}
	finishScoreboardRequest(callback,result)
	return true
end
function playdate.scoreboards.getScores(boardID, callback)
	finishScoreboardRequest(callback,{boardID=tostring(boardID or ""),scores={}})
	return true
end
function playdate.scoreboards.getPersonalBest(boardID, callback)
	finishScoreboardRequest(callback,{boardID=tostring(boardID or ""),rank=0,
		value=0,player={name="Pogopo"}})
	return true
end
function playdate.scoreboards.getScoreboards(callback)
	finishScoreboardRequest(callback,{scoreboards={}})
	return true
end

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

-- SDK key-repeat timers fire immediately, wait for the initial delay, then
-- repeat at the shorter interval until the returned timer is removed.
local function callKeyRepeatCallback(callback, arguments, timer)
	if arguments.n > 0 then
		callback(table.unpack(arguments, 1, arguments.n))
	else
		callback(timer)
	end
end

function playdate.timer.keyRepeatTimer(callback, ...)
	return playdate.timer.keyRepeatTimerWithDelay(300, 100, callback, ...)
end

function playdate.timer.keyRepeatTimerWithDelay(initialDelay, repeatDelay,
		callback, ...)
	assert(type(callback) == "function", "key repeat callback must be a function")
	initialDelay = math.max(1, tonumber(initialDelay) or 300)
	repeatDelay = math.max(1, tonumber(repeatDelay) or 100)
	local arguments = table.pack(...)
	local timer
	local firstTimedFiring = true
	local function timedCallback()
		callKeyRepeatCallback(callback, arguments, timer)
		if firstTimedFiring then
			firstTimedFiring = false
			timer.duration = repeatDelay
		end
	end
	timer = playdate.timer.new(initialDelay, timedCallback)
	timer.repeats = true
	callKeyRepeatCallback(callback, arguments, timer)
	return timer
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

-- CoreLibs/animator is not present in every compiled PDX even though shared
-- libraries such as Panels use gfx.animator directly.  Provide the public
-- time-based animator at boot; a bundled CoreLibs implementation can still
-- replace this table later when a game imports it explicitly.
local Animator = {}
Animator.__index = Animator

local function animatorLinear(t, b, c, d)
	if d <= 0 then return b + c end
	return b + c * (t / d)
end

local function animatorBlend(from, to, progress)
	if type(from) == "number" and type(to) == "number" then
		return from + (to - from) * progress
	end
	if type(from) == "table" and type(to) == "table" then
		local value = {}
		for key, startValue in pairs(from) do
			local endValue = to[key]
			if type(startValue) == "number" and type(endValue) == "number" then
				value[key] = startValue + (endValue - startValue) * progress
			else
				value[key] = progress < 1 and startValue or endValue
			end
		end
		for key, endValue in pairs(to) do
			if value[key] == nil then value[key] = endValue end
		end
		return setmetatable(value, getmetatable(from))
	end
	return progress < 1 and from or to
end

local function animatorElapsed(self)
	local elapsed = playdate.getCurrentTimeMilliseconds() - self._startedAt
	return math.max(0, elapsed - self._delay)
end

local function animatorPosition(self)
	local elapsed = animatorElapsed(self)
	if elapsed <= 0 then return 0, false, false end
	local duration = math.max(1, self._duration)
	local cycles = math.floor(elapsed / duration)
	local within = elapsed % duration
	local repeatCount = math.max(0, math.floor(tonumber(self.repeatCount) or 0))
	local totalCycles = repeatCount + 1
	if cycles >= totalCycles then
		local reverse = self.reverses == true and totalCycles % 2 == 0
		return reverse and 0 or 1, true, reverse
	end
	local reverse = self.reverses == true and cycles % 2 == 1
	local position = within / duration
	if reverse then position = 1 - position end
	return position, false, reverse
end

function Animator:currentValue()
	local position, finished, reversed = animatorPosition(self)
	local easing = reversed and self.reverseEasingFunction or self.easingFunction
	local eased = position
	if type(easing) == "function" then
		local ok, result = pcall(easing, position * self._duration, 0, 1,
			self._duration, self.easingAmplitude, self.easingPeriod)
		if ok and type(result) == "number" then eased = result end
	end
	if finished then eased = position end
	return animatorBlend(self.startValue, self.endValue, eased)
end

function Animator:ended()
	local _, finished = animatorPosition(self)
	return finished
end

function Animator:progress()
	local position = animatorPosition(self)
	return position
end

function Animator:remainingTime()
	return math.max(0, self._delay + self._duration *
		(math.max(0, math.floor(tonumber(self.repeatCount) or 0)) + 1) -
		(playdate.getCurrentTimeMilliseconds() - self._startedAt))
end

function Animator:duration() return self._duration end
function Animator:reset(duration)
	if duration ~= nil then self._duration = math.max(0, tonumber(duration) or 0) end
	self._startedAt = playdate.getCurrentTimeMilliseconds()
	return self
end
function Animator:reverse()
	self.startValue, self.endValue = self.endValue, self.startValue
	self._startedAt = playdate.getCurrentTimeMilliseconds()
	return self
end

playdate.graphics.animator = playdate.graphics.animator or {}
function playdate.graphics.animator.new(duration, startValue, endValue,
	easingFunction, startDelay)
	return setmetatable({
		_duration=math.max(0, tonumber(duration) or 0),
		_delay=math.max(0, tonumber(startDelay) or 0),
		_startedAt=playdate.getCurrentTimeMilliseconds(),
		startValue=startValue,
		endValue=endValue,
		easingFunction=type(easingFunction) == "function"
			and easingFunction or animatorLinear,
		reverseEasingFunction=nil,
		repeatCount=0,
		reverses=false,
	}, Animator)
end

-- Mixer channels only route sources/effects on real Playdate hardware.  Audio
-- objects on Pogopo already feed the single mono mixer, so this object keeps
-- the same ownership and volume API without duplicating decoded PCM buffers.
local SoundChannel = {}
SoundChannel.__index = SoundChannel
function SoundChannel:addSource(source)
	if source ~= nil then self._sources[source] = true end
	return source
end
function SoundChannel:removeSource(source)
	self._sources[source] = nil
end
function SoundChannel:addEffect(effect)
	if effect ~= nil then self._effects[effect] = true end
	return effect
end
function SoundChannel:removeEffect(effect)
	self._effects[effect] = nil
end
function SoundChannel:setVolume(left, right)
	self._leftVolume = math.max(0, math.min(1, tonumber(left) or 1))
	self._rightVolume = math.max(0, math.min(1, tonumber(right) or self._leftVolume))
end
function SoundChannel:getVolume() return self._leftVolume, self._rightVolume end
function SoundChannel:setPan(pan)
	self._pan = math.max(-1, math.min(1, tonumber(pan) or 0))
end
function SoundChannel:getPan() return self._pan end

playdate.sound.channel = playdate.sound.channel or {}
function playdate.sound.channel.new()
	return setmetatable({_sources={}, _effects={}, _leftVolume=1,
		_rightVolume=1, _pan=0}, SoundChannel)
end

-- Pulp and several SDK examples construct sound.sequence during startup.  The
-- ESP32 backend currently has sample/file players and synth voices but no MIDI
-- scheduler, so expose the complete lifecycle and track container.  A loaded
-- sequence is silent, but it advances in time and never prevents gameplay.
local SoundSequence = {}
SoundSequence.__index = SoundSequence
function SoundSequence:load(path) self.path = path or ""; return true end
function SoundSequence:play(callback)
	self.playing = true
	self.paused = false
	self._startedAt = playdate.sound.getCurrentTime()
	self._finishedCallback = callback or self._finishedCallback
	return true
end
function SoundSequence:stop()
	self.playing = false; self.paused = false; self.currentStep = 0
end
function SoundSequence:pause()
	if self.playing then self.currentStep = self:getCurrentStep() end
	self.playing = false; self.paused = true
end
function SoundSequence:isPlaying() return self.playing end
function SoundSequence:setLoops(loops) self.loops = loops ~= false and loops ~= 0 end
function SoundSequence:getLoops() return self.loops end
function SoundSequence:setTempo(tempo)
	self.tempo = math.max(1, tonumber(tempo) or 120)
end
function SoundSequence:getTempo() return self.tempo end
function SoundSequence:setCurrentStep(step)
	self.currentStep = math.max(0, tonumber(step) or 0)
	self._startedAt = playdate.sound.getCurrentTime()
end
function SoundSequence:getCurrentStep()
	if not self.playing then return self.currentStep end
	local elapsed = math.max(0, playdate.sound.getCurrentTime() - self._startedAt)
	local step = self.currentStep + elapsed * self.tempo / 60
	if self.length > 0 and self.loops then step = step % self.length end
	return step
end
function SoundSequence:getLength() return self.length end
function SoundSequence:addTrack(track)
	self.tracks[#self.tracks + 1] = track
	return track
end
function SoundSequence:getTrackAtIndex(index) return self.tracks[index] end
function SoundSequence:getTrackCount() return #self.tracks end
function SoundSequence:setFinishedCallback(callback) self._finishedCallback = callback end
function SoundSequence:allNotesOff() end

playdate.sound.sequence = playdate.sound.sequence or {}
function playdate.sound.sequence.new(path)
	local sequence = setmetatable({path="", tracks={}, playing=false,
		paused=false, loops=false, tempo=120, length=0, currentStep=0,
		_startedAt=playdate.sound.getCurrentTime()}, SoundSequence)
	if path ~= nil then sequence:load(path) end
	return sequence
end
