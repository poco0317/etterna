-- chart preview
-- it copies much of the offset plot code
-- except it uses some c++ utility functions to get stuff really working
-- good luck

local plotWidth, plotHeight = SCREEN_WIDTH * 0.3, SCREEN_HEIGHT
local plotX, plotY = SCREEN_WIDTH - plotWidth/2 - 25, -SCREEN_HEIGHT/2
local plotMargin = 4
local dotWidth = 8
local dotHeight = 0.5
local baralpha = 0.2
local bgalpha = 1
local textzoom = 0.5

local noteData = {}
local tracks = {}	-- table or whatever its called, for each column

local scrollReverse = false




-- utility to put a dot where it should be
local function fillVertStruct( vt, x, y, givencolor )
	vt[#vt + 1] = {{x - dotWidth, y + dotHeight, 0}, givencolor}
	vt[#vt + 1] = {{x + dotWidth, y + dotHeight, 0}, givencolor}
	vt[#vt + 1] = {{x + dotWidth, y - dotHeight, 0}, givencolor}
	vt[#vt + 1] = {{x - dotWidth, y - dotHeight, 0}, givencolor}
end

local function fitX( number, totalnumber ) -- find X relative to the center of the plot
	return -plotWidth / 2 + plotWidth * (number / totalnumber) - (plotWidth/(totalnumber*2))
end

local function fitY( number, tracks ) -- find Y relative to the middle of the screen
	return -SCREEN_HEIGHT / 2 + SCREEN_HEIGHT * (number / tracks) * (scrollReverse and -1 or 1)
end

local function randomTrack( numberoftracks )
	return math.random(numberoftracks)
end

local fakestruct = {}	-- table of each note row that has 4 tracks for each row
for i = 1, 750 do
	-- the length of the things here do not matter
	fakestruct[i] = { math.random(2), math.random(2), math.random(2), math.random(2), math.random(2), math.random(2), math.random(2) }
end

local function interpretNoteData()
	--[[
	This particular Notedata format...
	map of stuff, which is: int row -> [NoteInfoExtended]
	NoteInfoExtended is made of a TapNoteType and an int for the tracks.
	]]
	ms.ok(#noteData)
	
	
end

local function paginateNoteData()

end

-- button inputs because i dont want to use the metrics to do button inputs
local function input(event)
	if event.DeviceInput.button == "DeviceButton_right mouse button" or event.DeviceInput.button == "DeviceButton_left mouse button" then
		if event.type == "InputEventType_Release" then
			SCREENMAN:GetTopScreen():Cancel()
		end
	end
	if event.type ~= "InputEventType_Release" then
		if event.GameButton == "Back" or event.GameButton == "Start" then
			SCREENMAN:GetTopScreen():Cancel()
		end
	end
	return false
end


-- the fuel that lights this dumpsterfire
local p =
	Def.ActorFrame {
	OnCommand = function(self)
		SCREENMAN:GetTopScreen():AddInputCallback(input)
		self:xy(plotX, SCREEN_CENTER_Y)
		
		song = GAMESTATE:GetCurrentSong()
		steps = GAMESTATE:GetCurrentSteps(PLAYER_1)
		if song and steps then
			noteData = steps:GetNonEmptyNoteData()
		else
			noteData = nil
		end
		
		MESSAGEMAN:Broadcast("ChartPreviewUpdate")
	end
}

-- the big black background box
p[#p + 1] =
	Def.Quad {
	ChartPreviewUpdateMessageCommand = function(self)
		self:zoomto(plotWidth, plotHeight):diffuse(color("0.05,0.05,0.05,0.05")):diffusealpha(
			bgalpha
		)
	end
}


-- the DOTS
p[#p + 1] =
	Def.ActorMultiVertex {
	ChartPreviewUpdateMessageCommand = function(self)
		local verts = {} -- this is a structure which holds each dot and its parameters (x, y, z, color) i dunno this is a copy paste lmao
		local numberoftracks = #fakestruct[1] -- WOW EFFICIENCY !!!!
		local numberofrows = #fakestruct -- WOWWW!!!! !!
		
		interpretNoteData()
		paginateNoteData()
		
		for row = 1, numberofrows do
			local y = fitY(row, numberofrows)
			local dotcolor = offsetToJudgeColor(0, false) -- some color
			for track = 1, numberoftracks do
				local x = fitX(track, numberoftracks)
				if fakestruct[row][track] == 1 then
					fillVertStruct( verts, x, y, dotcolor )
				end
			end
		end
		self:SetVertices(verts)
		self:SetDrawState {Mode = "DrawMode_Quads", First = 1, Num = #verts}
	end
}

return p -- p stands for preview :)
