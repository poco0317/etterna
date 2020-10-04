#include "Etterna/Globals/global.h"
#include "ScreenEditMenu.h"
#include "Etterna/Models/Misc/CommonMetrics.h"
#include "Etterna/Models/Misc/GameConstantsAndTypes.h"
#include "Etterna/Singletons/GameManager.h"
#include "Etterna/Singletons/GameSoundManager.h"
#include "Etterna/Singletons/GameState.h"
#include "Etterna/Models/Misc/LocalizedString.h"
#include "RageUtil/File/RageFile.h"
#include "RageUtil/File/RageFileManager.h"
#include "RageUtil/Utils/RageUtil.h"
#include "Etterna/Singletons/ScreenManager.h"
#include "Etterna/Screen/Others/ScreenPrompt.h"
#include "Etterna/Screen/Others/ScreenTextEntry.h"
#include "Etterna/Models/Songs/Song.h"
#include "Etterna/Singletons/SongManager.h"
#include "Etterna/Models/Songs/SongUtil.h"
#include "Etterna/Models/StepsAndStyles/Steps.h"
#include "Etterna/Singletons/ThemeManager.h"

static const std::string TEMP_FILE_NAME = "--temp--";

#define EXPLANATION_TEXT(row)                                                  \
	THEME->GetString(m_sName, "Explanation" + EditMenuRowToString(row))
#define EDIT_MENU_TYPE THEME->GetMetric(m_sName, "EditMenuType")

AutoScreenMessage(SM_RefreshSelector);
AutoScreenMessage(SM_BackFromEditDescription);

REGISTER_SCREEN_CLASS(ScreenEditMenu);

void
ScreenEditMenu::Init()
{
	// HACK: Disable any style set by the editor.
	GAMESTATE->SetCurrentStyle(NULL, PLAYER_INVALID);

	// Enable all players.
	FOREACH_PlayerNumber(pn) GAMESTATE->JoinPlayer(pn);

	// Edit mode DOES NOT WORK if the master player is not player 1.  The same
	// is true of various parts of this poorly designed screen. -Kyz
	if (GAMESTATE->GetMasterPlayerNumber() != PLAYER_1) {
		Locator::getLogger()->warn("Master player number was not player 1, forcing it to player "
				  "1 so that edit mode will work.  If playing in edit mode "
				  "doesn't work, this might be related.");
		GAMESTATE->SetMasterPlayerNumber(PLAYER_1);
	}

	ScreenWithMenuElements::Init();

	m_Selector.SetName("EditMenu");
	m_Selector.Load(EDIT_MENU_TYPE);
	m_Selector.SetXY(0, 0);
	this->AddChild(&m_Selector);

	m_textExplanation.SetName("Explanation");
	m_textExplanation.LoadFromFont(THEME->GetPathF(m_sName, "explanation"));
	LOAD_ALL_COMMANDS_AND_SET_XY(m_textExplanation);
	RefreshExplanationText();
	this->AddChild(&m_textExplanation);

	m_textNumStepsLoadedFromProfile.SetName("NumStepsLoadedFromProfile");
	m_textNumStepsLoadedFromProfile.LoadFromFont(
	  THEME->GetPathF(m_sName, "NumStepsLoadedFromProfile"));
	LOAD_ALL_COMMANDS_AND_SET_XY_AND_ON_COMMAND(
	  m_textNumStepsLoadedFromProfile);
	RefreshNumStepsLoadedFromProfile();
	this->AddChild(&m_textNumStepsLoadedFromProfile);
	if (!m_Selector.SafeToUse()) {
		m_NoSongsMessage.SetName("NoSongsMessage");
		m_NoSongsMessage.LoadFromFont(
		  THEME->GetPathF(m_sName, "NoSongsMessage"));
		LOAD_ALL_COMMANDS_AND_SET_XY_AND_ON_COMMAND(m_NoSongsMessage);
		AddChild(&m_NoSongsMessage);
		m_Selector.SetVisible(false);
		m_textExplanation.SetVisible(false);
		m_textNumStepsLoadedFromProfile.SetVisible(false);
	}
}

