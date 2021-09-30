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
static const size_t UPLOAD_SCORE_BULK_CHUNK_SIZE = 100;

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
		request->setCredentials("Bearer", sessionToken);

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

	if (form != nullptr && jsonPOST != nullptr) {
		Locator::getLogger()->warn(
		  "A request to {} contained both HTMLForm and JSON data. It was "
		  "sent "
		  "as HTMLForm. This is a programming mistake. Report to developer",
		  req->getURI());
	}

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
	return !sessionToken.empty();
}

bool
DownloadManager::IsInGameplay()
{
	return inGameplay;
}

bool
DownloadManager::ShouldUploadScores()
{
	return IsLoggedIn() && automaticSync &&
		   GamePreferences::m_AutoPlay == PC_HUMAN;
}

std::string
DownloadManager::GetSessionUser()
{
	return sessionUser;
}

std::string
DownloadManager::GetSessionToken()
{
	return sessionToken;
}

void
DownloadManager::LoginRequest(const std::string& username, const std::string& password)
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
			auto reason = OnAuthFailure(in);
			Locator::getLogger()->info("Login FAILED - {}", reason);
		} else {
			Locator::getLogger()->warn("Login FAILED - Unexpected status: {}",
									   status);
		}

		{
			const std::lock_guard<std::mutex> lock(g_dlmutex);
			sessionToken = final_token;
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
		auto* prof = PROFILEMAN->GetProfile(PLAYER_1);
		if (prof != nullptr) {
			auto lastCheckDT = prof->m_lastRankedChartkeyCheck;
			Poco::DateTime pocoDT = Poco::DateTime(
			  lastCheckDT.tm_year, lastCheckDT.tm_mon, lastCheckDT.tm_mday);

			// pass only 1 date, which sets the start of the range
			// so it searches [start, inf]
			GetRankedChartkeys(true, pocoDT);
		}
		MESSAGEMAN->Broadcast("LoginSuccessful");
	}
	MESSAGEMAN->Broadcast("LoginFailed");
}

std::string
DownloadManager::OnAuthFailure(std::istream& responseData)
{
	// basically just force you to log out for now
	// this can be modified to handle specific 401 response "subtypes"
	// that is why the json is given
	// parsed result turned into object
	/*
	Poco::JSON::Parser parser;
	Poco::Dynamic::Var res = parser.parse(responseData);
	Poco::JSON::Object::Ptr ret = res.extract<Poco::JSON::Object::Ptr>();
	...
	*/

	Logout();

	return std::string("Authorization Failure");
}

void
DownloadManager::LoginWithToken(const std::string& sessionToken, const std::string& username)
{
	{
		const std::lock_guard<std::mutex> lock(g_dlmutex);
		this->sessionToken = sessionToken;
		sessionUser = username;
	}
	OnLogin();
}

void
DownloadManager::Logout()
{
	if (IsLoggedIn()) {
		{
			const std::lock_guard<std::mutex> lock(g_dlmutex);
			sessionToken = "";
			sessionUser = "";
		}
		// This is called on a shutdown, after MessageManager is gone
		if (MESSAGEMAN != nullptr)
			MESSAGEMAN->Broadcast("LogOut");
	}
}

void
DownloadManager::GetRankedChartkeysRequest(bool uploadAfterResponse, const Poco::DateTime start, const Poco::DateTime end)
{
	Locator::getLogger()->info("Generating ranked chartkeys request ...");

	HTMLForm* form = new HTMLForm;
	form->setEncoding(HTMLForm::ENCODING_URL);

	std::string startstr =
	  fmt::format("{}-{}-{}", start.year(), start.month(), start.day());
	std::string endstr =
	  fmt::format("{}-{}-{}", end.year(), end.month(), end.day());

	form->set("start", startstr);
	form->set("end", endstr);

	RequestCallback callback = [this, uploadAfterResponse](std::istream& in, HTTPResponse& response) {
		Poco::JSON::Parser parser;
		std::unordered_set<std::string> new_chartkeys;

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
					new_chartkeys.emplace(it->convert<std::string>());
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
			auto reason = OnAuthFailure(in);
			Locator::getLogger()->warn("GetRankedChartkeys FAILED (401) - {}",
									   reason);
		} else {
			Locator::getLogger()->warn(
			  "GetRankedChartkeys FAILED - Unexpected status: {}", status);
		}

		{
			const std::lock_guard<std::mutex> lock(g_dlmutex);
			newlyRankedChartkeys = new_chartkeys;
		}
		if (uploadAfterResponse) {
			UploadAllPBs(false);
		}

	};

	GenerateRequest(API_ROOT + API_RANKED_CHARTKEYS,
					callback,
					form,
					HTTPRequest::HTTP_GET,
					apiShouldUseHttps);
}

