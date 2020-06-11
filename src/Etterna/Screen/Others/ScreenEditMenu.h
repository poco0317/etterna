#ifndef SCREEN_EDIT_MENU_H
#define SCREEN_EDIT_MENU_H

#include "ScreenWithMenuElements.h"
#include "Etterna/Actor/Menus/EditMenu.h"
#include "Etterna/Actor/Base/BitmapText.h"

class ScreenEditMenu : public ScreenWithMenuElements
{
  public:
	virtual void Init();

	virtual void HandleScreenMessage(const ScreenMessage SM);

  private:
	bool MenuUp(const InputEventPlus& input);
	bool MenuDown(const InputEventPlus& input);
	bool MenuLeft(const InputEventPlus& input);
	bool MenuRight(const InputEventPlus& input);
	bool MenuBack(const InputEventPlus& input);
	bool MenuStart(const InputEventPlus& input);

	void RefreshExplanationText();
	void RefreshNumStepsLoadedFromProfile();

	EditMenu m_Selector;

  private:
	BitmapText m_textExplanation;
	BitmapText m_textNumStepsLoadedFromProfile;
	BitmapText m_NoSongsMessage;
};

#endif
