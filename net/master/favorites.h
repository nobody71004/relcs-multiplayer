// net/master/favorites.h — server-side favorites persistence (SQLite).
//
// The launcher's favorites used to be a local text file: reinstall the launcher
// or play from a second machine and the list was gone. The master now owns the
// list, keyed by username, so it follows the player everywhere.
#pragma once

#include <string>
#include <vector>

struct FavEntry {
	std::string address;   // "host:port"
	std::string name;      // last known hostname
};

class FavStore {
public:
	bool Open(const std::string& path);   // "" = in-memory (no persistence)
	void Close();
	bool Ready() const { return m_db != nullptr; }
	const char* LastError() const { return m_err.c_str(); }

	// all favorites of one username, in insertion order
	bool List(const std::string& nick, std::vector<FavEntry>& out);

	// add or update (same address = keep the original position, refresh the name)
	bool Add(const std::string& nick, const std::string& address, const std::string& name, std::string& err);

	// remove one address
	bool Del(const std::string& nick, const std::string& address, std::string& err);

	int Count(const std::string& nick);
	std::vector<std::string> Nicks();   // console: who has favorites

	// validation shared with the launcher/master protocol
	static bool ValidNick(const std::string& nick);
	static bool ValidAddress(const std::string& address);

private:
	void* m_db = nullptr;    // sqlite3*
	std::string m_err;
	std::string m_path;
};
