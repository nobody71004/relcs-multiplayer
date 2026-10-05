// net/server/profiles.h — SQLite-backed persistent player profiles.
// One profile per player name: wallet balance + full weapon inventory, so
// money and weapons survive reconnects. Header keeps sqlite3 out of the ABI.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ProfileWeapon {
	uint8_t  weapon = 0;   // eWeaponType
	uint16_t ammo = 0;
};

struct PlayerProfile {
	int32_t money = 0;
	uint8_t currentWeapon = 0;
	std::vector<ProfileWeapon> weapons;
};

class ProfileStore {
public:
	~ProfileStore();

	bool Open(const char* path);   // creates schema on first use
	void Close();
	bool IsOpen() const { return m_db != nullptr; }

	// false when the name has no row yet (out untouched) or on DB error
	bool Load(const std::string& name, PlayerProfile& out);
	// upsert: replaces the wallet + the whole weapon table for the name
	bool Save(const std::string& name, const PlayerProfile& p);

	const char* LastError() const { return m_err.c_str(); }

private:
	bool Exec(const char* sql);
	bool ExecErr(const char* what, int rc);

	void* m_db = nullptr;      // sqlite3*
	std::string m_err;
};
