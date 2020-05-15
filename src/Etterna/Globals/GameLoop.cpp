﻿#include "global.h"
#include "GameLoop.h"
#include "Etterna/Singletons/PrefsManager.h"
#include "RageUtil/Graphics/RageDisplay.h"
#include "RageUtil/Sound/RageSoundManager.h"
#include "RageUtil/Graphics/RageTextureManager.h"
#include "Core/Services/Locator.hpp"
#include "Etterna/Singletons/GameSoundManager.h"
#include "Etterna/Singletons/ThemeManager.h"
#include "Etterna/Singletons/SongManager.h"
#include "Etterna/Singletons/DownloadManager.h"
#include "Etterna/Singletons/GameState.h"
#include "Etterna/Singletons/InputFilter.h"
#include "Etterna/Singletons/InputMapper.h"
#include "Etterna/Singletons/NetworkSyncManager.h"
#include "RageUtil/Misc/RageInput.h"
#include "Etterna/Singletons/ScreenManager.h"

#include <chrono>

static auto g_AccurateGameplayTimer = std::chrono::steady_clock::now();

static Preference<bool> g_bNeverBoostAppPriority("NeverBoostAppPriority",
												 false);

/* experimental: force a specific update rate. This prevents big  animation
 * jumps on frame skips. 0 to disable. */
static Preference<float> g_fConstantUpdateDeltaSeconds(
  "ConstantUpdateDeltaSeconds",
  0);

void
HandleInputEvents(float fDeltaTime);

static float g_fUpdateRate = 1;
void
GameLoop::SetUpdateRate(float fUpdateRate)
{
	g_fUpdateRate = fUpdateRate;
}

static void
CheckGameLoopTimerSkips(float fDeltaTime)
{
	if (!PREFSMAN->m_bLogSkips)
		return;

	static int iLastFPS = 0;
	int iThisFPS = DISPLAY->GetFPS();

	/* If vsync is on, and we have a solid framerate (vsync == refresh and we've
	 * sustained this for at least one second), we expect the amount of time for
	 * the last frame to be 1/FPS. */
	if (iThisFPS != (*DISPLAY->GetActualVideoModeParams()).rate ||
		iThisFPS != iLastFPS) {
		iLastFPS = iThisFPS;
		return;
	}

	const float fExpectedTime = 1.0f / iThisFPS;
	const float fDifference = fDeltaTime - fExpectedTime;
	if (fabsf(fDifference) > 0.002f && fabsf(fDifference) < 0.100f)
		Locator::getLogger()->trace("GameLoop timer skip: {} FPS, expected {:.3f}, got {:.3f} ({:.3f} difference)",
				   iThisFPS, fExpectedTime, fDeltaTime, fDifference);
}

static bool
ChangeAppPri()
{
	if (g_bNeverBoostAppPriority.Get())
		return false;

		// if using NTPAD don't boost or else input is laggy
#ifdef _WIN32
	{
		vector<InputDeviceInfo> vDevices;

		// This can get called before INPUTMAN is constructed.
		if (INPUTMAN) {
			INPUTMAN->GetDevicesAndDescriptions(vDevices);
			for (auto& d : vDevices) {
				if (d.sDesc.find("NTPAD") != std::string::npos) {
					Locator::getLogger()->trace("Using NTPAD.  Don't boost priority.");
					return false;
				}
			}
		}
	}
#endif

	// If this is a debug build, don't. It makes the VC debugger sluggish.
#if defined(_WIN32) && defined(DEBUG)
	return false;
#else
	return true;
#endif
}

static void
CheckFocus()
{
    ArchHooks* hooks = Locator::getArchHooks();
	if (!hooks->AppFocusChanged())
		return;

	// If we lose focus, we may lose input events, especially key releases.
	INPUTFILTER->Reset();

	if (ChangeAppPri()) {
		if (hooks->AppHasFocus())
            hooks->BoostPriority();
		else
            hooks->UnBoostPriority();
	}
}

// On the next update, change themes, and load sNewScreen.
static std::string g_NewTheme;
static std::string g_NewGame;
void
GameLoop::ChangeTheme(const std::string& sNewTheme)
{
	g_NewTheme = sNewTheme;
}

void
GameLoop::ChangeGame(const std::string& new_game, const std::string& new_theme)
{
	g_NewGame = new_game;
	g_NewTheme = new_theme;
}

