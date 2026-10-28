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

inline void WriteInventory(BitWriter& w, const MsgInventory& m)
{
	w.WriteU16(m.playerId);
	w.WriteI32(m.money);
	w.WriteU8(m.currentWeapon);
	w.WriteU8(m.count);
	for(int i = 0; i < m.count && i < MAX_INV_ENTRIES; i++){
		w.WriteU8(m.entries[i].weapon);
		w.WriteU16(m.entries[i].ammo);
	}
}

inline bool ReadInventory(BitReader& r, MsgInventory& m)
{
	m.playerId = r.ReadU16();
	m.money = r.ReadI32();
	m.currentWeapon = r.ReadU8();
	m.count = r.ReadU8();
	if(m.count > MAX_INV_ENTRIES) return false;
	for(int i = 0; i < m.count; i++){
		m.entries[i].weapon = r.ReadU8();
		m.entries[i].ammo = r.ReadU16();
	}
	return r.Ok();
}

inline void WriteAppearance(BitWriter& w, const MsgAppearance& m)
{
	w.WriteU16(m.playerId);
	w.WriteU16(m.model);
	for(int i = 0; i < MAX_APPEARANCE_COMPONENTS; i++)
		w.WriteU8(m.components[i]);
}

inline bool ReadAppearance(BitReader& r, MsgAppearance& m)
{
	m.playerId = r.ReadU16();
	m.model = r.ReadU16();
	for(int i = 0; i < MAX_APPEARANCE_COMPONENTS; i++)
		m.components[i] = r.ReadU8();
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
	for(int i = 0; i < 6; i++) w.WriteU8(s.doors[i]);
}

inline bool ReadVehicleState(BitReader& r, VehicleState& s)
{
	s.vehicleId = r.ReadU16(); s.modelId = r.ReadU16();
	s.xMm = r.ReadI32(); s.yMm = r.ReadI32(); s.zMm = r.ReadI32();
	s.heading = r.ReadI16(); s.pitch = r.ReadI16(); s.roll = r.ReadI16();
	s.velX = r.ReadI16(); s.velY = r.ReadI16(); s.velZ = r.ReadI16();
	s.steer = r.ReadI8(); s.gas = r.ReadU8(); s.brake = r.ReadU8();
	s.flags = r.ReadU8(); s.health = r.ReadU16();
	for(int i = 0; i < 6; i++) s.doors[i] = r.ReadU8();
	return r.Ok();
}

// ---------------------------------------------------------------------------
// Relayed state delivery (MSG_STATE_BATCH)
//
// A remote player's puppet consumes far less than a full PlayerState: where it
// is, which way it faces, roughly how fast it moves (walk/run anim), its health
// and its vehicle seat. Those are relayed every tick; the rest (pitch, anim
// ids, weapon, ammo, wallet, armour, the velocity vector) stays on
// MSG_PLAYER_STATE.
//
// Positions are centimetre offsets from an anchor the packet carries — the
// receiver's own position — so a 200 m AOI fits signed 16-bit fields. A record
// more than ~327 m outside the anchor could not; it then carries absolute mm
// (RELAY_ABS_POS) instead, which keeps the format correct for any configured
// stream distance and costs the extra 48 bits only on that rare record.
//
// Both codecs are bounded by the compiling constant, never by a wire-supplied
// length: a peer cannot make the reader write past the caller's array.
// ---------------------------------------------------------------------------
constexpr uint8_t RELAY_TELEPORT     = 1;   // MOVEFLAG_TELEPORT
constexpr uint8_t RELAY_INVEHICLE    = 2;   // vehicleId/seat follow the record
constexpr uint8_t RELAY_ABS_POS      = 4;   // absolute mm instead of cm offsets
constexpr int     RELAY_MAX_DELTA_CM = 32767;
constexpr int     RELAY_ANCHOR_BITS  = 16 + 96;   // count + anchor x/y/z in mm

