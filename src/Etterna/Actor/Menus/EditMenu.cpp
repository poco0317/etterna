#include "Etterna/Globals/global.h"
#include "EditMenu.h"
#include "Etterna/Actor/Base/ActorUtil.h"
#include "Etterna/Singletons/SongManager.h"
#include "Etterna/Singletons/GameState.h"
#include "Etterna/Singletons/ThemeManager.h"
#include "Etterna/Singletons/GameManager.h"
#include "Etterna/Models/StepsAndStyles/Steps.h"
#include "Etterna/Models/Songs/Song.h"
#include "Etterna/Models/StepsAndStyles/StepsUtil.h"
#include "Etterna/Models/Misc/Foreach.h"
#include "Etterna/Models/Misc/CommonMetrics.h"
#include "Etterna/Models/Songs/SongUtil.h"

static const char* EditMenuRowNames[] = { "Group",			 "Song",
										  "StepsType",		 "Steps",
										  "SourceStepsType", "SourceSteps",
										  "Action" };
XToString(EditMenuRow);
XToLocalizedString(EditMenuRow);

static const char* EditMenuActionNames[] = {
	"Edit", "Delete", "Create", "Practice", "LoadAutosave",
};
XToString(EditMenuAction);
XToLocalizedString(EditMenuAction);
StringToX(EditMenuAction);

static std::string
ARROWS_X_NAME(size_t i)
{
	return ssprintf("Arrows%dX", static_cast<int>(i + 1));
}
static std::string
ROW_Y_NAME(size_t i)
{
	return ssprintf("Row%dY", static_cast<int>(i + 1));
}

void
EditMenu::GetSongsToShowForGroup(const std::string& sGroup,
								 vector<Song*>& vpSongsOut)
{
	if (sGroup == "") {
		vpSongsOut.clear();
		return;
	}
	vpSongsOut = SONGMAN->GetSongs(SHOW_GROUPS.GetValue() ? sGroup : GROUP_ALL);
	SongUtil::SortSongPointerArrayByTitle(vpSongsOut);
}

void
EditMenu::GetGroupsToShow(vector<std::string>& vsGroupsOut)
{
	vsGroupsOut.clear();
	if (!SHOW_GROUPS.GetValue())
		return;

	SONGMAN->GetSongGroupNames(vsGroupsOut);
	for (int i = vsGroupsOut.size() - 1; i >= 0; i--) {
		const std::string& sGroup = vsGroupsOut[i];
		vector<Song*> vpSongs;
		GetSongsToShowForGroup(sGroup, vpSongs);
		// strip groups that have no unlocked songs
		if (vpSongs.empty())
			vsGroupsOut.erase(vsGroupsOut.begin() + i);
	}
}

EditMenu::EditMenu() {}

EditMenu::~EditMenu() {}

