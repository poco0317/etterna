#include "Etterna/Globals/global.h"
#include "RageUtil/File/RageFileManager.h"
#include "ScreenManager.h"
#include "Etterna/Models/Misc/Preference.h"
#include "Core/Services/Locator.hpp"
#include "RageUtil/File/RageFile.h"
#include "DownloadManager.h"
#include "GameState.h"
#include "ScoreManager.h"
#include "Etterna/Models/Misc/GamePreferences.h"
#include "Etterna/Screen/Network/ScreenNetSelectMusic.h"
#include "ProfileManager.h"
#include "SongManager.h"
#include "Etterna/Screen/Others/ScreenInstallOverlay.h"
#include "Etterna/Screen/Others/ScreenSelectMusic.h"
#include "Etterna/Globals/SpecialFiles.h"
#include "Etterna/Models/Songs/Song.h"
#include "Etterna/Models/Misc/PlayerStageStats.h"
#include "Etterna/Models/Songs/SongOptions.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/error/en.h"
#include "rapidjson/stringbuffer.h"

#ifdef _WIN32
#include <intrin.h>
#endif

#include <unordered_set>
#include <algorithm>

#include "Poco/URI.h"
#include "Poco/Net/SSLManager.h"
#include "Poco/Net/ConsoleCertificateHandler.h"

#include "Poco/Dynamic/Var.h"
#include "Poco/JSON/Parser.h"
#include "Poco/JSON/Array.h"

using namespace rapidjson;

static RageThread DownloadManagerThread;
static bool g_Shutdown;
std::mutex g_dlmutex;

std::shared_ptr<DownloadManager> DLMAN = nullptr;

static Preference<unsigned int> maxDLPerSecond(
  "maximumBytesDownloadedPerSecond",
  0);
static Preference<unsigned int> maxDLPerSecondGameplay(
  "maximumBytesDownloadedPerSecondDuringGameplay",
  1000000);
static Preference<std::string> packListURL(
  "PackListURL",
  "https://api.etternaonline.com/v2/packs");
static Preference<std::string> serverURL(
  "BaseAPIURL",
  "http://api.beta.etternaonline.com/");
static Preference<unsigned int> automaticSync("automaticScoreSync", 1);
static Preference<unsigned int> downloadPacksToAdditionalSongs(
  "downloadPacksToAdditionalSongs",
  0);

static const std::string TEMP_ZIP_MOUNT_POINT = "/@temp-zip/";
static const std::string DL_DIR = SpecialFiles::CACHE_DIR + "Downloads/";
static const std::string wife3_rescore_upload_flag = "rescoredw3";

// endpoint construction constants
// all paths should begin with / and end without /
/// API root path
static const std::string API_ROOT = "/api/client";
static const std::string API_KEY = "testkey";

static const std::string API_LOGIN = "/login";
static const std::string API_RANKED_CHARTKEYS = "/charts/ranked";
static const std::string API_UPLOAD_SCORE = "/scores";
static const std::string API_UPLOAD_SCORE_BULK = "/scores/bulk";

bool
DownloadManager::InstallSmzip(const std::string& sZipFile)
{
	if (!FILEMAN->Mount("zip", sZipFile, TEMP_ZIP_MOUNT_POINT))
		FAIL_M(static_cast<std::string>("Failed to mount " + sZipFile).c_str());
	std::vector<std::string> v_packs;
	GetDirListing(TEMP_ZIP_MOUNT_POINT + "*", v_packs, true, true);

	std::string doot = TEMP_ZIP_MOUNT_POINT;
	if (v_packs.size() > 1) {
		doot += sZipFile.substr(sZipFile.find_last_of('/') +
								1); // attempt to whitelist pack name, this
									// should be pretty simple/safe solution for
									// a lot of pad packs -mina
		doot = doot.substr(0, doot.length() - 4) + "/";
	}

	std::vector<std::string> vsFiles;
	{
		std::vector<std::string> vsRawFiles;
		GetDirListingRecursive(doot, "*", vsRawFiles);

		if (vsRawFiles.empty()) {
			FILEMAN->Unmount("zip", sZipFile, TEMP_ZIP_MOUNT_POINT);
			return false;
		}

		std::vector<std::string> vsPrettyFiles;
		for (auto& s : vsRawFiles) {
			if (EqualsNoCase(GetExtension(s), "ctl"))
				continue;

			vsFiles.push_back(s);

			std::string s2 = tail(s, s.length() - TEMP_ZIP_MOUNT_POINT.length());
			vsPrettyFiles.push_back(s2);
		}
		sort(vsPrettyFiles.begin(), vsPrettyFiles.end());
	}
	std::string sResult = "Success installing " + sZipFile;
	std::string extractTo =
	  downloadPacksToAdditionalSongs ? "AdditionalSongs/" : "Songs/";
	for (auto& sSrcFile : vsFiles) {
		std::string sDestFile = sSrcFile;
		sDestFile = tail(std::string(sDestFile.c_str()),
						 sDestFile.length() - TEMP_ZIP_MOUNT_POINT.length());

		std::string sDir, sThrowAway;
		splitpath(sDestFile, sDir, sThrowAway, sThrowAway);

		if (!FileCopy(sSrcFile, extractTo + sDestFile)) {
			sResult = "Error extracting " + sDestFile;
			break;
		}
	}

	FILEMAN->Unmount("zip", sZipFile, TEMP_ZIP_MOUNT_POINT);

	SCREENMAN->SystemMessage(sResult);
	return true;
}

inline void
EmptyTempDLFileDir()
{
	std::vector<std::string> files;
	FILEMAN->GetDirListing(DL_DIR + "*", files, false, true);
	for (auto& file : files) {
		if (FILEMAN->IsAFile(file))
			FILEMAN->Remove(file);
	}
}

int
DownloadManager_Thread(void* p)
{
	auto* dlman = static_cast<DownloadManager*>(p);

	auto deltaTimeClock = std::chrono::steady_clock::now();
	while (!g_Shutdown) {
		auto now = std::chrono::steady_clock::now();
		std::chrono::duration<float> cdiff = now - deltaTimeClock;
		float fDeltaTime = cdiff.count();
		deltaTimeClock = now;

		if (!g_Shutdown)
			dlman->Update(fDeltaTime);
	}
	return 0;
}

DownloadManager::DownloadManager()
{
	EmptyTempDLFileDir();

	Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> pCert =
	  new Poco::Net::ConsoleCertificateHandler(false);
	Poco::Net::Context::Ptr pCtx =
	  new Poco::Net::Context(Poco::Net::Context::TLS_CLIENT_USE,
							 "",
							 "",
							 "",
							 Poco::Net::Context::VERIFY_NONE,
							 9,
							 false,
							 "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH");
	Poco::Net::SSLManager::instance().initializeClient(0, pCert, pCtx);

	p_httpsClientSession = new HTTPSClientSession;
	p_httpClientSession = new HTTPClientSession;

	SetClientSessionByURL(p_httpsClientSession, serverURL);
	SetClientSessionByURL(p_httpClientSession, serverURL);

	g_Shutdown = false;
	DownloadManagerThread.SetName("DownloadManager thread");
	DownloadManagerThread.Create(DownloadManager_Thread, this);

	// Register with Lua.
	{
		Lua* L = LUA->Get();
		lua_pushstring(L, "DLMAN");
		this->PushSelf(L);
		lua_settable(L, LUA_GLOBALSINDEX);
		LUA->Release(L);
	}
}

DownloadManager::~DownloadManager()
{
	// Unregister with Lua.
	LUA->UnsetGlobal("DLMAN");

	{
		const std::lock_guard<std::mutex> lock(g_dlmutex);
		g_Shutdown = true;
	}
	DownloadManagerThread.Wait();

	EmptyTempDLFileDir();

	if (p_httpsClientSession != nullptr)
		delete p_httpsClientSession;
	if (p_httpClientSession != nullptr)
		delete p_httpClientSession;
}

void
DownloadManager::Init()
{
	initialized = true;

}

void
DownloadManager::GenerateRequest(const std::string& url,
								 RequestCallback callback,
								 Poco::JSON::Object* jsonPOST,
								 HTMLForm* form,
								 const std::string requestMethod,
								 bool https)
{
	Poco::URI uri(url);
	std::string path(uri.getPathAndQuery());
	auto host = uri.getHost();
	auto port = uri.getPort();
	if (path.empty())
		path = "/";
	HTTPRequest* request =
	  new HTTPRequest(requestMethod, path, Poco::Net::HTTPMessage::HTTP_1_1);

	// required for all requests (with our api)
	request->setContentType("application/json");
	request->setContentLength(0);

	// when logged in, provide authorization
	if (IsLoggedIn())
		request->setCredentials("Bearer", loginToken);

	// select session and request queue based on http/https
	HTTPClientSession* session;
	std::vector<RequestData*>* requestQueue;
	if (https) {
		session = p_httpsClientSession;
		requestQueue = &apiHttpsRequests;
	} else {
		session = p_httpClientSession;
		requestQueue = &apiHttpRequests;
	}

	// if given a full url, change the host/port
	if (!host.empty()) {
		session->reset();
		session->setHost(host);
		session->setPort(port);
	}

	RequestData* reqdata = new RequestData();
	reqdata->req = request;
	reqdata->json = jsonPOST;
	reqdata->form = form;
	reqdata->callback = callback;

	{
		const std::lock_guard<std::mutex> lock(g_dlmutex);
		requestQueue->push_back(reqdata);
		// The thread updates should catch this one eventually
		// See UpdateHTTP[S]Requests
	}
}

void
DownloadManager::SetClientSessionByURL(HTTPClientSession* session,
									   const std::string url)
{
	Poco::URI uri(url);
	session->setHost(uri.getHost());
	session->setPort(uri.getPort());
}

void
DownloadManager::SetInGameplay(bool inGameplay)
{
	const std::lock_guard<std::mutex> lock(g_dlmutex);
	this->inGameplay = inGameplay;
}

void
DownloadManager::SetApiShouldUseHttps(bool state)
{
	const std::lock_guard<std::mutex> lock(g_dlmutex);
	this->apiShouldUseHttps = state;
}

bool
DownloadManager::EncodeSpaces(std::string& str)
{

	// Parse spaces (curl doesnt parse them properly)
	bool foundSpaces = false;
	size_t index = str.find(' ', 0);
	while (index != std::string::npos) {

		str.erase(index, 1);
		str.insert(index, "%20");
		index = str.find(' ', index);
		foundSpaces = true;
	}
	return foundSpaces;
}

