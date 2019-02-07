local t = Def.ActorFrame {}
t[#t + 1] = LoadActor("../_frame")
t[#t + 1] = LoadActor("../_PlayerInfo")
t[#t + 1] = LoadActor("currenttime")

--what the settext says
t[#t + 1] =
	LoadFont("Common Large") ..
	{
		InitCommand = function(self)
			self:xy(5, 32):halign(0):valign(1):zoom(0.55):diffuse(getMainColor("positive")):settext("Results:")
		end
	}

--Group folder name
local frameWidth = 280
local frameHeight = 20
local frameX = SCREEN_WIDTH - 5
local frameY = 15

-- bad way to do this
local modslevel = topscreen == "ScreenEditOptions" and "ModsLevel_Stage" or "ModsLevel_Preferred"
local playeroptions = GAMESTATE:GetPlayerState(PLAYER_1):GetPlayerOptions(modslevel)
local isPractice = playerConfig:get_data(pn_to_profile_slot(PLAYER_1)).PracticeMode
if isPractice then
	playeroptions:FailSetting("FailType_Immediate")
end

t[#t + 1] =
	LoadFont("Common Large") ..
	{
		InitCommand = function(self)
			self:xy(frameX, frameY + 5):halign(1):zoom(0.55):maxwidth((frameWidth - 40) / 0.35)
		end,
		BeginCommand = function(self)
			self:queuecommand("Set"):diffuse(getMainColor("positive"))
		end,
		SetCommand = function(self)
			local song = GAMESTATE:GetCurrentSong()
			if song ~= nil then
				self:settext(song:GetGroupName())
			end
		end
	}

t[#t + 1] = LoadActor("../_cursor")

return t
