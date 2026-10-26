// net/server/profiles.cpp — SQLite-backed persistent player profiles.
#include "profiles.h"

#include "passhash.h"
#include "sqlite3.h"

#include <ctime>
#include <cstdio>
#include <fstream>
#include <random>

namespace {

const char* kSchema =
	"PRAGMA journal_mode=WAL;"
	"CREATE TABLE IF NOT EXISTS profiles("
	"  name TEXT PRIMARY KEY COLLATE NOCASE,"
	"  money INTEGER NOT NULL DEFAULT 0,"
	"  current_weapon INTEGER NOT NULL DEFAULT 0,"
	"  updated INTEGER NOT NULL DEFAULT 0,"
	"  pass_hash BLOB,"
	"  pass_salt BLOB"
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
	if(!Exec(kSchema) || !EnsureCredentialColumns() || !EnsureStatColumns()){
		Close();
		return false;
	}
	return true;
}

bool ProfileStore::ColumnExists(const char* table, const char* column)
{
	if(!m_db) return false;
	sqlite3_stmt* st = nullptr;
	std::string sql = std::string("PRAGMA table_info(") + table + ");";
	if(sqlite3_prepare_v2((sqlite3*)m_db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
		return false;
	bool found = false;
	while(sqlite3_step(st) == SQLITE_ROW){
		const char* name = (const char*)sqlite3_column_text(st, 1);
		if(name && std::strcmp(name, column) == 0){ found = true; break; }
	}
	sqlite3_finalize(st);
	return found;
}

bool ProfileStore::EnsureColumn(const char* table, const char* column, const char* decl)
{
	if(ColumnExists(table, column)) return true;
	return Exec((std::string("ALTER TABLE ") + table + " ADD COLUMN " + column + " " + decl + ";").c_str());
}

// Older databases were created without the credential columns; add them so a
// pre-password profile keeps its wallet and can be claimed later (legacy rows
// have NULL hash/salt and read as "unclaimed").
bool ProfileStore::EnsureCredentialColumns()
{
	return EnsureColumn("profiles", "pass_hash", "BLOB") &&
	       EnsureColumn("profiles", "pass_salt", "BLOB");
}

// Match stats + last position. Legacy rows get 0 / no position.
bool ProfileStore::EnsureStatColumns()
{
	return EnsureColumn("profiles", "score", "INTEGER NOT NULL DEFAULT 0") &&
	       EnsureColumn("profiles", "kills", "INTEGER NOT NULL DEFAULT 0") &&
	       EnsureColumn("profiles", "deaths", "INTEGER NOT NULL DEFAULT 0") &&
	       EnsureColumn("profiles", "has_pos", "INTEGER NOT NULL DEFAULT 0") &&
	       EnsureColumn("profiles", "pos_x", "REAL NOT NULL DEFAULT 0") &&
	       EnsureColumn("profiles", "pos_y", "REAL NOT NULL DEFAULT 0") &&
	       EnsureColumn("profiles", "pos_z", "REAL NOT NULL DEFAULT 0") &&
	       EnsureColumn("profiles", "heading", "REAL NOT NULL DEFAULT 0") &&
	       EnsureColumn("profiles", "skin", "INTEGER NOT NULL DEFAULT 0") &&
	       EnsureColumn("profiles", "skin_comp", "BLOB");
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
	   "SELECT money, current_weapon, score, kills, deaths, has_pos, pos_x, pos_y, pos_z, heading, "
	   "skin, skin_comp "
	   "FROM profiles WHERE name = ?1;", -1, &st, nullptr)))
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
	p.score = (int32_t)sqlite3_column_int(st, 2);
	p.kills = (uint16_t)sqlite3_column_int(st, 3);
	p.deaths = (uint16_t)sqlite3_column_int(st, 4);
	p.hasPos = sqlite3_column_int(st, 5) != 0;
	p.x = (float)sqlite3_column_double(st, 6);
	p.y = (float)sqlite3_column_double(st, 7);
	p.z = (float)sqlite3_column_double(st, 8);
	p.heading = (float)sqlite3_column_double(st, 9);
	p.skin = (uint16_t)sqlite3_column_int(st, 10);
	{
		// component slots travel as a raw 8-byte blob (NULL for legacy rows)
		const void* blob = sqlite3_column_blob(st, 11);
		int n = sqlite3_column_bytes(st, 11);
		if(blob && n > 0){
			int c = n < (int)sizeof p.skinComponents ? n : (int)sizeof p.skinComponents;
			memcpy(p.skinComponents, blob, (size_t)c);
		}
	}
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
	   "INSERT INTO profiles(name, money, current_weapon, updated, score, kills, deaths, "
	   "has_pos, pos_x, pos_y, pos_z, heading, skin, skin_comp) "
	   "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14) "
	   "ON CONFLICT(name) DO UPDATE SET money=?2, current_weapon=?3, updated=?4, "
	   "score=?5, kills=?6, deaths=?7, has_pos=?8, pos_x=?9, pos_y=?10, pos_z=?11, heading=?12, skin=?13, skin_comp=?14;",
	   -1, &st, nullptr));
	if(ok){
		sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(st, 2, p.money);
		sqlite3_bind_int(st, 3, p.currentWeapon);
		sqlite3_bind_int64(st, 4, (sqlite3_int64)time(nullptr));
		sqlite3_bind_int(st, 5, p.score);
		sqlite3_bind_int(st, 6, p.kills);
		sqlite3_bind_int(st, 7, p.deaths);
		sqlite3_bind_int(st, 8, p.hasPos ? 1 : 0);
		sqlite3_bind_double(st, 9, p.x);
		sqlite3_bind_double(st, 10, p.y);
		sqlite3_bind_double(st, 11, p.z);
		sqlite3_bind_double(st, 12, p.heading);
		sqlite3_bind_int(st, 13, p.skin);
		sqlite3_bind_blob(st, 14, p.skinComponents, (int)sizeof p.skinComponents, SQLITE_TRANSIENT);
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

bool ProfileStore::SetPassword(const std::string& name, const std::string& password)
{
	if(!m_db) return false;
	if((int)password.size() < passhash::MIN_PASSWORD_LEN ||
	   (int)password.size() > passhash::MAX_PASSWORD_LEN){
		m_err = "password length out of range";
		return false;
	}

	uint8_t salt[passhash::SALT_LEN];
	uint8_t key[passhash::KEY_LEN];
	{
		std::ifstream urandom("/dev/urandom", std::ios::binary);
		if(urandom){
			urandom.read((char*)salt, sizeof salt);
		}else{
			std::random_device rd;
			for(size_t i = 0; i < sizeof salt; i++) salt[i] = (uint8_t)rd();
		}
	}
	passhash::DeriveKey(password.c_str(), salt, key);

	sqlite3_stmt* st = nullptr;
	bool ok = ExecErr("prepare setpass", sqlite3_prepare_v2((sqlite3*)m_db,
	   "INSERT INTO profiles(name, money, current_weapon, updated, pass_hash, pass_salt) "
	   "VALUES(?1, 0, 0, ?2, ?3, ?4) "
	   "ON CONFLICT(name) DO UPDATE SET pass_hash=?3, pass_salt=?4, updated=?2;",
	   -1, &st, nullptr));
	if(ok){
		sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(st, 2, (sqlite3_int64)time(nullptr));
		sqlite3_bind_blob(st, 3, key, (int)sizeof key, SQLITE_TRANSIENT);
		sqlite3_bind_blob(st, 4, salt, (int)sizeof salt, SQLITE_TRANSIENT);
		ok = ExecErr("set password", sqlite3_step(st) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR);
	}
	sqlite3_finalize(st);
	return ok;
}

AuthResult ProfileStore::Auth(const std::string& name, const std::string& password)
{
	if(!m_db) return AuthResult::UNCLAIMED;   // no persistent profiles: free join

	const void* hash = nullptr;
	const void* salt = nullptr;
	int hashLen = 0, saltLen = 0;
	sqlite3_stmt* st = nullptr;
	if(!ExecErr("prepare auth", sqlite3_prepare_v2((sqlite3*)m_db,
	   "SELECT pass_hash, pass_salt FROM profiles WHERE name = ?1;", -1, &st, nullptr)))
		return AuthResult::UNCLAIMED;
	sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
	int rc = sqlite3_step(st);
	if(rc != SQLITE_ROW){
		sqlite3_finalize(st);
		return AuthResult::UNCLAIMED;          // name never seen: free to claim
	}
	hash = sqlite3_column_blob(st, 0);
	hashLen = sqlite3_column_bytes(st, 0);
	salt = sqlite3_column_blob(st, 1);
	saltLen = sqlite3_column_bytes(st, 1);

	// legacy row from before credentials existed: unclaimed, keep the profile
	if(!hash || hashLen != (int)passhash::KEY_LEN ||
	   !salt || saltLen != (int)passhash::SALT_LEN){
		sqlite3_finalize(st);
		return AuthResult::UNCLAIMED;
	}

	uint8_t saltCopy[passhash::SALT_LEN];
	uint8_t hashCopy[passhash::KEY_LEN];
	std::memcpy(saltCopy, salt, sizeof saltCopy);
	std::memcpy(hashCopy, hash, sizeof hashCopy);
	sqlite3_finalize(st);

	uint8_t key[passhash::KEY_LEN];
	passhash::DeriveKey(password.c_str(), saltCopy, key);
	bool match = passhash::SecureEqual(key, hashCopy, sizeof key);
	std::memset(key, 0, sizeof key);
	return match ? AuthResult::OK : AuthResult::WRONG_PASSWORD;
}