#include "Etterna/Models/Misc/Game.h"
#include "Etterna/Singletons/GameManager.h"
#include "StepMania.h" // XXX
namespace {
void
DoChangeTheme()
{
	SAFE_DELETE(SCREENMAN);
	TEXTUREMAN->DoDelayedDelete();

	// In case the previous theme overloaded class bindings, reinitialize them.
	LUA->RegisterTypes();

	// We always need to force the theme to reload because we cleared the lua
	// state by calling RegisterTypes so the scripts in Scripts/ need to run.
	THEME->SwitchThemeAndLanguage(
	  g_NewTheme, THEME->GetCurLanguage(), PREFSMAN->m_bPseudoLocalize, true);
	PREFSMAN->m_sTheme.Set(g_NewTheme);

	// Apply the new window title, icon and aspect ratio.
	StepMania::ApplyGraphicOptions();

	SCREENMAN = new ScreenManager();

	StepMania::ResetGame();
	SCREENMAN->ThemeChanged();
	// The previous system for changing the theme fetched the "NextScreen"
	// metric from the current theme, then changed the theme, then tried to
	// set the new screen to the name that had been fetched.
	// If the new screen didn't exist in the new theme, there would be a
	// crash.
	// So now the correct thing to do is for a theme to specify its entry
	// point after a theme change, ensuring that we are going to a valid
	// screen and not crashing. -Kyz
	std::string new_screen = THEME->GetMetric("Common", "InitialScreen");
	if (THEME->HasMetric("Common", "AfterThemeChangeScreen")) {
		std::string after_screen =
		  THEME->GetMetric("Common", "AfterThemeChangeScreen");
		if (SCREENMAN->IsScreenNameValid(after_screen)) {
			new_screen = after_screen;
		}
	}
	if (!SCREENMAN->IsScreenNameValid(new_screen)) {
		new_screen = "ScreenInitialScreenIsInvalid";
	}
	SCREENMAN->SetNewScreen(new_screen);

	g_NewTheme = std::string();
}

void
DoChangeGame()
{
	const Game* g = GAMEMAN->StringToGame(g_NewGame);
	ASSERT(g != nullptr);
	GAMESTATE->SetCurGame(g);

	// reset gamestate to deal with new Game
	StepMania::ResetGame();

	// point us to the new Screen to end up on after Game change
	// either the initialscreen or something else
	std::string new_screen = THEME->GetMetric("Common", "InitialScreen");
	std::string after_screen;
	if (THEME->HasMetric("Common", "AfterGameChangeScreen")) {
		after_screen = THEME->GetMetric("Common", "AfterGameChangeScreen");
	}
	if (SCREENMAN->IsScreenNameValid(after_screen)) {
		new_screen = after_screen;
	}
	SCREENMAN->SetNewScreen(new_screen);

	// Set the input scheme for the new game, and load keymaps.
	if (INPUTMAPPER != nullptr) {
		INPUTMAPPER->SetInputScheme(&g->m_InputScheme);
		INPUTMAPPER->ReadMappingsFromDisk();
	}
	// aj's comment transplanted from ScreenOptionsMasterPrefs.cpp:GameSel. -Kyz
	/* Reload metrics to force a refresh of CommonMetrics::DIFFICULTIES_TO_SHOW,
	 * mainly if we're not switching themes. I'm not sure if this was the
	 * case going from theme to theme, but if it was, it should be fixed
	 * now. There's probably be a better way to do it, but I'm not sure
	 * what it'd be. -aj */
	THEME->UpdateLuaGlobals();
	THEME->ReloadMetrics();
	g_NewGame = std::string();
	g_NewTheme = std::string();
}
} // namespace

void
GameLoop::RunGameLoop()
{
	/* People may want to do something else while songs are loading, so do
	 * this after loading songs. */
	if (ChangeAppPri())
        Locator::getArchHooks()->BoostPriority();

	while (!ArchHooks::UserQuit()) {
		if (!g_NewGame.empty()) {
			DoChangeGame();
		}
		if (!g_NewTheme.empty()) {
			DoChangeTheme();
		}

		// Update
		auto now = std::chrono::steady_clock::now();
		std::chrono::duration<float> frameStart = now - g_AccurateGameplayTimer;
		float fDeltaTime = frameStart.count();
		g_AccurateGameplayTimer = now;

		if (g_fConstantUpdateDeltaSeconds > 0)
			fDeltaTime = g_fConstantUpdateDeltaSeconds;

		CheckGameLoopTimerSkips(fDeltaTime);

		fDeltaTime *= g_fUpdateRate;

		CheckFocus();

		// Update SOUNDMAN early (before any RageSound::GetPosition calls), to
		// flush position data.
		SOUNDMAN->Update();

		/* Update song beat information -before- calling update on all the
		 * classes that depend on it. If you don't do this first, the classes
		 * are all acting on old information and will lag. (but no longer
		 * fatally, due to timestamping -glenn) */
		SOUND->Update(fDeltaTime);
		TEXTUREMAN->Update(fDeltaTime);
		GAMESTATE->Update(fDeltaTime);
		SCREENMAN->Update(fDeltaTime);
		NSMAN->Update(fDeltaTime);
		DLMAN->Update(fDeltaTime);

		/* Important: Process input AFTER updating game logic, or input will be
		 * acting on song beat from last frame */
		HandleInputEvents(fDeltaTime);

		static float deviceCheckWait = 0.f;
		deviceCheckWait += fDeltaTime;

		if (deviceCheckWait >= 1.0f) {
			deviceCheckWait = 0.f;

			if (INPUTMAN->DevicesChanged()) {
				INPUTFILTER->Reset(); // fix "buttons stuck" if button held
									  // while unplugged
				INPUTMAN->LoadDrivers();
				std::string sMessage;
				if (INPUTMAPPER->CheckForChangedInputDevicesAndRemap(sMessage))
					SCREENMAN->SystemMessage(sMessage);
			}
		}

		// Render
		SCREENMAN->Draw();
	}

	if (ChangeAppPri())
        Locator::getArchHooks()->UnBoostPriority();
}

