// src/net/rnet_validator.h — tough server-side validation surface.
//
// Header-only so the unit tests exercise exactly the code the server runs.
// Everything here is validation the server applies IN ADDITION to
// ValidateMove's movement budgets (rnet_protocol.h):
//   * payload sanity   — finite values, world bounds, enum ranges, hard caps
//   * rate limiting    — token buckets per message class (flood defence)
//   * chat spam        — sustained/burst limits + control-character refusal
//   * economy tracking — wallet may only grow by server-granted amounts
//   * name sanitation  — printable ASCII, bounded, never empty
#pragma once

#include "rnet_common.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace rnet {

// ---------------------------------------------------------------------------
// tunables
// ---------------------------------------------------------------------------
constexpr float    WORLD_LIMIT_XY   = 2500.0f;    // metres from origin
constexpr float    WORLD_LIMIT_Z    = 1200.0f;    // metres altitude
constexpr int32_t  MONEY_MAX        = 100000000;  // $100m hard wallet cap
constexpr uint16_t AMMO_MAX         = 9999;
constexpr uint8_t  WEAPON_MAX_ID    = 41;         // WEAPONTYPE_CAMERA
constexpr int      VIOLATION_KICK   = 40;

// ---------------------------------------------------------------------------
// token-bucket rate limiter (sustained rate + burst)
// ---------------------------------------------------------------------------
struct RateLimiter {
	float  tokens    = 0.0f;
	float  burst     = 80.0f;
	float  perSec    = 60.0f;
	double lastRefill = -1.0;

	void Reset(double now, float rate, float b)
	{
		perSec = rate;
		burst = b;
		tokens = b;
		lastRefill = now;
	}

	bool Allow(double now, float cost = 1.0f)
	{
		if(lastRefill < 0.0){
			lastRefill = now;
			tokens = burst;
		}
		if(now < lastRefill) lastRefill = now; // clock went backwards
		float dt = (float)(now - lastRefill);
		lastRefill = now;
		tokens += dt * perSec;
		if(tokens > burst) tokens = burst;
		if(tokens < cost) return false;
		tokens -= cost;
		return true;
	}
};

// ---------------------------------------------------------------------------
// payload sanity — reject structurally-decodable but impossible states
// before anything else touches them
// ---------------------------------------------------------------------------
inline bool ValidateStateSanity(const PlayerState& st)
{
	const int32_t limXY = (int32_t)(WORLD_LIMIT_XY * 1000.0f);
	const int32_t limZ  = (int32_t)(WORLD_LIMIT_Z * 1000.0f);
	if(st.xMm < -limXY || st.xMm > limXY) return false;
	if(st.yMm < -limXY || st.yMm > limXY) return false;
	if(st.zMm < -limZ  || st.zMm > limZ)  return false;
	if(st.weapon > WEAPON_MAX_ID) return false;
	if(st.health > 100) return false;
	if(st.armour > 100) return false;
	if(st.money < 0 || st.money > MONEY_MAX) return false;
	// velocities decode to float; hostile quantized input must never yield inf/nan
	float vx = DequantVel(st.velX), vy = DequantVel(st.velY), vz = DequantVel(st.velZ);
	if(!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz)) return false;
	return true;
}

inline uint16_t ClampAmmo(uint16_t ammo)
{
	return ammo > AMMO_MAX ? AMMO_MAX : ammo;
}

// ---------------------------------------------------------------------------
// chat spam + hygiene
// ---------------------------------------------------------------------------
inline bool ValidateChat(const char* text, RateLimiter& lim, double now)
{
	if(!text || !text[0]) return false;
	size_t n = std::strlen(text);
	if(n >= MAX_CHAT_LEN) return false;          // over codec cap = malformed
	for(size_t i = 0; i < n; i++)
		if((unsigned char)text[i] < 0x20) return false; // control chars break HUD/log
	return lim.Allow(now);
}

// ---------------------------------------------------------------------------
// name sanitation — printable ASCII only, bounded, never empty
// ---------------------------------------------------------------------------
inline void SanitizeName(char* dst, size_t dstLen, const char* src)
{
	if(!dst || dstLen == 0) return;
	size_t o = 0;
	bool pendingSpace = false;
	for(size_t i = 0; src && src[i] && o + 1 < dstLen; i++){
		unsigned char c = (unsigned char)src[i];
		if(c < 0x20 || c > 0x7E) continue;        // drop control/non-ascii
		if(c == ' '){ pendingSpace = (o > 0); continue; } // collapse spaces
		if(pendingSpace && o + 1 < dstLen) dst[o++] = ' ';
		pendingSpace = false;
		dst[o++] = (char)c;
	}
	dst[o] = 0;
	if(!dst[0]){
		dst[0] = 'P'; dst[1] = 'l'; dst[2] = 'a'; dst[3] = 'y';
		dst[4] = 'e'; dst[5] = 'r'; dst[6] = 0;
	}
}

// ---------------------------------------------------------------------------
// server-tracked economy — the wallet may only grow by amounts the server
// granted (admin give, gamemode rewards). Anything else is a cheat client
// editing its own money: the gain is clamped away and flagged.
// ---------------------------------------------------------------------------
inline int32_t ValidateMoney(int32_t reported, int32_t& seen, int32_t& granted, bool& cheated)
{
	cheated = false;
	if(reported < 0 || reported > MONEY_MAX){
		cheated = true;
		return seen;
	}
	int32_t gain = reported - seen;
	if(gain > 0){
		if(gain > granted){
			cheated = true;
			granted = 0;
			return seen;               // clamp: no unearned money
		}
		granted -= gain;
	}
	seen = reported;
	return reported;
}

} // namespace rnet
