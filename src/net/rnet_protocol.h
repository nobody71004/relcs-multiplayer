// rnet_protocol.h — message codec (structs <-> BitStream), quantization, validation.
// Header-only.
#pragma once

#include "rnet_common.h"
#include "rnet_bitstream.h"

#include <cmath>

namespace rnet {

// ---------------------------------------------------------------------------
// Quantization
// ---------------------------------------------------------------------------
constexpr float RNET_PI = 3.14159265358979323846f;

// double-precision intermediates: float32 alone drifts ~0.6 mm at kilometre-scale coords
inline int32_t QuantPos(float meters)   { return (int32_t)std::lround((double)meters * 1000.0); }  // mm
inline float   DequantPos(int32_t mm)   { return (float)(mm / 1000.0); }
inline int16_t QuantVel(float mps)      { return (int16_t)std::lround((double)mps * 10.0); }      // dm/s
inline float   DequantVel(int16_t v)    { return (float)(v / 10.0); }
inline int16_t QuantAngle(float rad)
{
	double turns = rad / (2.0 * (double)RNET_PI);
	turns -= std::floor(turns);               // wrap to [0,1)
	return (int16_t)((int64_t)std::lround(turns * 65536.0) & 0xFFFF);
}
inline float DequantAngle(int16_t a)    { return (float)(a * (2.0 * (double)RNET_PI / 65536.0)); }

// ---------------------------------------------------------------------------
// State codecs
// ---------------------------------------------------------------------------
inline void WritePlayerState(BitWriter& w, const PlayerState& s)
{
	w.WriteU16(s.playerId);
	w.WriteI32(s.xMm); w.WriteI32(s.yMm); w.WriteI32(s.zMm);
	w.WriteI16(s.velX); w.WriteI16(s.velY); w.WriteI16(s.velZ);
	w.WriteI16(s.heading); w.WriteI16(s.pitch);
	w.WriteU16(s.animId); w.WriteU8(s.animFlags);
	w.WriteU8(s.health); w.WriteU8(s.armour);
	w.WriteU8(s.weapon); w.WriteU16(s.ammo); w.WriteI32(s.money);
	w.WriteU16(s.vehicleId); w.WriteU8(s.seat);
	w.WriteU8(s.moveFlags);
}

inline bool ReadPlayerState(BitReader& r, PlayerState& s)
{
	s.playerId = r.ReadU16();
	s.xMm = r.ReadI32(); s.yMm = r.ReadI32(); s.zMm = r.ReadI32();
	s.velX = r.ReadI16(); s.velY = r.ReadI16(); s.velZ = r.ReadI16();
	s.heading = r.ReadI16(); s.pitch = r.ReadI16();
	s.animId = r.ReadU16(); s.animFlags = r.ReadU8();
	s.health = r.ReadU8(); s.armour = r.ReadU8();
	s.weapon = r.ReadU8(); s.ammo = r.ReadU16(); s.money = r.ReadI32();
	s.vehicleId = r.ReadU16(); s.seat = r.ReadU8();
	s.moveFlags = r.ReadU8();
	return r.Ok();
}

inline void WriteVehicleState(BitWriter& w, const VehicleState& s)
{
	w.WriteU16(s.vehicleId); w.WriteU16(s.modelId);
	w.WriteI32(s.xMm); w.WriteI32(s.yMm); w.WriteI32(s.zMm);
	w.WriteI16(s.heading); w.WriteI16(s.pitch); w.WriteI16(s.roll);
	w.WriteI16(s.velX); w.WriteI16(s.velY); w.WriteI16(s.velZ);
	w.WriteI8(s.steer); w.WriteU8(s.gas); w.WriteU8(s.brake);
	w.WriteU8(s.flags); w.WriteU16(s.health);
}

inline bool ReadVehicleState(BitReader& r, VehicleState& s)
{
	s.vehicleId = r.ReadU16(); s.modelId = r.ReadU16();
	s.xMm = r.ReadI32(); s.yMm = r.ReadI32(); s.zMm = r.ReadI32();
	s.heading = r.ReadI16(); s.pitch = r.ReadI16(); s.roll = r.ReadI16();
	s.velX = r.ReadI16(); s.velY = r.ReadI16(); s.velZ = r.ReadI16();
	s.steer = r.ReadI8(); s.gas = r.ReadU8(); s.brake = r.ReadU8();
	s.flags = r.ReadU8(); s.health = r.ReadU16();
	return r.Ok();
}

// ---------------------------------------------------------------------------
// Small message codecs
// ---------------------------------------------------------------------------
inline void WriteHello(BitWriter& w, const MsgHello& m)
{
	w.WriteU16(m.protocolVersion); w.WriteU8(m.flags); w.WriteU32(m.gameVersion);
	w.WriteString(m.name, MAX_NAME_LEN - 1);
	w.WriteString(m.password, 31);
}
inline bool ReadHello(BitReader& r, MsgHello& m)
{
	m.protocolVersion = r.ReadU16(); m.flags = r.ReadU8(); m.gameVersion = r.ReadU32();
	std::string name = r.ReadString(MAX_NAME_LEN - 1);
	std::string pass = r.ReadString(31);
	CopyStr(m.name, sizeof m.name, name.c_str());
	CopyStr(m.password, sizeof m.password, pass.c_str());
	return r.Ok();
}

inline void WriteWelcome(BitWriter& w, const MsgWelcome& m)
{
	w.WriteU8(m.playerId); w.WriteU16(m.tickRate); w.WriteF32(m.streamDistance);
	w.WriteU8(m.hour); w.WriteU8(m.minute); w.WriteU8(m.weather);
	w.WriteString(m.gameMode, 31);
	w.WriteString(m.hostname, MAX_HOSTNAME_LEN - 1);
}
inline bool ReadWelcome(BitReader& r, MsgWelcome& m)
{
	m.playerId = r.ReadU8(); m.tickRate = r.ReadU16(); m.streamDistance = r.ReadF32();
	m.hour = r.ReadU8(); m.minute = r.ReadU8(); m.weather = r.ReadU8();
	std::string gm = r.ReadString(31);
	std::string hn = r.ReadString(MAX_HOSTNAME_LEN - 1);
	CopyStr(m.gameMode, sizeof m.gameMode, gm.c_str());
	CopyStr(m.hostname, sizeof m.hostname, hn.c_str());
	return r.Ok();
}

inline void WritePlayerJoin(BitWriter& w, const MsgPlayerJoin& m)
{
	w.WriteU8(m.playerId); w.WriteU16(m.skinModel);
	w.WriteString(m.name, MAX_NAME_LEN - 1);
}
inline bool ReadPlayerJoin(BitReader& r, MsgPlayerJoin& m)
{
	m.playerId = r.ReadU8(); m.skinModel = r.ReadU16();
	std::string n = r.ReadString(MAX_NAME_LEN - 1);
	CopyStr(m.name, sizeof m.name, n.c_str());
	return r.Ok();
}

inline void WriteChat(BitWriter& w, const MsgChat& m)
{
	w.WriteU8(m.fromId);
	w.WriteString(m.text, MAX_CHAT_LEN - 1);
}
inline bool ReadChat(BitReader& r, MsgChat& m)
{
	m.fromId = r.ReadU8();
	std::string t = r.ReadString(MAX_CHAT_LEN - 1);
	m.len = (uint16_t)t.size();
	CopyStr(m.text, sizeof m.text, t.c_str());
	return r.Ok();
}

inline void WriteSpawn(BitWriter& w, const MsgSpawn& m)
{
	w.WriteU8(m.playerId); w.WriteU16(m.skinModel);
	w.WriteF32(m.x); w.WriteF32(m.y); w.WriteF32(m.z); w.WriteF32(m.heading);
	w.WriteU8(m.health); w.WriteU8(m.armour);
}
inline bool ReadSpawn(BitReader& r, MsgSpawn& m)
{
	m.playerId = r.ReadU8(); m.skinModel = r.ReadU16();
	m.x = r.ReadF32(); m.y = r.ReadF32(); m.z = r.ReadF32(); m.heading = r.ReadF32();
	m.health = r.ReadU8(); m.armour = r.ReadU8();
	return r.Ok();
}

inline void WriteDeath(BitWriter& w, const MsgDeath& m)
{
	w.WriteU8(m.victimId); w.WriteU8(m.killerId); w.WriteU8(m.weaponId);
}
inline bool ReadDeath(BitReader& r, MsgDeath& m)
{
	m.victimId = r.ReadU8(); m.killerId = r.ReadU8(); m.weaponId = r.ReadU8();
	return r.Ok();
}

inline void WriteDamage(BitWriter& w, const MsgDamage& m)
{
	w.WriteU8(m.targetId); w.WriteU8(m.attackerId); w.WriteU8(m.weaponId); w.WriteU16(m.damage);
}
inline bool ReadDamage(BitReader& r, MsgDamage& m)
{
	m.targetId = r.ReadU8(); m.attackerId = r.ReadU8(); m.weaponId = r.ReadU8(); m.damage = r.ReadU16();
	return r.Ok();
}

inline void WriteGive(BitWriter& w, const MsgGive& m)
{
	w.WriteU8(m.targetId); w.WriteU8(m.kind);
	w.WriteI32(m.amount); w.WriteU16(m.arg);
}
inline bool ReadGive(BitReader& r, MsgGive& m)
{
	m.targetId = r.ReadU8(); m.kind = r.ReadU8();
	m.amount = r.ReadI32(); m.arg = r.ReadU16();
	return r.Ok();
}

inline void WriteKick(BitWriter& w, const MsgKick& m) { w.WriteString(m.reason, MAX_REASON_LEN - 1); }
inline bool ReadKick(BitReader& r, MsgKick& m)
{
	std::string t = r.ReadString(MAX_REASON_LEN - 1);
	m.len = (uint16_t)t.size();
	CopyStr(m.reason, sizeof m.reason, t.c_str());
	return r.Ok();
}

inline void WriteVehEnter(BitWriter& w, const MsgVehEnter& m)
{
	w.WriteU8(m.playerId); w.WriteU16(m.vehicleId); w.WriteU8(m.seat); w.WriteU16(m.modelId);
}
inline bool ReadVehEnter(BitReader& r, MsgVehEnter& m)
{
	m.playerId = r.ReadU8(); m.vehicleId = r.ReadU16(); m.seat = r.ReadU8(); m.modelId = r.ReadU16();
	return r.Ok();
}

inline void WriteVehExit(BitWriter& w, const MsgVehExit& m) { w.WriteU8(m.playerId); w.WriteU16(m.vehicleId); }
inline bool ReadVehExit(BitReader& r, MsgVehExit& m) { m.playerId = r.ReadU8(); m.vehicleId = r.ReadU16(); return r.Ok(); }

inline void WriteVehSpawn(BitWriter& w, const MsgVehSpawn& m)
{
	w.WriteU16(m.vehicleId); w.WriteU16(m.modelId);
	w.WriteF32(m.x); w.WriteF32(m.y); w.WriteF32(m.z); w.WriteF32(m.heading);
}
inline bool ReadVehSpawn(BitReader& r, MsgVehSpawn& m)
{
	m.vehicleId = r.ReadU16(); m.modelId = r.ReadU16();
	m.x = r.ReadF32(); m.y = r.ReadF32(); m.z = r.ReadF32(); m.heading = r.ReadF32();
	return r.Ok();
}

inline void WritePickup(BitWriter& w, const MsgPickup& m)
{
	w.WriteU16(m.pickupId); w.WriteU16(m.modelIndex);
	w.WriteU8(m.type); w.WriteU16(m.weapon); w.WriteU32(m.quantity);
	w.WriteF32(m.x); w.WriteF32(m.y); w.WriteF32(m.z);
}
inline bool ReadPickup(BitReader& r, MsgPickup& m)
{
	m.pickupId = r.ReadU16(); m.modelIndex = r.ReadU16();
	m.type = r.ReadU8(); m.weapon = r.ReadU16(); m.quantity = r.ReadU32();
	m.x = r.ReadF32(); m.y = r.ReadF32(); m.z = r.ReadF32();
	return r.Ok();
}

inline void WriteWorld(BitWriter& w, const MsgWorld& m) { w.WriteU8(m.hour); w.WriteU8(m.minute); w.WriteU8(m.weather); }
inline bool ReadWorld(BitReader& r, MsgWorld& m) { m.hour = r.ReadU8(); m.minute = r.ReadU8(); m.weather = r.ReadU8(); return r.Ok(); }

inline void WriteScoreUpdate(BitWriter& w, const MsgScoreUpdate& m)
{
	w.WriteU8(m.count);
	for(uint8_t i = 0; i < m.count && i < MAX_SCORE_ENTRIES; i++){
		w.WriteU8(m.entries[i].playerId);
		w.WriteI32(m.entries[i].score);
		w.WriteU16(m.entries[i].kills); w.WriteU16(m.entries[i].deaths);
	}
}
inline bool ReadScoreUpdate(BitReader& r, MsgScoreUpdate& m)
{
	m.count = r.ReadU8();
	if(m.count > MAX_SCORE_ENTRIES){ return false; }
	for(uint8_t i = 0; i < m.count; i++){
		m.entries[i].playerId = r.ReadU8();
		m.entries[i].score = r.ReadI32();
		m.entries[i].kills = r.ReadU16(); m.entries[i].deaths = r.ReadU16();
	}
	return r.Ok();
}

inline void WriteGamemodeEvent(BitWriter& w, const MsgGamemodeEvent& m)
{
	w.WriteU8(m.type); w.WriteU8(m.arg1); w.WriteU8(m.arg2);
	w.WriteString(m.text, MAX_CHAT_LEN - 1);
}
inline bool ReadGamemodeEvent(BitReader& r, MsgGamemodeEvent& m)
{
	m.type = r.ReadU8(); m.arg1 = r.ReadU8(); m.arg2 = r.ReadU8();
	std::string t = r.ReadString(MAX_CHAT_LEN - 1);
	m.len = (uint16_t)t.size();
	CopyStr(m.text, sizeof m.text, t.c_str());
	return r.Ok();
}

// RCON / generic string messages: [u8 type][string]
inline void WriteStringMsg(BitWriter& w, const char* s, uint16_t maxLen) { w.WriteString(s, maxLen); }
inline bool ReadStringMsg(BitReader& r, char* dst, size_t dstSize, uint16_t maxLen)
{
	std::string t = r.ReadString(maxLen);
	CopyStr(dst, dstSize, t.c_str());
	return r.Ok();
}

// ---------------------------------------------------------------------------
// Master codecs
// ---------------------------------------------------------------------------
inline void WriteMasterAnnounce(BitWriter& w, const MsgMasterAnnounce& m)
{
	w.WriteU16(m.gamePort); w.WriteU8(m.players); w.WriteU8(m.maxPlayers); w.WriteU16(m.protocolVersion);
	w.WriteString(m.hostname, MAX_HOSTNAME_LEN - 1);
	w.WriteString(m.gameMode, 31);
}
inline bool ReadMasterAnnounce(BitReader& r, MsgMasterAnnounce& m)
{
	m.gamePort = r.ReadU16(); m.players = r.ReadU8(); m.maxPlayers = r.ReadU8(); m.protocolVersion = r.ReadU16();
	std::string hn = r.ReadString(MAX_HOSTNAME_LEN - 1);
	std::string gm = r.ReadString(31);
	CopyStr(m.hostname, sizeof m.hostname, hn.c_str());
	CopyStr(m.gameMode, sizeof m.gameMode, gm.c_str());
	return r.Ok();
}

inline void WriteMasterEntry(BitWriter& w, const MasterListEntry& e)
{
	w.WriteU32(e.ipv4); w.WriteU16(e.gamePort);
	w.WriteU8(e.players); w.WriteU8(e.maxPlayers); w.WriteU16(e.pingMs);
	w.WriteString(e.hostname, MAX_HOSTNAME_LEN - 1);
	w.WriteString(e.gameMode, 31);
}
inline bool ReadMasterEntry(BitReader& r, MasterListEntry& e)
{
	e.ipv4 = r.ReadU32(); e.gamePort = r.ReadU16();
	e.players = r.ReadU8(); e.maxPlayers = r.ReadU8(); e.pingMs = r.ReadU16();
	std::string hn = r.ReadString(MAX_HOSTNAME_LEN - 1);
	std::string gm = r.ReadString(31);
	CopyStr(e.hostname, sizeof e.hostname, hn.c_str());
	CopyStr(e.gameMode, sizeof e.gameMode, gm.c_str());
	return r.Ok();
}

// ---------------------------------------------------------------------------
// Packet framing: [u8 msgType][payload]
// ---------------------------------------------------------------------------
inline void BeginMsg(BitWriter& w, MsgType type) { w.WriteU8(type); }
inline MsgType PeekType(const uint8_t* data, size_t size)
{
	return size > 0 ? (MsgType)data[0] : (MsgType)0;
}

// ---------------------------------------------------------------------------
// Validation (server-authoritative-lite)
// ---------------------------------------------------------------------------
enum ValFlags : uint8_t {
	VAL_OK          = 0,
	VAL_TOO_FAST    = 1,
	VAL_TELEPORT    = 2,
	VAL_BAD_HEALTH  = 4,
	VAL_BAD_ANIM    = 8,
	VAL_BAD_VEHICLE = 16,
};

inline float DistMm(int32_t ax, int32_t ay, int32_t az, int32_t bx, int32_t by, int32_t bz)
{
	float dx = (ax - bx) / 1000.0f, dy = (ay - by) / 1000.0f, dz = (az - bz) / 1000.0f;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Returns VAL_* bit flags for the step prev -> cur over dt seconds.
inline uint8_t ValidateMove(const PlayerState& prev, const PlayerState& cur, float dt)
{
	uint8_t flags = VAL_OK;
	if(dt <= 0.0f) dt = TICK_DT;

	// a client-flagged teleport is a trusted game-side relocation (respawn,
	// scripted warp): movement checks are skipped (the server rate-limits
	// flagged teleports separately), vitals are still validated below
	if(!(cur.moveFlags & MOVEFLAG_TELEPORT)){
		float dx = (cur.xMm - prev.xMm) / 1000.0f;
		float dy = (cur.yMm - prev.yMm) / 1000.0f;
		float dz = (cur.zMm - prev.zMm) / 1000.0f;
		float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

		bool inVehicle = (cur.vehicleId != 0xFFFF);
		float maxSpeed = inVehicle ? MAX_SPEED_VEHICLE : MAX_SPEED_ONFOOT;

		// falling is legitimate physics (spawns, drops, ragdolls): budget the
		// horizontal and vertical-descent speed separately so a fall never trips
		// the anti-teleport rules (it used to kick real players mid-drop)
		float hSpeed = std::sqrt(dx*dx + dy*dy) / dt;
		float vSpeed = dz / dt;
		float maxFall = inVehicle ? MAX_SPEED_VEHICLE : MAX_SPEED_FALL;

		if(hSpeed > maxSpeed || vSpeed > maxSpeed || vSpeed < -maxFall){
			// distinguish a smooth overspeed from a hard teleport
			if(dist > 50.0f)
				flags |= VAL_TELEPORT;
			else
				flags |= VAL_TOO_FAST;
		}
	}

	// health can only drop (damage is an event); armour never rises here
	if(cur.health > prev.health && prev.health > 0)
		flags |= VAL_BAD_HEALTH;
	if(cur.armour > prev.armour + 10)
		flags |= VAL_BAD_HEALTH;

	return flags;
}

inline uint8_t ValidateVehicleMove(const VehicleState& prev, const VehicleState& cur, float dt)
{
	uint8_t flags = VAL_OK;
	if(dt <= 0.0f) dt = TICK_DT;
	float dist = DistMm(prev.xMm, prev.yMm, prev.zMm, cur.xMm, cur.yMm, cur.zMm);
	if(dist / dt > MAX_SPEED_VEHICLE)
		flags |= (dist > 100.0f) ? VAL_TELEPORT : VAL_TOO_FAST;
	if(cur.health > prev.health + 50 && prev.health > 0)
		flags |= VAL_BAD_HEALTH;
	return flags;
}

} // namespace rnet
