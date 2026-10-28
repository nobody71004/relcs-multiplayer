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
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

struct ServerConfig {
	std::string hostname = "reLCS Server";
	uint16_t port = rnet::DEFAULT_PORT;
	int maxPlayers = 32;                   // clamped to rnet::MAX_SERVER_PLAYERS (2000)
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
	std::string logFile;                     // "" = stdout only; else tee every line here
	bool verboseJoin = true;                 // log one line per join/leave/refusal
	std::string profilesDb = "profiles.db";  // SQLite player profiles ("" disables)
	std::string resourcesDir = "resources";  // mod/resource packs ("" disables)
	std::string vehiclesFile = "vehicles.cfg"; // static vehicle spawn list ("" disables)
	std::string mapName = "Liberty City";    // shown in the launcher's server tooltip
	ac::Config ac;                           // EOS Anti-Cheat session (server.cfg eac_*)
};

struct ServerPlayer {
	bool active = false;
	rnet::PlayerId id = 0;
	ENetPeer* peer = nullptr;
	uint32_t ip = 0;
	uint16_t port = 0;
	std::string name;
	uint16_t skin = 0;
	bool spawned = false;
	bool alive = false;
	rnet::PlayerState state;         // latest client-reported
	double lastStateTime = 0.0;
	double joinTime = 0.0;           // Now() at HELLO (session-length logging)
	int statesReceived = 0;          // MSG_PLAYER_STATE packets accepted
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
	rnet::PlayerId driverId = rnet::PLAYER_ID_NONE;
	rnet::PlayerId ownerId = rnet::PLAYER_ID_NONE; // player whose enter allocated it
	rnet::PlayerId occupants[8];     // seat -> playerId (PLAYER_ID_NONE = free)
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
	ServerPlayer* FindPlayer(rnet::PlayerId id);
	ServerVehicle* FindVehicle(uint16_t id);
	int ActivePlayerCount() const;

	void BroadcastChat(const char* text, rnet::PlayerId fromId = rnet::PLAYER_ID_SERVER);
	void SendChatTo(rnet::PlayerId playerId, const char* text,
	                rnet::PlayerId fromId = rnet::PLAYER_ID_SERVER);
	void BroadcastEvent(uint8_t type, rnet::PlayerId arg1 = 0, rnet::PlayerId arg2 = 0,
	                    const char* text = "");
	void BroadcastWorld(void);   // MSG_WORLD clock/weather sync to everyone
	void BroadcastResList(void); // MSG_RES_LIST mod/resource inventory to everyone
	void SendResList(ENetPeer* peer);
	void SpawnPlayer(rnet::PlayerId playerId, float x, float y, float z, float heading,
	                 uint16_t skin, uint8_t health, uint8_t armour);
	void KillPlayer(rnet::PlayerId victimId, rnet::PlayerId killerId, uint8_t weaponId);
	void TeleportPlayer(rnet::PlayerId playerId, float x, float y, float z, float heading);
	ServerVehicle* SpawnVehicle(uint16_t modelId, float x, float y, float z, float heading, bool permanent,
	                            rnet::PlayerId ownerId = rnet::PLAYER_ID_NONE);
	void RemoveVehicle(uint16_t vehicleId);
	void SetPlayerVitals(rnet::PlayerId playerId, uint8_t health, uint8_t armour);
	void BroadcastScores();
	void BroadcastPlayerPing();   // MSG_PLAYER_PING: ping/score rows for the scoreboard
	void KickPlayer(rnet::PlayerId playerId, const char* reason);
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
	void SendGive(rnet::PlayerId targetId, uint8_t kind, int32_t amount, uint16_t arg);
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
	void Broadcast(const rnet::BitWriter& w, uint8_t channel, bool reliable,
	               rnet::PlayerId exceptId = rnet::PLAYER_ID_SERVER);
	void Log(const char* fmt, ...);

	// --- snapshot / roster delivery ---------------------------------------
	// RelayStates groups every state a client can see into MSG_STATE_BATCH
	// packets: one send per player per tick instead of one per (state,
	// receiver) pair, which is what makes 1000+ players affordable.
	void RelayStates();
	void SendRoster(ENetPeer* peer, rnet::PlayerId skipId = rnet::PLAYER_ID_SERVER);

	// --- capture-grade telemetry ------------------------------------------
	void LogNetStats(double t);       // 1 Hz: players, tick time, packets, bytes, rtt
	void ResetTickStats();
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

	// AOI grid, rebuilt every tick: intrusive linked lists per cell, so a
	// neighbour scan never allocates and never walks the whole player table.
	std::vector<int> m_aoiHead;      // cell -> first player index (-1 = empty)
	std::vector<int> m_aoiNext;      // player index -> next in the same cell
	int m_aoiDim = 0;                // cells per side (m_aoiDim² cells)
	float m_aoiCell = 200.0f;        // cell width in metres (= stream distance)
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
	uint64_t m_totalPacketsIn = 0, m_totalPacketsOut = 0;

	// capture telemetry (all reset once a second by LogNetStats)
	std::mutex m_logMutex;
	std::ofstream m_logFile;
	double m_statsWindowStart = 0.0;
	uint64_t m_statsTicks = 0, m_statsStates = 0, m_statsStatePackets = 0;
	uint64_t m_statsJoins = 0, m_statsLeaves = 0, m_statsRefusals = 0, m_statsKicks = 0;
	uint64_t m_statsInboundPackets = 0, m_statsInboundBad = 0;
	double m_statsTickMsSum = 0.0, m_statsTickMsMax = 0.0;
	// per-phase tick cost: which stage is eating the 50 ms budget is the
	// question a 1k/2k capture exists to answer
	double m_statsPumpMsSum = 0.0, m_statsPumpMsMax = 0.0;
	double m_statsRelayMsSum = 0.0, m_statsRelayMsMax = 0.0;
	double m_statsFlushMsSum = 0.0, m_statsFlushMsMax = 0.0;
	// AOI relay volume, counted where it is produced (SendTo also carries
	// chat/roster/reconcile traffic, so its counter is not a relay measure)
	uint64_t m_statsRelayStates = 0, m_statsRelayBatches = 0;
	int m_reconcileCursor = 0;       // staggered 1 Hz reconciliation sweep
	int m_relayPhase = 0;            // alternate ticks carry the far-AOI sweep

	friend class GameMode;
};