void
ScreenEditMenu::HandleScreenMessage(const ScreenMessage SM)
{
	if (SM == SM_RefreshSelector) {
		m_Selector.RefreshAll();
		RefreshNumStepsLoadedFromProfile();
	} else if (SM == SM_Success &&
			   m_Selector.GetSelectedAction() == EditMenuAction_Delete) {
		Locator::getLogger()->trace("Delete successful; deleting steps from memory");

		Song* pSong = GAMESTATE->m_pCurSong;
		Steps* pStepsToDelete = GAMESTATE->m_pCurSteps;
		FOREACH_PlayerNumber(pn) { GAMESTATE->m_pCurSteps.Set(NULL); }
		pSong->DeleteSteps(pStepsToDelete);
		SONGMAN->Invalidate(pSong);

		/* Only save to the main .SM file if the steps we're deleting
		 * were loaded from it. */
		if (true) {
			pSong->Save();
			SCREENMAN->ZeroNextUpdate();
		}
		SCREENMAN->SendMessageToTopScreen(SM_RefreshSelector);
	} else if (SM == SM_Failure &&
			   m_Selector.GetSelectedAction() == EditMenuAction_Delete) {
		Locator::getLogger()->trace("Delete failed; not deleting steps");
	} else if (SM == SM_BackFromEditDescription) {
		if (!ScreenTextEntry::s_bCancelledLast) {
			SOUND->StopMusic();
			StartTransitioningScreen(SM_GoToNextScreen);
		}
	}

	ScreenWithMenuElements::HandleScreenMessage(SM);
}

bool
ScreenEditMenu::MenuUp(const InputEventPlus&)
{
	if (m_Selector.CanGoUp()) {
		m_Selector.Up();
		RefreshExplanationText();
	}
	return true;
}

bool
ScreenEditMenu::MenuDown(const InputEventPlus&)
{
	if (m_Selector.CanGoDown()) {
		m_Selector.Down();
		RefreshExplanationText();
	}
	return true;
}

bool
ScreenEditMenu::MenuLeft(const InputEventPlus&)
{
	if (m_Selector.CanGoLeft()) {
		m_Selector.Left();
	}
	return true;
}

bool
ScreenEditMenu::MenuRight(const InputEventPlus&)
{
	if (m_Selector.CanGoRight()) {
		m_Selector.Right();
	}
	return true;
}

static std::string
GetCopyDescription(const Steps* pSourceSteps)
{
	std::string s = pSourceSteps->GetDescription();
	return s;
}

static void
SetCurrentStepsDescription(const std::string& s)
{
	GAMESTATE->m_pCurSteps->SetDescription(s);
}

static void
DeleteCurrentSteps()
{
	GAMESTATE->m_pCurSong->DeleteSteps(GAMESTATE->m_pCurSteps);
	GAMESTATE->m_pCurSteps.Set(NULL);
}

static LocalizedString MISSING_MUSIC_FILE(
  "ScreenEditMenu",
  "This song is missing a music file and cannot be edited.");
static LocalizedString SONG_DIR_READ_ONLY(
  "ScreenEditMenu",
  "The song directory is read-only and cannot be edited.");
static LocalizedString DELETED_AUTOGEN_STEPS(
  "ScreenEditMenu",
  "These steps are produced by autogen.  You do not need to delete them.");
static LocalizedString STEPS_WILL_BE_LOST(
  "ScreenEditMenu",
  "These steps will be lost permanently.");
static LocalizedString CONTINUE_WITH_DELETE("ScreenEditMenu",
											"Continue with delete?");
static LocalizedString ENTER_EDIT_DESCRIPTION(
  "ScreenEditMenu",
  "Enter a description for this edit.");
static LocalizedString INVALID_SELECTION(
  "ScreenEditMenu",
  "One of the selected things is invalid.  Pick something valid instead.");

