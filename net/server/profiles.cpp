// net/server/profiles.cpp — SQLite-backed persistent player profiles.
#include "profiles.h"

#include "sqlite3.h"

#include <ctime>

namespace {

const char* kSchema =
	"PRAGMA journal_mode=WAL;"
	"CREATE TABLE IF NOT EXISTS profiles("
	"  name TEXT PRIMARY KEY COLLATE NOCASE,"
	"  money INTEGER NOT NULL DEFAULT 0,"
	"  current_weapon INTEGER NOT NULL DEFAULT 0,"
	"  updated INTEGER NOT NULL DEFAULT 0"
	");"
	"CREATE TABLE IF NOT EXISTS weapons("
	"  name TEXT NOT NULL COLLATE NOCASE,"
	"  weapon INTEGER NOT NULL,"
	"  ammo INTEGER NOT NULL,"
	"  PRIMARY KEY(name, weapon)"
	");";

} // namespace

ProfileStore::~ProfileStore()
{
	Close();
}

bool ProfileStore::ExecErr(const char* what, int rc)
{
	if(rc == SQLITE_OK) return true;
	m_err = std::string(what) + ": " + (m_db ? sqlite3_errmsg((sqlite3*)m_db) : "no db");
	return false;
}

bool ProfileStore::Exec(const char* sql)
{
	char* err = nullptr;
	int rc = sqlite3_exec((sqlite3*)m_db, sql, nullptr, nullptr, &err);
	if(rc != SQLITE_OK){
		m_err = err ? err : "exec failed";
		sqlite3_free(err);
		return false;
	}
	return true;
}

bool ProfileStore::Open(const char* path)
{
	Close();
	sqlite3* db = nullptr;
	int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
	if(rc != SQLITE_OK){
		m_err = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
		if(db) sqlite3_close(db);
		return false;
	}
	m_db = db;
	sqlite3_busy_timeout(db, 3000);
	if(!Exec(kSchema)){
		Close();
		return false;
	}
	return true;
}

void ProfileStore::Close()
{
	if(m_db){
		sqlite3_close((sqlite3*)m_db);
		m_db = nullptr;
	}
}

bool ProfileStore::Load(const std::string& name, PlayerProfile& out)
{
	if(!m_db) return false;

	PlayerProfile p;
	sqlite3_stmt* st = nullptr;
	if(!ExecErr("prepare profiles", sqlite3_prepare_v2((sqlite3*)m_db,
	   "SELECT money, current_weapon FROM profiles WHERE name = ?1;", -1, &st, nullptr)))
		return false;
	sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(st);
	if(rc != SQLITE_ROW){
		sqlite3_finalize(st);
		if(rc == SQLITE_DONE){ m_err = "no profile"; return false; }
		m_err = sqlite3_errmsg((sqlite3*)m_db);
		return false;
	}
	p.money = (int32_t)sqlite3_column_int(st, 0);
	p.currentWeapon = (uint8_t)sqlite3_column_int(st, 1);
	sqlite3_finalize(st);

	if(!ExecErr("prepare weapons", sqlite3_prepare_v2((sqlite3*)m_db,
	   "SELECT weapon, ammo FROM weapons WHERE name = ?1 ORDER BY weapon;", -1, &st, nullptr)))
		return false;
	sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
	while(sqlite3_step(st) == SQLITE_ROW){
		ProfileWeapon w;
		w.weapon = (uint8_t)sqlite3_column_int(st, 0);
		w.ammo = (uint16_t)sqlite3_column_int(st, 1);
		p.weapons.push_back(w);
	}
	sqlite3_finalize(st);

	out = p;
	return true;
}

bool ProfileStore::Save(const std::string& name, const PlayerProfile& p)
{
	if(!m_db) return false;

	if(!Exec("BEGIN IMMEDIATE")) return false;

	sqlite3_stmt* st = nullptr;
	bool ok = ExecErr("prepare upsert", sqlite3_prepare_v2((sqlite3*)m_db,
	   "INSERT INTO profiles(name, money, current_weapon, updated) VALUES(?1, ?2, ?3, ?4) "
	   "ON CONFLICT(name) DO UPDATE SET money=?2, current_weapon=?3, updated=?4;",
	   -1, &st, nullptr));
	if(ok){
		sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 2, p.money);
		sqlite3_bind_int(st, 3, p.currentWeapon);
		sqlite3_bind_int64(st, 4, (sqlite3_int64)time(nullptr));
		ok = ExecErr("upsert profile", sqlite3_step(st) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR);
	}
	sqlite3_finalize(st);
	if(!ok){ Exec("ROLLBACK"); return false; }

	// replace the weapon table wholesale
	ok = ExecErr("prepare wipe", sqlite3_prepare_v2((sqlite3*)m_db,
	   "DELETE FROM weapons WHERE name = ?1;", -1, &st, nullptr));
	if(ok){
		sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
		ok = ExecErr("wipe weapons", sqlite3_step(st) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR);
	}
	sqlite3_finalize(st);
	if(!ok){ Exec("ROLLBACK"); return false; }

	ok = ExecErr("prepare weapon", sqlite3_prepare_v2((sqlite3*)m_db,
	   "INSERT INTO weapons(name, weapon, ammo) VALUES(?1, ?2, ?3);", -1, &st, nullptr));
	for(size_t i = 0; ok && i < p.weapons.size(); i++){
		sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 2, p.weapons[i].weapon);
		sqlite3_bind_int(st, 3, p.weapons[i].ammo);
		if(sqlite3_step(st) != SQLITE_DONE){ ok = ExecErr("insert weapon", SQLITE_ERROR); break; }
		sqlite3_reset(st);
	}
	sqlite3_finalize(st);
	if(!ok){ Exec("ROLLBACK"); return false; }

	return Exec("COMMIT");
}