void
EditMenu::Load(const std::string& sType)
{
	Locator::getLogger()->trace("EditMenu::Load");

	SHOW_GROUPS.Load(sType, "ShowGroups");
	ARROWS_X.Load(sType, ARROWS_X_NAME, NUM_ARROWS);
	ARROWS_ENABLED_COMMAND.Load(sType, "ArrowsEnabledCommand");
	ARROWS_DISABLED_COMMAND.Load(sType, "ArrowsDisabledCommand");
	ROW_Y.Load(sType, ROW_Y_NAME, NUM_EditMenuRow);
	EDIT_MODE.Load(sType, "EditMode");
	TEXT_BANNER_TYPE.Load(m_sName, "TextBannerType");

	for (int i = 0; i < 2; i++) {
		m_sprArrows[i].Load(THEME->GetPathG(sType, i == 0 ? "left" : "right"));
		m_sprArrows[i]->SetX(ARROWS_X.GetValue(i));
		this->AddChild(m_sprArrows[i]);
	}

	m_SelectedRow = GetFirstRow();

	ZERO(m_iSelection);

	FOREACH_EditMenuRow(r)
	{
		m_textLabel[r].SetName(ssprintf("Label%i", r + 1));
		m_textLabel[r].LoadFromFont(THEME->GetPathF(sType, "title"));
		m_textLabel[r].SetText(EditMenuRowToLocalizedString(r));
		ActorUtil::LoadAllCommandsAndSetXY(m_textLabel[r], sType);
		// m_textLabel[r].SetHorizAlign( align_left );
		this->AddChild(&m_textLabel[r]);

		m_textValue[r].SetName(ssprintf("Value%i", r + 1));
		m_textValue[r].LoadFromFont(THEME->GetPathF(sType, "value"));
		m_textValue[r].SetText("blah");
		ActorUtil::LoadAllCommandsAndSetXY(m_textValue[r], sType);
		this->AddChild(&m_textValue[r]);
	}

	m_textLabel[ROW_GROUP].SetVisible(SHOW_GROUPS.GetValue());
	m_textValue[ROW_GROUP].SetVisible(SHOW_GROUPS.GetValue());

	m_SongTextBanner.SetName("SongTextBanner");
	m_SongTextBanner.Load(TEXT_BANNER_TYPE);
	ActorUtil::SetXY(m_SongTextBanner, sType);
	ActorUtil::LoadAllCommands(m_SongTextBanner, sType);
	this->AddChild(&m_SongTextBanner);

	m_StepsDisplay.SetName("StepsDisplay");
	m_StepsDisplay.Load("StepsDisplayEdit", NULL);
	ActorUtil::SetXY(m_StepsDisplay, sType);
	this->AddChild(&m_StepsDisplay);

	m_StepsDisplaySource.SetName("StepsDisplaySource");
	m_StepsDisplaySource.Load("StepsDisplayEdit", NULL);
	ActorUtil::SetXY(m_StepsDisplaySource, sType);
	this->AddChild(&m_StepsDisplaySource);

	m_soundChangeRow.Load(THEME->GetPathS(sType, "row"), true);
	m_soundChangeValue.Load(THEME->GetPathS(sType, "value"), true);

	// fill in data structures
	GetGroupsToShow(m_sGroups);

	// In EditMode_Practice this will be filled in by OnRowValueChanged()
	if (EDIT_MODE.GetValue() != EditMode_Practice)
		m_StepsTypes = CommonMetrics::STEPS_TYPES_TO_SHOW.GetValue();

	RefreshAll();
}

void
EditMenu::RefreshAll()
{
	if (!SafeToUse()) {
		return;
	}

	ChangeToRow(GetFirstRow());
	OnRowValueChanged((EditMenuRow)0);

	// Select the current song if any
	if (GAMESTATE->m_pCurSong) {
		for (unsigned i = 0; i < m_sGroups.size(); i++)
			if (GAMESTATE->m_pCurSong->m_sGroupName == m_sGroups[i])
				m_iSelection[ROW_GROUP] = i;
		OnRowValueChanged(ROW_GROUP);

		for (unsigned i = 0; i < m_pSongs.size(); i++)
			if (GAMESTATE->m_pCurSong == m_pSongs[i])
				m_iSelection[ROW_SONG] = i;
		OnRowValueChanged(ROW_SONG);

		// Select the current StepsType and difficulty if any
		if (GAMESTATE->m_pCurSteps) {
			for (unsigned i = 0; i < m_StepsTypes.size(); i++) {
				if (m_StepsTypes[i] == GAMESTATE->m_pCurSteps->m_StepsType) {
					m_iSelection[ROW_STEPS_TYPE] = i;
					OnRowValueChanged(ROW_STEPS_TYPE);
				}
			}

			for (unsigned i = 0; i < m_vpSteps.size(); i++) {
				const Steps* pSteps = m_vpSteps[i].pSteps;
				if (pSteps == GAMESTATE->m_pCurSteps) {
					m_iSelection[ROW_STEPS] = i;
					OnRowValueChanged(ROW_STEPS);
				}
			}
		}
	}
}