void
DownloadManager::Update(float fDeltaSeconds)
{
	if (!initialized)
		Init();

	{
		const std::lock_guard<std::mutex> lock(g_dlmutex);
		if (inGameplay)
			return;
		if (apiHttpRequests.empty() && apiHttpsRequests.empty())
			return;
	}
	UpdateHTTPSRequests(fDeltaSeconds);
	UpdateHTTPRequests(fDeltaSeconds);
}

void
DownloadManager::UpdateHTTPSRequests(float fDeltaSeconds)
{
	std::vector<RequestData*> reqs;
	{
		const std::lock_guard<std::mutex> lock(g_dlmutex);
		reqs = apiHttpsRequests;
		apiHttpsRequests.clear();
	}
	for (auto& p : reqs) {
		ProcessRequest(p, *p_httpsClientSession);
		delete p;
	}
}
void
DownloadManager::UpdateHTTPRequests(float fDeltaSeconds)
{
	std::vector<RequestData*> reqs;
	{
		const std::lock_guard<std::mutex> lock(g_dlmutex);
		reqs = apiHttpRequests;
		apiHttpRequests.clear();
	}
	for (auto& p : reqs) {
		ProcessRequest(p, *p_httpClientSession);
		delete p;
	}
}

inline void
DownloadManager::ProcessRequest(RequestData*& data, HTTPClientSession& client)
{
	auto& req = data->req;
	auto& jsonPOST = data->json;
	auto& form = data->form;
	auto& callback = data->callback;
	HTTPResponse response;
	try {

		if (form != nullptr) {
			// Usually a GET
			// usually sends information as query params
			// can also be very simple POSTs
			form->prepareSubmit(*req);
			form->write(client.sendRequest(*req));
		} else if (jsonPOST != nullptr) {
			// Usually a POST
			// can send complex structured JSON
			std::stringstream ss;
			jsonPOST->stringify(ss);
			req->setContentLength(ss.str().length());
			std::ostream& os = client.sendRequest(*req);
			jsonPOST->stringify(os);
		} else {
			// Any request type, no params attached
			// usually a dumb GET
			client.sendRequest(*req);
		}

		// Get response output
		std::istream& respStream = client.receiveResponse(response);

		// Callback handles parsing and further success checks
		callback(respStream, response);

		// TODO: TEMPORARY FOR DEBUGGING
		Locator::getLogger()->info("{} {} {} {}",
							response.getStatus(),
							response.getContentType(),
							response.getReason(),
							response.getContentLength());
	} catch (Poco::Exception& e) {
		Locator::getLogger()->info("HTTP Request Exception: {} {} {}",
								   e.className(),
								   e.displayText(),
								   e.message());
		client.reset();
	}
}


/*
void
DownloadManager::UpdateDLSpeed()
{
	if (gameplay)
		MESSAGEMAN->Broadcast("PausingDownloads");
	else
		MESSAGEMAN->Broadcast("ResumingDownloads");
}
*/

bool
DownloadManager::IsLoggedIn()
{
	return !loginToken.empty();
}

bool
DownloadManager::IsInGameplay()
{
	return inGameplay;
}

bool
DownloadManager::ShouldUploadScores()
{
	return false;
	// return LoggedIn() && automaticSync &&
	//	   GamePreferences::m_AutoPlay == PC_HUMAN;
}

void
DownloadManager::Login(const std::string& username, const std::string& password)
{
	Locator::getLogger()->info("Generating user+pass login request ...");

	Poco::JSON::Object* json = new Poco::JSON::Object();
	json->set("email", username);
	json->set("password", password);
	json->set("key", API_KEY);

	RequestCallback callback = [this](std::istream& in, HTTPResponse& response) {
		Poco::JSON::Parser parser;
		std::string final_token = "";

		auto status = response.getStatus();
		if (status == HTTPResponse::HTTPStatus::HTTP_OK) {
			try {
				// parsed result turned into object
				Poco::Dynamic::Var res = parser.parse(in);
				Poco::JSON::Object::Ptr ret =
				  res.extract<Poco::JSON::Object::Ptr>();

				// field info
				auto access_token = ret->getValue<std::string>("access_token");

				// naturally provided these but do not need
				auto token_type = ret->getValue<std::string>("token_type");
				auto expires_in = ret->getValue<int>("expires_in");

				if (!access_token.empty()) {
					final_token = access_token;
				} else {
					Locator::getLogger()->error(
					  "Login FAILED - Parse error: Missing response token");
				}
			} catch (Poco::Exception& e) {
				Locator::getLogger()->error(
				  "Login FAILED - Exception occurred: {} {}",
				  e.name(),
				  e.message());
			}
		} else if (status == HTTPResponse::HTTPStatus::HTTP_UNPROCESSABLE_ENTITY) {
			Locator::getLogger()->warn(
			  "Login FAILED - Client out of date or other error");
		} else if (status == HTTPResponse::HTTPStatus::HTTP_UNAUTHORIZED) {
			Locator::getLogger()->info("Login FAILED - Bad credentials");
		} else {
			Locator::getLogger()->warn("Login FAILED - Unexpected status: {}",
									   status);
		}

		{
			const std::lock_guard<std::mutex> lock(g_dlmutex);
			loginToken = final_token;
		}
		OnLogin();
	};

	GenerateRequest(API_ROOT + API_LOGIN,
					callback,
					json,
					HTTPRequest::HTTP_POST,
					apiShouldUseHttps);
}

void
DownloadManager::OnLogin()
{
	if (IsLoggedIn()) {
		if (ShouldUploadScores()) {
			UploadScores();
		}
		GetRankedChartkeys();
		MESSAGEMAN->Broadcast("LoginSuccessful");
	} else {
		MESSAGEMAN->Broadcast("LoginFailed");
	}
}

void
DownloadManager::GetRankedChartkeys()
{
	Locator::getLogger()->info("Generating ranked chartkeys request ...");

	HTMLForm* form = new HTMLForm;
	form->setEncoding(HTMLForm::ENCODING_URL);
	form->set("start", "2021-09-26");
	form->set("end", "2021-12-31");

	RequestCallback callback = [this](std::istream& in, HTTPResponse& response) {
		Poco::JSON::Parser parser;
		std::vector<std::string> new_chartkeys;

		auto status = response.getStatus();
		if (status == HTTPResponse::HTTPStatus::HTTP_OK) {
			try {
				// parsed result turned into object
				Poco::Dynamic::Var res = parser.parse(in);
				Poco::JSON::Object::Ptr ret =
				  res.extract<Poco::JSON::Object::Ptr>();

				auto data = ret->getArray("data");
				for (auto it = data.get()->begin(); it != data.get()->end();
					 it++) {
					new_chartkeys.push_back(it->convert<std::string>());
				}
				Locator::getLogger()->info("Found {} newly ranked chartkeys",
										   new_chartkeys.size());
			} catch (Poco::Exception& e) {
				Locator::getLogger()->error(
				  "GetRankedChartkeys FAILED (Parse Error) - {} {}",
				  e.name(),
				  e.message());
			}
		} else if (status == HTTPResponse::HTTPStatus::HTTP_UNAUTHORIZED) {
			try {
				// parsed result turned into object
				Poco::Dynamic::Var res = parser.parse(in);
				Poco::JSON::Object::Ptr ret =
				  res.extract<Poco::JSON::Object::Ptr>();

				auto reason = ret->getValue<std::string>("message");
				Locator::getLogger()->warn(
				  "GetRankedChartkeys FAILED (401) - {}", reason);
			} catch (Poco::Exception& e) {
				Locator::getLogger()->error(
				  "GetRankedChartkeys FAILED (401 + Parse Error) - {} {}",
				  e.name(),
				  e.message());
			}
		} else {
			Locator::getLogger()->warn(
			  "GetRankedChartkeys FAILED - Unexpected status: {}", status);
		}

		{
			const std::lock_guard<std::mutex> lock(g_dlmutex);
			newlyRankedChartkeys = new_chartkeys;
		}
	};

	GenerateRequest(API_ROOT + API_RANKED_CHARTKEYS,
					callback,
					form,
					HTTPRequest::HTTP_GET,
					apiShouldUseHttps);
}

void
DownloadManager::UploadSingleScore(HighScore* hs)
{
	Locator::getLogger()->info("Generating single score upload request ({})",
							   hs->GetChartKey());

	Poco::JSON::Object* json = GenerateHighScoreObj(hs);

	RequestCallback callback = [this](std::istream& in,
									  HTTPResponse& response) {
		Poco::JSON::Parser parser;
		std::vector<std::string> new_chartkeys;

		auto status = response.getStatus();
		if (status == HTTPResponse::HTTPStatus::HTTP_OK) {
			try {
				// parsed result turned into object
				Poco::Dynamic::Var res = parser.parse(in);
				Poco::JSON::Object::Ptr ret =
				  res.extract<Poco::JSON::Object::Ptr>();

				
			} catch (Poco::Exception& e) {
				Locator::getLogger()->error(
				  "UploadSingleScore FAILED (Parse Error) - {} {}",
				  e.name(),
				  e.message());
			}
		} else if (status == HTTPResponse::HTTPStatus::HTTP_UNAUTHORIZED) {
			try {
				// parsed result turned into object
				Poco::Dynamic::Var res = parser.parse(in);
				Poco::JSON::Object::Ptr ret =
				  res.extract<Poco::JSON::Object::Ptr>();

				auto reason = ret->getValue<std::string>("message");
				Locator::getLogger()->warn(
				  "UploadSingleScore FAILED (401) - {}", reason);
			} catch (Poco::Exception& e) {
				Locator::getLogger()->error(
				  "UploadSingleScore FAILED (401 + Parse Error) - {} {}",
				  e.name(),
				  e.message());
			}
		} else if (status == HTTPResponse::HTTPStatus::HTTP_UNPROCESSABLE_ENTITY) {
			try {
				// parsed result turned into object
				Poco::Dynamic::Var res = parser.parse(in);
				Poco::JSON::Object::Ptr ret =
				  res.extract<Poco::JSON::Object::Ptr>();

				auto errors = ret->getObject("errors");
				std::vector<std::string> reasons;
				auto input_value_arr = errors->getArray("input_value");
				
				for (auto it = input_value_arr.get()->begin();
					 it != input_value_arr.get()->end();
					 it++) {
					reasons.push_back(it->convert<std::string>());
				}
				std::ostringstream reasonstr;
				if (!reasons.empty()) {
					std::copy(
					  reasons.begin(),
					  reasons.end() - 1,
					  std::ostream_iterator<std::string>(reasonstr, ", "));
					reasonstr << reasons.back();
				}
				Locator::getLogger()->warn(
				  "UploadSingleScore FAILED (422) - {}", reasonstr.str());
			} catch (Poco::Exception& e) {
				Locator::getLogger()->error(
				  "UploadSingleScore FAILED (422 + Parse Error) - {} {}",
				  e.name(),
				  e.message());
			}
		} else {
			Locator::getLogger()->warn(
			  "GetRankedChartkeys FAILED - Unexpected status: {}", status);
		}

		{
			const std::lock_guard<std::mutex> lock(g_dlmutex);
		}
	};

	GenerateRequest(API_ROOT + API_UPLOAD_SCORE,
					callback,
					json,
					HTTPRequest::HTTP_POST,
					apiShouldUseHttps);
}