inline bool RelayDeltaCm(int32_t mm, int32_t anchorMm, int16_t& out)
{
	int64_t d = (int64_t)mm - (int64_t)anchorMm;
	// nearest centimetre; halves away from zero
	int64_t cm = d >= 0 ? (d + 5) / 10 : (d - 5) / 10;
	if(cm > RELAY_MAX_DELTA_CM || cm < -RELAY_MAX_DELTA_CM) return false;
	out = (int16_t)cm;
	return true;
}

inline bool RelayPosIsDelta(const PlayerState& s, int32_t ax, int32_t ay, int32_t az)
{
	int16_t d;
	return RelayDeltaCm(s.xMm, ax, d) && RelayDeltaCm(s.yMm, ay, d) &&
	       RelayDeltaCm(s.zMm, az, d);
}

// Encoded size of one record. The server sizes packets with this before
// writing them, so the split and the codec cannot disagree about a record.
inline int RelayEntryBits(const PlayerState& s, int32_t ax, int32_t ay, int32_t az)
{
	int bits = 12 + 4 + 8 + 8 + 7;                    // id, flags, heading, speed, health
	bits += RelayPosIsDelta(s, ax, ay, az) ? 48 : 96;
	if(s.vehicleId != VEHICLE_NONE) bits += 16 + 3;   // vehicleId, seat
	return bits;
}

inline void WriteRelayState(BitWriter& w, const PlayerState& s,
                            int32_t ax, int32_t ay, int32_t az)
{
	int16_t dx = 0, dy = 0, dz = 0;
	// bitwise & so all three offsets are decoded, not just up to the first
	// one that overflows the delta range
	bool delta = RelayDeltaCm(s.xMm, ax, dx) & RelayDeltaCm(s.yMm, ay, dy) &
	             RelayDeltaCm(s.zMm, az, dz);
	uint8_t flags = 0;
	if(s.moveFlags & MOVEFLAG_TELEPORT) flags |= RELAY_TELEPORT;
	if(s.vehicleId != VEHICLE_NONE)     flags |= RELAY_INVEHICLE;
	if(!delta)                          flags |= RELAY_ABS_POS;

	w.WriteBits((uint32_t)s.playerId & 0x0FFF, 12);
	w.WriteBits(flags, 4);
	if(delta){
		w.WriteI16(dx); w.WriteI16(dy); w.WriteI16(dz);
	}else{
		w.WriteI32(s.xMm); w.WriteI32(s.yMm); w.WriteI32(s.zMm);
	}
	w.WriteBits((uint32_t)((uint16_t)s.heading >> 8), 8);   // 1.4 deg steps
	// horizontal speed in 0.2 m/s steps; the direction is not relayed because
	// a puppet only uses the magnitude, to pick idle/walk/run
	float vx = DequantVel(s.velX), vy = DequantVel(s.velY);
	long spd = std::lround(std::sqrt(vx * vx + vy * vy) * 5.0f);
	if(spd < 0) spd = 0;
	if(spd > 255) spd = 255;
	w.WriteBits((uint32_t)spd, 8);
	w.WriteBits(s.health > 127 ? 127u : (uint32_t)s.health, 7);
	if(flags & RELAY_INVEHICLE){
		w.WriteU16(s.vehicleId);
		w.WriteBits(s.seat == 0xFF ? 7u : (uint32_t)(s.seat & 7), 3);
	}
}

inline bool ReadRelayState(BitReader& r, int32_t ax, int32_t ay, int32_t az, PlayerState& s)
{
	s = PlayerState{};      // relay records are sparse, not full snapshots
	s.playerId = (PlayerId)r.ReadBits(12);
	uint32_t flags = r.ReadBits(4);
	if(flags & RELAY_ABS_POS){
		s.xMm = r.ReadI32(); s.yMm = r.ReadI32(); s.zMm = r.ReadI32();
	}else{
		s.xMm = ax + (int32_t)r.ReadI16() * 10;
		s.yMm = ay + (int32_t)r.ReadI16() * 10;
		s.zMm = az + (int32_t)r.ReadI16() * 10;
	}
	s.heading = (int16_t)(r.ReadBits(8) << 8);
	// no velocity vector on the wire: keep the horizontal speed in velX (dm/s),
	// the form every puppet's anim-speed test already reads
	s.velX = (int16_t)(r.ReadBits(8) * 2);
	s.health = (uint8_t)r.ReadBits(7);
	if(flags & RELAY_TELEPORT) s.moveFlags = MOVEFLAG_TELEPORT;
	if(flags & RELAY_INVEHICLE){
		s.vehicleId = r.ReadU16();
		uint32_t seat = r.ReadBits(3);
		s.seat = seat == 7 ? 0xFF : (uint8_t)seat;
	}
	return r.Ok();
}

