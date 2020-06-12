#ifndef SONG_CACHE_INDEX_H
#define SONG_CACHE_INDEX_H

#include "Etterna/FileTypes/IniFile.h"
#include "Etterna/Models/Misc/TimingData.h"
#include "Etterna/Models/Songs/Song.h"
#include "Etterna/Models/StepsAndStyles/Steps.h"
#include "arch/LoadingWindow/LoadingWindow.h"

#include <SQLiteCpp/SQLiteCpp.h>
#include <SQLiteCpp/VariadicBind.h>

class SongDBCacheItem
{
};
class SongCacheIndex
{
	IniFile CacheIndex;
	static RString MangleName(const RString& Name);

	bool OpenDB();
	void ResetDB();
	void DeleteDB();
	void CreateDBTables();
	bool DBEmpty{ true };
	SQLite::Transaction* curTransaction{ nullptr };

  public:
	SQLite::Database* db{ nullptr };
	SongCacheIndex();
	~SongCacheIndex();
	inline pair<RString, int> SongFromStatement(Song* song,
												SQLite::Statement& query);
	void LoadHyperCache(LoadingWindow* ld, map<RString, Song*>& hyperCache);
	void LoadCache(LoadingWindow* ld,
				   vector<pair<pair<RString, unsigned int>, Song*>*>& cache);
	void DeleteSongFromDBByCondition(string& condition);
	void DeleteSongFromDB(Song* songPtr);
	void DeleteSongFromDBByDir(string dir);
	void DeleteSongFromDBByDirHash(unsigned int hash);
	static RString GetCacheFilePath(const RString& sGroup,
									const RString& sPath);
	unsigned GetCacheHash(const RString& path) const;
	bool delay_save_cache;

	int64_t InsertStepsTimingData(const TimingData& timing);
	int64_t InsertSteps(Steps* pSteps, int64_t songID);
	bool LoadSongFromCache(Song* song, string dir);
	bool CacheSong(Song& song, string dir);
	void StartTransaction();
	void FinishTransaction();
};

extern SongCacheIndex*
  SONGINDEX; // global and accessible from anywhere in our program

#endif
