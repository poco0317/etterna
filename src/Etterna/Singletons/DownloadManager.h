#pragma once
#ifndef SM_DOWNMANAGER
#define SM_DOWNMANAGER

#include "Etterna/Globals/global.h"
#include "RageUtil/File/RageFile.h"
#include "Etterna/Models/Misc/HighScore.h"
#include "ScreenManager.h"
#include "RageUtil/File/RageFileManager.h"
#include "Etterna/Models/Misc/Difficulty.h"

#include <deque>
#include "Poco/Net/HTTPRequest.h"
#include "Poco/Net/HTTPResponse.h"
#include "Poco/Net/HTTPSClientSession.h"
#include "Poco/Net/HTMLForm.h"

using Poco::Net::HTTPSClientSession;
using Poco::Net::HTTPClientSession;
using Poco::Net::HTTPRequest;
using Poco::Net::HTTPResponse;
using Poco::Net::HTMLForm;

typedef std::function<void(std::istream&, HTTPResponse)> RequestCallback;
typedef std::tuple<HTTPRequest*, HTMLForm*, RequestCallback>
  RequestData;

class ProgressData
{
  public:
	size_t dltotal{ 0 };		// total bytes
	size_t downloaded{ 0 }; // bytes downloaded
	size_t ultotal{ 0 };
	size_t uploaded{ 0 };
	float time{ 0 };			// seconds passed
};

class RageFileWrapper
{
  public:
	RageFile file;
	size_t bytes{ 0 };
	bool stop{ false };
};

class DownloadablePack
{
  public:
	std::string name{ "" };
	size_t size{ 0 };
	int id{ 0 };
	float avgDifficulty{ 0 };
	std::string url{ "" };
	std::string mirror{ "" };
	bool downloading{ false };
	// Lua
	void PushSelf(lua_State* L);
};
class Download
{
  public:
	std::function<void(Download*)> Done;
	Download(
	  std::string url,
	  std::string filename = "",
	  std::function<void(Download*)> done = [](Download*) {});
	~Download();
	void Install();
	void Update(float fDeltaSeconds);
	void Failed();
	std::string StartMessage()
	{
		return "Downloading file " + m_TempFileName + " from " + m_Url;
	};
	std::string Status()
	{
		return m_TempFileName + "\n" + speed + " KB/s\n" + "Downloaded " +
			   std::to_string((progress.downloaded > 0 ? progress.downloaded
													   : p_RFWrapper.bytes) /
							  1024) +
			   (progress.dltotal > 0
				  ? "/" + std::to_string(progress.dltotal / 1024) + " (KB)"
				  : "");
	}
	int running{ 1 };
	ProgressData progress;
	std::string speed{ "" };
	size_t downloadedAtLastUpdate{ 0 };
	size_t lastUpdateDone{ 0 };
	std::string m_Url{ "" };
	RageFileWrapper p_RFWrapper;
	DownloadablePack* p_Pack{ nullptr };
	std::string m_TempFileName{ "" };
	// Lua
	void PushSelf(lua_State* L);

  protected:
	std::string MakeTempFileName(std::string s);
};
class OnlineTopScore
{
  public:
	float wifeScore{ 0.0f };
	std::string songName;
	float rate{ 0.0f };
	float ssr{ 0.0f };
	float overall{ 0.0f };
	std::string chartkey;
	std::string scorekey;
	Difficulty difficulty;
	std::string steps;
};
struct OnlineHighScore : HighScore
{
  public:
	bool hasReplay;
	bool HasReplayData() override { return hasReplay; }
};
class OnlineScore
{
  public:
	std::map<Skillset, float> SSRs;
	float rate{ 0.0f };
	float wife{ 0.0f };
	int wifeversion{ 0 };
	int maxcombo{ 0 };
	int miss{ 0 };
	int bad{ 0 };
	int good{ 0 };
	int great{ 0 };
	int perfect{ 0 };
	int marvelous{ 0 };
	int minehits{ 0 };
	int held{ 0 };
	std::string songId;
	int letgo{ 0 };
	bool valid{ false };
	bool nocc{ false };
	std::string username;
	float playerRating{ 0.0f };
	std::string modifiers;
	std::string scoreid;
	std::string avatar;
	int userid;
	DateTime datetime;
	bool hasReplay{ false };
	std::vector<std::pair<float, float>> replayData;
	std::string countryCode;
	OnlineHighScore hs;
	void Push(lua_State* L) { hs.PushSelf(L); }
	bool HasReplayData() { return hasReplay; }
};