inline void WriteStateBatch(BitWriter& w, const PlayerState* states, int count,
                            int32_t ax, int32_t ay, int32_t az)
{
	int n = count;
	if(n < 0) n = 0;
	if(n > MAX_STATES_PER_PACKET) n = MAX_STATES_PER_PACKET;
	w.WriteU16((uint16_t)n);
	w.WriteI32(ax); w.WriteI32(ay); w.WriteI32(az);
	for(int i = 0; i < n; i++) WriteRelayState(w, states[i], ax, ay, az);
}

// count written to `out` (cap entries), or -1 when the frame is malformed.
inline int ReadStateBatch(BitReader& r, PlayerState* out, int cap)
{
	uint16_t n = r.ReadU16();
	if(!r.Ok() || n > MAX_STATES_PER_PACKET || (int)n > cap) return -1;
	int32_t ax = r.ReadI32(), ay = r.ReadI32(), az = r.ReadI32();
	if(!r.Ok()) return -1;
	for(uint16_t i = 0; i < n; i++){
		if(!ReadRelayState(r, ax, ay, az, out[i])) return -1;
	}
	return (int)n;
}

inline void WriteRoster(BitWriter& w, const MsgRosterEntry* entries, int count)
{
	int n = count;
	if(n < 0) n = 0;
	if(n > MAX_ROSTER_PER_PACKET) n = MAX_ROSTER_PER_PACKET;
	w.WriteU16((uint16_t)n);
	for(int i = 0; i < n; i++){
		const MsgRosterEntry& e = entries[i];
		w.WriteU16(e.id); w.WriteU16(e.skin); w.WriteU8(e.flags);
		w.WriteU8(e.health); w.WriteU8(e.armour);
		w.WriteI32(e.xMm); w.WriteI32(e.yMm); w.WriteI32(e.zMm);
		w.WriteI16(e.heading);
		w.WriteString(e.name, MAX_NAME_LEN - 1);
	}
}

// count written to `out` (cap entries), or -1 when the frame is malformed.
inline int ReadRoster(BitReader& r, MsgRosterEntry* out, int cap)
{
	uint16_t n = r.ReadU16();
	if(!r.Ok() || n > MAX_ROSTER_PER_PACKET || (int)n > cap) return -1;
	for(uint16_t i = 0; i < n; i++){
		MsgRosterEntry& e = out[i];
		e = MsgRosterEntry();
		e.id = r.ReadU16(); e.skin = r.ReadU16(); e.flags = r.ReadU8();
		e.health = r.ReadU8(); e.armour = r.ReadU8();
		e.xMm = r.ReadI32(); e.yMm = r.ReadI32(); e.zMm = r.ReadI32();
		e.heading = r.ReadI16();
		std::string nm = r.ReadString(MAX_NAME_LEN - 1);
		CopyStr(e.name, sizeof e.name, nm.c_str());
		if(!r.Ok()) return -1;
	}
	return (int)n;
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
	w.WriteU16(m.playerId); w.WriteU16(m.tickRate); w.WriteF32(m.streamDistance);
	w.WriteU8(m.hour); w.WriteU8(m.minute); w.WriteU8(m.weather);
	w.WriteString(m.gameMode, 31);
	w.WriteString(m.hostname, MAX_HOSTNAME_LEN - 1);
	// v5: live server info (launcher tooltip: map, players)
	w.WriteU16(m.players); w.WriteU16(m.maxPlayers);
	w.WriteString(m.mapName, MAX_MAP_LEN - 1);
}
inline bool ReadWelcome(BitReader& r, MsgWelcome& m)
{
	m.playerId = r.ReadU16(); m.tickRate = r.ReadU16(); m.streamDistance = r.ReadF32();
	m.hour = r.ReadU8(); m.minute = r.ReadU8(); m.weather = r.ReadU8();
	std::string gm = r.ReadString(31);
	std::string hn = r.ReadString(MAX_HOSTNAME_LEN - 1);
	CopyStr(m.gameMode, sizeof m.gameMode, gm.c_str());
	CopyStr(m.hostname, sizeof m.hostname, hn.c_str());
	// v5 tail: tolerate a v4 server that stops after the hostname
	m.players = m.maxPlayers = 0;
	m.mapName[0] = 0;
	if(r.BitsLeft() >= 16 + 16){
		m.players = r.ReadU16();
		m.maxPlayers = r.ReadU16();
		if(r.BitsLeft() >= 16){
			std::string mp = r.ReadString(MAX_MAP_LEN - 1);
			CopyStr(m.mapName, sizeof m.mapName, mp.c_str());
		}
	}
	return r.Ok();
}

