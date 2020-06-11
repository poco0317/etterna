#ifndef SNAPDISPLAY_H
#define SNAPDISPLAY_H

#include "Etterna/Actor/Base/ActorFrame.h"
#include "Etterna/Models/Misc/NoteTypes.h"
#include "Etterna/Actor/Base/Sprite.h"

/** @brief Graphics on ends of receptors on Edit screen that show the current
 * snap type. */
class SnapDisplay : public ActorFrame
{
  public:
	SnapDisplay();

	void Load();

	bool PrevSnapMode();
	bool NextSnapMode();

	NoteType GetNoteType() const { return m_NoteType; }

  protected:
	int m_iNumCols;

	void SnapModeChanged();

	/** @brief the NoteType to snap to. */
	NoteType m_NoteType;
	/**
	 * @brief Indicators showing what NoteType is currently snapped to.
	 *
	 * TODO: Convert to an AutoActor. -aj */
	Sprite m_sprIndicators[2]; // left and right side
};

#endif