bool
ScreenEditMenu::MenuStart(const InputEventPlus&)
{
	if (IsTransitioning())
		return false;
	if (!m_Selector.SafeToUse()) {
		return false;
	}

	if (m_Selector.CanGoDown()) {
		m_Selector.Down();
		RefreshExplanationText();
		return true;
	}

	Song* pSong = m_Selector.GetSelectedSong();
	StepsType st = m_Selector.GetSelectedStepsType();
	Difficulty dc = m_Selector.GetSelectedDifficulty();
	Steps* pSteps = m_Selector.GetSelectedSteps();
	//	StepsType soureNT	= m_Selector.GetSelectedSourceStepsType();
	//	Difficulty sourceDiff	= m_Selector.GetSelectedSourceDifficulty();
	Steps* pSourceSteps = m_Selector.GetSelectedSourceSteps();
	EditMenuAction action = m_Selector.GetSelectedAction();
	if (st == StepsType_Invalid) {
		ScreenPrompt::Prompt(SM_None, INVALID_SELECTION);
		return true;
	}

	GAMESTATE->m_pCurSong.Set(pSong);
	GAMESTATE->SetCurrentStyle(GAMEMAN->GetEditorStyleForStepsType(st),
							   PLAYER_INVALID);
	GAMESTATE->m_pCurSteps.Set(pSteps);

	// handle error cases
	if (!pSong->HasMusic()) {
		ScreenPrompt::Prompt(SM_None, MISSING_MUSIC_FILE);
		return true;
	}

	switch (m_Selector.EDIT_MODE) {
		case EditMode_Full: {
			std::string sDir = pSong->GetSongDir();
			std::string sTempFile = sDir + TEMP_FILE_NAME;
			RageFile file;
			if (!file.Open(sTempFile, RageFile::WRITE)) {
				ScreenPrompt::Prompt(SM_None, SONG_DIR_READ_ONLY);
				return true;
			}

			file.Close();
			FILEMAN->Remove(sTempFile);
			break;
		}
		default:
			break;
	}

	switch (action) {
		case EditMenuAction_Delete: {
			ASSERT(pSteps != NULL);
		}
		default:
			break;
	}

	// Do work
	switch (action) {
		case EditMenuAction_Edit:
		case EditMenuAction_Practice:
			break;
		case EditMenuAction_Delete:
			ASSERT(pSteps != NULL);
			ScreenPrompt::Prompt(SM_None,
								 STEPS_WILL_BE_LOST.GetValue() + "\n\n" +
								   CONTINUE_WITH_DELETE.GetValue(),
								 PROMPT_YES_NO,
								 ANSWER_NO);
			break;
		case EditMenuAction_LoadAutosave:
			if (pSong) {
				FOREACH_PlayerNumber(pn) { GAMESTATE->m_pCurSteps.Set(NULL); }
				//pSong->LoadAutosaveFile();
				SONGMAN->Invalidate(pSong);
				SCREENMAN->SendMessageToTopScreen(SM_RefreshSelector);
			}
			break;
		case EditMenuAction_Create:
			ASSERT(!pSteps);
			{
				pSteps = pSong->CreateSteps();

				EditMode mode = m_Selector.EDIT_MODE;
				switch (mode) {
					default:
						FAIL_M(ssprintf("Invalid EditMode: %i", mode));
					case EditMode_Full:
						break;
					case EditMode_Home:
						pSteps->SetLoadedFromProfile(ProfileSlot_Player1);
						break;
					case EditMode_Practice:
						FAIL_M("Cannot create steps in EditMode_Practice");
				}

				std::string sEditName;
				if (pSourceSteps) {
					pSteps->CopyFrom(
					  pSourceSteps, st);
					sEditName = GetCopyDescription(pSourceSteps);
				} else {
					pSteps->CreateBlank(st);
					pSteps->SetMeter(1);
					sEditName = "";
				}

				pSteps->SetDifficulty(
				  dc); // override difficulty with the user's choice
				SongUtil::MakeUniqueEditDescription(pSong, st, sEditName);
				pSteps->SetDescription(sEditName);
				pSong->AddSteps(pSteps);

				SCREENMAN->PlayStartSound();

				GAMESTATE->m_pCurSong.Set(pSong);
				GAMESTATE->m_pCurSteps.Set(pSteps);
			}
			break;
		default:
			FAIL_M(ssprintf("Invalid edit menu action: %i", action));
	}

	// Go to the next screen.
	switch (action) {
		case EditMenuAction_Edit:
		case EditMenuAction_Create:
		case EditMenuAction_Practice: {
			// Prepare for ScreenEdit
			ASSERT(pSteps != NULL);
			bool bPromptToNameSteps =
			  (action == EditMenuAction_Create && dc == Difficulty_Edit);
			if (bPromptToNameSteps) {
				ScreenTextEntry::TextEntry(
				  SM_BackFromEditDescription,
				  ENTER_EDIT_DESCRIPTION,
				  GAMESTATE->m_pCurSteps->GetDescription(),
				  MAX_STEPS_DESCRIPTION_LENGTH,
				  SongUtil::ValidateCurrentStepsDescription,
				  SetCurrentStepsDescription,
				  DeleteCurrentSteps);
			} else {
				SOUND->StopMusic();
				SCREENMAN->PlayStartSound();
				StartTransitioningScreen(SM_GoToNextScreen);
			}
		}
			return true;
		case EditMenuAction_Delete:
		case EditMenuAction_LoadAutosave:
			return true;
		default:
			FAIL_M(ssprintf("Invalid edit menu action: %i", action));
	}
}

bool
ScreenEditMenu::MenuBack(const InputEventPlus& input)
{
	Cancel(SM_GoToPrevScreen);
	return true;
}

void
ScreenEditMenu::RefreshExplanationText()
{
	m_textExplanation.SetText(EXPLANATION_TEXT(m_Selector.GetSelectedRow()));
	m_textExplanation.StopTweening();
	ON_COMMAND(m_textExplanation);
}

void
ScreenEditMenu::RefreshNumStepsLoadedFromProfile()
{
	std::string s =
	  ssprintf("edits used: %d", 0);
	m_textNumStepsLoadedFromProfile.SetText(s);
}