inline void WritePlayerJoin(BitWriter& w, const MsgPlayerJoin& m)
{
	w.WriteU16(m.playerId); w.WriteU16(m.skinModel);
	w.WriteString(m.name, MAX_NAME_LEN - 1);
}
inline bool ReadPlayerJoin(BitReader& r, MsgPlayerJoin& m)
{
	m.playerId = r.ReadU16(); m.skinModel = r.ReadU16();
	std::string n = r.ReadString(MAX_NAME_LEN - 1);
	CopyStr(m.name, sizeof m.name, n.c_str());
	return r.Ok();
}

inline void WriteChat(BitWriter& w, const MsgChat& m)
{
	w.WriteU16(m.fromId);
	w.WriteString(m.text, MAX_CHAT_LEN - 1);
}
inline bool ReadChat(BitReader& r, MsgChat& m)
{
	m.fromId = r.ReadU16();
	std::string t = r.ReadString(MAX_CHAT_LEN - 1);
	m.len = (uint16_t)t.size();
	CopyStr(m.text, sizeof m.text, t.c_str());
	return r.Ok();
}

inline void WriteSpawn(BitWriter& w, const MsgSpawn& m)
{
	w.WriteU16(m.playerId); w.WriteU16(m.skinModel);
	w.WriteF32(m.x); w.WriteF32(m.y); w.WriteF32(m.z); w.WriteF32(m.heading);
	w.WriteU8(m.health); w.WriteU8(m.armour);
}
inline bool ReadSpawn(BitReader& r, MsgSpawn& m)
{
	m.playerId = r.ReadU16(); m.skinModel = r.ReadU16();
	m.x = r.ReadF32(); m.y = r.ReadF32(); m.z = r.ReadF32(); m.heading = r.ReadF32();
	m.health = r.ReadU8(); m.armour = r.ReadU8();
	return r.Ok();
}

inline void WriteDeath(BitWriter& w, const MsgDeath& m)
{
	w.WriteU16(m.victimId); w.WriteU16(m.killerId); w.WriteU8(m.weaponId);
}
inline bool ReadDeath(BitReader& r, MsgDeath& m)
{
	m.victimId = r.ReadU16(); m.killerId = r.ReadU16(); m.weaponId = r.ReadU8();
	return r.Ok();
}

inline void WriteDamage(BitWriter& w, const MsgDamage& m)
{
	w.WriteU16(m.targetId); w.WriteU16(m.attackerId); w.WriteU8(m.weaponId); w.WriteU16(m.damage);
}
inline bool ReadDamage(BitReader& r, MsgDamage& m)
{
	m.targetId = r.ReadU16(); m.attackerId = r.ReadU16(); m.weaponId = r.ReadU8(); m.damage = r.ReadU16();
	return r.Ok();
}

inline void WriteGive(BitWriter& w, const MsgGive& m)
{
	w.WriteU16(m.targetId); w.WriteU8(m.kind);
	w.WriteI32(m.amount); w.WriteU16(m.arg);
}
inline bool ReadGive(BitReader& r, MsgGive& m)
{
	m.targetId = r.ReadU16(); m.kind = r.ReadU8();
	m.amount = r.ReadI32(); m.arg = r.ReadU16();
	return r.Ok();
}