inline Poco::JSON::Object*
DownloadManager::GenerateHighScoreObj(HighScore* hs)
{
	bool success = hs->LoadReplayData();
	const auto& offsets = hs->GetOffsetVector();
	const auto& columns = hs->GetTrackVector();
	const auto& types = hs->GetTapNoteTypeVector();
	const auto& rows = hs->GetNoteRowVector();
	auto steps = SONGMAN->GetStepsByChartkey(hs->GetChartKey());

	success |= steps != nullptr && (offsets.size() == columns.size() ==
									types.size() == rows.size());

	if (!success) {
		hs->UnloadReplayData();
		return nullptr;
	}

	Poco::JSON::Object* hsObject = new Poco::JSON::Object;

	hsObject->set("key", hs->GetScoreKey());
	hsObject->set("chart_key", hs->GetChartKey());
	hsObject->set("wife", hs->GetSSRNormPercent());
	hsObject->set("judge", hs->GetJudgeScale());
	hsObject->set("rate", hs->GetMusicRate());
	hsObject->set("modifiers", hs->GetModifiers());

	hsObject->set("grade", static_cast<int>(hs->GetGrade()));
	hsObject->set("max_combo", hs->GetMaxCombo());
	hsObject->set("marvelous", hs->GetTapNoteScore(TNS_W1));
	hsObject->set("perfect", hs->GetTapNoteScore(TNS_W2));
	hsObject->set("great", hs->GetTapNoteScore(TNS_W3));
	hsObject->set("good", hs->GetTapNoteScore(TNS_W4));
	hsObject->set("bad", hs->GetTapNoteScore(TNS_W5));
	hsObject->set("miss", hs->GetTapNoteScore(TNS_Miss));
	hsObject->set("hit_mine", hs->GetTapNoteScore(TNS_HitMine));

	hsObject->set("held", hs->GetHoldNoteScore(HNS_Held));
	hsObject->set("let_go", hs->GetHoldNoteScore(HNS_LetGo));
	hsObject->set("missed_hold", hs->GetHoldNoteScore(HNS_Missed));

	hsObject->set("datetime", hs->GetDateTime().GetString());
	hsObject->set("chord_cohesion", hs->GetChordCohesion());
	hsObject->set("calculator_version", hs->GetSSRCalcVersion());
	hsObject->set("top_score", hs->GetTopScore());
	hsObject->set("wife_version", hs->GetWifeVersion());
	hsObject->set("validation_key", hs->GetValidationKey(ValidationKey_Brittle));
	hsObject->set("machine_guid", hs->GetMachineGuid());

	Poco::JSON::Array replaydataArrObj;
	std::vector<float> timestamps =
	  steps->GetTimingData()->ConvertReplayNoteRowsToTimestamps(
		rows, hs->GetMusicRate());
	for (size_t i = 0; i < offsets.size(); i++) {
		Poco::JSON::Array replaydataArrRowObj;
		replaydataArrRowObj.add(timestamps[i]);
		replaydataArrRowObj.add(1000.f * offsets[i]);
		if (hs->GetReplayType() >= 2) {
			replaydataArrRowObj.add(columns[i]);
			replaydataArrRowObj.add(static_cast<int>(types[i]));
		}
		replaydataArrRowObj.add(rows[i]);

		replaydataArrObj.add(replaydataArrRowObj);
	}
	hsObject->set("replay_data", replaydataArrObj);

	return hsObject;
}

/*
inline void
SetCURLPOSTScore(CURL*& curlHandle,
				 curl_httppost*& form,
				 curl_httppost*& lastPtr,
				 HighScore*& hs)
{
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "scorekey", hs->GetScoreKey());
	hs->GenerateValidationKeys();
	SetCURLFormPostField(curlHandle, form, lastPtr, "ssr_norm", hs->norms);
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "max_combo", hs->GetMaxCombo());
	SetCURLFormPostField(curlHandle,
						 form,
						 lastPtr,
						 "valid",
						 static_cast<int>(hs->GetEtternaValid()));
	SetCURLFormPostField(curlHandle, form, lastPtr, "mods", hs->GetModifiers());
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "miss", hs->GetTapNoteScore(TNS_Miss));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "bad", hs->GetTapNoteScore(TNS_W5));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "good", hs->GetTapNoteScore(TNS_W4));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "great", hs->GetTapNoteScore(TNS_W3));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "perfect", hs->GetTapNoteScore(TNS_W2));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "marv", hs->GetTapNoteScore(TNS_W1));
	SetCURLFormPostField(curlHandle,
						 form,
						 lastPtr,
						 "datetime",
						 string(hs->GetDateTime().GetString().c_str()));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "hitmine", hs->GetTapNoteScore(TNS_HitMine));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "held", hs->GetHoldNoteScore(HNS_Held));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "letgo", hs->GetHoldNoteScore(HNS_LetGo));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "ng", hs->GetHoldNoteScore(HNS_Missed));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "chartkey", hs->GetChartKey());
	SetCURLFormPostField(curlHandle, form, lastPtr, "rate", hs->musics);
	auto chart = SONGMAN->GetStepsByChartkey(hs->GetChartKey());
	if (chart == nullptr)
		return;
	SetCURLFormPostField(curlHandle,
						 form,
						 lastPtr,
						 "negsolo",
						 chart->GetTimingData()->HasWarps() ||
						   chart->m_StepsType != StepsType_dance_single);
	SetCURLFormPostField(curlHandle,
						 form,
						 lastPtr,
						 "nocc",
						 static_cast<int>(!hs->GetChordCohesion()));
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "calc_version", hs->GetSSRCalcVersion());
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "wife_version", hs->GetWifeVersion());
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "topscore", hs->GetTopScore());
	SetCURLFormPostField(curlHandle,
						 form,
						 lastPtr,
						 "hash",
						 hs->GetValidationKey(ValidationKey_Brittle));
	SetCURLFormPostField(curlHandle, form, lastPtr, "wife", hs->GetWifeScore());
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "wifePoints", hs->GetWifePoints());
	SetCURLFormPostField(curlHandle, form, lastPtr, "judgeScale", hs->judges);
	SetCURLFormPostField(
	  curlHandle, form, lastPtr, "machineGuid", hs->GetMachineGuid());
	SetCURLFormPostField(curlHandle, form, lastPtr, "grade", hs->GetGrade());
	SetCURLFormPostField(curlHandle,
						 form,
						 lastPtr,
						 "wifeGrade",
						 string(GradeToString(hs->GetWifeGrade()).c_str()));
}*/

void
DownloadManager::UploadScore(HighScore* hs,
							 std::function<void()> callback,
							 bool load_from_disk)
{
	/*
	Locator::getLogger()->trace("Creating UploadScore request");
	if (!LoggedIn()) {
		Locator::getLogger()->trace(
		  "Attempted to upload score when not logged in (scorekey: \"{}\")",
		  hs->GetScoreKey().c_str());
		callback();
		return;
	}

	if (load_from_disk)
		hs->LoadReplayData();

	string replayString;
	const auto& offsets = hs->GetOffsetVector();
	const auto& columns = hs->GetTrackVector();
	const auto& types = hs->GetTapNoteTypeVector();
	const auto& rows = hs->GetNoteRowVector();
	if (!offsets.empty()) {
		replayString = "[";
		auto steps = SONGMAN->GetStepsByChartkey(hs->GetChartKey());
		if (steps == nullptr) {
			Locator::getLogger()->trace("Attempted to upload score with no loaded steps "
					   "(scorekey: \"{}\" chartkey: \"{}\")",
					   hs->GetScoreKey().c_str(),
					   hs->GetChartKey().c_str());
			return;
		}
		std::vector<float> timestamps =
		  steps->GetTimingData()->ConvertReplayNoteRowsToTimestamps(
			rows, hs->GetMusicRate());
		for (size_t i = 0; i < offsets.size(); i++) {
			replayString += "[";
			replayString += std::to_string(timestamps[i]) + ",";
			replayString += std::to_string(1000.f * offsets[i]) + ",";
			if (hs->GetReplayType() == 2) {
				replayString += to_string(columns[i]) + ",";
				replayString += to_string(types[i]) + ",";
			}
			replayString += to_string(rows[i]);
			replayString += "],";
		}
		replayString =
		  replayString.substr(0, replayString.size() - 1); // remove ","
		replayString += "]";
		if (load_from_disk)
			hs->UnloadReplayData();
	} else {
		// this should never be true unless we are using the manual forceupload
		// functions
		replayString = "[]";
	}

	auto done = [this, hs, callback, load_from_disk](HTTPRequest& req) {
		long response_code;
		Document d;
		if (d.Parse(req.result.c_str()).HasParseError()) {
			Locator::getLogger()->trace("Score upload response json parse error (error: \"{}\" "
					   "response body: \"{}\")",
					   rapidjson::GetParseError_En(d.GetParseError()),
					   req.result.c_str());
			callback();
			return;
		}
		if (d.HasMember("errors")) {
			auto onStatus = [hs,
							 response_code,
							 load_from_disk,
							 &callback,
							 &req](int status) {
				if (status == 22) {
					Locator::getLogger()->trace("Score upload response contains error, retrying "
							   "(http status: {} error status: {} response "
							   "body: \"{}\")",
							   response_code,
							   status,
							   req.result.c_str());
					DLMAN->StartSession(
					  DLMAN->sessionUser,
					  DLMAN->sessionPass,
					  [hs, callback, load_from_disk](bool logged) {
						  if (logged) {
							  DLMAN->UploadScore(hs, callback, load_from_disk);
						  }
					  });
					return true;
				} else if (status == 404 || status == 405 || status == 406) {
					if (hs->GetWifeVersion() == 3)
						hs->AddUploadedServer(wife3_rescore_upload_flag);
					hs->AddUploadedServer(serverURL.Get());
					hs->forceuploadedthissession = true;
				}
				// We don't log 406s because those are "not a a pb"
				// Which are normal, unless we're using verbose logging
				if (status != 406 || PREFSMAN->m_verbose_log > 1)
					Locator::getLogger()->trace(
					  "Score upload response contains error "
					  "(http status: {} error status: {} response body: "
					  "\"{}\" score key: \"{}\")",
					  response_code,
					  status,
					  req.result.c_str(),
					  hs->GetScoreKey().c_str());
				return false;
			};
			if (d["errors"].IsArray()) {
				for (auto& error : d["errors"].GetArray()) {
					if (!error["status"].IsInt())
						continue;
					int status = error["status"].GetInt();
					if (onStatus(status))
						return;
				}
			} else if (d["errors"].HasMember("status") &&
					   d["errors"]["status"].IsInt()) {
				if (onStatus(d["errors"]["status"].GetInt()))
					return;
			} else {
				Locator::getLogger()->trace("Score upload response contains error and we failed "
						   "to recognize it"
						   "(http status: {} response body: \"{}\")",
						   response_code,
						   req.result.c_str());
			}
			callback();
			return;
		}
		if (d.HasMember("data") && d["data"].IsObject() &&
			d["data"].HasMember("type") && d["data"]["type"].IsString() &&
			std::strcmp(d["data"]["type"].GetString(), "ssrResults") == 0 &&
			d["data"].HasMember("attributes") &&
			d["data"]["attributes"].IsObject() &&
			d["data"]["attributes"].HasMember("diff") &&
			d["data"]["attributes"]["diff"].IsObject()) {
			auto& diffs = d["data"]["attributes"]["diff"];
			FOREACH_ENUM(Skillset, ss)
			{
				auto str = SkillsetToString(ss);
				if (ss != Skill_Overall && diffs.HasMember(str.c_str()) &&
					diffs[str.c_str()].IsNumber())
					(DLMAN->sessionRatings)[ss] +=
					  diffs[str.c_str()].GetFloat();
			}
			if (diffs.HasMember("Rating") && diffs["Rating"].IsNumber())
				(DLMAN->sessionRatings)[Skill_Overall] +=
				  diffs["Rating"].GetFloat();
			if (hs->GetWifeVersion() == 3)
				hs->AddUploadedServer(wife3_rescore_upload_flag);
			hs->AddUploadedServer(serverURL.Get());
			hs->forceuploadedthissession = true;
			// HTTPRunning = response_code;// TODO: Why were we doing this?
		} else {
			Locator::getLogger()->trace("Score upload response malformed json "
					   "(http status: {} response body: \"{}\")",
					   response_code,
					   req.result.c_str());
		}
		callback();
	};
	HTTPRequest* req = new HTTPRequest(
	  done, [callback](HTTPRequest& req) { callback(); });
	Locator::getLogger()->trace("Finished creating UploadScore request");
	*/
}