void
DownloadManager::UploadSingleScoreRequest(HighScore* hs)
{
	Locator::getLogger()->info("Generating single score upload request ({})",
							   hs->GetChartKey());

	Poco::JSON::Object* json = GenerateHighScoreObj(hs);

	RequestCallback callback = [this, hs](std::istream& in,
									  HTTPResponse& response) {
		Poco::JSON::Parser parser;
		bool success = false;

		auto status = response.getStatus();
		if (status == HTTPResponse::HTTPStatus::HTTP_OK) {
			try {
				/*
				// parsed result turned into object
				Poco::Dynamic::Var res = parser.parse(in);
				Poco::JSON::Object::Ptr ret =
				  res.extract<Poco::JSON::Object::Ptr>();

				auto overall = ret->getValue<float>("overall");
				auto stream = ret->getValue<float>("stream");
				auto jumpstream = ret->getValue<float>("jumpstream");
				auto handstream = ret->getValue<float>("handstream");
				auto jacks = ret->getValue<float>("jacks");
				auto chordjacks = ret->getValue<float>("chordjacks");
				auto stamina = ret->getValue<float>("stamina");
				auto technical = ret->getValue<float>("technical");

				Locator::getLogger()->info(
				  "Uploaded score {} - \n\tOverall {}\n\tStream {}\n\tJS "
				  "{}\n\tHS {}\n\tJacks {}\n\tCJ {}\n\tStamina {}\n\tTech {}",
				  hs->GetChartKey(),
				  overall,
				  stream,
				  jumpstream,
				  handstream,
				  jacks,
				  chordjacks,
				  stamina,
				  technical);
				*/
				Locator::getLogger()->info("Score {} uploaded",
										   hs->GetScoreKey());

				UpdateScoreAfterUploadSuccess(hs);
				success = true;
			} catch (Poco::Exception& e) {
				ResetScoreAfterUploadFailure(hs);
				Locator::getLogger()->error(
				  "UploadSingleScore FAILED (Parse Error) - {} {}",
				  e.name(),
				  e.message());
			}
		} else if (status == HTTPResponse::HTTPStatus::HTTP_UNAUTHORIZED) {
			ResetScoreAfterUploadFailure(hs);
			auto reason = OnAuthFailure(in);
			Locator::getLogger()->warn("UploadSingleScore FAILED (401) - {}",
									   reason);
		} else if (status == HTTPResponse::HTTPStatus::HTTP_UNPROCESSABLE_ENTITY) {
			ResetScoreAfterUploadFailure(hs);
			try {
				// parsed result turned into object
				Poco::Dynamic::Var res = parser.parse(in);
				Poco::JSON::Object::Ptr ret =
				  res.extract<Poco::JSON::Object::Ptr>();

				auto reasonstr = ExtractHTTP422Reasons(ret);
				Locator::getLogger()->warn(
				  "UploadSingleScore FAILED (422) - {}", reasonstr);
			} catch (Poco::Exception& e) {
				Locator::getLogger()->error(
				  "UploadSingleScore FAILED (422 + Parse Error) - {} {}",
				  e.name(),
				  e.message());
			}
		} else {
			ResetScoreAfterUploadFailure(hs);
			Locator::getLogger()->warn(
			  "UploadSingleScore FAILED - Unexpected status: {}", status);
		}

		if (success)
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

void
DownloadManager::UploadBulkScoresRequest(std::vector<HighScore*>& hsList)
{
	Locator::getLogger()->info(
	  "Generating bulk score upload request ({} "
	  "scores dividing into {} chunks)",
	  hsList.size(),
	  static_cast<int>(
		std::ceil(static_cast<float>(hsList.size()) /
				  static_cast<float>(UPLOAD_SCORE_BULK_CHUNK_SIZE))));

	std::vector<HighScore*> hsCompiling;
	for (auto it = hsList.begin(); it != hsList.end(); it++) {
		hsCompiling.emplace_back(*it);
		if (hsCompiling.size() >= UPLOAD_SCORE_BULK_CHUNK_SIZE) {
			UploadBulkScoresRequestInternal(hsCompiling);
			hsCompiling.clear();
			hsCompiling.shrink_to_fit();
		}
	}
	if (!hsCompiling.empty()) {
		UploadBulkScoresRequestInternal(hsCompiling);
	}
}

void
DownloadManager::UploadBulkScoresRequestInternal(const std::vector<HighScore*> hsList)
{

	Poco::JSON::Object* json = new Poco::JSON::Object;
	Poco::JSON::Array dataArr;
	for (auto& hs : hsList) {
		// wow this is really bad
		Poco::JSON::Object* tmp = GenerateHighScoreObj(hs);
		Poco::JSON::Object hsObj = *tmp;
		delete tmp;

		dataArr.add(hsObj);
	}
	json->set("data", dataArr);

	RequestCallback callback = [this, hsList](std::istream& in,
									  HTTPResponse& response) {
		Poco::JSON::Parser parser;
		bool success = false;

		auto status = response.getStatus();
		if (status == HTTPResponse::HTTPStatus::HTTP_OK) {
			try {
				/*
				// parsed result turned into object
				Poco::Dynamic::Var res = parser.parse(in);
				Poco::JSON::Object::Ptr ret =
				  res.extract<Poco::JSON::Object::Ptr>();

				// nothing returned?
				*/
				UpdateBulkScoresAfterUploadSuccess(hsList);
				success = true;
			} catch (Poco::Exception& e) {
				ResetBulkScoresAfterUploadFailure(hsList);
				Locator::getLogger()->error(
				  "UploadBulkScores FAILED (Parse Error) - {} {}",
				  e.name(),
				  e.message());
			}
		} else if (status == HTTPResponse::HTTPStatus::HTTP_UNAUTHORIZED) {
			ResetBulkScoresAfterUploadFailure(hsList);
			auto reason = OnAuthFailure(in);
			Locator::getLogger()->warn("UploadBulkScores FAILED (401) - {}",
									   reason);
		} else if (status ==
				   HTTPResponse::HTTPStatus::HTTP_UNPROCESSABLE_ENTITY) {
			ResetBulkScoresAfterUploadFailure(hsList);
			try {
				// parsed result turned into object
				Poco::Dynamic::Var res = parser.parse(in);
				Poco::JSON::Object::Ptr ret =
				  res.extract<Poco::JSON::Object::Ptr>();

				auto reasonstr = ExtractHTTP422Reasons(ret);
				Locator::getLogger()->warn("UploadBulkScores FAILED (422) - {}",
										   reasonstr);
			} catch (Poco::Exception& e) {
				Locator::getLogger()->error(
				  "UploadBulkScores FAILED (422 + Parse Error) - {} {}",
				  e.name(),
				  e.message());
			}
		} else {
			ResetBulkScoresAfterUploadFailure(hsList);
			Locator::getLogger()->warn(
			  "UploadBulkScores FAILED - Unexpected status: {}", status);
		}

		if (success)
		{
			const std::lock_guard<std::mutex> lock(g_dlmutex);

			auto* prof = PROFILEMAN->GetProfile(PLAYER_1);
			// reset profile check date to latest score chunk date
			// (if it is new enough)
			auto lastDT = hsList.back()->GetDateTime();
			lastDT.Yesterday();
			if (prof->m_lastRankedChartkeyCheck < lastDT)
				prof->m_lastRankedChartkeyCheck = lastDT;
			// this wont save until you save your profile
		}
	};

	GenerateRequest(API_ROOT + API_UPLOAD_SCORE_BULK,
					callback,
					json,
					HTTPRequest::HTTP_POST,
					apiShouldUseHttps);
}

inline Poco::JSON::Object*
DownloadManager::GenerateHighScoreObj(HighScore* hs)
{
	bool success = hs->LoadReplayData();
	const auto& offsets = hs->GetCopyOfOffsetVector();
	const auto& columns = hs->GetCopyOfTrackVector();
	const auto& types = hs->GetCopyOfTapNoteTypeVector();
	const auto& rows = hs->GetCopyOfNoteRowVector();
	auto steps = SONGMAN->GetStepsByChartkey(hs->GetChartKey());

	success |= steps != nullptr && (offsets.size() == columns.size() ==
									types.size() == rows.size());

	if (!success) {
		hs->UnloadReplayData();
		return nullptr;
	}
	// leaving replay data loaded here is a bad idea
	// it gets unloaded eventually but for thousands of scores
	// it adds up

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
		//DLMAN->UploadScoreWithReplayDataFromDisk(hs, uploadSequentially);
	}
}


inline void
DownloadManager::ResetScoreAfterUploadFailure(HighScore* hs)
{
	// the purpose of this is to undo the actions of CanUploadScore
	// it should only be called in the event of an error
	hs->beingUploaded = false;
}

inline void
DownloadManager::UpdateScoreAfterUploadSuccess(HighScore* hs)
{
	// the purpose of this is to confirm a score is uploaded
	// an uploaded score should not be reuploaded
	hs->AddUploadedServer(serverURL);
	hs->forceuploadedthissession = false;
}

inline bool
DownloadManager::CanUploadScore(HighScore* hs, bool forceReupload)
{
	// the force bool does not let you upload all scores
	// force lets reuploads go through

	// worse than D (means it's a fail but ... you never know)
	if (hs->GetGrade() > Grade_Tier16)
		return false;

	// old
	if (hs->GetWifeVersion() < cur_wife_version)
		return false;

	// need replay
	if (!hs->HasReplayData())
		return false;

	// invalid score
	if (!hs->GetEtternaValid())
		return false;

	// invalider score
	if (hs->GetChordCohesion())
		return false;

	// cannot upload a score twice
	if (!forceReupload && hs->IsUploadedToServer(serverURL))
		return false;

	// shouldnt upload an unranked file
	if (!forceReupload && newlyRankedChartkeys.count(hs->GetChartKey()) == 0)
		return false;

	// no double reuploads
	// this will stop accidentally queueing the same score
	// multiple times in a session
	// (and on purpose)
	// will be set true if upload is successful
	if (hs->forceuploadedthissession)
		return false;

	// no double queues
	if (hs->beingUploaded)
		return false;

	// set true on a force upload
	// will also be set true if upload is successful
	hs->forceuploadedthissession = forceReupload;

	// set this true for all scores that pass through here.
	// if any error occurs with the request, this is unset
	// that will allow a score to pass through again.
	hs->beingUploaded = true;

	return true;
}

void
DownloadManager::UploadScore(HighScore* hs)
{
	// there is no reason to force upload a single score...
	// unless..
	if (CanUploadScore(hs, false))
		UploadSingleScoreRequest(hs);
}

void
DownloadManager::UploadAllPBs(bool forceReupload)
{
	auto scores = SCOREMAN->GetAllPBPtrs();
	std::vector<HighScore*> toUpload;
	for (auto& vec : scores) {
		for (auto& s : vec) {
			if (CanUploadScore(s, forceReupload))
				toUpload.push_back(s);
		}
	}

	// sort all scores by date set
	// as bulk upload happens, the check date is set to the last upload time of the chunk
	std::sort(toUpload.begin(), toUpload.end(), [](HighScore* a, HighScore* b) {
		return a->GetDateTime() < b->GetDateTime();
	});

	if (!toUpload.empty()) {
		Locator::getLogger()->info(
		  "UploadAllPBs: uploading {} scores - {} to {}",
		  toUpload.size(),
		  toUpload.front()->GetDateTime().GetString(),
		  toUpload.back()->GetDateTime().GetString());

		UploadBulkScoresRequest(toUpload);
	} else {
		Locator::getLogger()->info("UploadAllPBs: no scores to upload");
	}
}

void
DownloadManager::UploadPBsForChart(const std::string& ck, bool forceReupload)
{
	std::vector<HighScore*> toUpload;

	auto scores = SCOREMAN->GetAllChartPBPtrs(ck);
	for (auto& vec : scores) {
		for (auto& s : vec) {
			if (CanUploadScore(s, forceReupload))
				toUpload.push_back(s);
		}
	}

	if (!toUpload.empty()) {
		Locator::getLogger()->info(
		  "ForceUploadPBsForChart: uploading {} scores - {} to {}",
		  toUpload.size(),
		  toUpload.front()->GetDateTime().GetString(),
		  toUpload.back()->GetDateTime().GetString());

		UploadBulkScoresRequest(toUpload);
	} else {
		Locator::getLogger()->info("ForceUploadPBsForChart: no scores to upload");
	}
}

void
DownloadManager::UploadPBsForPack(const std::string& pack, bool forceReupload)
{
	std::vector<HighScore*> toUpload;
	auto songs = SONGMAN->GetSongs(pack);
	for (auto so : songs)
		for (auto c : so->GetAllSteps()) {
			auto scores = SCOREMAN->GetAllChartPBPtrs(c->GetChartKey());
			for (auto& v : scores)
				for (auto& s : v)
					if (CanUploadScore(s, forceReupload))
						toUpload.push_back(s);
		}

	if (!toUpload.empty()) {
		Locator::getLogger()->info(
		  "ForceUploadPBsForPack: uploading {} scores - {} to {}",
		  toUpload.size(),
		  toUpload.front()->GetDateTime().GetString(),
		  toUpload.back()->GetDateTime().GetString());

		UploadBulkScoresRequest(toUpload);
	} else {
		Locator::getLogger()->info("ForceUploadPBsForPack: no scores to upload");
	}
}

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

inline std::string
DownloadManager::ExtractFromJSONArray(Poco::JSON::Array::Ptr arr)
{
	std::ostringstream out;
	for (size_t i = 0; i < arr->size(); i++) {
		if (arr->isArray(i)) {
			out << "\n" << i << " : " << ExtractFromJSONArray(arr->getArray(i));
		} else if (arr->isNull(i)) {
			// impossible?
		} else if (arr->isObject(i)) {
			out << "\n"
				<< i << " : " << ExtractFromJSONObject(arr->getObject(i));
		} else {
			// a value?
			try {
				// please
				out << "\n" << i << " : " << arr->getElement<std::string>(i);
			} catch (...) {
				try {
					// please work
					out << "\n" << i << " : " << arr->getElement<int>(i);
				} catch (...) {
					// i beg
					try {
						out << "\n" << i << " : " << arr->getElement<float>(i);
					} catch (...) {
						out << "\n<invalid>";
					}
				}
			}
		}
	}
	return out.str();
}

inline std::string
DownloadManager::ExtractFromJSONObject(Poco::JSON::Object::Ptr obj)
{
	std::ostringstream out;
	auto keys = obj->getNames();
	for (auto& k : keys) {
		if (obj->isArray(k)) {
			// arr
			out << k << " : " << ExtractFromJSONArray(obj->getArray(k));
		} else if (obj->isNull(k)) {
			// this shouldnt really ever happen
		} else if (obj->isObject(k)) {
			// obj
			out << k << " : " << ExtractFromJSONObject(obj->getObject(k));
		} else {
			// a value?
			try {
				// please
				out << "\n" << k << " : " << obj->getValue<std::string>(k);
			} catch (...) {
				try {
					// please work
					out << "\n" << k << " : " << obj->getValue<int>(k);
				} catch (...) {
					// i beg
					try {
						out << "\n" << k << " : " << obj->getValue<float>(k);
					} catch (...) {
						out << "\n<invalid>";
					}
				}
			}
		}
	}
	return out.str();
}

std::string
DownloadManager::ExtractHTTP422Reasons(Poco::JSON::Object::Ptr errors)
{
	return ExtractFromJSONObject(errors);
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
		auto& codes = p->countryCodes;
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
		std::vector<DownloadablePack>& packs = p->downloadablePacks;
		lua_createtable(L, packs.size(), 0);
		for (unsigned i = 0; i < packs.size(); ++i) {
			packs[i].PushSelf(L);
			lua_rawseti(L, -2, i + 1);
		}
		return 1;
	}
	static int GetDownloadingPacks(T* p, lua_State* L)
	{
		std::vector<DownloadablePack>& packs = p->downloadablePacks;
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
		lua_pushstring(L, p->GetSessionUser().c_str());
		return 1;
	}
	static int GetSkillsetRank(T* p, lua_State* L)
	{
		lua_pushnumber(L, p->GetSkillsetRank(Enum::Check<Skillset>(L, 1)));
		return 1;
	}
	static int GetSkillsetRating(T* p, lua_State* L)
	{
		lua_pushnumber(L, p->GetSkillsetRating(Enum::Check<Skillset>(L, 1)));
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
		lua_pushboolean(L, p->IsLoggedIn());
		return 1;
	}
	static int Login(T* p, lua_State* L)
	{
		p->Login(SArg(1), SArg(2));
		return 0;
	}
	static int LoginWithToken(T* p, lua_State* L)
	{
		std::string user = SArg(1);
		std::string token = SArg(2);
		p->LoginWithToken(token, user);
		return 0;
	}
	static int Logout(T* p, lua_State* L)
	{
		p->Logout();
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
		auto onlineScore = p->GetTopSkillsetScore(rank, ss, result);
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
		if (p->chartLeaderboards.count(ck))
			lua_pushnumber(L, p->chartLeaderboards[ck].size());
		else
			lua_pushnumber(L, 0);
		return 1;
	}
	static int GetTopChartScore(T* p, lua_State* L)
	{
		std::string chartkey = SArg(1);
		int rank = IArg(2);
		int index = rank - 1;
		if (index < 0 || !p->chartLeaderboards.count(chartkey) ||
			index >=
			  static_cast<int>(p->chartLeaderboards[chartkey].size())) {
			lua_pushnil(L);
			return 1;
		}
		auto& score = p->chartLeaderboards[chartkey][index];
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
		lua_pushstring(L, p->GetSessionToken().c_str());
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
		auto& leaderboardScores = p->chartLeaderboards[chart];
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
		auto& leaderboardScores = p->chartLeaderboards[ck];
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
		//DLMAN->UploadScoreWithReplayDataFromDisk(
		//  SCOREMAN->GetScoresByKey().at(SArg(1)));
		// DLMAN->UpdateOnlineScoreReplayData(SArg(1));
		return 0;
	}
	static int UploadScoresForChart(T* p, lua_State* L)
	{
		DLMAN->ForceUploadPBsForChart(SArg(1));
		return 0;
	}
	static int UploadScoresForPack(T* p, lua_State* L)
	{
		DLMAN->ForceUploadPBsForPack(SArg(1));
		return 0;
	}
	static int UploadAllScores(T* p, lua_State* L)
	{
		DLMAN->ForceUploadAllPBs();
		return 0;
	}
	static int UploadThisScore(T* p, lua_State* L) {
		auto* hs = Luna<HighScore>::check(L, 1);

		DLMAN->UploadScore(hs);
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
