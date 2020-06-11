#include "Etterna/Globals/global.h"
#include "SnapDisplay.h"
#include "Etterna/Singletons/GameState.h"
#include "Etterna/Singletons/ThemeManager.h"
#include "Etterna/Models/StepsAndStyles/Style.h"
#include "Etterna/Models/Misc/ScreenDimensions.h"
#include "Etterna/Models/Misc/EnumHelper.h"

SnapDisplay::SnapDisplay()
{
	for (int i = 0; i < 2; i++) {
		m_sprIndicators[i].Load(THEME->GetPathG("SnapDisplay", "icon 9x1"));
		m_sprIndicators[i].StopAnimating();
		this->AddChild(&m_sprIndicators[i]);
	}

	m_NoteType = NOTE_TYPE_4TH;

	m_iNumCols = 0;
}

void
SnapDisplay::Load()
{
	m_iNumCols = GAMESTATE->GetCurrentStyle(GAMESTATE->GetMasterPlayerNumber())
				   ->m_iColsPerPlayer;

	m_sprIndicators[0].SetX(-ARROW_SIZE * (m_iNumCols / 2 + 0.5f));
	m_sprIndicators[1].SetX(ARROW_SIZE * (m_iNumCols / 2 + 0.5f));
}

bool
SnapDisplay::PrevSnapMode()
{
	if (m_NoteType == 0)
		return false;
	enum_add(m_NoteType, -1);

	SnapModeChanged();
	return true;
}

bool
SnapDisplay::NextSnapMode()
{
	if (m_NoteType == NOTE_TYPE_192ND) // the smallest snap we should allow
		return false;
	enum_add(m_NoteType, 1);

	SnapModeChanged();
	return true;
}

void
SnapDisplay::SnapModeChanged()
{
	for (int i = 0; i < 2; i++)
		m_sprIndicators[i].SetState(m_NoteType);
}
