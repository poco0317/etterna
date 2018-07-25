local tzoom = 0.5
local pdh = 48 * tzoom
local ygap = 2
local packspaceY = pdh + ygap

local numgoals = 10
local ind = 0
local offx = 5
local width = SCREEN_WIDTH * 0.6
local dwidth = width - offx * 2
local height = (numgoals+2) * packspaceY

local adjx = 14
local c0x = 10
local c1x = 10 + c0x
local c2x = c1x + (tzoom*7*adjx)			-- guesswork adjustment for epxected text length
local c5x = dwidth							-- right aligned cols
local c4x = c5x - adjx - (tzoom*3*adjx) 	-- right aligned cols
local c3x = c4x - adjx - (tzoom*10*adjx) 	-- right aligned cols
local headeroff = packspaceY/1.5

local function highlight(self)
	self:queuecommand("Highlight")
end

local function highlightIfOver(self)
	if isOver(self) then
		self:diffusealpha(0.6)
	else
		self:diffusealpha(1)
	end
end

local function byAchieved(scoregoal)
	if not scoregoal or scoregoal:IsAchieved() then
		return getMainColor('positive')
	end
	return color("#aaaaaa")
end


local packlist
local goaltable
local o = Def.ActorFrame{
	Name = "GoalDisplay",
	InitCommand=function(self)
		self:xy(0,0)
	end,
	OnCommand=function(self)
		self:SetUpdateFunction(highlight)
		packlist = DLMAN:GetPacklist()
		packlist:SetFromAll()
		self:queuecommand("PackTableRefresh")
	end,
	PackTableRefreshCommand=function(self)
		goaltable = GetPlayerOrMachineProfile(PLAYER_1):GetAllGoals()
		ind = 0
		self:queuecommand("Update")
	end,
	UpdateCommand=function(self)
		if ind < 0 then
			ind = 0
		elseif ind > #goaltable - (#goaltable % numgoals) then
			ind = #goaltable - (#goaltable % numgoals)
		end
	end,
	DFRFinishedMessageCommand=function(self)
		self:queuecommand("PackTableRefresh")
	end,
	NextPageCommand=function(self)
		ind = ind + numgoals
		self:queuecommand("Update")
	end,
	PrevPageCommand=function(self)
		ind = ind - numgoals
		self:queuecommand("Update")
	end,

	Def.Quad{InitCommand=function(self) self:zoomto(width,height-headeroff):halign(0):valign(0):diffuse(color("#888888")) end},
	
	-- headers
	Def.Quad{
		InitCommand=function(self)
			self:xy(offx, headeroff):zoomto(dwidth,pdh):halign(0):diffuse(color("#333333"))
		end,
	},
	
	LoadFont("Common normal") .. {	--priority
		InitCommand=function(self)
			self:xy(c0x + 10, headeroff):zoom(tzoom):halign(0.5)
		end,
		UpdateCommand=function(self)
			self:settext("P")
		end,
		HighlightCommand=function(self)
			if isOver(self) then
				self:settext("Priority"):diffusealpha(0.6)
			else
				self:settext("P"):diffusealpha(1)
			end
		end,
	},
	
	LoadFont("Common normal") .. {	--rate
		InitCommand=function(self)
			self:xy(c1x + 25, headeroff):zoom(tzoom):halign(0.5)
		end,
		UpdateCommand=function(self)
			self:settext("R")
		end,
		HighlightCommand=function(self)
			if isOver(self) then
				self:settext("Rate"):diffusealpha(0.6)
			else
				self:settext("R"):diffusealpha(1)
			end
		end,
	},
	
	LoadFont("Common normal") .. {	--name
		InitCommand=function(self)
			self:xy(c2x, headeroff):zoom(tzoom):halign(0):settext("Song")
		end,
		HighlightCommand=function(self)
			highlightIfOver(self)
		end,
		MouseLeftClickMessageCommand=function(self)
			if isOver(self) then
				packlist:SortByName()
				ind = 0
				self:GetParent():queuecommand("PackTableRefresh")
			end
		end,
	},
	
	LoadFont("Common normal") .. {	--completed toggle
		InitCommand=function(self)
			self:xy(c3x- 40, headeroff):zoom(tzoom):halign(0):settext("All Goals")
		end,
		HighlightCommand=function(self)
			highlightIfOver(self)
		end,
		MouseLeftClickMessageCommand=function(self)
			if isOver(self) then
				packlist:SortByDiff()
				ind = 0
				self:GetParent():queuecommand("PackTableRefresh")
			end
		end,
	},
	
	LoadFont("Common normal") .. {	--date
		InitCommand=function(self)
			self:xy(c4x- 5, headeroff):zoom(tzoom):halign(1):settext("Date")
		end,
		HighlightCommand=function(self)
			highlightIfOver(self)
		end,
		MouseLeftClickMessageCommand=function(self)
			if isOver(self) then
				packlist:SortByDiff()
				ind = 0
				self:GetParent():queuecommand("PackTableRefresh")
			end
		end,
	},
	
	LoadFont("Common normal") .. {	--diff
		InitCommand=function(self)
			self:xy(c5x, headeroff):zoom(tzoom):halign(1):settext("Diff")
		end,
		HighlightCommand=function(self)
			highlightIfOver(self)
		end,
		MouseLeftClickMessageCommand=function(self)
			if isOver(self) then
				packlist:SortBySize()
				ind = 0
				self:GetParent():queuecommand("PackTableRefresh")
			end
		end,
	},
}

