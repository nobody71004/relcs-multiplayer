// rnet_common.h — reLCS multiplayer shared protocol: constants and structures.
// Header-only, engine-independent: compiled into the game, server, bots, launcher, tests.
#pragma once

#include <cstdint>
#include <cstring>

namespace rnet {

// ---------------------------------------------------------------------------
// Identity / ports
// ---------------------------------------------------------------------------
constexpr uint16_t PROTOCOL_VERSION   = 4;  // v4: MSG_INVENTORY profile sync
constexpr uint16_t DEFAULT_PORT       = 7777;  // ENet game traffic (UDP)
constexpr uint16_t DISCOVERY_PORT     = 7778;  // raw-UDP LAN discovery
constexpr uint16_t DEFAULT_MASTER_PORT= 7800;  // ENet master server
constexpr uint32_t GAME_VERSION_TAG   = 0x4C435331; // "LCS1"

// ---------------------------------------------------------------------------
// Limits / rates
// ---------------------------------------------------------------------------
constexpr int MAX_PLAYERS   = 200;
constexpr int MAX_VEHICLES  = 200;
constexpr int MAX_NAME_LEN  = 24;
constexpr int MAX_CHAT_LEN  = 160;
constexpr int MAX_REASON_LEN= 96;
constexpr int MAX_HOSTNAME_LEN = 48;
constexpr int TICK_RATE     = 20;    // state broadcast Hz
constexpr float TICK_DT     = 1.0f / (float)TICK_RATE;

// ENet channels
constexpr uint8_t CHAN_RELIABLE = 0; // RPCs / handshake / chat / gamemode
constexpr uint8_t CHAN_STATE    = 1; // unreliable-sequenced snapshots

// Validation limits (generous — server-authoritative-lite)
constexpr float MAX_SPEED_ONFOOT   = 15.0f;  // m/s
constexpr float MAX_SPEED_FALL     = 40.0f;  // m/s downward; drops/ragdolls are legit physics
constexpr float MAX_SPEED_VEHICLE  = 95.0f;  // m/s
constexpr float STREAM_DISTANCE    = 200.0f; // m, AOI radius
constexpr float TELEPORT_WINDOW_SEC  = 5.0f;  // flood-guard window for client-flagged teleports
constexpr int   MAX_TELEPORTS_PER_WINDOW = 200; // >20 Hz sustained is impossible from a real client

// ---------------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------------
enum MsgType : uint8_t {
	// session
	MSG_HELLO           = 1,
	MSG_WELCOME         = 2,
	MSG_DISCONNECT      = 3,
	MSG_PING            = 4,
	MSG_PONG            = 5,
	// entities
	MSG_PLAYER_JOIN     = 10,
	MSG_PLAYER_QUIT     = 11,
	MSG_PLAYER_STATE    = 12,
	MSG_VEHICLE_STATE   = 13,
	// rpcs
	MSG_CHAT            = 20,
	MSG_SPAWN_REQ       = 21,
	MSG_SPAWN           = 22,
	MSG_DEATH           = 23,
	MSG_DAMAGE          = 24,
	MSG_KICK            = 25,
	MSG_CORRECTION      = 26,
	MSG_GIVE            = 27, // admin effect: money/weapon/god/health
	MSG_VEH_ENTER       = 30,
	MSG_VEH_EXIT        = 31,
	MSG_VEH_SPAWN       = 32,
	MSG_INVENTORY       = 34, // profile sync: full money+weapon snapshot (both ways)
	// world / gamemode
	MSG_WORLD           = 40,
	MSG_SCORE_UPDATE    = 41,
	MSG_GAMEMODE_EVENT  = 42,
	MSG_PICKUP_SPAWN    = 43,
	MSG_PICKUP_COLLECT  = 44,
	// admin
	MSG_RCON_AUTH       = 50,
	MSG_RCON_OK         = 51,
	MSG_RCON_CMD        = 52,
	MSG_RCON_RESP       = 53,
	// discovery / master
	MSG_MASTER_ANNOUNCE = 80,
	MSG_MASTER_LIST_REQ = 81,
	MSG_MASTER_LIST_RESP= 82,
};

// LAN discovery (raw UDP, human-readable magic strings)
constexpr const char* LAN_DISCOVER_MAGIC = "RELCS_DISCOVER_V1";
constexpr const char* LAN_RESP_MAGIC     = "RELCS_RESP_V1";

// HELLO flags
constexpr uint8_t HELLO_PROBE = 1; // latency probe — no player slot allocated

// ---------------------------------------------------------------------------
// Entity state (quantized, see rnet_protocol.h for codec)
// ---------------------------------------------------------------------------
// PlayerState.vehicleId values
constexpr uint16_t VEHICLE_NONE     = 0xFFFF; // on foot
constexpr uint16_t VEHICLE_UNSYNCED = 0xFFFE; // riding a vehicle that has no net id yet
                                               // (gets the vehicle movement budget)
struct PlayerState {
	uint16_t playerId = 0;
	// position in millimetres (absolute world coords, ±2147 km)
	int32_t  xMm = 0, yMm = 0, zMm = 0;
	// velocity in decimetres/second
	int16_t  velX = 0, velY = 0, velZ = 0;
	// angles: int16 full circle
	int16_t  heading = 0, pitch = 0;
	uint16_t animId = 0;
	uint8_t  animFlags = 0;   // ANIMFLAG_*
	uint8_t  health = 100, armour = 0;
	uint8_t  weapon = 0;         // current eWeaponType
	uint16_t ammo = 0;
	int32_t  money = 0;          // wallet (synced for HUD/inventory panels)
	uint16_t vehicleId = 0xFFFF; // 0xFFFF = on foot
	uint8_t  seat = 0xFF;
	uint8_t  moveFlags = 0;      // MOVEFLAG_*
};

// Full inventory snapshot (MSG_INVENTORY): the player's wallet and weapon table.
// client -> server: periodic report (server persists it in the profile DB)
// server -> client: profile restore on join (client applies it absolutely)
constexpr int MAX_INV_ENTRIES = 32;
struct InvEntry {
	uint8_t  weapon = 0;    // eWeaponType (0 = unarmed, unused)
	uint16_t ammo = 0;
};
struct MsgInventory {
	uint16_t playerId = 0;
	int32_t  money = 0;
	uint8_t  currentWeapon = 0;
	uint8_t  count = 0;
	InvEntry entries[MAX_INV_ENTRIES];
};

// animFlags bits
constexpr uint8_t ANIM_CROUCH   = 1;
constexpr uint8_t ANIM_AIM      = 2;
constexpr uint8_t ANIM_FIRE     = 4;
constexpr uint8_t ANIM_JUMP     = 8;
constexpr uint8_t ANIM_SPRINT   = 16;
constexpr uint8_t ANIM_INVEHICLE= 32;

// moveFlags bits (PlayerState.moveFlags)
constexpr uint8_t MOVEFLAG_TELEPORT = 1; // game-side relocation (respawn, scripted
                                         // warp): server resets its move reference
                                         // instead of counting movement violations

struct VehicleState {
	uint16_t vehicleId = 0;
	uint16_t modelId = 0;
	int32_t  xMm = 0, yMm = 0, zMm = 0;
	int16_t  heading = 0, pitch = 0, roll = 0;
	int16_t  velX = 0, velY = 0, velZ = 0; // dm/s
	int8_t   steer = 0;
	uint8_t  gas = 0, brake = 0;
	uint8_t  flags = 0;   // VEHFLAG_*
	uint16_t health = 1000;
};

// vehicle flags
constexpr uint8_t VEH_ENGINE = 1;
constexpr uint8_t VEH_SIREN  = 2;
constexpr uint8_t VEH_LIGHTS = 4;
constexpr uint8_t VEH_HORN   = 8;
constexpr uint8_t VEH_ALARM  = 16;

// ---------------------------------------------------------------------------
// RPC payloads
// ---------------------------------------------------------------------------
struct MsgHello {
	uint16_t protocolVersion = PROTOCOL_VERSION;
	uint8_t  flags = 0;
	uint32_t gameVersion = GAME_VERSION_TAG;
	char     name[MAX_NAME_LEN] = {};
	char     password[32] = {};
};

struct MsgWelcome {
	uint8_t  playerId = 0;
	uint16_t tickRate = TICK_RATE;
	float    streamDistance = STREAM_DISTANCE;
	uint8_t  hour = 12, minute = 0, weather = 0;
	char     gameMode[32] = {};
	char     hostname[MAX_HOSTNAME_LEN] = {};
};

struct MsgPlayerJoin {
	uint8_t  playerId = 0;
	uint16_t skinModel = 0;
	char     name[MAX_NAME_LEN] = {};
};

struct MsgChat {
	uint8_t  fromId = 0;      // 0 = server console
	uint16_t len = 0;
	char     text[MAX_CHAT_LEN] = {};
};

struct MsgSpawn {
	uint8_t  playerId = 0;
	uint16_t skinModel = 0;
	float    x = 0, y = 0, z = 0, heading = 0;
	uint8_t  health = 100, armour = 0;
};

struct MsgDeath {
	uint8_t victimId = 0;
	uint8_t killerId = 0xFF;
	uint8_t weaponId = 0;
};

struct MsgDamage {
	uint8_t  targetId = 0;
	uint8_t  attackerId = 0;
	uint8_t  weaponId = 0;
	uint16_t damage = 0;
};

struct MsgKick {
	uint16_t len = 0;
	char     reason[MAX_REASON_LEN] = {};
};

// MSG_GIVE kinds (admin effects, server -> target client only)
enum GiveKind : uint8_t {
	GIVE_MONEY  = 1, // amount added to wallet
	GIVE_WEAPON = 2, // arg = eWeaponType, amount = ammo
	GIVE_GOD    = 3, // amount 0/1
	GIVE_HEALTH = 4, // amount = hit points
};
struct MsgGive {
	uint8_t  targetId = 0;
	uint8_t  kind = 0;
	int32_t  amount = 0;
	uint16_t arg = 0;
};

struct MsgVehEnter {
	uint8_t  playerId = 0;
	uint16_t vehicleId = 0xFFFF; // 0xFFFF = server assigns new id
	uint8_t  seat = 0;
	uint16_t modelId = 0;
};

struct MsgVehExit {
	uint8_t  playerId = 0;
	uint16_t vehicleId = 0;
};

struct MsgVehSpawn {
	uint16_t vehicleId = 0;
	uint16_t modelId = 0;
	float    x = 0, y = 0, z = 0, heading = 0;
};

struct MsgWorld {
	uint8_t hour = 12, minute = 0, weather = 0;
};

// MSG_PICKUP_SPAWN — admin "spawn prop": a synced pickup marker (weapon crate,
// money bag, ...) placed in the world; every client materializes it locally.
struct MsgPickup {
	uint16_t pickupId = 0;
	uint16_t modelIndex = 0; // 0 = derive from weapon
	uint8_t  type = 4;       // ePickupType (4 = PICKUP_ONCE)
	uint16_t weapon = 0;     // eWeaponType for weapon pickups
	uint32_t quantity = 0;   // ammo / money amount
	float    x = 0, y = 0, z = 0;
};

struct ScoreEntry {
	uint8_t  playerId = 0;
	int32_t  score = 0;
	uint16_t kills = 0, deaths = 0;
};

constexpr int MAX_SCORE_ENTRIES = 32;
struct MsgScoreUpdate {
	uint8_t count = 0;
	ScoreEntry entries[MAX_SCORE_ENTRIES] = {};
};

// gamemode event types
enum : uint8_t {
	GMEV_ROUND_START = 1,
	GMEV_ROUND_END   = 2,
	GMEV_KILL        = 3,
	GMEV_TEXT        = 4, // free text line
};

struct MsgGamemodeEvent {
	uint8_t  type = 0;
	uint8_t  arg1 = 0;   // killer / winner …
	uint8_t  arg2 = 0;   // victim …
	uint16_t len = 0;
	char     text[MAX_CHAT_LEN] = {};
};

// ---------------------------------------------------------------------------
// Master server payloads
// ---------------------------------------------------------------------------
struct MsgMasterAnnounce {
	uint16_t gamePort = DEFAULT_PORT;
	uint8_t  players = 0, maxPlayers = 0;
	uint16_t protocolVersion = PROTOCOL_VERSION;
	char     hostname[MAX_HOSTNAME_LEN] = {};
	char     gameMode[32] = {};
};

struct MasterListEntry {
	uint32_t ipv4 = 0;       // network byte order
	uint16_t gamePort = DEFAULT_PORT;
	uint8_t  players = 0, maxPlayers = 0;
	uint16_t pingMs = 0;
	char     hostname[MAX_HOSTNAME_LEN] = {};
	char     gameMode[32] = {};
};

constexpr int MAX_MASTER_ENTRIES = 256;
struct MsgMasterListResp {
	uint16_t count = 0;
	// followed by count × MasterListEntry written field-by-field
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
inline void CopyStr(char* dst, size_t dstSize, const char* src)
{
	if(dstSize == 0) return;
	std::strncpy(dst, src, dstSize - 1);
	dst[dstSize - 1] = '\0';
}

} // namespace rnet