// this is for new/live played scores that have replaydata in memory
void
DownloadManager::UploadScoreWithReplayData(HighScore* hs)
{
	this->UploadScore(
	  hs, []() {}, false /* (Without replay data loading from disk)*/);
}

// for older scores or newer scores that failed to upload using the above
// function we should probably do some refactoring of this
void
DownloadManager::UploadScoreWithReplayDataFromDisk(HighScore* hs,
												   std::function<void()> callback)
{
	this->UploadScore(
	  hs, callback, true /* (With replay data loading from disk)*/);
}

// This function begins uploading the given list (deque) of scores
// It does so one score at a time, sequentially (But without blocking)
// So as to not spam the server with possibly hundreds or thousands of scores
// the way it does that is by using a callback and moving the remaining scores
// into the callback which calls this function again
// (So it is essentially kind of recursive, with the base case of an empty
// deque)
void
uploadSequentially()
{
	Message msg("UploadProgress");
	msg.SetParam(
	  "percent",
	  1.f - (static_cast<float>(DLMAN->ScoreUploadSequentialQueue.size()) /
			 static_cast<float>(DLMAN->sequentialScoreUploadTotalWorkload)));
	MESSAGEMAN->Broadcast(msg);

	if (!DLMAN->ScoreUploadSequentialQueue.empty()) {
		auto hs = DLMAN->ScoreUploadSequentialQueue.front();
		DLMAN->ScoreUploadSequentialQueue.pop_front();
		DLMAN->UploadScoreWithReplayDataFromDisk(hs, uploadSequentially);
	}
}

bool
DownloadManager::UploadScores()
{
	// First we accumulate scores that have not been uploaded and have
	// replay data. There is no reason to upload updated calc versions to the
	// site anymore - the site uses its own calc and afaik ignores the provided
	// values, we only need to upload scores that have not been uploaded, and
	// scores that have been rescored from wife2 to wife3
	auto scores = SCOREMAN->GetAllPBPtrs();
	auto& newly_rescored = SCOREMAN->rescores;
	std::vector<HighScore*> toUpload;
	for (auto& vec : scores) {
		for (auto& s : vec) {
			// probably not worth uploading fails, they get rescored now
			if (s->GetGrade() == Grade_Failed)
				continue;
			// handle rescores, ignore upload check
			if (newly_rescored.count(s))
				toUpload.push_back(s);
			// ok so i think we probably do need an upload flag for wife3
			// resyncs, and to actively check it, since if people rescore
			// everything, play 1 song and close their game or whatever,
			// rescore list won't be built again and scores won't auto
			// sync
			else if (s->GetWifeVersion() == 3 &&
					 !s->IsUploadedToServer(wife3_rescore_upload_flag))
				toUpload.push_back(s);
			// normal behavior, upload scores that haven't been uploaded and
			// have replays
			else if (!s->IsUploadedToServer(serverURL.Get()) &&
					 s->HasReplayData())
				toUpload.push_back(s);
		}
	}

	if (!toUpload.empty())
		Locator::getLogger()->trace("Updating online scores. (Uploading {} scores)",
				   toUpload.size());
	else
		return false;

	bool was_not_uploading_already = this->ScoreUploadSequentialQueue.empty();
	if (was_not_uploading_already)
		this->sequentialScoreUploadTotalWorkload = toUpload.size();
	else
		this->sequentialScoreUploadTotalWorkload += toUpload.size();
	this->ScoreUploadSequentialQueue.insert(
	  this->ScoreUploadSequentialQueue.end(), toUpload.begin(), toUpload.end());
	if (was_not_uploading_already)
		uploadSequentially();

	return true;
}

// manual upload function that will upload all scores for a chart
// that skips some of the constraints of the auto uploaders
void
DownloadManager::ForceUploadScoresForChart(const std::string& ck, bool startnow)
{
	startnow = startnow && this->ScoreUploadSequentialQueue.empty();
	auto cs = SCOREMAN->GetScoresForChart(ck);
	if (cs) {
		auto& test = cs->GetAllScores();
		for (auto& s : test)
			if (!s->forceuploadedthissession) {
				if (s->GetGrade() != Grade_Failed) {
					// don't add stuff we're already uploading
					auto res =
					  std::find(this->ScoreUploadSequentialQueue.begin(),
								this->ScoreUploadSequentialQueue.end(),
								s);
					if (res != this->ScoreUploadSequentialQueue.end())
						continue;

					this->ScoreUploadSequentialQueue.push_back(s);
					this->sequentialScoreUploadTotalWorkload += 1;
				}
			}
	}

	if (startnow) {
		this->sequentialScoreUploadTotalWorkload =
		  this->ScoreUploadSequentialQueue.size();
		Locator::getLogger()->trace("Starting sequential upload of {} scores",
				   this->ScoreUploadSequentialQueue.size());
		uploadSequentially();
	}
}
// wrapper for packs
void
DownloadManager::ForceUploadScoresForPack(const std::string& pack,
										  bool startnow)
{
	startnow = startnow && this->ScoreUploadSequentialQueue.empty();
	auto songs = SONGMAN->GetSongs(pack);
	for (auto so : songs)
		for (auto c : so->GetAllSteps())
			ForceUploadScoresForChart(c->GetChartKey(), false);

	if (startnow) {
		this->sequentialScoreUploadTotalWorkload =
		  this->ScoreUploadSequentialQueue.size();
		Locator::getLogger()->trace("Starting sequential upload of {} scores",
				   this->ScoreUploadSequentialQueue.size());
		uploadSequentially();
	}
}
void
DownloadManager::ForceUploadAllScores()
{
	bool not_already_uploading = this->ScoreUploadSequentialQueue.empty();

	auto songs = SONGMAN->GetSongs(GROUP_ALL);
	for (auto so : songs)
		for (auto c : so->GetAllSteps())
			ForceUploadScoresForChart(c->GetChartKey(), false);

	if (not_already_uploading) {
		this->sequentialScoreUploadTotalWorkload =
		  this->ScoreUploadSequentialQueue.size();
		Locator::getLogger()->trace("Starting sequential upload of {} scores",
				   this->ScoreUploadSequentialQueue.size());
		uploadSequentially();
	}
}
/*
void
DownloadManager::EndSession()
{
	sessionUser = sessionPass = authToken = "";
	topScores.clear();
	sessionRatings.clear();
	// This is called on a shutdown, after MessageManager is gone
	if (MESSAGEMAN != nullptr)
		MESSAGEMAN->Broadcast("LogOut");
}
*/