class ConcurrentRenderer
{
  public:
	ConcurrentRenderer();
	~ConcurrentRenderer();

	void Start();
	void Stop();

  private:
	RageThread m_Thread;
	RageEvent m_Event;
	bool m_bShutdown;
	void RenderThread();
	static int StartRenderThread(void* p);

	enum State
	{
		RENDERING_IDLE,
		RENDERING_START,
		RENDERING_ACTIVE,
		RENDERING_END
	};
	State m_State;
};
static ConcurrentRenderer* g_pConcurrentRenderer = NULL;

ConcurrentRenderer::ConcurrentRenderer()
  : m_Event("ConcurrentRenderer")
{
	m_bShutdown = false;
	m_State = RENDERING_IDLE;

	m_Thread.SetName("ConcurrentRenderer");
	m_Thread.Create(StartRenderThread, this);
}

ConcurrentRenderer::~ConcurrentRenderer()
{
	ASSERT(m_State == RENDERING_IDLE);
	m_bShutdown = true;
	m_Thread.Wait();
}

void
ConcurrentRenderer::Start()
{
	DISPLAY->BeginConcurrentRenderingMainThread();

	m_Event.Lock();
	ASSERT(m_State == RENDERING_IDLE);
	m_State = RENDERING_START;
	m_Event.Signal();
	while (m_State != RENDERING_ACTIVE)
		m_Event.Wait();
	m_Event.Unlock();
}

void
ConcurrentRenderer::Stop()
{
	m_Event.Lock();
	ASSERT(m_State == RENDERING_ACTIVE);
	m_State = RENDERING_END;
	m_Event.Signal();
	while (m_State != RENDERING_IDLE)
		m_Event.Wait();
	m_Event.Unlock();

	DISPLAY->EndConcurrentRenderingMainThread();
}

void
ConcurrentRenderer::RenderThread()
{
	ASSERT(SCREENMAN != NULL);

	while (!m_bShutdown) {
		m_Event.Lock();
		while (m_State == RENDERING_IDLE && !m_bShutdown)
			m_Event.Wait();
		m_Event.Unlock();

		if (m_State == RENDERING_START) {
			/* We're starting to render. Set up, and then kick the event to wake
			 * up the calling thread. */
			DISPLAY->BeginConcurrentRendering();
            Locator::getArchHooks()->SetupConcurrentRenderingThread();

			Locator::getLogger()->trace("ConcurrentRenderer::RenderThread start");

			m_Event.Lock();
			m_State = RENDERING_ACTIVE;
			m_Event.Signal();
			m_Event.Unlock();
		}

		/* This is started during Update(). The next thing the game loop
		 * will do is Draw, so shift operations around to put Draw at the
		 * top. This makes sure updates are seamless. */
		if (m_State == RENDERING_ACTIVE) {
			SCREENMAN->Draw();

			std::chrono::duration<float> frameStart =
			  std::chrono::steady_clock::now() - g_AccurateGameplayTimer;
			float fDeltaTime = frameStart.count();
			SCREENMAN->Update(fDeltaTime);
		}

		if (m_State == RENDERING_END) {
			Locator::getLogger()->trace("ConcurrentRenderer::RenderThread done");

			DISPLAY->EndConcurrentRendering();

			m_Event.Lock();
			m_State = RENDERING_IDLE;
			m_Event.Signal();
			m_Event.Unlock();
		}
	}
}

int
ConcurrentRenderer::StartRenderThread(void* p)
{
	(reinterpret_cast<ConcurrentRenderer*>(p))->RenderThread();
	return 0;
}

void
GameLoop::StartConcurrentRendering()
{
	if (g_pConcurrentRenderer == NULL)
		g_pConcurrentRenderer = new ConcurrentRenderer;
	g_pConcurrentRenderer->Start();
}

void
GameLoop::FinishConcurrentRendering()
{
	g_pConcurrentRenderer->Stop();
}