local function makeGoalDisplay(i)
	local sg
	local ck
	local goalsong
	local goalsteps
	
	local o = Def.ActorFrame{
		InitCommand=function(self)
			self:y(packspaceY*i + headeroff)
		end,
		UpdateCommand=function(self)
			sg = goaltable[(i + ind)]
			if sg then
				ck = sg:GetChartKey()
				goalsong = SONGMAN:GetSongByChartKey(ck)
				goalsteps = SONGMAN:GetStepsByChartKey(ck)
				self:queuecommand("Display")
				self:visible(true)
			else
				self:visible(false)
			end
		end,
		
		Def.Quad{
			InitCommand=function(self)
				self:x(offx):zoomto(dwidth,pdh):halign(0)
			end,
			DisplayCommand=function(self)
				self:diffuse(color("#111111CC"))
			end
		},
		
		LoadFont("Common normal") .. {	--priority
			InitCommand=function(self)
				self:x(c0x):zoom(tzoom):halign(-0.5):valign(1)
			end,
			DisplayCommand=function(self)
				self:settext(sg:GetPriority())
			end,
			HighlightCommand=function(self)
				highlightIfOver(self)
			end,
			MouseLeftClickMessageCommand=function(self)
				if isOver(self) and sg then
					sg:SetPriority(sg:GetPriority()+1)
					self:GetParent():queuecommand("Update")
				end
			end,
			MouseRightClickMessageCommand=function(self)
				if isOver(self) and sg then
					sg:SetPriority(sg:GetPriority()-1)
					self:GetParent():queuecommand("Update")
				end
			end
		},
		
		LoadFont("Common normal") .. {	--steps diff
			InitCommand=function(self)
				self:x(c0x):zoom(tzoom):halign(0):valign(0)
			end,
			DisplayCommand=function(self)
				if goalsteps and goalsong then
					local diff = goalsteps:GetDifficulty()
					self:settext(getShortDifficulty(diff))
					self:diffuse(byDifficulty(diff))
				else
					self:settext("??")
					self:diffuse(getMainColor('negative'))
				end
			end,
			HighlightCommand=function(self)
				highlightIfOver(self)
			end,
		},
		
		LoadFont("Common normal") .. {	--rate
			InitCommand=function(self)
				self:x(c1x):zoom(tzoom):halign(-0.5):valign(1)
			end,
			DisplayCommand=function(self)
				local ratestring = string.format("%.2f", sg:GetRate()):gsub("%.?0$", "").."x"
				self:settext(ratestring)
			end,
			HighlightCommand=function(self)
				highlightIfOver(self)
			end,
			MouseLeftClickMessageCommand=function(self)
				if isOver(self) and sg then
					sg:SetRate(sg:GetRate()+0.1)
					self:GetParent():queuecommand("Update")
				end
			end,
			MouseRightClickMessageCommand=function(self)
				if isOver(self) and sg then
					sg:SetRate(sg:GetRate()-0.1)
					self:GetParent():queuecommand("Update")
				end
			end,
		},
		
		LoadFont("Common normal") .. {	--percent
			InitCommand=function(self)
				self:x(c1x):zoom(tzoom):halign(-0.5):valign(0)
			end,
			DisplayCommand=function(self)
				local perc = notShit.floor(sg:GetPercent()*10000)/100
				if perc < 99 then
					self:settextf("%.f%%", perc)
				else
					self:settextf("%.2f%%", perc)
				end
				self:diffuse(byAchieved(sg)):x(c1x + (2*adjx) - self:GetZoomedWidth())	-- def doing this alignment wrong
			end,
			HighlightCommand=function(self)
				highlightIfOver(self)
			end,
			MouseLeftClickMessageCommand=function(self)
				if isOver(self) and sg then
					sg:SetPercent(sg:GetPercent()+0.01)
					self:GetParent():queuecommand("Update")
				end
			end,
			MouseRightClickMessageCommand=function(self)
				if isOver(self) and sg then
					sg:SetPercent(sg:GetPercent()-0.01)
					self:GetParent():queuecommand("Update")
				end
			end,
		},
		LoadFont("Common normal") .. {	--name
			InitCommand=function(self)
				self:x(c2x):zoom(tzoom):maxwidth((c3x-c2x - (tzoom*7*adjx))/tzoom):halign(0):valign(1) -- x of left aligned col 2 minus x of right aligned col 3 minus roughly how wide column 3 is plus margin
			end,
			DisplayCommand=function(self)
				if goalsong then
					self:settext(goalsong:GetDisplayMainTitle()):diffuse(getMainColor('positive'))
				else
					self:settext("Song not found"):diffuse(getMainColor('negative'))
				end
			end,
			HighlightCommand=function(self)
				highlightIfOver(self)
			end,
			MouseLeftClickMessageCommand=function(self)
				if update and sg then 
					if isOver(self) and sg and goalsong and goalsteps then
						SCREENMAN:GetTopScreen():GetMusicWheel():SelectSong(goalsong)
						GAMESTATE:GetSongOptionsObject('ModsLevel_Preferred'):MusicRate(sg:GetRate())
						GAMESTATE:GetSongOptionsObject('ModsLevel_Song'):MusicRate(sg:GetRate())
						GAMESTATE:GetSongOptionsObject('ModsLevel_Current'):MusicRate(sg:GetRate())
						MESSAGEMAN:Broadcast("GoalSelected")
					end
				end
			end
		},
		LoadFont("Common normal") .. {	--pb
			InitCommand=function(self)
				self:x(c2x):zoom(tzoom):maxwidth((c3x-c2x - (tzoom*6*adjx))/tzoom):halign(0):valign(0)
			end,
			DisplayCommand=function(self)
				local pb = sg:GetPBUpTo()
				if pb then
					if pb:GetMusicRate() < sg:GetRate() then
						local ratestring = string.format("%.2f", pb:GetMusicRate()):gsub("%.?0$", "").."x"
							self:settextf("Best: %5.2f%% (%s)", pb:GetWifeScore() * 100, ratestring)
						else
							self:settextf("Best: %5.2f%%", pb:GetWifeScore() * 100)
						end
						self:diffuse(getGradeColor(pb:GetWifeGrade()))
						self:visible(true)
					else
					self:settextf("(Best: %5.2f%%)", 0)
					self:diffuse(byAchieved(sg))
				end
			end,
		},
		
		LoadFont("Common normal") .. {	--assigned
			InitCommand=function(self)
				self:x(c4x):zoom(tzoom):halign(1):valign(0):maxwidth((c3x-c2x - (tzoom*10*adjx))/tzoom)
			end,
			DisplayCommand=function(self)
				self:settext("Assigned: "..sg:WhenAssigned()):diffuse(byAchieved(sg))
			end
		},
		
		LoadFont("Common normal") .. {	--achieved
			InitCommand=function(self)
				self:x(c4x):zoom(tzoom):halign(1):valign(1):maxwidth((c3x-c2x - (tzoom*10*adjx))/tzoom)
			end,
			DisplayCommand=function(self)
				if sg:IsAchieved() then
					self:settext("Achieved: "..sg:WhenAchieved())
				elseif sg:IsVacuous() then
					self:settext("Vacuous goal")
				else
					self:settext("")
				end
				self:diffuse(byAchieved(sg))
			end
		},
		
		LoadFont("Common normal") .. {	--diff
			InitCommand=function(self)
				self:x(c5x):zoom(tzoom):halign(1):valign(1)
			end,
			DisplayCommand=function(self)
				if goalsteps then
					local msd = goalsteps:GetMSD(sg:GetRate(), 1)
					self:settextf("%5.1f", msd):diffuse(byMSD(msd))
				else
					self:settext("??")
				end
			end,
		},
		
		Def.Quad{	-- delete button
			InitCommand=function(self)
				self:x(c5x):zoom(tzoom):halign(1):valign(-1):zoomto(4,4):diffuse(byJudgment('TapNoteScore_Miss'))
			end,
			MouseLeftClickMessageCommand=function(self)
				if sg and isOver(self) and update and sg then
					sg:Delete()
					MESSAGEMAN:Broadcast("UpdateGoals")
				end
			end,
			HighlightCommand=function(self)
				highlightIfOver(self)
			end,
			MouseLeftClickMessageCommand=function(self)
				if isOver(self) then
					sg:Delete()
				end
			end
		},
	}
	return o
end

for i=1,numgoals do
	o[#o+1] = makeGoalDisplay(i)
end

return o