OnlineTopScore
DownloadManager::GetTopSkillsetScore(unsigned int rank,
									 Skillset ss,
									 bool& result)
{
	unsigned int index = rank - 1;
	if (index < topScores[ss].size()) {
		result = true;
		return topScores[ss][index];
	}
	result = false;
	return OnlineTopScore();
}
/*
void
DownloadManager::RequestReplayData(const string& scoreid,
								   int userid,
								   const string& username,
								   const string& chartkey,
								   LuaReference& callback)
{
	auto done = [scoreid, callback, userid, username, chartkey](
				  HTTPRequest& req, CURLMsg*) {
		std::vector<pair<float, float>> replayData;
		std::vector<float> timestamps;
		std::vector<float> offsets;
		std::vector<int> tracks;
		std::vector<int> rows;
		std::vector<TapNoteType> types;

		Document d;
		if (d.Parse(req.result.c_str()).HasParseError()) {
			Locator::getLogger()->trace("Malformed replay data request response: {}", req.result);
			return;
		}
		if (d.HasMember("errors")) {
			StringBuffer buffer;
			Writer<StringBuffer> writer(buffer);
			d.Accept(writer);
			Locator::getLogger()->trace("Replay data request failed for {} (Response: {})", scoreid, buffer.GetString());
			return;
		}

		if (d.HasMember("data") && d["data"].IsObject() &&
			d["data"].HasMember("attributes") &&
			d["data"]["attributes"].IsObject() &&
			d["data"]["attributes"].HasMember("replay") &&
			d["data"]["attributes"]["replay"].IsArray()) {
			for (auto& note : d["data"]["attributes"]["replay"].GetArray()) {
				if (!note.IsArray() || note.Size() < 2 || !note[0].IsNumber() ||
					!note[1].IsNumber())
					continue;
				replayData.push_back(
				  std::make_pair(note[0].GetFloat(), note[1].GetFloat()));

				timestamps.push_back(note[0].GetFloat());
				offsets.push_back(note[1].GetFloat() / 1000.f);
				if (note.Size() == 3 &&
					note[2].IsInt()) { // pre-0.6 with noterows
					rows.push_back(note[2].GetInt());
				}
				if (note.Size() > 3 && note[2].IsInt() &&
					note[3].IsInt()) { // 0.6 without noterows
					tracks.push_back(note[2].GetInt());
					types.push_back(static_cast<TapNoteType>(note[3].GetInt()));
				}
				if (note.Size() == 5 && note[4].IsInt()) { // 0.6 with noterows
					rows.push_back(note[4].GetInt());
				}
			}
			auto& lbd = DLMAN->chartLeaderboards[chartkey];
			auto it = find_if(lbd.begin(),
							  lbd.end(),
							  [userid, username, scoreid](OnlineScore& a) {
								  return a.userid == userid &&
										 a.username == username &&
										 a.scoreid == scoreid;
							  });
			if (it != lbd.end()) {
				it->hs.SetOnlineReplayTimestampVector(timestamps);
				it->hs.SetOffsetVector(offsets);
				it->hs.SetTrackVector(tracks);
				it->hs.SetTapNoteTypeVector(types);
				it->hs.SetNoteRowVector(rows);

				if (tracks.empty())
					it->hs.SetReplayType(1);
				else
					it->hs.SetReplayType(2);
			}
		}

		auto& lbd = DLMAN->chartLeaderboards[chartkey];
		auto it = find_if(
		  lbd.begin(), lbd.end(), [userid, username, scoreid](OnlineScore& a) {
			  return a.userid == userid && a.username == username &&
					 a.scoreid == scoreid;
		  });
		if (it != lbd.end()) {
			it->hs.SetOnlineReplayTimestampVector(timestamps);
			it->hs.SetOffsetVector(offsets);
			it->hs.SetTrackVector(tracks);
			it->hs.SetTapNoteTypeVector(types);
			it->hs.SetNoteRowVector(rows);

			if (tracks.empty())
				it->hs.SetReplayType(1);
			else
				it->hs.SetReplayType(2);
		}

		if (!callback.IsNil() && callback.IsSet()) {
			auto L = LUA->Get();
			callback.PushSelf(L);
			std::string Error =
			  "Error running RequestChartLeaderBoard Finish Function: ";
			lua_newtable(L); // dunno whats going on here -mina
			for (unsigned i = 0; i < replayData.size(); ++i) {
				auto& pair = replayData[i];
				lua_newtable(L);
				lua_pushnumber(L, pair.first);
				lua_rawseti(L, -2, 1);
				lua_pushnumber(L, pair.second);
				lua_rawseti(L, -2, 2);
				lua_rawseti(L, -2, i + 1);
			}
			if (it != lbd.end())
				it->hs.PushSelf(L);
			LuaHelpers::RunScriptOnStack(
			  L, Error, 2, 0, true); // 2 args, 0 results
			LUA->Release(L);
		}
	};
	SendRequest("/replay/" + to_string(userid) + "/" + scoreid,
				std::vector<pair<string, string>>(),
				done,
				true);
}

void
DownloadManager::RequestChartLeaderBoard(const string& chartkey,
										 LuaReference& ref)
{
	auto done = [chartkey, ref](HTTPRequest& req, CURLMsg*) {
		Document d;
		if (d.Parse(req.result.c_str()).HasParseError()) {
			Locator::getLogger()->trace("RequestChartLeaderBoard Error: Malformed request response: {}", req.result);
			return;
		}
		std::vector<OnlineScore>& vec = DLMAN->chartLeaderboards[chartkey];
		vec.clear();

		long response_code;
		curl_easy_getinfo(req.handle, CURLINFO_RESPONSE_CODE, &response_code);

		// keep track of unranked charts
		if (response_code == 404)
			DLMAN->unrankedCharts.emplace(chartkey);
		else if (response_code == 200)
			DLMAN->unrankedCharts.erase(chartkey);

		if (!d.HasMember("errors") && d.HasMember("data") &&
			d["data"].IsArray()) {
			auto& scores = d["data"];
			for (auto& score_obj : scores.GetArray()) {
				if (!score_obj.HasMember("attributes") ||
					!score_obj["attributes"].IsObject() ||
					!score_obj["attributes"].HasMember("hasReplay") ||
					!score_obj["attributes"]["hasReplay"].IsBool() ||
					!score_obj["attributes"].HasMember("user") ||
					!score_obj["attributes"]["user"].IsObject() ||
					!score_obj["attributes"].HasMember("judgements") ||
					!score_obj["attributes"]["judgements"].IsObject() ||
					!score_obj["attributes"].HasMember("skillsets") ||
					!score_obj["attributes"]["skillsets"].IsObject()) {
					StringBuffer buffer;
					Writer<StringBuffer> writer(buffer);
					score_obj.Accept(writer);
					Locator::getLogger()->trace(
					  "Malformed score in chart leaderboard (chart: {}): {}",
					  chartkey,
					  buffer.GetString());
					continue;
				}
				auto& score = score_obj["attributes"];

				OnlineScore tmp;
				// tmp.songId = score.value("songId", 0);
				auto& user = score["user"];
				if (score.HasMember("songId") && score["songId"].IsString())
					tmp.songId = score["songId"].GetString();
				else
					tmp.songId = "";
				if (user.HasMember("userName") && user["userName"].IsString())
					tmp.username = user["userName"].GetString();
				else
					tmp.username = "";
				if (user.HasMember("avatar") && user["avatar"].IsString())
					tmp.avatar = user["avatar"].GetString();
				else
					tmp.avatar = "";
				if (user.HasMember("userId") && user["userId"].IsInt())
					tmp.userid = user["userId"].GetInt();
				else
					tmp.userid = 0;
				if (user.HasMember("countryCode") &&
					user["countryCode"].IsString())
					tmp.countryCode = user["countryCode"].GetString();
				else
					tmp.countryCode = "";
				if (user.HasMember("countryCode") &&
					user["countryCode"].IsString())
					tmp.countryCode = user["countryCode"].GetString();
				else
					tmp.countryCode = "";
				if (user.HasMember("playerRating") &&
					user["playerRating"].IsNumber())
					tmp.playerRating = user["playerRating"].GetFloat();
				else
					tmp.playerRating = 0.f;
				if (score.HasMember("wife") && score["wife"].IsNumber())
					tmp.wife = score["wife"].GetFloat() / 100.f;
				else
					tmp.wife = 0.f;
				if (score.HasMember("modifiers") &&
					score["modifiers"].IsString())
					tmp.modifiers = score["modifiers"].GetString();
				else
					tmp.modifiers = "";
				if (score.HasMember("maxCombo") && score["maxCombo"].IsInt())
					tmp.maxcombo = score["maxCombo"].GetInt();
				else
					tmp.maxcombo = 0;
				{
					auto& judgements = score["judgements"];
					if (judgements.HasMember("marvelous") &&
						judgements["marvelous"].IsInt())
						tmp.marvelous = judgements["marvelous"].GetInt();
					else
						tmp.marvelous = 0;
					if (judgements.HasMember("perfect") &&
						judgements["perfect"].IsInt())
						tmp.perfect = judgements["perfect"].GetInt();
					else
						tmp.perfect = 0;
					if (judgements.HasMember("great") &&
						judgements["great"].IsInt())
						tmp.great = judgements["great"].GetInt();
					else
						tmp.great = 0;
					if (judgements.HasMember("good") &&
						judgements["good"].IsInt())
						tmp.good = judgements["good"].GetInt();
					else
						tmp.good = 0;
					if (judgements.HasMember("bad") &&
						judgements["bad"].IsInt())
						tmp.bad = judgements["bad"].GetInt();
					else
						tmp.bad = 0;
					if (judgements.HasMember("miss") &&
						judgements["miss"].IsInt())
						tmp.miss = judgements["miss"].GetInt();
					else
						tmp.miss = 0;
					if (judgements.HasMember("hitMines") &&
						judgements["hitMines"].IsInt())
						tmp.minehits = judgements["hitMines"].GetInt();
					else
						tmp.minehits = 0;
					if (judgements.HasMember("heldHold") &&
						judgements["heldHold"].IsInt())
						tmp.held = judgements["heldHold"].GetInt();
					else
						tmp.held = 0;
					if (judgements.HasMember("letGoHold") &&
						judgements["letGoHold"].IsInt())
						tmp.letgo = judgements["letGoHold"].GetInt();
					else
						tmp.letgo = 0;
				}
				if (score.HasMember("datetime") && score["datetime"].IsString())
					tmp.datetime.FromString(score["datetime"].GetString());
				else
					tmp.datetime.FromString("0");
				if (score_obj.HasMember("id") && score_obj["id"].IsString())
					tmp.scoreid = score_obj["id"].GetString();
				else
					tmp.scoreid = "";

				// filter scores not on the current rate out if enabled...
				// dunno if we need this precision -mina
				if (score.HasMember("rate") && score["rate"].IsNumber())
					tmp.rate = score["rate"].GetFloat();
				else
					tmp.rate = 0.0;
				if (score.HasMember("noCC") && score["noCC"].IsBool())
					tmp.nocc = score["noCC"].GetBool();
				else
					tmp.nocc = false;
				if (score.HasMember("valid") && score["valid"].IsBool())
					tmp.valid = score["valid"].GetBool();
				else
					tmp.valid = false;
				if (score.HasMember("wifeVersion") &&
					score["wifeVersion"].IsInt()) {
					auto v = score["wifeVersion"].GetInt();
					if (v == 3)
						tmp.wifeversion = 3;
					else
						tmp.wifeversion = 2;
				}
				else
					tmp.wifeversion = 2;

				auto& ssrs = score["skillsets"];
				FOREACH_ENUM(Skillset, ss)
				{
					auto str = SkillsetToString(ss);
					if (ssrs.HasMember(str.c_str()) &&
						ssrs[str.c_str()].IsNumber())
						tmp.SSRs[ss] = ssrs[str.c_str()].GetFloat();
					else
						tmp.SSRs[ss] = 0.0;
				}
				if (score.HasMember("hasReplay") && score["hasReplay"].IsBool())
					tmp.hasReplay = score["hasReplay"].GetBool();
				else
					tmp.hasReplay = false;

				// eo still has some old profiles with various edge issues
				// that unfortunately need to be handled here screen out old
				// 11111 flags (my greatest mistake) and it's probably a
				// safe bet to throw out below 25% scores -mina
				if (tmp.wife > 1.f || tmp.wife < 0.25f || !tmp.valid)
					continue;

				// it seems prudent to maintain the eo functionality in this
				// way and screen out multiple scores from the same user
				// even more prudent would be to put this last where it
				// belongs, we don't want to screen out scores for players
				// who wouldn't have had them registered in the first place
				// -mina Moved this filtering to the Lua call. -poco if
				// (userswithscores.count(tmp.username) == 1)
				//	continue;

				// userswithscores.emplace(tmp.username);

				auto& hs = tmp.hs;
				hs.SetDateTime(tmp.datetime);
				hs.SetMaxCombo(tmp.maxcombo);
				hs.SetName(tmp.username);
				hs.SetModifiers(tmp.modifiers);
				hs.SetChordCohesion(tmp.nocc);
				hs.SetWifeScore(tmp.wife);
				hs.SetWifeVersion(tmp.wifeversion);
				hs.SetSSRNormPercent(tmp.wife);
				hs.SetMusicRate(tmp.rate);
				hs.SetChartKey(chartkey);
				hs.SetScoreKey("Online_" + tmp.scoreid);
				hs.SetGrade(hs.GetWifeGrade());

				hs.SetTapNoteScore(TNS_W1, tmp.marvelous);
				hs.SetTapNoteScore(TNS_W2, tmp.perfect);
				hs.SetTapNoteScore(TNS_W3, tmp.great);
				hs.SetTapNoteScore(TNS_W4, tmp.good);
				hs.SetTapNoteScore(TNS_W5, tmp.bad);
				hs.SetTapNoteScore(TNS_Miss, tmp.miss);
				hs.SetTapNoteScore(TNS_HitMine, tmp.minehits);

				hs.SetHoldNoteScore(HNS_Held, tmp.held);
				hs.SetHoldNoteScore(HNS_LetGo, tmp.letgo);

				FOREACH_ENUM(Skillset, ss)
				hs.SetSkillsetSSR(ss, tmp.SSRs[ss]);

				hs.userid = tmp.userid;
				hs.scoreid = tmp.scoreid;
				hs.avatar = tmp.avatar;
				hs.countryCode = tmp.countryCode;
				hs.hasReplay = tmp.hasReplay;

				vec.push_back(tmp);
			}
		}

		if (!ref.IsNil() && ref.IsSet()) {
			Lua* L = LUA->Get();
			ref.PushSelf(L);
			if (!lua_isnil(L, -1)) {
				std::string Error =
				  "Error running RequestChartLeaderBoard Finish Function: ";

				// 404: Chart not ranked
				// 401: Invalid login token
				if (response_code == 404 || response_code == 401) {
					lua_pushnil(L);
					// nil output means unranked to Lua
				} else {
					// expecting only 200 as the alternative
					// 200: success
					lua_newtable(L);
					for (unsigned i = 0; i < vec.size(); ++i) {
						auto& s = vec[i];
						s.Push(L);
						lua_rawseti(L, -2, i + 1);
					}
					// table size of 0 means ranked but no scores
					// any larger table size means ranked with scores (duh)
				}
				LuaHelpers::RunScriptOnStack(
				  L, Error, 1, 0, true); // 1 args, 0 results
			}
			LUA->Release(L);
		}
	};
	SendRequest("/charts/" + chartkey + "/leaderboards",
				std::vector<pair<string, string>>(),
				done,
				true);
}
*/

