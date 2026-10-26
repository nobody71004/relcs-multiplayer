// net/server/server.h — reLCS dedicated multiplayer server.
#pragma once

#include "anticheat.h"
#include "compat.h"
#include "profiles.h"
#include "resource.h"
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
	std::string publicIp;                  // WAN address to list (port-forwarded LAN box); empty = LAN IP
	uint16_t masterPort = rnet::DEFAULT_MASTER_PORT;
	bool lanDiscover = true;
	std::string rconPassword = "";
	float streamDistance = rnet::STREAM_DISTANCE;
	int tickRate = rnet::TICK_RATE;
	uint8_t hour = 12, minute = 0, weather = 0;
	int netstatsInterval = 0;		// >0: log bandwidth/rtt stats every N seconds
	std::string profilesDb = "profiles.db";  // SQLite player profiles ("" disables)
	std::string resourcesDir = "resources";  // mod/resource packs ("" disables)
	std::string vehiclesFile = "vehicles.cfg"; // static vehicle spawn list ("" disables)
	std::string mapName = "Liberty City";    // shown in the launcher's server tooltip
	ac::Config ac;                           // EOS Anti-Cheat session (server.cfg eac_*)
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
	double lastStateTime = 0.0;
	double respawnAt = 0.0;          // server clock seconds
	// authoritative vitals
	uint8_t health = 100, armour = 0;
	int score = 0;
	uint16_t kills = 0, deaths = 0;
	bool rconAuthed = false;
	// EOS Anti-Cheat: 0 = no AC session requested, 1 = ID token being verified,
	// 2 = registered with EOS_AntiCheatServer
	uint8_t acState = 0;
	// set from an EOS callback and acted on in Tick (never re-enter the SDK)
	bool acKick = false;
	std::string acKickReason;
	// Now()-based deadline for "protected clients only"; 0 = not enforced
	double acDeadline = 0.0;
	int violations = 0;
	int teleportBurst = 0;            // client-flagged teleports in the current window
	double teleportWindowStart = 0.0;
	// tough validator state (rnet_validator.h)
	rnet::RateLimiter msgRate;        // every packet — flood defence
	rnet::RateLimiter chatRate;       // chat lines — spam defence
	rnet::RateLimiter voiceRate;      // voice frames (~50/s while talking), own budget
	rnet::RateLimiter damageRate;     // damage claims
	int32_t moneySeen = 0;            // last server-accepted wallet
	int32_t moneyGranted = 0;         // wallet growth the server has granted
	// persistent profile (SQLite): canonical wallet + weapon inventory
	rnet::MsgInventory inv;           // last known inventory (DB at join <- client reports)
	bool invSet = false;
	double invTime = 0;               // last accepted client inventory report
	PlayerProfile savedProfile;       // loaded at join, kept for restore
	bool hasProfile = false;          // a DB row existed for this name
	bool profileStranger = false;     // name owned by another account — never save through
	bool profileSent = false;         // restore already sent this session
	bool moneyInit = false;           // wallet baseline established
	int32_t savedMoney = 0;           // last wallet value written to the DB
	bool profileDirty = false;        // needs a save
	bool resumePos = false;           // first spawn uses the saved last position
	int floodStrikes = 0;
	uint64_t bytesInAtLastSample = 0, bytesOutAtLastSample = 0;
};

struct ServerVehicle {
	bool active = false;
	bool permanent = false;          // static spawn: kept in the world while the server runs
	uint16_t id = 0;
	uint16_t modelId = 0;
	rnet::VehicleState state;
	uint8_t driverId = 0xFF;
	uint8_t ownerId = 0xFF;            // player whose enter allocated it (0xFF = static/admin)
	uint8_t occupants[8];            // seat -> playerId (0xFF = free)
	int teleportDrops = 0;           // consecutive driver states dropped as teleports
	double lastStateTime = 0.0;
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
	void BroadcastWorld(void);   // MSG_WORLD clock/weather sync to everyone
	void BroadcastResList(void); // MSG_RES_LIST mod/resource inventory to everyone
	void SendResList(ENetPeer* peer);
	void SpawnPlayer(uint8_t playerId, float x, float y, float z, float heading,
	                 uint16_t skin, uint8_t health, uint8_t armour);
	void KillPlayer(uint8_t victimId, uint8_t killerId, uint8_t weaponId);
	void TeleportPlayer(uint8_t playerId, float x, float y, float z, float heading);
	ServerVehicle* SpawnVehicle(uint16_t modelId, float x, float y, float z, float heading, bool permanent, uint8_t ownerId = 0xFF);
	void RemoveVehicle(uint16_t vehicleId);
	void SetPlayerVitals(uint8_t playerId, uint8_t health, uint8_t armour);
	void BroadcastScores();
	void BroadcastPlayerPing();   // MSG_PLAYER_PING: ping/score rows for the scoreboard
	void KickPlayer(uint8_t playerId, const char* reason);
	void RestartGameMode();
	void SaveProfile(ServerPlayer* p);      // write wallet + inventory to the DB
	void FlushProfiles();                   // save all dirty profiles