class DownloadManager
{
  public:
	DownloadManager();
	~DownloadManager();

	void Init();
	void Update(float fDeltaSeconds);
	void UpdateHTTPSRequests(float fDeltaSeconds);
	void UpdateHTTPRequests(float fDeltaSeconds);

	bool IsLoggedIn();
	bool IsInGameplay();
	bool ShouldUploadScores();

	void SetInGameplay(bool inGameplay);
	void SetApiShouldUseHttps(bool state);

	void GenerateRequest(
	  const std::string& url,
	  RequestCallback callback,
	  const std::string requestMethod = HTTPRequest::HTTP_GET,
	  HTMLForm* requestForm = nullptr,
	  bool https = true);
	void SetClientSessionByURL(Poco::Net::HTTPClientSession* session,
							   const std::string url);

	// API Requests
	void Login(const std::string& username, const std::string& password);
	void Login(const std::string& token);
	void GetRankedChartkeys();
	void UploadSingleScore();
	void UploadBulkScores();


	std::vector<DownloadablePack> downloadablePacks;
	std::map<std::string, std::vector<OnlineScore>> chartLeaderboards;
	std::set<std::string> unrankedCharts;
	std::vector<std::string> countryCodes;
	/// Leaderboard ranks for logged in user by skillset
	std::map<Skillset, int> sessionRanks;
	std::map<Skillset, double> sessionRatings;
	std::map<Skillset, std::vector<OnlineTopScore>> topScores;

	void OnLogin();
	/// Uploads all scores not yet uploaded to current
	bool UploadScores();
	/// forced upload wrapper for charts
	void ForceUploadScoresForChart(const std::string& ck, bool startnow = true);
	/// forced upload wrapper for packs
	void ForceUploadScoresForPack(const std::string& pack,
								  bool startnow = true);
	void ForceUploadAllScores();

	bool InstallSmzip(const std::string& sZipFile);

	bool EncodeSpaces(std::string& str);

	void UploadScore(HighScore* hs,
					 std::function<void()> callback,
					 bool load_from_disk);
	void UploadScoreWithReplayData(HighScore* hs);
	void UploadScoreWithReplayDataFromDisk(
	  HighScore* hs,
	  std::function<void()> callback = []() {});

	bool currentrateonly = false;
	bool topscoresonly = true;
	bool ccoffonly = false;
	OnlineTopScore GetTopSkillsetScore(unsigned int rank,
									   Skillset ss,
									   bool& result);
	float GetSkillsetRating(Skillset ss);
	int GetSkillsetRank(Skillset ss);

	/// most recent single score upload result
	std::string mostrecentresult = "";
	/// (pack,isMirror)
	std::deque<std::pair<DownloadablePack*, bool>> DownloadQueue;
	std::deque<HighScore*> ScoreUploadSequentialQueue;
	unsigned int sequentialScoreUploadTotalWorkload{ 0 };
	const int maxPacksToDownloadAtOnce = 1;
	const float DownloadCooldownTime = 5.f;
	float timeSinceLastDownload = 0.f;

	// Lua
	void PushSelf(lua_State* L);

  private:
	/// Active HTTP requests
	std::vector<RequestData> apiHttpsRequests{};
	std::vector<RequestData> apiHttpRequests{};
	/// Main HTTPS Client Session
	HTTPSClientSession* p_httpsClientSession;
	/// Alternate HTTP Client Session
	HTTPClientSession* p_httpClientSession;
	/// Allow toggling https
	bool apiShouldUseHttps = false;

	bool initialized = false;
	bool inGameplay = false;

	std::string loginToken = "";

	std::vector<std::string> newlyRankedChartkeys{};
};

extern std::shared_ptr<DownloadManager> DLMAN;

#endif
