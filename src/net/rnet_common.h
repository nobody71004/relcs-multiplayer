// rnet_common.h — reLCS multiplayer shared protocol: constants and structures.
// Header-only, engine-independent: compiled into the game, server, bots, launcher, tests.
#pragma once

#include <cstdint>
#include <cstring>

namespace rnet {

// ---------------------------------------------------------------------------
// Identity / ports
// ---------------------------------------------------------------------------
constexpr uint16_t PROTOCOL_VERSION   = 9;  // v9: relayed states are delta-coded against the receiver
// v8: 16-bit player ids (a server may host >255 players)
// v7: MsgVehSpawn carries the owner (entering player) id
// v5: server map + static vehicles + master favorites
constexpr uint16_t DEFAULT_PORT       = 7777;  // ENet game traffic (UDP)
constexpr uint16_t DISCOVERY_PORT     = 7778;  // raw-UDP LAN discovery
constexpr uint16_t DEFAULT_MASTER_PORT= 7800;  // ENet master server
constexpr uint32_t GAME_VERSION_TAG   = 0x4C435331; // "LCS1"

// Admin RCON password used when neither the launcher config nor -rcon supplies one.
// The public fleet is started with this value so a fresh launcher install can open the
// admin panel without extra setup; change it on the servers (server.cfg rcon_password)
// and pass -rcon <new value> to the game to rotate it. Rides the game UDP port in-band.
constexpr const char* DEFAULT_RCON_PASSWORD = "relcs-admin-2026";

// ---------------------------------------------------------------------------
// Limits / rates
// ---------------------------------------------------------------------------
// Player identity is 16-bit on the wire: ids are 1..MAX_PLAYERS-1, 0 is the
// server console (chat/RCON origin) and 0xFFFF means "no player".
using PlayerId = uint16_t;
constexpr PlayerId PLAYER_ID_SERVER = 0;
constexpr PlayerId PLAYER_ID_NONE   = 0xFFFF;

// Hard slot capacity. `maxplayers` in a server config may not exceed
// MAX_SERVER_PLAYERS (2000 — the 2k load-test target); the extra slots are
// headroom so a peer can be admitted while another is being retired.
constexpr int MAX_PLAYERS        = 2048;
constexpr int MAX_SERVER_PLAYERS = 2000;
constexpr int MAX_VEHICLES  = 200;
constexpr int MAX_NAME_LEN  = 24;
constexpr int MAX_CHAT_LEN  = 160;
constexpr int MAX_REASON_LEN= 96;
constexpr int MAX_HOSTNAME_LEN = 48;
constexpr int MAX_MAP_LEN   = 32;
constexpr int MAX_ADDR_LEN  = 64;
constexpr int MAX_FAV_ENTRIES = 64;
constexpr int TICK_RATE     = 20;    // state broadcast Hz
constexpr float TICK_DT     = 1.0f / (float)TICK_RATE;

// ENet channels
constexpr uint8_t CHAN_RELIABLE = 0; // RPCs / handshake / chat / gamemode
constexpr uint8_t CHAN_STATE    = 1; // unreliable-sequenced snapshots

// Proximity voice: 8 kHz mono, 20 ms frames, Opus (variable-length packets)
constexpr int   VOICE_SAMPLE_RATE   = 8000;
constexpr int   VOICE_FRAME_SAMPLES = 160;             // 20 ms
constexpr int   VOICE_MAX_PACKET    = 160;             // largest Opus packet accepted on the wire
constexpr int   VOICE_BITRATE       = 20000;           // bit/s target (mu-law used 64 kbit/s)
constexpr float VOICE_RANGE_M       = 40.0f;           // server relays and clients hear within this radius

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
	// One packet carrying several player states (server -> client). Every tick
	// the server groups the states one client is in range of into a handful of
	// these instead of one packet per (state, receiver) pair — the send path
	// stays O(players) instead of O(players²) at 1000+ players.
	MSG_STATE_BATCH     = 14,
	// A batch of player identities (name/skin/last spawn) so a newcomer's
	// roster costs a few packets rather than one MSG_PLAYER_JOIN + MSG_SPAWN
	// per player, and a full server can refresh names/skins cheaply.
	MSG_ROSTER          = 15,
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
	MSG_VEH_DESPAWN     = 33, // server removed a vehicle (static spawn cleanup)
	MSG_INVENTORY       = 34, // profile sync: full money+weapon snapshot (both ways)
	MSG_APPEARANCE      = 35, // player appearance: ped model + clothing/component slots
	                          // client->server: "this is my look" (server persists + rebroadcasts)
	                          // server->client: a player's look (own restore on join, remotes on join/change)
	// world / gamemode
	MSG_WORLD           = 40,
	MSG_SCORE_UPDATE    = 41,
	MSG_GAMEMODE_EVENT  = 42,
	MSG_PICKUP_SPAWN    = 43,
	MSG_PICKUP_COLLECT  = 44,
	MSG_RES_LIST        = 45, // mod/resource inventory for the admin panel
	MSG_VOICE           = 46, // proximity voice frame (client->server: [u8 unused][u8 len][len x Opus]; server->client: [u8 fromId][u8 len][len x Opus])
	MSG_PLAYER_PING     = 47, // server->client ~1 Hz: top players with ping/score (scoreboard + player list)
	// EOS Anti-Cheat relay (see MsgAcHello/MsgAcPayload). Additive and
	// negotiated: a client that never sends MSG_AC_HELLO is simply not
	// registered and the server relays nothing to it, so old builds keep
	// joining the same server without a protocol bump.
	MSG_AC_HELLO        = 60, // client -> server: register this peer with EOS AC (PUID + id token + platform)
	MSG_AC_TO_CLIENT    = 61, // server -> client: opaque EOS_AntiCheatServer payload
	MSG_AC_FROM_CLIENT  = 62, // client -> server: opaque EOS_AntiCheatClient payload
	MSG_AC_STATUS       = 63, // server -> client: registration result
	// admin
	MSG_RCON_AUTH       = 50,
	MSG_RCON_OK         = 51,
	MSG_RCON_CMD        = 52,
	MSG_RCON_RESP       = 53,
	// discovery / master
	MSG_MASTER_ANNOUNCE = 80,
	MSG_MASTER_LIST_REQ = 81,
	MSG_MASTER_LIST_RESP= 82,
	MSG_MASTER_FAV_LIST = 83, // launcher -> master: send my favorites (MsgFavReq)
	MSG_MASTER_FAV_ADD  = 84, // launcher -> master: add/update one favorite (MsgFavAdd)
	MSG_MASTER_FAV_DEL  = 85, // launcher -> master: forget one favorite (MsgFavAdd, name unused)
	MSG_MASTER_FAV_RESP = 86, // master -> launcher: the current list
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

// Player appearance (MSG_APPEARANCE): the character model a player renders as,
// plus a fixed set of clothing/component variation slots.
//
// GTA:LCS renders a ped by its *model index* (one model = one look); `model`
// is the ped model id a player chose (MI_MALE01, MI_GANG01, ...). The component
// slots are an additive, forward-compatible channel for per-slot clothing/skin
// variation (hats/torso/legs/...) — a modded build or a future ped variation
// system can honour them; a stock LCS client applies `model` and ignores the
// rest. Kept fixed-size so the codec never trusts a wire-supplied length.
constexpr int MAX_APPEARANCE_COMPONENTS = 8;
constexpr uint16_t APPEARANCE_MODEL_DEFAULT = 7;  // MI_MALE01 — the stock avatar

struct MsgAppearance {
	uint16_t playerId = 0;
	uint16_t model = APPEARANCE_MODEL_DEFAULT;  // ped model index (the character/outfit)
	uint8_t  components[MAX_APPEARANCE_COMPONENTS] = {}; // per-slot variation (0 = default)
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
	// door open ratio per eDoors index (bonnet, boot, front L/R, rear L/R):
	// 0 = closed, 255 = fully open. Non-car vehicles send zeros.
	uint8_t  doors[6] = {};
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
	PlayerId playerId = 0;
	uint16_t tickRate = TICK_RATE;
	float    streamDistance = STREAM_DISTANCE;
	uint8_t  hour = 12, minute = 0, weather = 0;
	char     gameMode[32] = {};
	char     hostname[MAX_HOSTNAME_LEN] = {};
	// v5: live server info — the launcher's browser tooltip reads these
	// straight off a probe handshake (no master round trip needed)
	uint16_t players = 0, maxPlayers = 0;
	char     mapName[MAX_MAP_LEN] = {};
};

struct MsgPlayerJoin {
	PlayerId playerId = 0;
	uint16_t skinModel = 0;
	char     name[MAX_NAME_LEN] = {};
};

struct MsgChat {
	PlayerId fromId = 0;      // PLAYER_ID_SERVER = server console
	uint16_t len = 0;
	char     text[MAX_CHAT_LEN] = {};
};

struct MsgSpawn {
	PlayerId playerId = 0;
	uint16_t skinModel = 0;
	float    x = 0, y = 0, z = 0, heading = 0;
	uint8_t  health = 100, armour = 0;
};

// How many records may arrive in one MSG_STATE_BATCH / MSG_ROSTER packet.
// Batches are split at these sizes so a snapshot stays well under the path MTU
// (~1.2 KB) instead of fragmenting: a lost fragment must never cost a whole
// tick. A relayed state is ~11-14 bytes (rnet_protocol.h), so the record cap is
// high and RELAY_PAYLOAD_BYTES is what actually bounds a state packet: one
// datagram then carries a whole 2000-player AOI.
constexpr int MAX_STATES_PER_PACKET = 128;
constexpr int RELAY_PAYLOAD_BYTES   = 1200;
constexpr int MAX_ROSTER_PER_PACKET = 16;

// MsgRosterEntry.flags
constexpr uint8_t ROSTER_SPAWNED = 1;   // the position/health below are valid
constexpr uint8_t ROSTER_ALIVE   = 2;

// MSG_ROSTER — a batch of player identities:
//   [u16 count][count × entry]
// entry: [u16 id][u16 skin][u8 flags][u8 health][u8 armour]
//        [i32 xMm][i32 yMm][i32 zMm][i16 heading][u16 len][name]
// The roster is how a newcomer learns who is already here (a few packets
// instead of one JOIN+SPAWN pair per player) and how a busy server refreshes
// names/skins when the per-join broadcast is suppressed.
struct MsgRosterEntry {
	PlayerId id = 0;
	uint16_t skin = 0;
	uint8_t  flags = 0;
	uint8_t  health = 100, armour = 0;
	int32_t  xMm = 0, yMm = 0, zMm = 0;
	int16_t  heading = 0;
	char     name[MAX_NAME_LEN] = {};
};

struct MsgDeath {
	PlayerId victimId = 0;
	PlayerId killerId = PLAYER_ID_NONE;   // environment/suicide
	uint8_t weaponId = 0;
};

struct MsgDamage {
	PlayerId targetId = 0;
	PlayerId attackerId = 0;
	uint8_t  weaponId = 0;
	uint16_t damage = 0;
};

// Why a HELLO was refused or a player was kicked. 0 = generic (reason text
// only, what a pre-code client/server sends).
enum JoinRefused : uint8_t {
	REFUSE_NONE     = 0,
	REFUSE_PROTOCOL = 1, // protocolVersion mismatch
	REFUSE_BANNED   = 2, // IP banned
	REFUSE_FULL     = 3, // server at maxPlayers
	REFUSE_NAME     = 4, // name rejected
	REFUSE_AUTH     = 5, // name is claimed and the password was wrong/missing
	REFUSE_PASSWORD = 6, // password rejected (e.g. too short to claim)
	REFUSE_ANTICHEAT = 7, // this server requires a protected (EOS AC) client
};

struct MsgKick {
	uint16_t len = 0;
	char     reason[MAX_REASON_LEN] = {};
	uint8_t  code = 0;   // JoinRefused (0 for legacy peers)
};

// MSG_GIVE kinds (admin effects, server -> target client only)
enum GiveKind : uint8_t {
	GIVE_MONEY  = 1, // amount added to wallet
	GIVE_WEAPON = 2, // arg = eWeaponType, amount = ammo
	GIVE_GOD    = 3, // amount 0/1
	GIVE_HEALTH = 4, // amount = hit points
};
struct MsgGive {
	PlayerId targetId = 0;
	uint8_t  kind = 0;
	int32_t  amount = 0;
	uint16_t arg = 0;
};

struct MsgVehEnter {
	PlayerId playerId = 0;
	uint16_t vehicleId = 0xFFFF; // 0xFFFF = server assigns new id
	uint8_t  seat = 0;
	uint16_t modelId = 0;
};

struct MsgVehExit {
	PlayerId playerId = 0;
	uint16_t vehicleId = 0;
};

struct MsgVehSpawn {
	uint16_t vehicleId = 0;
	uint16_t modelId = 0;
	float    x = 0, y = 0, z = 0, heading = 0;
	PlayerId ownerId = PLAYER_ID_NONE; // player whose enter allocated it (NONE = static/admin)
};

// MSG_VEH_DESPAWN — the server retired a vehicle (admin delcar, static spawn
// cleanup): clients drop the puppet everywhere.
struct MsgVehDespawn {
	uint16_t vehicleId = 0;
};

struct MsgWorld {
	uint8_t hour = 12, minute = 0, weather = 0;
};

// MSG_RES_LIST — mod/resource inventory snapshot (server -> client, on change)
struct MsgResEntry {
	char    name[24] = {};      // resource dir name
	char    version[8] = {};
	uint8_t type = 0;           // 0 = internal, 1 = external
	uint8_t enabled = 0;
	uint8_t valid = 0;          // 0 = parse error
};
constexpr int MAX_RES_ENTRIES = 32;
struct MsgResList {
	uint8_t count = 0;
	MsgResEntry entries[MAX_RES_ENTRIES] = {};
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
	PlayerId playerId = 0;
	int32_t  score = 0;
	uint16_t kills = 0, deaths = 0;
};

constexpr int MAX_SCORE_ENTRIES = 32;
struct MsgScoreUpdate {
	uint8_t count = 0;
	ScoreEntry entries[MAX_SCORE_ENTRIES] = {};
};

// one row of the player list: ping is the ENet round trip in ms (0 for NPCs)
struct PingEntry {
	PlayerId playerId = 0;
	uint16_t pingMs = 0;
	int32_t  score = 0;
	uint16_t kills = 0, deaths = 0;
};

constexpr int MAX_PING_ENTRIES = 32;
struct MsgPlayerPing {
	uint8_t count = 0;
	PingEntry entries[MAX_PING_ENTRIES] = {};
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
	PlayerId arg1 = 0;   // killer / winner …
	PlayerId arg2 = 0;   // victim …
	uint16_t len = 0;
	char     text[MAX_CHAT_LEN] = {};
};

// ---------------------------------------------------------------------------
// EOS Anti-Cheat relay (MSG_AC_*)
//
// The game server owns a live EOS_AntiCheatServer session. Clients that have a
// local EOS_AntiCheatClient session register here with their ProductUserId and
// a Connect ID token; the server verifies the token, calls
// EOS_AntiCheatServer_RegisterClient with the peer as the opaque client handle
// and then relays kernel-driver traffic verbatim in both directions:
//
//   EOS_AntiCheatClient_OnMessageToServer   -> MSG_AC_FROM_CLIENT -> ReceiveMessageFromClient
//   EOS_AntiCheatServer OnMessageToClient    -> MSG_AC_TO_CLIENT   -> ReceiveMessageFromServer
//
// Payloads are opaque to reLCS: never parsed, only bounded.
constexpr int AC_PUID_LEN    = 40;   // EOS ProductUserId string (32 hex chars) + slack
constexpr int AC_TOKEN_LEN   = 4096; // Connect ID token JWT (EOS tokens are ~1.5 KB; 512 truncated them)
constexpr int MAX_AC_PAYLOAD = 1200; // one EOS AC message; larger ones are dropped

// EOS_EAntiCheatCommonClientPlatform, narrowed to what a reLCS client can be.
enum AcPlatform : uint8_t {
	AC_PLATFORM_UNKNOWN = 0,
	AC_PLATFORM_WINDOWS = 1,
	AC_PLATFORM_MAC     = 2,
	AC_PLATFORM_LINUX   = 3,
};

// MSG_AC_STATUS result codes.
enum AcRegisterResult : uint8_t {
	AC_REG_OK           = 0, // registered: AC traffic may flow
	AC_REG_DISABLED     = 1, // this server runs no AC session (eac=0 / SDK missing)
	AC_REG_BAD_PLATFORM = 2, // platform byte not understood
	AC_REG_BAD_PUID     = 3, // ProductUserId string could not be parsed
	AC_REG_BAD_TOKEN    = 4, // Connect ID token missing or failed verification
	AC_REG_REJECTED     = 5, // EOS_AntiCheatServer_RegisterClient refused
	AC_REG_ALREADY      = 6, // already registered on this peer
};

struct MsgAcHello {
	uint8_t  platform = AC_PLATFORM_UNKNOWN;
	char     productUserId[AC_PUID_LEN] = {};
	char     idToken[AC_TOKEN_LEN] = {};
};

struct MsgAcStatus {
	uint8_t  status = AC_REG_DISABLED;
	uint8_t  registered = 0;             // 1 = the server's AC session holds this peer
	char     detail[MAX_REASON_LEN] = {};
};

// One opaque EOS AC message in either direction.
struct MsgAcPayload {
	uint16_t len = 0;
	uint8_t  data[MAX_AC_PAYLOAD] = {};
};

// ---------------------------------------------------------------------------
// Master server payloads
// ---------------------------------------------------------------------------
struct MsgMasterAnnounce {
	uint16_t gamePort = DEFAULT_PORT;
	uint16_t players = 0, maxPlayers = 0;
	uint16_t protocolVersion = PROTOCOL_VERSION;
	char     hostname[MAX_HOSTNAME_LEN] = {};
	char     gameMode[32] = {};
	char     mapName[MAX_MAP_LEN] = {};
	// LAN address the game is reachable on (network byte order), for masters
	// that observe a loopback source (server + master on the same machine);
	// 0 = fall back to the observed address
	uint32_t advertisedIp = 0;
};

struct MasterListEntry {
	uint32_t ipv4 = 0;       // network byte order
	uint16_t gamePort = DEFAULT_PORT;
	uint16_t players = 0, maxPlayers = 0;
	uint16_t pingMs = 0;
	char     hostname[MAX_HOSTNAME_LEN] = {};
	char     gameMode[32] = {};
	char     mapName[MAX_MAP_LEN] = {};
	uint16_t protocolVersion = 0;   // v5: lets the launcher flag stale servers
};

// ---------------------------------------------------------------------------
// Master favorites (server-side persistence, one list per username)
// ---------------------------------------------------------------------------
struct MsgFavEntry {
	char address[MAX_ADDR_LEN] = {};   // "host:port"
	char name[MAX_HOSTNAME_LEN] = {};  // last known hostname
};

// launcher -> master: fetch my list
struct MsgFavReq {
	char nick[MAX_NAME_LEN] = {};
};

// launcher -> master: remember / forget one server
struct MsgFavAdd {
	char nick[MAX_NAME_LEN] = {};
	char address[MAX_ADDR_LEN] = {};
	char name[MAX_HOSTNAME_LEN] = {};
};

// master -> launcher: the full current list (sent after every list/add/del)
struct MsgFavResp {
	uint8_t ok = 1;                 // 0 = the request was rejected
	uint8_t count = 0;
	char    error[64] = {};
	MsgFavEntry entries[MAX_FAV_ENTRIES] = {};
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