	double Now() const;               // seconds since start
	const ServerConfig& Config() const { return m_cfg; }
	void SetServerVar(const std::string& key, const std::string& value);
	ResourceManager& Resources() { return m_resources; }

private:
	void PumpNetwork();
	void SendRefusal(ENetPeer* peer, uint8_t code, const char* reason); // MSG_DISCONNECT + JoinRefused code
	void OnConnect(ENetPeer* peer);
	void HandleInventory(ServerPlayer* p, rnet::BitReader& r);
	void HandleAppearance(ServerPlayer* p, rnet::BitReader& r);
	void SendInventory(ServerPlayer* p);    // restore the persisted profile
	void OnDisconnect(ENetPeer* peer);
	void OnReceive(ENetPeer* peer, ENetPacket* packet, uint8_t channel);
	void Tick();

	// message handlers
	void HandleHello(ServerPlayer* p, rnet::BitReader& r, ENetPeer* peer, uint8_t flagsProbe);
	void HandlePlayerState(ServerPlayer* p, rnet::BitReader& r);
	void SendGive(uint8_t targetId, uint8_t kind, int32_t amount, uint16_t arg);
	void HandleVehicleState(ServerPlayer* p, rnet::BitReader& r);
	void HandleChat(ServerPlayer* p, rnet::BitReader& r);
	void HandleVoice(ServerPlayer* p, rnet::BitReader& r);
	void HandleSpawnReq(ServerPlayer* p);
	void HandleDamage(ServerPlayer* p, rnet::BitReader& r);
	void HandleVehEnter(ServerPlayer* p, rnet::BitReader& r);
	void HandleVehExit(ServerPlayer* p, rnet::BitReader& r);
	void ClearPlayerVehicles(ServerPlayer* p, bool broadcast); // drop all seats held by p
	void HandleRcon(ServerPlayer* p, rnet::BitReader& r, bool auth);
	void HandleAcHello(ServerPlayer* p, rnet::BitReader& r);
	void HandleAcPayload(ServerPlayer* p, rnet::BitReader& r);
	void LoadVehicleSpawns(const std::string& path); // vehicles.cfg -> permanent vehicles

	// --- EOS Anti-Cheat bridge ---------------------------------------------
	// (EOS calls these from inside EOS_Platform_Tick, i.e. on the server thread)
	void InitAntiCheat();
	void PumpAntiCheat();
	// true when the protected-clients-only policy is actually in force
	bool AcEnforced() const;
	void DropAcDeadlines();
	void OnAcMessageToClient(void* handle, const void* data, uint32_t len);
	void OnAcActionRequired(void* handle, int action, const char* reason);
	void OnAcRegisterResult(void* handle, int status, const std::string& detail);
	ServerPlayer* PlayerForHandle(void* handle);
	void SendAcStatus(ServerPlayer* p, uint8_t status, const std::string& detail);

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
	ac::Server m_ac;
	int m_acRegistered = 0;      // clients that completed registration
	int m_acRejected = 0;        // EOS_AntiCheatServer_RegisterClient refusals
	bool m_acSuspend = false;    // enforcement off: EOS refuses this server's registrations
	ProfileStore m_profiles;
	ResourceManager m_resources;
	double m_lastProfileFlush = -100.0;

	std::mutex m_cmdMutex;
	std::deque<std::string> m_cmdQueue;

	std::atomic<bool> m_running{false};
	double m_startTime = 0.0;
	double m_lastTick = 0.0;
	double m_lastNetStats = 0.0;
	double m_lastWorldUpdate = 0.0;
	double m_lastPingBroadcast = 0.0;
	double m_lastReconcile = 0.0;     // 1 Hz out-of-range player + vehicle state heartbeat
	uint64_t m_totalBytesIn = 0, m_totalBytesOut = 0;

	friend class GameMode;
};
