// net/server/gamemode.h — server-side gamemode API + built-in modes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Server;

struct SpawnPoint {
	float x, y, z, heading;
};

class GameMode {
public:
	virtual ~GameMode() {}
	virtual const char* Name() const = 0;
	virtual void OnPlayerConnect(Server& server, uint8_t playerId) { (void)server; (void)playerId; }
	virtual void OnPlayerDisconnect(Server& server, uint8_t playerId) { (void)server; (void)playerId; }
	// decide where/how a player spawns; call server.SpawnPlayer(...)
	virtual void OnPlayerSpawnRequest(Server& server, uint8_t playerId) = 0;
	virtual void OnPlayerDeath(Server& server, uint8_t victimId, uint8_t killerId, uint8_t weaponId)
	{ (void)server; (void)victimId; (void)killerId; (void)weaponId; }
	virtual void OnTick(Server& server, float dt) { (void)server; (void)dt; }
	virtual bool OnChat(Server& server, uint8_t playerId, const char* text)
	{ (void)server; (void)playerId; (void)text; return true; }  // false = suppress
	virtual bool OnPlayerCommand(Server& server, uint8_t playerId, const std::string& cmd)
	{ (void)server; (void)playerId; (void)cmd; return false; }  // true = handled
	virtual void OnRoundStart(Server& server) { (void)server; }
	virtual void OnRoundEnd(Server& server, uint8_t winnerId) { (void)server; (void)winnerId; }
	virtual float RespawnDelay() const { return 3.0f; }
	virtual void Reset(Server& server) { (void)server; }
};

// Simple free-roam: just spawn and wander.
class FreeRoamMode : public GameMode {
public:
	const char* Name() const override { return "freeroam"; }
	void OnPlayerSpawnRequest(Server& server, uint8_t playerId) override;
};

// Official PSP LCS mode #1: Liberty City Survivor — deathmatch to kill limit or timer.
class LibertyCitySurvivorMode : public GameMode {
public:
	LibertyCitySurvivorMode();
	const char* Name() const override { return "survivor"; }
	void OnPlayerSpawnRequest(Server& server, uint8_t playerId) override;
	void OnPlayerDeath(Server& server, uint8_t victimId, uint8_t killerId, uint8_t weaponId) override;
	void OnTick(Server& server, float dt) override;
	void OnRoundStart(Server& server) override;
	void OnRoundEnd(Server& server, uint8_t winnerId) override;
	void Reset(Server& server) override;

	int scoreLimit = 20;
	float roundTime = 300.0f; // seconds

private:
	enum class Phase { Warmup, Active, Ended };
	Phase m_phase = Phase::Warmup;
	float m_phaseTimer = 0.0f;
	bool m_roundStarted = false;
};

// Shared spawn point table (Liberty City, tunable).
const std::vector<SpawnPoint>& GetSpawnPoints();
SpawnPoint RandomSpawnPoint();