/*
void
DownloadManager::StartSession(
  string user,
  string pass,
  std::function<void(bool loggedIn)> callback = [](bool) {})
{
	
	string url = serverURL.Get() + "/login";
	if (loggingIn || user.empty()) {
		return;
	}
	DLMAN->loggingIn = true;
	EndSessionIfExists();

	
	CURLFormPostField(curlHandle, form, lastPtr, "username", user.c_str());
	CURLFormPostField(curlHandle, form, lastPtr, "password", pass.c_str());
	CURLFormPostField(
	  curlHandle, form, lastPtr, "clientData", CLIENT_DATA_KEY.c_str());
	
	auto done = [user, pass, callback](HTTPRequest& req) {
		Document d;
		if (d.Parse(req.result.c_str()).HasParseError()) {
			Locator::getLogger()->trace(
			  "StartSession Error: Malformed request response: {}", req.result);
			MESSAGEMAN->Broadcast("LoginFailed");
			DLMAN->loggingIn = false;
			return;
		}

		// Site 404s when login fails
		if (d.HasMember("errors") && d["errors"].IsArray()) {
			DLMAN->authToken = DLMAN->sessionUser = DLMAN->sessionPass = "";
			MESSAGEMAN->Broadcast("LoginFailed");
			DLMAN->loggingIn = false;
		}

		if (d.HasMember("data") && d["data"].IsObject() &&
			d["data"].HasMember("attributes") &&
			d["data"]["attributes"].IsObject() &&
			d["data"]["attributes"].HasMember("accessToken") &&
			d["data"]["attributes"]["accessToken"].IsString()) {
			DLMAN->authToken =
			  d["data"]["attributes"]["accessToken"].GetString();
			DLMAN->sessionUser = user;
			DLMAN->sessionPass = pass;
		} else {
			DLMAN->authToken = DLMAN->sessionUser = DLMAN->sessionPass = "";
		}
		DLMAN->OnLogin();
		callback(DLMAN->LoggedIn());
	};
	req->Failed = [](HTTPRequest& req) {
		DLMAN->authToken = DLMAN->sessionUser = DLMAN->sessionPass = "";
		MESSAGEMAN->Broadcast("LoginFailed");
		DLMAN->loggingIn = false;
	};
}
*/
int
DownloadManager::GetSkillsetRank(Skillset ss)
{
	return sessionRanks[ss];
}

float
DownloadManager::GetSkillsetRating(Skillset ss)
{
	return static_cast<float>(sessionRatings[ss]);
}

std::string
Download::MakeTempFileName(std::string s)
{
	return Basename(s);
}

void
Download::Update(float fDeltaSeconds)
{
	progress.time += fDeltaSeconds;
	if (progress.time > 1.0) {
		speed =
		  std::to_string(progress.downloaded / 1024 - downloadedAtLastUpdate);
		progress.time = 0;
		downloadedAtLastUpdate = progress.downloaded / 1024;
	}
}

Download::Download(std::string url,
				   std::string filename,
				   std::function<void(Download*)> done)
{
	Done = done;
	m_Url = url;
	m_TempFileName =
	  DL_DIR + (!filename.empty() ? filename : MakeTempFileName(url));
	auto opened = p_RFWrapper.file.Open(m_TempFileName, 2);
	ASSERT_M(opened, p_RFWrapper.file.GetError());
	DLMAN->EncodeSpaces(m_Url);
}

Download::~Download()
{
	FILEMAN->Remove(m_TempFileName);
	if (p_Pack)
		p_Pack->downloading = false;
}

void
Download::Install()
{
	Message* msg;
	if (!DLMAN->InstallSmzip(m_TempFileName))
		msg = new Message("DownloadFailed");
	else
		msg = new Message("PackDownloaded");
	msg->SetParam("pack", LuaReference::CreateFromPush(*p_Pack));
	MESSAGEMAN->Broadcast(*msg);
	delete msg;
}

void
Download::Failed()
{
	Message msg("DownloadFailed");
	msg.SetParam("pack", LuaReference::CreateFromPush(*p_Pack));
	MESSAGEMAN->Broadcast(msg);
}
/// Try to find in the Haystack the Needle - ignore case
bool
findStringIC(const std::string& strHaystack, const std::string& strNeedle)
{
	auto it = std::search(
	  strHaystack.begin(),
	  strHaystack.end(),
	  strNeedle.begin(),
	  strNeedle.end(),
	  [](char ch1, char ch2) { return toupper(ch1) == toupper(ch2); });
	return (it != strHaystack.end());
}

