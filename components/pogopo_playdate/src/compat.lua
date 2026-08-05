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
local wall_cells = {}
local background_callback = nil
local sprite_sequence = 0

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
	self.x, self.y = 0, 0
	self.centerX, self.centerY = 0.5, 0.5
	self.width, self.height = 0, 0
	if image then self.width, self.height = image:getSize() end
	self.zIndex = 0
	self.visible = true
	self.added = false
	self.updatesEnabled = true
	self.collisionsEnabled = true
	self.imageDrawMode = gfx.kDrawModeCopy
	self.flip = gfx.kImageUnflipped
	self.groups = {}
	self.collidesWithGroups = {}
	self._sequence = 0
end

function Sprite.new(image) return Sprite(image) end

function Sprite:setImage(image, flip)
	self.image = image
	self.flip = flip or gfx.kImageUnflipped
	if image then
		local width, height = image:getSize()
		if self.width == 0 then self.width = width end
		if self.height == 0 then self.height = height end
	end
end
function Sprite:getImage() return self.image end
function Sprite:setSize(width, height) self.width, self.height = width, height end
function Sprite:setCenter(x, y) self.centerX, self.centerY = x, y end
function Sprite:moveTo(x, y) self.x, self.y = x, y end
function Sprite:moveBy(x, y) self.x, self.y = self.x + x, self.y + y end
function Sprite:setZIndex(value) self.zIndex = value end
function Sprite:setVisible(value) self.visible = value == true end
function Sprite:isVisible() return self.visible end
function Sprite:setUpdatesEnabled(value) self.updatesEnabled = value == true end
function Sprite:setImageDrawMode(value) self.imageDrawMode = value end
function Sprite:setGroups(value) self.groups = value or {} end
function Sprite:setCollidesWithGroups(value) self.collidesWithGroups = value or {} end
function Sprite:setCollisionsEnabled(value) self.collisionsEnabled = value == true end

function Sprite:setCollideRect(x, y, width, height)
	if type(x) == "table" then
		self.collideRect = {x=x.x or 0, y=x.y or 0,
			width=x.width or x.w or 0, height=x.height or x.h or 0}
	else
		self.collideRect = {x=x or 0, y=y or 0, width=width or 0, height=height or 0}
	end
end
function Sprite:clearCollideRect() self.collideRect = nil end

function Sprite:add()
	if self.added then return self end
	self.added = true
	sprite_sequence = sprite_sequence + 1
	self._sequence = sprite_sequence
	table.insert(sprites, self)
	return self
end

function Sprite:remove()
	self.added = false
	return self
end

function Sprite:update()
	-- Base-instance update is intentionally empty. A call with no receiver is
	-- the static Playdate sprite frame update.
	if self ~= nil then return end
	gfx._beginFrame()
	if background_callback then background_callback(0, 0, 400, 240) end
	table.sort(sprites, function(a, b)
		if a.zIndex == b.zIndex then return a._sequence < b._sequence end
		return a.zIndex < b.zIndex
	end)
	for i=1,#sprites do
		local item = sprites[i]
		if item.added and item.updatesEnabled then
			local update = item.update
			if update and update ~= Sprite.update then update(item) end
		end
	end
	for i=1,#sprites do
		local item = sprites[i]
		if item.added and item.visible and item.image then
			local previous = gfx._getImageDrawMode()
			gfx.setImageDrawMode(item.imageDrawMode)
			local x = math.floor(item.x - item.width * item.centerX)
			local y = math.floor(item.y - item.height * item.centerY)
			item.image:draw(x, y, item.flip)
			gfx.setImageDrawMode(previous)
		end
	end
end

function Sprite.performOnAllSprites(callback)
	for i=1,#sprites do if sprites[i].added then callback(sprites[i]) end end
end

function Sprite.removeAll()
	for i=1,#sprites do sprites[i].added = false end
	sprites = {}
	wall_cells = {}
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
		if other ~= self and other.added and other.collisionsEnabled and
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

function Sprite.querySpritesInRect(x, y, width, height)
	if type(x) == "table" then
		local rect = x
		x, y = rect.x, rect.y
		width, height = rect.width or rect.w, rect.height or rect.h
	end
	local result, seen = {}, {}
	local minx = math.floor((x or 0) / 8)
	local maxx = math.floor(((x or 0) + math.max(0, (width or 0) - 1)) / 8)
	local miny = math.floor((y or 0) / 8)
	local maxy = math.floor(((y or 0) + math.max(0, (height or 0) - 1)) / 8)
	for cy=miny,maxy do
		for cx=minx,maxx do
			local cell = wall_cells[cx .. ":" .. cy]
			if cell then
				for i=1,#cell do
					local item = cell[i]
					if item.added and item.collisionsEnabled and not seen[item] then
						local sx, sy, sw, sh = spriteBounds(item)
						if overlaps(x,y,width,height,sx,sy,sw,sh) then
							seen[item] = true
							table.insert(result, item)
						end
					end
				end
			end
		end
	end
	for i=1,#sprites do
		local item = sprites[i]
		if not item._wall and item.added and item.collisionsEnabled and not seen[item] then
			local sx, sy, sw, sh = spriteBounds(item)
			if overlaps(x,y,width,height,sx,sy,sw,sh) then
				seen[item] = true
				table.insert(result, item)
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
			item:setSize(8, 8)
			item:setCollideRect(0, 0, 8, 8)
			item:moveTo(offsetX + column * 8 + 4, offsetY + row * 8 + 4)
			item:add()
			local key = math.floor((offsetX + column * 8) / 8) .. ":" ..
				math.floor((offsetY + row * 8) / 8)
			wall_cells[key] = wall_cells[key] or {}
			table.insert(wall_cells[key], item)
			table.insert(result, item)
		end
	end
	return result
end

Sprite.kCollisionTypeOverlap = 1
gfx.sprite = Sprite

local Tilemap = {}
Tilemap.__index = Tilemap
function Tilemap.new() return setmetatable({tiles={}, width=1, imagetable=nil}, Tilemap) end
function Tilemap:setTiles(tiles, width) self.tiles, self.width = tiles, width end
function Tilemap:setImageTable(value) self.imagetable = value end
function Tilemap:draw(x, y)
	if not self.imagetable then return end
	for index=1,#self.tiles do
		local tile = self.tiles[index]
		if tile and tile > 0 then
			local image = self.imagetable:getImage(tile)
			if image then
				local column = (index - 1) % self.width
				local row = math.floor((index - 1) / self.width)
				image:draw((x or 0) + column * 8, (y or 0) + row * 8)
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
