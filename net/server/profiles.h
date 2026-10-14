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

// Why a name is or isn't claimable by a given password.
enum class AuthResult {
	OK,            // password matches the stored hash (or name is unclaimed)
	UNCLAIMED,     // name has no password yet — first joiner may claim it
	WRONG_PASSWORD // name is claimed and this password does not match
};

struct PassHash {
	uint8_t salt[16] = {};
	uint8_t hash[32] = {};
};

// A player's saved appearance: the ped character/outfit model plus a fixed set
// of clothing/component variation slots. Mirrors rnet::MsgAppearance but kept
// sqlite-only here so profiles.h stays free of the protocol headers.
constexpr int PROFILE_APPEARANCE_COMPONENTS = 8;
struct ProfileAppearance {
	uint16_t model = 7;   // rnet::APPEARANCE_MODEL_DEFAULT (MI_MALE01)
	uint8_t  components[PROFILE_APPEARANCE_COMPONENTS] = {};
};

struct PlayerProfile {
	int32_t money = 0;
	uint8_t currentWeapon = 0;
	std::vector<ProfileWeapon> weapons;
	// match stats — survive reconnects
	int32_t score = 0;
	uint16_t kills = 0, deaths = 0;
	// last on-foot position (world metres) + heading (degrees); hasPos gates it
	bool hasPos = false;
	float x = 0.0f, y = 0.0f, z = 0.0f, heading = 0.0f;
	// appearance — persisted so a custom look survives a reconnect. hasAppearance
	// stays false for a legacy/first-join row so the server keeps the default.
	bool hasAppearance = false;
	ProfileAppearance appearance;
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

	// name+password auth: an unclaimed name (no credential set) is free to
	// take; once claimed, only the matching password may restore the profile.
	AuthResult Auth(const std::string& name, const std::string& password);
	// set (or change) the credential, claiming the name. PBKDF2-HMAC-SHA256
	// with a fresh per-account salt; the plaintext is never stored.
	bool SetPassword(const std::string& name, const std::string& password);

	const char* LastError() const { return m_err.c_str(); }

private:
	bool Exec(const char* sql);
	bool ExecErr(const char* what, int rc);
	bool ColumnExists(const char* table, const char* column);
	bool EnsureColumn(const char* table, const char* column, const char* decl);
	bool EnsureCredentialColumns();
	bool EnsureStatColumns();
	bool EnsureAppearanceColumns();

	void* m_db = nullptr;      // sqlite3*
	std::string m_err;
};