// lua start
#include "Etterna/Models/Lua/LuaBinding.h"
#include "LuaManager.h"
/** @brief Allow Lua to have access to the ProfileManager. */
class LunaDownloadManager : public Luna<DownloadManager>
{
  public:
	static int GetCountryCodes(T* p, lua_State* L)
	{
		auto& codes = DLMAN->countryCodes;
		LuaHelpers::CreateTableFromArray(codes, L);
		return 1;
	}
	static int GetUserCountryCode(T* p, lua_State* L)
	{
		lua_pushstring(L, "World");
		return 1;
	}
	static int GetAllPacks(T* p, lua_State* L)
	{
		std::vector<DownloadablePack>& packs = DLMAN->downloadablePacks;
		lua_createtable(L, packs.size(), 0);
		for (unsigned i = 0; i < packs.size(); ++i) {
			packs[i].PushSelf(L);
			lua_rawseti(L, -2, i + 1);
		}
		return 1;
	}
	static int GetDownloadingPacks(T* p, lua_State* L)
	{
		std::vector<DownloadablePack>& packs = DLMAN->downloadablePacks;
		std::vector<DownloadablePack*> dling;
		for (auto& pack : packs) {
			if (pack.downloading)
				dling.push_back(&pack);
		}
		lua_createtable(L, dling.size(), 0);
		for (unsigned i = 0; i < dling.size(); ++i) {
			dling[i]->PushSelf(L);
			lua_rawseti(L, -2, i + 1);
		}
		return 1;
	}
	static int GetQueuedPacks(T* p, lua_State* L)
	{
		lua_createtable(L, p->DownloadQueue.size(), 0);
		for (unsigned i = 0; i < p->DownloadQueue.size(); i++) {
			p->DownloadQueue[i].first->PushSelf(L);
			lua_rawseti(L, -2, i + 1);
		}
		return 1;
	}
	static int GetUsername(T* p, lua_State* L)
	{
		lua_pushstring(L, "");
		return 1;
	}
	static int GetSkillsetRank(T* p, lua_State* L)
	{
		lua_pushnumber(L, DLMAN->GetSkillsetRank(Enum::Check<Skillset>(L, 1)));
		return 1;
	}
	static int GetSkillsetRating(T* p, lua_State* L)
	{
		lua_pushnumber(L,
					   DLMAN->GetSkillsetRating(Enum::Check<Skillset>(L, 1)));
		return 1;
	}
	static int GetDownloads(T* p, lua_State* L)
	{
		/*
		map<string, Download*>& dls = DLMAN->downloads;
		lua_createtable(L, dls.size(), 0);
		int j = 0;
		for (auto& dl : dls) {
			dl.second->PushSelf(L);
			lua_rawseti(L, -2, j + 1);
			j++;
		}
		*/
		LuaHelpers::CreateTableFromArray(std::vector<Download*>(), L);
		return 1;
	}
	static int IsLoggedIn(T* p, lua_State* L)
	{
		lua_pushboolean(L, false);
		return 1;
	}
	static int Login(T* p, lua_State* L)
	{
		std::string user = SArg(1);
		std::string pass = SArg(2);
		//DLMAN->StartSession(user, pass);
		return 0;
	}
	static int LoginWithToken(T* p, lua_State* L)
	{
		std::string user = SArg(1);
		std::string token = SArg(2);
		//DLMAN->EndSessionIfExists();
		//DLMAN->authToken = token;
		//DLMAN->sessionUser = user;
		//DLMAN->sessionPass = "";
		//DLMAN->OnLogin();
		return 0;
	}
	static int Logout(T* p, lua_State* L)
	{
		//DLMAN->EndSessionIfExists();
		return 0;
	}
	static int GetLastVersion(T* p, lua_State* L)
	{
		lua_pushstring(L, "0.70.4");
		return 1;
	}
	static int GetRegisterPage(T* p, lua_State* L)
	{
		lua_pushstring(L, "");
		return 1;
	}
	static int GetTopSkillsetScore(T* p, lua_State* L)
	{
		int rank = IArg(1);
		auto ss = Enum::Check<Skillset>(L, 2);
		bool result;
		auto onlineScore = DLMAN->GetTopSkillsetScore(rank, ss, result);
		if (!result) {
			lua_pushnil(L);
			return 1;
		}
		lua_createtable(L, 0, 7);
		lua_pushstring(L, onlineScore.songName.c_str());
		lua_setfield(L, -2, "songName");
		lua_pushnumber(L, onlineScore.rate);
		lua_setfield(L, -2, "rate");
		lua_pushnumber(L, onlineScore.ssr);
		lua_setfield(L, -2, "ssr");
		lua_pushnumber(L, onlineScore.wifeScore);
		lua_setfield(L, -2, "wife");
		lua_pushstring(L, onlineScore.chartkey.c_str());
		lua_setfield(L, -2, "chartkey");
		LuaHelpers::Push(L, onlineScore.difficulty);
		lua_setfield(L, -2, "difficulty");
		LuaHelpers::Push(L, PlayerStageStats::GetGrade(onlineScore.wifeScore));
		lua_setfield(L, -2, "grade");
		return 1;
	}
	static int GetTopChartScoreCount(T* p, lua_State* L)
	{
		std::string ck = SArg(1);
		if (DLMAN->chartLeaderboards.count(ck))
			lua_pushnumber(L, DLMAN->chartLeaderboards[ck].size());
		else
			lua_pushnumber(L, 0);
		return 1;
	}
	static int GetTopChartScore(T* p, lua_State* L)
	{
		std::string chartkey = SArg(1);
		int rank = IArg(2);
		int index = rank - 1;
		if (index < 0 || !DLMAN->chartLeaderboards.count(chartkey) ||
			index >=
			  static_cast<int>(DLMAN->chartLeaderboards[chartkey].size())) {
			lua_pushnil(L);
			return 1;
		}
		auto& score = DLMAN->chartLeaderboards[chartkey][index];
		lua_createtable(
		  L, 0, 17 + NUM_Skillset + (score.replayData.empty() ? 0 : 1));
		FOREACH_ENUM(Skillset, ss)
		{
			lua_pushnumber(L, score.SSRs[ss]);
			lua_setfield(L, -2, SkillsetToString(ss).c_str());
		}
		lua_pushboolean(L, score.valid);
		lua_setfield(L, -2, "valid");
		lua_pushnumber(L, score.rate);
		lua_setfield(L, -2, "rate");
		lua_pushnumber(L, score.wife);
		lua_setfield(L, -2, "wife");
		lua_pushnumber(L, score.wifeversion);
		lua_setfield(L, -2, "wifeversion");
		lua_pushnumber(L, score.miss);
		lua_setfield(L, -2, "miss");
		lua_pushnumber(L, score.marvelous);
		lua_setfield(L, -2, "marvelous");
		lua_pushnumber(L, score.perfect);
		lua_setfield(L, -2, "perfect");
		lua_pushnumber(L, score.bad);
		lua_setfield(L, -2, "bad");
		lua_pushnumber(L, score.good);
		lua_setfield(L, -2, "good");
		lua_pushnumber(L, score.great);
		lua_setfield(L, -2, "great");
		lua_pushnumber(L, score.maxcombo);
		lua_setfield(L, -2, "maxcombo");
		lua_pushnumber(L, score.held);
		lua_setfield(L, -2, "held");
		lua_pushnumber(L, score.letgo);
		lua_setfield(L, -2, "letgo");
		lua_pushnumber(L, score.minehits);
		lua_setfield(L, -2, "minehits");
		lua_pushboolean(L, score.nocc);
		lua_setfield(L, -2, "nocc");
		lua_pushstring(L, score.modifiers.c_str());
		lua_setfield(L, -2, "modifiers");
		lua_pushstring(L, score.username.c_str());
		lua_setfield(L, -2, "username");
		lua_pushnumber(L, score.playerRating);
		lua_setfield(L, -2, "playerRating");
		lua_pushstring(L, score.datetime.GetString().c_str());
		lua_setfield(L, -2, "datetime");
		lua_pushstring(L, score.scoreid.c_str());
		lua_setfield(L, -2, "scoreid");
		lua_pushnumber(L, score.userid);
		lua_setfield(L, -2, "userid");
		lua_pushstring(L, score.avatar.c_str());
		lua_setfield(L, -2, "avatar");
		if (!score.replayData.empty()) {
			lua_createtable(L, 0, score.replayData.size());
			int i = 1;
			for (auto& pair : score.replayData) {
				lua_createtable(L, 0, 2);
				lua_pushnumber(L, pair.first);
				lua_rawseti(L, -2, 1);
				lua_pushnumber(L, pair.second);
				lua_rawseti(L, -2, 2);
				lua_rawseti(L, -2, i++);
			}
			lua_setfield(L, -2, "replaydata");
		}
		return 1;
	}
	static int GetCoreBundle(T* p, lua_State* L)
	{
		/*
		// don't remove this yet or at all idk yet -mina
		auto bundle = DLMAN->GetCoreBundle(SArg(1));
		lua_createtable(L, bundle.size(), 0);
		for (size_t i = 0; i < bundle.size(); ++i) {
			bundle[i]->PushSelf(L);
			lua_rawseti(L, -2, i + 1);
		}

		size_t totalsize = 0;
		float avgpackdiff = 0.f;

		for (auto p : bundle) {
			totalsize += p->size / 1024 / 1024;
			avgpackdiff += p->avgDifficulty;
		}

		if (!bundle.empty())
			avgpackdiff /= bundle.size();
		

		// this may be kind of unintuitive but lets roll with it for now
		// -mina
		lua_pushnumber(L, totalsize);
		lua_setfield(L, -2, "TotalSize");
		lua_pushnumber(L, avgpackdiff);
		lua_setfield(L, -2, "AveragePackDifficulty");
		*/
		return 1;
	}
	static int DownloadCoreBundle(T* p, lua_State* L)
	{
		bool bMirror = false;
		if (!lua_isnoneornil(L, 2)) {
			bMirror = BArg(2);
		}
		//DLMAN->DownloadCoreBundle(SArg(1), bMirror);
		return 0;
	}
	static int GetToken(T* p, lua_State* L)
	{
		lua_pushstring(L, "token");
		return 1;
	}

	static int RequestOnlineScoreReplayData(T* p, lua_State* L)
	{
		OnlineHighScore* hs =
		  (OnlineHighScore*)GetPointerFromStack(L, "HighScore", 1);
		int userid = hs->userid;
		std::string username = hs->GetDisplayName();
		std::string scoreid = hs->scoreid;
		std::string ck = hs->GetChartKey();

		bool alreadyHasReplay = false;
		alreadyHasReplay |= !hs->GetNoteRowVector().empty();
		alreadyHasReplay |=
		  !hs->GetCopyOfSetOnlineReplayTimestampVector().empty();
		alreadyHasReplay |= !hs->GetOffsetVector().empty();

		LuaReference f;
		if (lua_isfunction(L, 2))
			f = GetFuncArg(2, L);

		if (alreadyHasReplay) {
			if (!f.IsNil() && f.IsSet()) {
				auto L = LUA->Get();
				f.PushSelf(L);
				std::string Error =
				  "Error running RequestChartLeaderBoard Finish Function: ";
				hs->PushSelf(L);
				LuaHelpers::RunScriptOnStack(
				  L, Error, 2, 0, true); // 2 args, 0 results
			}
			return 0;
		}

		//DLMAN->RequestReplayData(scoreid, userid, username, ck, f);
		return 0;
	}

	// This requests the leaderboard from online. This may cause lag.
	// Use this sparingly.
	// This will NOT request a leaderboard if it already exists for the
	// chartkey. This needs to be updated in the future to do so. That means
	// this will not update a leaderboard to a new state.
	static int RequestChartLeaderBoardFromOnline(T* p, lua_State* L)
	{
		// an unranked chart check could be done here
		// but just in case, don't check
		// allow another request for those -- if it gets ranked during a session
		std::string chart = SArg(1);
		LuaReference ref;
		auto& leaderboardScores = DLMAN->chartLeaderboards[chart];
		if (lua_isfunction(L, 2)) {
			lua_pushvalue(L, 2);
			ref.SetFromStack(L);
		}
		if (!leaderboardScores.empty()) {
			if (!ref.IsNil()) {
				ref.PushSelf(L);
				if (!lua_isnil(L, -1)) {
					std::string Error = "Error running RequestChartLeaderBoard "
										"Finish Function: ";
					lua_newtable(L);
					for (unsigned i = 0; i < leaderboardScores.size(); ++i) {
						auto& s = leaderboardScores[i];
						s.Push(L);
						lua_rawseti(L, -2, i + 1);
					}
					LuaHelpers::RunScriptOnStack(
					  L, Error, 1, 0, true); // 1 args, 0 results
				}
			}
			return 0;
		}
		//DLMAN->RequestChartLeaderBoard(chart, ref);

		return 0;
	}