inline void WriteKick(BitWriter& w, const MsgKick& m)
{
	w.WriteString(m.reason, MAX_REASON_LEN - 1);
	w.WriteU8(m.code);            // JoinRefused; 0 for generic
}
inline bool ReadKick(BitReader& r, MsgKick& m)
{
	std::string t = r.ReadString(MAX_REASON_LEN - 1);
	m.len = (uint16_t)t.size();
	CopyStr(m.reason, sizeof m.reason, t.c_str());
	m.code = 0;                   // a pre-code peer stops after the reason
	if(r.BitsLeft() >= 8)
		m.code = r.ReadU8();
	return r.Ok();
}

inline void WriteVehEnter(BitWriter& w, const MsgVehEnter& m)
{
	w.WriteU16(m.playerId); w.WriteU16(m.vehicleId); w.WriteU8(m.seat); w.WriteU16(m.modelId);
}
inline bool ReadVehEnter(BitReader& r, MsgVehEnter& m)
{
	m.playerId = r.ReadU16(); m.vehicleId = r.ReadU16(); m.seat = r.ReadU8(); m.modelId = r.ReadU16();
	return r.Ok();
}

inline void WriteVehExit(BitWriter& w, const MsgVehExit& m) { w.WriteU16(m.playerId); w.WriteU16(m.vehicleId); }
inline bool ReadVehExit(BitReader& r, MsgVehExit& m) { m.playerId = r.ReadU16(); m.vehicleId = r.ReadU16(); return r.Ok(); }

inline void WriteVehSpawn(BitWriter& w, const MsgVehSpawn& m)
{
	w.WriteU16(m.vehicleId); w.WriteU16(m.modelId);
	w.WriteF32(m.x); w.WriteF32(m.y); w.WriteF32(m.z); w.WriteF32(m.heading);
	w.WriteU16(m.ownerId);
}
inline bool ReadVehSpawn(BitReader& r, MsgVehSpawn& m)
{
	m.vehicleId = r.ReadU16(); m.modelId = r.ReadU16();
	m.x = r.ReadF32(); m.y = r.ReadF32(); m.z = r.ReadF32(); m.heading = r.ReadF32();
	m.ownerId = r.ReadU16();
	return r.Ok();
}

