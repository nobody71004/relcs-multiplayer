// net/server/server.h — reLCS dedicated multiplayer server.
#pragma once

#include "rnet_protocol.h"
#include "rnet_validator.h"

#include <enet/enet.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct ServerConfig {
	std::string hostname = "reLCS Server";
	uint16_t port = rnet::DEFAULT_PORT;
	int maxPlayers = 32;
	std::string gamemode = "survivor";     // survivor | freeroam
	bool announce = false;                 // list on master server
	std::string master = "127.0.0.1";      // master server host
	uint16_t masterPort = rnet::DEFAULT_MASTER_PORT;
	bool lanDiscover = true;
	std::string rconPassword = "";
	float streamDistance = rnet::STREAM_DISTANCE;
	int tickRate = rnet::TICK_RATE;
	uint8_t hour = 12, minute = 0, weather = 0;
	int netstatsInterval = 0;		// >0: log bandwidth/rtt stats every N seconds
};

struct ServerPlayer {
	bool active = false;
	uint8_t id = 0;
	ENetPeer* peer = nullptr;
	uint32_t ip = 0;
	uint16_t port = 0;
	std::string name;
	uint16_t skin = 0;
	bool spawned = false;
	bool alive = false;
	rnet::PlayerState state;         // latest client-reported
	float lastStateTime = 0.0;
	double respawnAt = 0.0;          // server clock seconds
	// authoritative vitals
	uint8_t health = 100, armour = 0;
	int score = 0;
	uint16_t kills = 0, deaths = 0;
	bool rconAuthed = false;
	int violations = 0;
	int teleportBurst = 0;            // client-flagged teleports in the current window
	double teleportWindowStart = 0.0;
	// tough validator state (rnet_validator.h)
	rnet::RateLimiter msgRate;        // every packet — flood defence
	rnet::RateLimiter chatRate;       // chat lines — spam defence
	rnet::RateLimiter damageRate;     // damage claims
	int32_t moneySeen = 0;            // last server-accepted wallet
	int32_t moneyGranted = 0;         // wallet growth the server has granted
	int floodStrikes = 0;
	uint64_t bytesInAtLastSample = 0, bytesOutAtLastSample = 0;
};

struct ServerVehicle {
	bool active = false;
	uint16_t id = 0;
	uint16_t modelId = 0;
	rnet::VehicleState state;
	uint8_t driverId = 0xFF;
	uint8_t occupants[8];            // seat -> playerId (0xFF = free)
	float lastStateTime = 0.0;
};

class GameMode;

class Server {
public:
	~Server();

	bool Init(const ServerConfig& cfg);
	void Run();                       // blocks until Quit()
	void Shutdown();

	// console/RCON command entry (any thread)
	void PushConsoleLine(const std::string& line);
	bool HandleCommand(const std::string& line, int respondTo = -1); // -1=console, else playerId for RCON

	// --- gamemode API ---
	ServerPlayer* FindPlayer(uint8_t id);
	ServerVehicle* FindVehicle(uint16_t id);
	int ActivePlayerCount() const;

	void BroadcastChat(const char* text, uint8_t fromId = 0);
	void SendChatTo(uint8_t playerId, const char* text, uint8_t fromId = 0);
	void BroadcastEvent(uint8_t type, uint8_t arg1 = 0, uint8_t arg2 = 0, const char* text = "");
	void SpawnPlayer(uint8_t playerId, float x, float y, float z, float heading,
	                 uint16_t skin, uint8_t health, uint8_t armour);
	void KillPlayer(uint8_t victimId, uint8_t killerId, uint8_t weaponId);
	void TeleportPlayer(uint8_t playerId, float x, float y, float z, float heading);
	void SetPlayerVitals(uint8_t playerId, uint8_t health, uint8_t armour);
	void BroadcastScores();
	void KickPlayer(uint8_t playerId, const char* reason);
	void RestartGameMode();

	double Now() const;               // seconds since start
	const ServerConfig& Config() const { return m_cfg; }
	void SetServerVar(const std::string& key, const std::string& value);

private:
	void PumpNetwork();
	void OnConnect(ENetPeer* peer);
	void OnDisconnect(ENetPeer* peer);
	void OnReceive(ENetPeer* peer, ENetPacket* packet, uint8_t channel);
	void Tick();

	// message handlers
	void HandleHello(ServerPlayer* p, rnet::BitReader& r, ENetPeer* peer, uint8_t flagsProbe);
	void HandlePlayerState(ServerPlayer* p, rnet::BitReader& r);
	void SendGive(uint8_t targetId, uint8_t kind, int32_t amount, uint16_t arg);
	void HandleVehicleState(ServerPlayer* p, rnet::BitReader& r);
	void HandleChat(ServerPlayer* p, rnet::BitReader& r);
	void HandleSpawnReq(ServerPlayer* p);
	void HandleDamage(ServerPlayer* p, rnet::BitReader& r);
	void HandleVehEnter(ServerPlayer* p, rnet::BitReader& r);
	void HandleVehExit(ServerPlayer* p, rnet::BitReader& r);
	void ClearPlayerVehicles(ServerPlayer* p, bool broadcast); // drop all seats held by p
	void HandleRcon(ServerPlayer* p, rnet::BitReader& r, bool auth);

	// helpers
	void SendTo(ENetPeer* peer, const rnet::BitWriter& w, uint8_t channel, bool reliable);
	void Broadcast(const rnet::BitWriter& w, uint8_t channel, bool reliable, uint8_t exceptId = 0);
	void Log(const char* fmt, ...);
	void UpdateMasterAnnounce();
	void InitDiscovery();
	void PumpDiscovery();

	ServerConfig m_cfg;
	ENetHost* m_host = nullptr;
	ENetHost* m_masterClient = nullptr;
	ENetPeer* m_masterPeer = nullptr;
	double m_masterLastAnnounce = 0.0;

	SOCKET m_discSock = INVALID_SOCKET;

	ServerPlayer m_players[rnet::MAX_PLAYERS];   // index = id-1
	ServerVehicle m_vehicles[rnet::MAX_VEHICLES];
	std::vector<uint32_t> m_bannedIps;

	GameMode* m_gamemode = nullptr;

	std::mutex m_cmdMutex;
	std::deque<std::string> m_cmdQueue;

	std::atomic<bool> m_running{false};
	double m_startTime = 0.0;
	double m_lastTick = 0.0;
	double m_lastNetStats = 0.0;
	double m_lastWorldUpdate = 0.0;
	uint64_t m_totalBytesIn = 0, m_totalBytesOut = 0;

	friend class GameMode;
};