	static int GetChartLeaderBoard(T* p, lua_State* L)
	{
		std::vector<HighScore*> filteredLeaderboardScores;
		std::unordered_set<std::string> userswithscores;
		auto ck = SArg(1);
		auto& leaderboardScores = DLMAN->chartLeaderboards[ck];
		std::string country = "";
		if (!lua_isnoneornil(L, 2)) {
			country = SArg(2);
		}
		float currentrate = GAMESTATE->m_SongOptions.GetCurrent().m_fMusicRate;

		// empty chart leaderboards return empty lists
		// unranked charts return NO lists
		if (DLMAN->unrankedCharts.count(ck)) {
			lua_pushnil(L);
			return 1;
		}

		for (auto& score : leaderboardScores) {
			auto& leaderboardHighScore = score.hs;
			if (p->ccoffonly && !score.nocc)
				continue;
			if (p->currentrateonly &&
				lround(leaderboardHighScore.GetMusicRate() * 10000.f) !=
				  lround(currentrate * 10000.f))
				continue;
			if (p->topscoresonly &&
				userswithscores.count(leaderboardHighScore.GetName()) == 1)
				continue;
			if (!country.empty() && country != "Global" &&
				leaderboardHighScore.countryCode != country)
				continue;

			filteredLeaderboardScores.push_back(&(score.hs));
			userswithscores.emplace(leaderboardHighScore.GetName());
		}

		if (!filteredLeaderboardScores.empty() && p->currentrateonly) {
			std::sort(filteredLeaderboardScores.begin(),
					  filteredLeaderboardScores.end(),
					  [](const HighScore* a, const HighScore* b) -> bool {
						  return a->GetWifeScore() > b->GetWifeScore();
					  });
		}

		LuaHelpers::CreateTableFromArray(filteredLeaderboardScores, L);
		return 1;
	}

	static int ToggleRateFilter(T* p, lua_State* L)
	{
		p->currentrateonly = !p->currentrateonly;
		return 0;
	}
	static int GetCurrentRateFilter(T* p, lua_State* L)
	{
		lua_pushboolean(L, p->currentrateonly);
		return 1;
	}
	static int ToggleTopScoresOnlyFilter(T* p, lua_State* L)
	{
		p->topscoresonly = !p->topscoresonly;
		return 0;
	}
	static int GetTopScoresOnlyFilter(T* p, lua_State* L)
	{
		lua_pushboolean(L, p->topscoresonly);
		return 1;
	}
	static int ToggleCCFilter(T* p, lua_State* L)
	{
		p->ccoffonly = !p->ccoffonly;
		return 0;
	}
	static int GetCCFilter(T* p, lua_State* L)
	{
		lua_pushboolean(L, p->ccoffonly);
		return 1;
	}
	static int SendReplayDataForOldScore(T* p, lua_State* L)
	{
		DLMAN->UploadScoreWithReplayDataFromDisk(
		  SCOREMAN->GetScoresByKey().at(SArg(1)));
		// DLMAN->UpdateOnlineScoreReplayData(SArg(1));
		return 0;
	}
	static int UploadScoresForChart(T* p, lua_State* L)
	{
		DLMAN->ForceUploadScoresForChart(SArg(1));
		return 0;
	}
	static int UploadScoresForPack(T* p, lua_State* L)
	{
		DLMAN->ForceUploadScoresForPack(SArg(1));
		return 0;
	}
	static int UploadAllScores(T* p, lua_State* L)
	{
		DLMAN->ForceUploadAllScores();
		return 0;
	}
	static int UploadThisScore(T* p, lua_State* L) {
		auto* hs = Luna<HighScore>::check(L, 1);

		DLMAN->UploadSingleScore(hs);
		return 0;
	}
	LunaDownloadManager()
	{
		ADD_METHOD(GetCountryCodes);
		ADD_METHOD(GetUserCountryCode);
		ADD_METHOD(DownloadCoreBundle);
		ADD_METHOD(GetCoreBundle);
		ADD_METHOD(GetAllPacks);
		ADD_METHOD(GetDownloadingPacks);
		ADD_METHOD(GetQueuedPacks);
		ADD_METHOD(GetDownloads);
		ADD_METHOD(GetToken);
		ADD_METHOD(IsLoggedIn);
		ADD_METHOD(Login);
		ADD_METHOD(LoginWithToken);
		ADD_METHOD(GetUsername);
		ADD_METHOD(GetSkillsetRank);
		ADD_METHOD(GetSkillsetRating);
		ADD_METHOD(GetTopSkillsetScore);
		ADD_METHOD(GetTopChartScore);
		ADD_METHOD(GetTopChartScoreCount);
		ADD_METHOD(GetLastVersion);
		ADD_METHOD(GetRegisterPage);
		ADD_METHOD(RequestChartLeaderBoardFromOnline);
		ADD_METHOD(RequestOnlineScoreReplayData);
		ADD_METHOD(GetChartLeaderBoard);
		// This does not actually request the leaderboard from online.
		// It gets the already retrieved data from DLMAN
		// Why does this alias exist?
		AddMethod("GetChartLeaderboard", GetChartLeaderBoard);
		ADD_METHOD(ToggleRateFilter);
		ADD_METHOD(GetCurrentRateFilter);
		ADD_METHOD(ToggleTopScoresOnlyFilter);
		ADD_METHOD(GetTopScoresOnlyFilter);
		ADD_METHOD(ToggleCCFilter);
		ADD_METHOD(GetCCFilter);
		ADD_METHOD(SendReplayDataForOldScore);
		ADD_METHOD(UploadScoresForChart);
		ADD_METHOD(UploadScoresForPack);
		ADD_METHOD(UploadAllScores);
		ADD_METHOD(Logout);

		ADD_METHOD(UploadThisScore);
	}
};
LUA_REGISTER_CLASS(DownloadManager)

class LunaDownloadablePack : public Luna<DownloadablePack>
{
  public:
	static int DownloadAndInstall(T* p, lua_State* L)
	{
		bool mirror = false;
		if (lua_gettop(L) > 0)
			mirror = BArg(1);
		if (p->downloading) {
			p->PushSelf(L);
			return 1;
		}
		Download* dl = nullptr; //DLMAN->DownloadAndInstallPack(p, mirror);
		if (dl) {
			dl->PushSelf(L);
			p->downloading = true;
		} else
			lua_pushnil(L);
		IsQueued(p, L);
		return 1;
	}
	static int GetName(T* p, lua_State* L)
	{
		lua_pushstring(L, p->name.c_str());
		return 1;
	}
	static int GetSize(T* p, lua_State* L)
	{
		lua_pushnumber(L, p->size);
		return 1;
	}
	static int GetAvgDifficulty(T* p, lua_State* L)
	{
		lua_pushnumber(L, p->avgDifficulty);
		return 1;
	}
	static int IsQueued(T* p, lua_State* L)
	{
		auto it = std::find_if(
		  DLMAN->DownloadQueue.begin(),
		  DLMAN->DownloadQueue.end(),
		  [p](std::pair<DownloadablePack*, bool> pair) { return pair.first == p; });
		lua_pushboolean(L, it != DLMAN->DownloadQueue.end());
		return 1;
	}
	static int RemoveFromQueue(T* p, lua_State* L)
	{
		auto it = std::find_if(
		  DLMAN->DownloadQueue.begin(),
		  DLMAN->DownloadQueue.end(),
		  [p](std::pair<DownloadablePack*, bool> pair) { return pair.first == p; });
		if (it == DLMAN->DownloadQueue.end())
			// does not exist
			lua_pushboolean(L, false);
		else {
			DLMAN->DownloadQueue.erase(it);
			// success?
			lua_pushboolean(L, true);
		}
		return 1;
	}
	static int IsDownloading(T* p, lua_State* L)
	{
		lua_pushboolean(L, p->downloading == 0);
		return 1;
	}
	static int GetDownload(T* p, lua_State* L)
	{
		if (p->downloading) {
			// using GetDownload on a download started by a Mirror isn't keyed by the Mirror url
			// have to check both
			auto u = p->url;
			auto m = p->mirror;
			/*
			if (DLMAN->downloads.count(u))
				DLMAN->downloads[u]->PushSelf(L);
			else if (DLMAN->downloads.count(m))
				DLMAN->downloads[m]->PushSelf(L);
			else
				lua_pushnil(L); // this shouldnt happen
				*/
		}
		else
			lua_pushnil(L);
		return 1;
	}
	static int GetID(T* p, lua_State* L)
	{
		lua_pushnumber(L, p->id);
		return 1;
	}
	static int GetURL(T* p, lua_State* L)
	{
		lua_pushstring(L, p->url.c_str());
		return 1;
	}
	static int GetMirror(T* p, lua_State* L)
	{
		lua_pushstring(L, p->mirror.c_str());
		return 1;
	}
	LunaDownloadablePack()
	{
		ADD_METHOD(DownloadAndInstall);
		ADD_METHOD(IsDownloading);
		ADD_METHOD(IsQueued);
		ADD_METHOD(RemoveFromQueue);
		ADD_METHOD(GetAvgDifficulty);
		ADD_METHOD(GetName);
		ADD_METHOD(GetSize);
		ADD_METHOD(GetDownload);
		ADD_METHOD(GetID);
		ADD_METHOD(GetURL);
		ADD_METHOD(GetMirror);
	}
};

LUA_REGISTER_CLASS(DownloadablePack)

class LunaDownload : public Luna<Download>
{
  public:
	static int GetKBDownloaded(T* p, lua_State* L)
	{
		lua_pushnumber(L, static_cast<int>(p->progress.downloaded));
		return 1;
	}
	static int GetKBPerSecond(T* p, lua_State* L)
	{
		lua_pushnumber(L, atoi(p->speed.c_str()));
		return 1;
	}
	static int GetTotalKB(T* p, lua_State* L)
	{
		lua_pushnumber(L, static_cast<int>(p->progress.dltotal));
		return 1;
	}
	static int Stop(T* p, lua_State* L)
	{
		p->p_RFWrapper.stop = true;
		return 0;
	}
	LunaDownload()
	{
		ADD_METHOD(GetTotalKB);
		ADD_METHOD(GetKBDownloaded);
		ADD_METHOD(GetKBPerSecond);
		ADD_METHOD(Stop);
	}
};

LUA_REGISTER_CLASS(Download)