inline void WriteVehDespawn(BitWriter& w, const MsgVehDespawn& m) { w.WriteU16(m.vehicleId); }
inline bool ReadVehDespawn(BitReader& r, MsgVehDespawn& m)
{
	m.vehicleId = r.ReadU16();
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

// MSG_VOICE payload: [u8 fromId][u8 len][len x u8 Opus packet]. The client
// sends its own id (ignored: the server stamps the real sender). `packet` must
// hold VOICE_MAX_PACKET bytes.
inline void WriteVoice(BitWriter& w, PlayerId fromId, const uint8_t* packet, uint8_t len)
{
	w.WriteU16(fromId);
	w.WriteU8(len);
	for(int i = 0; i < len; i++) w.WriteU8(packet[i]);
}
// false on a short read or a length outside 1..VOICE_MAX_PACKET
inline bool ReadVoice(BitReader& r, PlayerId& fromId, uint8_t* packet, uint8_t& len)
{
	fromId = r.ReadU16();
	len = r.ReadU8();
	if(!r.Ok() || len == 0 || len > VOICE_MAX_PACKET) return false;
	for(int i = 0; i < len; i++) packet[i] = r.ReadU8();
	return r.Ok();
}

inline void WriteWorld(BitWriter& w, const MsgWorld& m) { w.WriteU8(m.hour); w.WriteU8(m.minute); w.WriteU8(m.weather); }
inline bool ReadWorld(BitReader& r, MsgWorld& m) { m.hour = r.ReadU8(); m.minute = r.ReadU8(); m.weather = r.ReadU8(); return r.Ok(); }

inline void WriteResList(BitWriter& w, const MsgResList& m)
{
	w.WriteU8(m.count);
	for(uint8_t i = 0; i < m.count && i < MAX_RES_ENTRIES; i++){
		w.WriteString(m.entries[i].name, sizeof m.entries[i].name - 1);
		w.WriteString(m.entries[i].version, sizeof m.entries[i].version - 1);
		w.WriteU8(m.entries[i].type);
		w.WriteU8(m.entries[i].enabled);
		w.WriteU8(m.entries[i].valid);
	}
}
inline bool ReadResList(BitReader& r, MsgResList& m)
{
	m = MsgResList();
	m.count = r.ReadU8();
	if(m.count > MAX_RES_ENTRIES) return false;
	for(uint8_t i = 0; i < m.count; i++){
		std::string n = r.ReadString(sizeof m.entries[i].name - 1);
		std::string v = r.ReadString(sizeof m.entries[i].version - 1);
		CopyStr(m.entries[i].name, sizeof m.entries[i].name, n.c_str());
		CopyStr(m.entries[i].version, sizeof m.entries[i].version, v.c_str());
		m.entries[i].type = r.ReadU8();
		m.entries[i].enabled = r.ReadU8();
		m.entries[i].valid = r.ReadU8();
	}
	return r.Ok();
}

inline void WriteScoreUpdate(BitWriter& w, const MsgScoreUpdate& m)
{
	w.WriteU8(m.count);
	for(uint8_t i = 0; i < m.count && i < MAX_SCORE_ENTRIES; i++){
		w.WriteU16(m.entries[i].playerId);
		w.WriteI32(m.entries[i].score);
		w.WriteU16(m.entries[i].kills); w.WriteU16(m.entries[i].deaths);
	}
}
inline bool ReadScoreUpdate(BitReader& r, MsgScoreUpdate& m)
{
	m.count = r.ReadU8();
	if(m.count > MAX_SCORE_ENTRIES){ return false; }
	for(uint8_t i = 0; i < m.count; i++){
		m.entries[i].playerId = r.ReadU16();
		m.entries[i].score = r.ReadI32();
		m.entries[i].kills = r.ReadU16(); m.entries[i].deaths = r.ReadU16();
	}
	return r.Ok();
}

inline void WritePlayerPing(BitWriter& w, const MsgPlayerPing& m)
{
	uint8_t n = m.count < MAX_PING_ENTRIES ? m.count : (uint8_t)MAX_PING_ENTRIES;
	w.WriteU8(n);
	for(uint8_t i = 0; i < n; i++){
		w.WriteU16(m.entries[i].playerId);
		w.WriteU16(m.entries[i].pingMs);
		w.WriteI32(m.entries[i].score);
		w.WriteU16(m.entries[i].kills); w.WriteU16(m.entries[i].deaths);
	}
}
inline bool ReadPlayerPing(BitReader& r, MsgPlayerPing& m)
{
	m.count = r.ReadU8();
	if(m.count > MAX_PING_ENTRIES){ return false; }
	for(uint8_t i = 0; i < m.count; i++){
		m.entries[i].playerId = r.ReadU16();
		m.entries[i].pingMs = r.ReadU16();
		m.entries[i].score = r.ReadI32();
		m.entries[i].kills = r.ReadU16(); m.entries[i].deaths = r.ReadU16();
	}
	return r.Ok();
}

inline void WriteGamemodeEvent(BitWriter& w, const MsgGamemodeEvent& m)
{
	w.WriteU8(m.type); w.WriteU16(m.arg1); w.WriteU16(m.arg2);
	w.WriteString(m.text, MAX_CHAT_LEN - 1);
}
inline bool ReadGamemodeEvent(BitReader& r, MsgGamemodeEvent& m)
{
	m.type = r.ReadU8(); m.arg1 = r.ReadU16(); m.arg2 = r.ReadU16();
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
// EOS Anti-Cheat relay codecs
//
// Payloads are treated as opaque bytes: the length is checked against the
// frame on the wire, never trusted from the peer for bounds.
// ---------------------------------------------------------------------------
inline void WriteAcHello(BitWriter& w, const MsgAcHello& m)
{
	w.WriteU8(m.platform);
	w.WriteString(m.productUserId, AC_PUID_LEN - 1);
	w.WriteString(m.idToken, AC_TOKEN_LEN - 1);
}
inline bool ReadAcHello(BitReader& r, MsgAcHello& m)
{
	m = MsgAcHello();
	m.platform = r.ReadU8();
	std::string puid = r.ReadString(AC_PUID_LEN - 1);
	std::string token = r.ReadString(AC_TOKEN_LEN - 1);
	CopyStr(m.productUserId, sizeof m.productUserId, puid.c_str());
	CopyStr(m.idToken, sizeof m.idToken, token.c_str());
	return r.Ok();
}
inline void WriteAcStatus(BitWriter& w, const MsgAcStatus& m)
{
	w.WriteU8(m.status);
	w.WriteU8(m.registered);
	w.WriteString(m.detail, MAX_REASON_LEN - 1);
}
inline bool ReadAcStatus(BitReader& r, MsgAcStatus& m)
{
	m = MsgAcStatus();
	m.status = r.ReadU8();
	m.registered = r.ReadU8();
	std::string d = r.ReadString(MAX_REASON_LEN - 1);
	CopyStr(m.detail, sizeof m.detail, d.c_str());
	return r.Ok();
}
inline bool WriteAcPayload(BitWriter& w, const MsgAcPayload& m)
{
	if(m.len > MAX_AC_PAYLOAD) return false;
	w.WriteU16(m.len);
	for(uint16_t i = 0; i < m.len; i++) w.WriteU8(m.data[i]);
	return true;
}
inline bool ReadAcPayload(BitReader& r, MsgAcPayload& m)
{
	m = MsgAcPayload();
	uint16_t len = r.ReadU16();
	// the frame must actually carry what the header claims: a truncated or
	// oversized message is dropped instead of read past its end
	if(len > MAX_AC_PAYLOAD) return false;
	if(r.BitsLeft() < (int)len * 8) return false;
	m.len = len;
	for(uint16_t i = 0; i < len; i++) m.data[i] = r.ReadU8();
	return r.Ok();
}

// ---------------------------------------------------------------------------
// Master codecs
// ---------------------------------------------------------------------------
inline void WriteMasterAnnounce(BitWriter& w, const MsgMasterAnnounce& m)
{
	w.WriteU16(m.gamePort); w.WriteU16(m.players); w.WriteU16(m.maxPlayers); w.WriteU16(m.protocolVersion);
	w.WriteString(m.hostname, MAX_HOSTNAME_LEN - 1);
	w.WriteString(m.gameMode, 31);
	w.WriteString(m.mapName, MAX_MAP_LEN - 1);   // v5
	w.WriteU32(m.advertisedIp);
}
inline bool ReadMasterAnnounce(BitReader& r, MsgMasterAnnounce& m)
{
	m.gamePort = r.ReadU16(); m.players = r.ReadU16(); m.maxPlayers = r.ReadU16(); m.protocolVersion = r.ReadU16();
	std::string hn = r.ReadString(MAX_HOSTNAME_LEN - 1);
	std::string gm = r.ReadString(31);
	CopyStr(m.hostname, sizeof m.hostname, hn.c_str());
	CopyStr(m.gameMode, sizeof m.gameMode, gm.c_str());
	// optional tails, oldest first: a v4 announce has only advertisedIp (32 bits)
	// or nothing left, so the map is only read when a v5 tail actually fits
	m.mapName[0] = 0;
	if(r.Ok() && r.BitsLeft() >= 48){
		std::string mp = r.ReadString(MAX_MAP_LEN - 1);
		CopyStr(m.mapName, sizeof m.mapName, mp.c_str());
	}
	m.advertisedIp = (r.Ok() && r.BitsLeft() >= 32) ? r.ReadU32() : 0;
	return r.Ok();
}

inline void WriteMasterEntry(BitWriter& w, const MasterListEntry& e)
{
	w.WriteU32(e.ipv4); w.WriteU16(e.gamePort);
	w.WriteU16(e.players); w.WriteU16(e.maxPlayers); w.WriteU16(e.pingMs);
	w.WriteString(e.hostname, MAX_HOSTNAME_LEN - 1);
	w.WriteString(e.gameMode, 31);
	w.WriteString(e.mapName, MAX_MAP_LEN - 1);   // v5
	w.WriteU16(e.protocolVersion);               // v5
}
inline bool ReadMasterEntry(BitReader& r, MasterListEntry& e)
{
	e.ipv4 = r.ReadU32(); e.gamePort = r.ReadU16();
	e.players = r.ReadU16(); e.maxPlayers = r.ReadU16(); e.pingMs = r.ReadU16();
	std::string hn = r.ReadString(MAX_HOSTNAME_LEN - 1);
	std::string gm = r.ReadString(31);
	CopyStr(e.hostname, sizeof e.hostname, hn.c_str());
	CopyStr(e.gameMode, sizeof e.gameMode, gm.c_str());
	// v5 tails; a v4 entry ends right here
	e.mapName[0] = 0;
	e.protocolVersion = 0;
	if(r.Ok() && r.BitsLeft() >= 16){
		std::string mp = r.ReadString(MAX_MAP_LEN - 1);
		CopyStr(e.mapName, sizeof e.mapName, mp.c_str());
		if(r.Ok() && r.BitsLeft() >= 16) e.protocolVersion = r.ReadU16();
	}
	return r.Ok();
}

// ---------------------------------------------------------------------------
// Master favorites codecs
// ---------------------------------------------------------------------------
inline void WriteFavReq(BitWriter& w, const MsgFavReq& m) { w.WriteString(m.nick, MAX_NAME_LEN - 1); }
inline bool ReadFavReq(BitReader& r, MsgFavReq& m)
{
	std::string n = r.ReadString(MAX_NAME_LEN - 1);
	CopyStr(m.nick, sizeof m.nick, n.c_str());
	return r.Ok();
}

inline void WriteFavAdd(BitWriter& w, const MsgFavAdd& m)
{
	w.WriteString(m.nick, MAX_NAME_LEN - 1);
	w.WriteString(m.address, MAX_ADDR_LEN - 1);
	w.WriteString(m.name, MAX_HOSTNAME_LEN - 1);
}
inline bool ReadFavAdd(BitReader& r, MsgFavAdd& m)
{
	std::string n = r.ReadString(MAX_NAME_LEN - 1);
	std::string a = r.ReadString(MAX_ADDR_LEN - 1);
	std::string h = r.ReadString(MAX_HOSTNAME_LEN - 1);
	CopyStr(m.nick, sizeof m.nick, n.c_str());
	CopyStr(m.address, sizeof m.address, a.c_str());
	CopyStr(m.name, sizeof m.name, h.c_str());
	return r.Ok();
}

inline void WriteFavResp(BitWriter& w, const MsgFavResp& m)
{
	w.WriteU8(m.ok);
	w.WriteU8(m.count > MAX_FAV_ENTRIES ? (uint8_t)MAX_FAV_ENTRIES : m.count);
	w.WriteString(m.error, sizeof m.error - 1);
	uint8_t n = m.count > MAX_FAV_ENTRIES ? (uint8_t)MAX_FAV_ENTRIES : m.count;
	for(uint8_t i = 0; i < n; i++){
		w.WriteString(m.entries[i].address, MAX_ADDR_LEN - 1);
		w.WriteString(m.entries[i].name, MAX_HOSTNAME_LEN - 1);
	}
}
inline bool ReadFavResp(BitReader& r, MsgFavResp& m)
{
	m = MsgFavResp();
	m.ok = r.ReadU8();
	m.count = r.ReadU8();
	std::string err = r.ReadString(sizeof m.error - 1);
	CopyStr(m.error, sizeof m.error, err.c_str());
	if(m.count > MAX_FAV_ENTRIES) m.count = (uint8_t)MAX_FAV_ENTRIES;
	for(uint8_t i = 0; i < m.count; i++){
		std::string a = r.ReadString(MAX_ADDR_LEN - 1);
		std::string h = r.ReadString(MAX_HOSTNAME_LEN - 1);
		CopyStr(m.entries[i].address, sizeof m.entries[i].address, a.c_str());
		CopyStr(m.entries[i].name, sizeof m.entries[i].name, h.c_str());
	}
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