bool
EditMenu::SafeToUse()
{
	return !m_sGroups.empty();
}

bool
EditMenu::CanGoUp()
{
	return m_SelectedRow != GetFirstRow();
}

bool
EditMenu::CanGoDown()
{
	return m_SelectedRow != NUM_EditMenuRow - 1;
}

bool
EditMenu::CanGoLeft()
{
	if (GetRowSize(m_SelectedRow) <= 0) {
		return false;
	}
	if (m_SelectedRow == ROW_SONG || m_SelectedRow == ROW_GROUP)
		return true; // wraps
	return m_iSelection[m_SelectedRow] != 0;
}

int
EditMenu::GetRowSize(EditMenuRow er) const
{
	switch (er) {
		case ROW_GROUP:
			return m_sGroups.size();
		case ROW_SONG:
			return m_pSongs.size();
		case ROW_STEPS_TYPE:
			return m_StepsTypes.size();
		case ROW_STEPS:
			return m_vpSteps.size();
		case ROW_SOURCE_STEPS_TYPE:
			return m_StepsTypes.size();
		case ROW_SOURCE_STEPS:
			return m_vpSourceSteps.size();
		case ROW_ACTION:
			return m_Actions.size();
		default:
			FAIL_M(ssprintf("Non-existant EditMenuRow %i", er));
	}
}

bool
EditMenu::CanGoRight()
{
	if (GetRowSize(m_SelectedRow) <= 0) {
		return false;
	}
	if (m_SelectedRow == ROW_SONG || m_SelectedRow == ROW_GROUP)
		return true; // wraps
	return m_iSelection[m_SelectedRow] != GetRowSize(m_SelectedRow) - 1;
}

bool
EditMenu::RowIsSelectable(EditMenuRow row)
{
	if (EDIT_MODE == EditMode_Home && row == ROW_STEPS)
		return false;

	if (GetSelectedSteps()) {
		switch (row) {
			case ROW_SOURCE_STEPS_TYPE:
			case ROW_SOURCE_STEPS:
				return false;
			default:
				return true;
		}
	}
	return true;
}

void
EditMenu::Up()
{
	EditMenuRow dest = m_SelectedRow;
	do {
		dest = (EditMenuRow)(dest - 1);
	} while (!RowIsSelectable(dest));
	ASSERT(dest >= 0);
	ChangeToRow(dest);
	m_soundChangeRow.Play(true);
}

void
EditMenu::Down()
{
	EditMenuRow dest = m_SelectedRow;
	do {
		dest = (EditMenuRow)(dest + 1);
	} while (!RowIsSelectable(dest));
	ASSERT(dest < NUM_EditMenuRow);
	ChangeToRow(dest);
	m_soundChangeRow.Play(true);
}

void
EditMenu::Left()
{
	if (CanGoLeft()) {
		m_iSelection[m_SelectedRow]--;
		wrap(m_iSelection[m_SelectedRow], GetRowSize(m_SelectedRow));
		OnRowValueChanged(m_SelectedRow);
		m_soundChangeValue.Play(true);
	}
}

void
EditMenu::Right()
{
	if (CanGoRight()) {
		m_iSelection[m_SelectedRow]++;
		wrap(m_iSelection[m_SelectedRow], GetRowSize(m_SelectedRow));
		OnRowValueChanged(m_SelectedRow);
		m_soundChangeValue.Play(true);
	}
}

void
EditMenu::ChangeToRow(EditMenuRow newRow)
{
	m_textLabel[newRow].PlayCommand("GainFocus");
	m_textValue[newRow].PlayCommand("GainFocus");
	m_textLabel[m_SelectedRow].PlayCommand("LoseFocus");
	m_textValue[m_SelectedRow].PlayCommand("LoseFocus");

	m_SelectedRow = newRow;

	for (int i = 0; i < 2; i++)
		m_sprArrows[i]->SetY(ROW_Y.GetValue(newRow));
	UpdateArrows();
}

