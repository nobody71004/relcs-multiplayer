// net/master/favorites.cpp — SQLite-backed favorites store.
#include "favorites.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <ctime>

static const int MAX_FAV_PER_NICK = 64;

bool FavStore::ValidNick(const std::string& nick)
{
	if(nick.size() < 3 || nick.size() > 20) return false;
	for(char c : nick)
		if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		     (c >= '0' && c <= '9') || c == '_'))
			return false;
	return true;
}

// "host:port" — host may be an IPv4 literal or a hostname, port 1..65535
bool FavStore::ValidAddress(const std::string& address)
{
	if(address.empty() || address.size() >= 64) return false;
	for(char c : address){
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		          (c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-' || c == '_';
		if(!ok) return false;
	}
	size_t colon = address.rfind(':');
	if(colon == std::string::npos) return false;
	std::string host = address.substr(0, colon);
	int port = atoi(address.c_str() + colon + 1);
	return !host.empty() && port > 0 && port <= 65535;
}

bool FavStore::Open(const std::string& path)
{
	Close();
	m_path = path;
	int rc = sqlite3_open(path.empty() ? ":memory:" : path.c_str(), (sqlite3**)&m_db);
	if(rc != SQLITE_OK){
		m_err = m_db ? sqlite3_errmsg((sqlite3*)m_db) : "cannot open database";
		if(m_db){ sqlite3_close((sqlite3*)m_db); m_db = nullptr; }
		return false;
	}
	const char* schema =
		"CREATE TABLE IF NOT EXISTS favorites ("
		"  nick     TEXT NOT NULL,"
		"  address  TEXT NOT NULL,"
		"  name     TEXT NOT NULL DEFAULT '',"
		"  added_at INTEGER NOT NULL DEFAULT 0,"
		"  PRIMARY KEY (nick, address));"
		"CREATE INDEX IF NOT EXISTS favorites_nick ON favorites(nick);";
	char* errmsg = nullptr;
	rc = sqlite3_exec((sqlite3*)m_db, schema, nullptr, nullptr, &errmsg);
	if(rc != SQLITE_OK){
		m_err = errmsg ? errmsg : "schema failed";
		if(errmsg) sqlite3_free(errmsg);
		sqlite3_close((sqlite3*)m_db);
		m_db = nullptr;
		return false;
	}
	return true;
}

void FavStore::Close()
{
	if(m_db){
		sqlite3_close((sqlite3*)m_db);
		m_db = nullptr;
	}
}

int FavStore::Count(const std::string& nick)
{
	if(!m_db) return 0;
	sqlite3_stmt* st = nullptr;
	if(sqlite3_prepare_v2((sqlite3*)m_db, "SELECT COUNT(*) FROM favorites WHERE nick=?1", -1, &st, nullptr) != SQLITE_OK)
		return 0;
	sqlite3_bind_text(st, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
	int n = 0;
	if(sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	return n;
}

bool FavStore::List(const std::string& nick, std::vector<FavEntry>& out)
{
	out.clear();
	if(!m_db){ m_err = "favorites store not open"; return false; }
	if(!ValidNick(nick)){ m_err = "invalid username"; return false; }
	sqlite3_stmt* st = nullptr;
	if(sqlite3_prepare_v2((sqlite3*)m_db,
	      "SELECT address, name FROM favorites WHERE nick=?1 ORDER BY added_at, address",
	      -1, &st, nullptr) != SQLITE_OK){
		m_err = sqlite3_errmsg((sqlite3*)m_db);
		return false;
	}
	sqlite3_bind_text(st, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
	while(sqlite3_step(st) == SQLITE_ROW){
		FavEntry e;
		const char* a = (const char*)sqlite3_column_text(st, 0);
		const char* n = (const char*)sqlite3_column_text(st, 1);
		e.address = a ? a : "";
		e.name = n ? n : "";
		out.push_back(e);
	}
	sqlite3_finalize(st);
	return true;
}

bool FavStore::Add(const std::string& nick, const std::string& address, const std::string& name, std::string& err)
{
	if(!m_db){ err = "favorites store not open"; return false; }
	if(!ValidNick(nick)){ err = "invalid username"; return false; }
	if(!ValidAddress(address)){ err = "invalid address (expected host:port)"; return false; }
	int have = Count(nick);
	sqlite3_stmt* st = nullptr;
	// already there? update the display name, keep its position in the list
	if(sqlite3_prepare_v2((sqlite3*)m_db,
	      "SELECT COUNT(*) FROM favorites WHERE nick=?1 AND address=?2",
	      -1, &st, nullptr) != SQLITE_OK){
		err = sqlite3_errmsg((sqlite3*)m_db);
		return false;
	}
	sqlite3_bind_text(st, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, address.c_str(), -1, SQLITE_TRANSIENT);
	bool exists = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0;
	sqlite3_finalize(st);

	if(!exists && have >= MAX_FAV_PER_NICK){
		char buf[96];
		snprintf(buf, sizeof buf, "favorite limit reached (%d)", MAX_FAV_PER_NICK);
		err = buf;
		return false;
	}

	if(exists){
		if(sqlite3_prepare_v2((sqlite3*)m_db,
		      "UPDATE favorites SET name=?3 WHERE nick=?1 AND address=?2",
		      -1, &st, nullptr) != SQLITE_OK){
			err = sqlite3_errmsg((sqlite3*)m_db);
			return false;
		}
		sqlite3_bind_text(st, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, address.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
	}else{
		if(sqlite3_prepare_v2((sqlite3*)m_db,
		      "INSERT INTO favorites (nick, address, name, added_at) VALUES (?1, ?2, ?3, ?4)",
		      -1, &st, nullptr) != SQLITE_OK){
			err = sqlite3_errmsg((sqlite3*)m_db);
			return false;
		}
		sqlite3_bind_text(st, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 2, address.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(st, 4, (sqlite3_int64)time(nullptr));
	}
	if(sqlite3_step(st) != SQLITE_DONE){
		err = sqlite3_errmsg((sqlite3*)m_db);
		sqlite3_finalize(st);
		return false;
	}
	sqlite3_finalize(st);
	return true;
}

bool FavStore::Del(const std::string& nick, const std::string& address, std::string& err)
{
	if(!m_db){ err = "favorites store not open"; return false; }
	if(!ValidNick(nick)){ err = "invalid username"; return false; }
	sqlite3_stmt* st = nullptr;
	if(sqlite3_prepare_v2((sqlite3*)m_db,
	      "DELETE FROM favorites WHERE nick=?1 AND address=?2", -1, &st, nullptr) != SQLITE_OK){
		err = sqlite3_errmsg((sqlite3*)m_db);
		return false;
	}
	sqlite3_bind_text(st, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(st, 2, address.c_str(), -1, SQLITE_TRANSIENT);
	bool ok = sqlite3_step(st) == SQLITE_DONE;
	if(!ok) err = sqlite3_errmsg((sqlite3*)m_db);
	sqlite3_finalize(st);
	return ok;
}

std::vector<std::string> FavStore::Nicks()
{
	std::vector<std::string> out;
	if(!m_db) return out;
	sqlite3_stmt* st = nullptr;
	if(sqlite3_prepare_v2((sqlite3*)m_db,
	      "SELECT nick, COUNT(*) FROM favorites GROUP BY nick ORDER BY nick",
	      -1, &st, nullptr) != SQLITE_OK)
		return out;
	while(sqlite3_step(st) == SQLITE_ROW){
		const char* n = (const char*)sqlite3_column_text(st, 0);
		int c = sqlite3_column_int(st, 1);
		char buf[64];
		snprintf(buf, sizeof buf, "%s (%d)", n ? n : "?", c);
		out.push_back(buf);
	}
	sqlite3_finalize(st);
	return out;
}