void
EditMenu::UpdateArrows()
{
	m_sprArrows[0]->RunCommands(CanGoLeft() ? ARROWS_ENABLED_COMMAND
											: ARROWS_DISABLED_COMMAND);
	m_sprArrows[1]->RunCommands(CanGoRight() ? ARROWS_ENABLED_COMMAND
											 : ARROWS_DISABLED_COMMAND);
	m_sprArrows[0]->EnableAnimation(CanGoLeft());
	m_sprArrows[1]->EnableAnimation(CanGoRight());
}

static LocalizedString BLANK("EditMenu", "Blank");
void
EditMenu::OnRowValueChanged(EditMenuRow row)
{
	if (!SafeToUse()) {
		return;
	}

	UpdateArrows();

	EditMode mode = EDIT_MODE.GetValue();

	switch (row) {
		case ROW_GROUP:
			m_pSongs.clear();
			if (GetSelectedGroup() == "") {
				m_textValue[ROW_GROUP].SetText(
				  THEME->GetString(m_sName, "No Group Selected."));
			} else {
				m_textValue[ROW_GROUP].SetText(
				  SONGMAN->ShortenGroupName(GetSelectedGroup()));
				if (mode == EditMode_Practice) {
					vector<Song*> vtSongs;
					GetSongsToShowForGroup(GetSelectedGroup(), vtSongs);
					// Filter out songs that aren't playable.
					FOREACH(Song*, vtSongs, s)
					{
						if (SongUtil::IsSongPlayable(*s)) {
							m_pSongs.push_back(*s);
						}
					}
				} else {
					GetSongsToShowForGroup(GetSelectedGroup(), m_pSongs);
				}
			}

			m_iSelection[ROW_SONG] = 0;
			// fall through
		case ROW_SONG:
			if (GetSelectedSong() == NULL) {
				m_textValue[ROW_SONG].SetText("");
				m_SongTextBanner.SetFromString("", "", "", "", "", "");
				if (mode == EditMode_Practice) {
					m_iSelection[ROW_STEPS_TYPE] = 0;
					m_StepsTypes.clear();
				}
			} else {
				m_textValue[ROW_SONG].SetText("");
				m_SongTextBanner.SetFromSong(GetSelectedSong());

				if (mode == EditMode_Practice) {
					StepsType orgSel = StepsType_Invalid;
					if (!m_StepsTypes.empty()) // Not first run
					{
						ASSERT((int)m_StepsTypes.size() >
							   m_iSelection[ROW_STEPS_TYPE]);
						orgSel = m_StepsTypes[m_iSelection[ROW_STEPS_TYPE]];
					}

					// The StepsType selection may no longer be valid. Zero it
					// for now.
					m_iSelection[ROW_STEPS_TYPE] = 0;
					m_StepsTypes.clear();

					// Only show StepsTypes for which we have valid Steps.
					vector<StepsType> vSts =
					  CommonMetrics::STEPS_TYPES_TO_SHOW.GetValue();
					FOREACH(StepsType, vSts, st)
					{
						if (SongUtil::GetStepsByDifficulty(GetSelectedSong(),
														   *st,
														   Difficulty_Invalid,
														   false) != NULL)
							m_StepsTypes.push_back(*st);

						// Try to preserve the user's StepsType selection.
						if (*st == orgSel)
							m_iSelection[ROW_STEPS_TYPE] =
							  m_StepsTypes.size() - 1;
					}
				}
			}

			// fall through
		case ROW_STEPS_TYPE:
			if (GetSelectedStepsType() == StepsType_Invalid) {
				m_textValue[ROW_STEPS_TYPE].SetText(
				  THEME->GetString(m_sName, "No StepsType selected."));
				m_vpSteps.clear();
			} else {
				m_textValue[ROW_STEPS_TYPE].SetText(
				  GAMEMAN->GetStepsTypeInfo(GetSelectedStepsType())
					.GetLocalizedString());

				Difficulty dcOld = Difficulty_Invalid;
				if (!m_vpSteps.empty()) {
					dcOld = GetSelectedDifficulty();
				}

				m_vpSteps.clear();

				FOREACH_ENUM(Difficulty, dc)
				{
					if (dc == Difficulty_Edit) {
						switch (mode) {
							case EditMode_Full:
							case EditMode_Practice: {
								vector<Steps*> v;
								SongUtil::GetSteps(GetSelectedSong(),
												   v,
												   GetSelectedStepsType(),
												   Difficulty_Edit);
								StepsUtil::SortStepsByDescription(v);
								FOREACH_CONST(Steps*, v, p)
								m_vpSteps.push_back(StepsAndDifficulty(*p, dc));
							} break;
							case EditMode_Home:
								// have only "New Edit"
								break;
							default:
								FAIL_M(ssprintf("Invalid edit mode: %i", mode));
						}

						switch (mode) {
							case EditMode_Practice:
							case EditMode_Home:
							case EditMode_Full:
								m_vpSteps.push_back(
								  StepsAndDifficulty(NULL, dc)); // "New Edit"
								break;
							default:
								FAIL_M(ssprintf("Invalid edit mode: %i", mode));
						}
					} else {
						Steps* pSteps = SongUtil::GetStepsByDifficulty(
						  GetSelectedSong(), GetSelectedStepsType(), dc);

						switch (mode) {
							case EditMode_Home:
								// don't allow selecting of non-edits in
								// HomeMode
								break;
							case EditMode_Practice:
							case EditMode_Full:
								// show this difficulty whether or not steps
								// exist.
								m_vpSteps.push_back(
								  StepsAndDifficulty(pSteps, dc));
								break;
							default:
								FAIL_M(ssprintf("Invalid edit mode: %i", mode));
						}
					}
				}

				FOREACH(StepsAndDifficulty, m_vpSteps, s)
				{
					if (s->dc == dcOld) {
						m_iSelection[ROW_STEPS] = s - m_vpSteps.begin();
						break;
					}
				}
				CLAMP(m_iSelection[ROW_STEPS], 0, m_vpSteps.size() - 1);
			}
			// fall through
		case ROW_STEPS:
			if (GetSelectedSteps() == NULL && mode == EditMode_Practice) {
				m_textValue[ROW_STEPS].SetText(
				  THEME->GetString(m_sName, "No Steps selected."));
				m_StepsDisplay.Unset();
			} else {
				std::string s =
				  CustomDifficultyToLocalizedString(GetCustomDifficulty(
					GetSelectedStepsType(), GetSelectedDifficulty()));

				m_textValue[ROW_STEPS].SetText(s);
				if (GetSelectedSteps()) {
					m_StepsDisplay.SetFromSteps(GetSelectedSteps());
				} else {
					m_StepsDisplay
					  .SetFromStepsTypeAndMeterAndDifficultyAndCourseType(
						GetSelectedSourceStepsType(),
						0,
						GetSelectedDifficulty());
				}
			}
		// fall through
		case ROW_SOURCE_STEPS_TYPE:
			if (mode == EditMode_Practice) {
				m_textLabel[ROW_SOURCE_STEPS_TYPE].SetVisible(false);
				m_textValue[ROW_SOURCE_STEPS_TYPE].SetVisible(false);
			} else {
				m_textLabel[ROW_SOURCE_STEPS_TYPE].SetVisible(
				  GetSelectedSteps() ? false : true);
				m_textValue[ROW_SOURCE_STEPS_TYPE].SetVisible(
				  GetSelectedSteps() ? false : true);
				m_textValue[ROW_SOURCE_STEPS_TYPE].SetText(
				  GAMEMAN->GetStepsTypeInfo(GetSelectedSourceStepsType())
					.GetLocalizedString());
				m_vpSourceSteps.clear();
				m_vpSourceSteps.push_back(
				  StepsAndDifficulty(NULL, Difficulty_Invalid)); // "blank"

				FOREACH_ENUM(Difficulty, dc)
				{
					// fill in m_vpSourceSteps
					if (dc != Difficulty_Edit) {
						Steps* pSteps = SongUtil::GetStepsByDifficulty(
						  GetSelectedSong(), GetSelectedSourceStepsType(), dc);
						if (pSteps != NULL)
							m_vpSourceSteps.push_back(
							  StepsAndDifficulty(pSteps, dc));
					} else {
						vector<Steps*> v;
						SongUtil::GetSteps(GetSelectedSong(),
										   v,
										   GetSelectedSourceStepsType(),
										   dc);
						StepsUtil::SortStepsByDescription(v);
						FOREACH_CONST(Steps*, v, pSteps)
						m_vpSourceSteps.push_back(
						  StepsAndDifficulty(*pSteps, dc));
					}
				}
				CLAMP(m_iSelection[ROW_SOURCE_STEPS],
					  0,
					  m_vpSourceSteps.size() - 1);
			}
			// fall through
		case ROW_SOURCE_STEPS:
			if (mode == EditMode_Practice) {
				m_textLabel[ROW_SOURCE_STEPS].SetVisible(false);
				m_textValue[ROW_SOURCE_STEPS].SetVisible(false);
				m_Actions.clear();
				m_Actions.push_back(EditMenuAction_Practice);
			} else {
				m_textLabel[ROW_SOURCE_STEPS].SetVisible(
				  GetSelectedSteps() ? false : true);
				m_textValue[ROW_SOURCE_STEPS].SetVisible(
				  GetSelectedSteps() ? false : true);
				{
					std::string s;
					if (GetSelectedSourceDifficulty() == Difficulty_Invalid) {
						s = BLANK;
					} else {
						s = CustomDifficultyToLocalizedString(
						  GetCustomDifficulty(GetSelectedSourceStepsType(),
											  GetSelectedSourceDifficulty()));
					}
					m_textValue[ROW_SOURCE_STEPS].SetText(s);
				}
				bool bHideMeter = false;
				if (GetSelectedSourceDifficulty() == Difficulty_Invalid)
					bHideMeter = true;
				else if (GetSelectedSourceSteps())
					m_StepsDisplaySource.SetFromSteps(GetSelectedSourceSteps());
				else
					m_StepsDisplaySource
					  .SetFromStepsTypeAndMeterAndDifficultyAndCourseType(
						GetSelectedSourceStepsType(),
						0,
						GetSelectedSourceDifficulty());
				m_StepsDisplaySource.SetVisible(
				  !(bHideMeter || GetSelectedSteps()));

				m_Actions.clear();
				// Stick autosave in the list first so that people will see it.
				// -Kyz
				Song* cur_song = GetSelectedSong();
				if (GetSelectedSteps()) {
					switch (mode) {
						case EditMode_Practice:
						case EditMode_Home:
						case EditMode_Full:
							m_Actions.push_back(EditMenuAction_Edit);
							m_Actions.push_back(EditMenuAction_Delete);
							break;
						default:
							FAIL_M(ssprintf("Invalid edit mode: %i", mode));
					}
				} else {
					m_Actions.push_back(EditMenuAction_Create);
				}
				m_iSelection[ROW_ACTION] = 0;
			}
			// fall through
		case ROW_ACTION:
			if (GetSelectedAction() == EditMenuAction_Invalid) {
				m_textValue[ROW_ACTION].SetText(
				  THEME->GetString(m_sName, "No valid action."));
			} else {
				m_textValue[ROW_ACTION].SetText(
				  EditMenuActionToLocalizedString(GetSelectedAction()));
			}
			break;
		default:
			FAIL_M(ssprintf("Invalid EditMenuRow: %i", row));
	}
}
