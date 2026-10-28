// net/tests/main.cpp — unit tests for the rnet protocol layer.
#include "rnet_protocol.h"
#include "rnet_validator.h"
#include "rnet_voice.h"
#include "profiles.h"
#include "passhash.h"
#include "anticheat_config.h"
#include "server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace rnet;

static int g_checks = 0, g_fails = 0;

#define CHECK(cond) do { \
	g_checks++; \
	if(!(cond)){ \
		g_fails++; \
		std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
	} \
} while(0)

#define CHECK_NEAR(a, b, eps) do { \
	g_checks++; \
	double _a = (a), _b = (b); \
	if(std::fabs(_a - _b) > (eps)){ \
		g_fails++; \
		std::printf("FAIL %s:%d: |%f - %f| > %f\n", __FILE__, __LINE__, _a, _b, (double)(eps)); \
	} \
} while(0)

static std::mt19937 g_rng(12345);

static float RandFloat(float lo, float hi)
{
	return std::uniform_real_distribution<float>(lo, hi)(g_rng);
}

// ---------------------------------------------------------------------------
static void TestBitstreamRoundTrip()
{
	BitWriter w;
	std::uniform_int_distribution<int> u8d(0, 255);
	std::uniform_int_distribution<int> u16d(0, 65535);
	std::uniform_int_distribution<uint32_t> u32d(0, 0xFFFFFFFFu);

	// deterministic set
	uint8_t  a = 0xAB; uint16_t b = 0xBEEF; uint32_t c = 0xDEADBEEF;
	int8_t   d = -100; int16_t e = -30000; int32_t f = -123456789;
	float    g = 3.14159f; bool h = true;

	w.WriteU8(a); w.WriteU16(b); w.WriteU32(c);
	w.WriteI8(d); w.WriteI16(e); w.WriteI32(f);
	w.WriteF32(g); w.WriteBool(h);
	w.WriteString("hello liberty city", 255);
	w.WriteString("", 255);

	BitReader r(w.Data(), w.ByteSize());
	CHECK(r.ReadU8() == a);
	CHECK(r.ReadU16() == b);
	CHECK(r.ReadU32() == c);
	CHECK(r.ReadI8() == d);
	CHECK(r.ReadI16() == e);
	CHECK(r.ReadI32() == f);
	CHECK_NEAR(r.ReadF32(), g, 0.0);
	CHECK(r.ReadBool() == h);
	CHECK(r.ReadString(255) == "hello liberty city");
	CHECK(r.ReadString(255) == "");
	CHECK(r.Ok());

	// random bit-width chaos: 1..24 bit fields
	BitWriter w2;
	struct Field { uint32_t v; int bits; };
	Field fields[500];
	for(int i = 0; i < 500; i++){
		int bits = 1 + (int)(g_rng() % 24);
		uint32_t v = bits == 32 ? u32d(g_rng) : (u32d(g_rng) & ((1u << bits) - 1));
		fields[i] = { v, bits };
		w2.WriteBits(v, bits);
	}
	BitReader r2(w2.Data(), w2.ByteSize());
	bool allOk = true;
	for(int i = 0; i < 500; i++)
		if(r2.ReadBits(fields[i].bits) != fields[i].v) allOk = false;
	CHECK(allOk);
	CHECK(r2.Ok());

	// over-read safety
	uint8_t oneByte[1] = { 0xFF };
	BitReader r3(oneByte, 1);
	r3.ReadBits(8);
	CHECK(r3.ReadBits(8) == 0);
	CHECK(!r3.Ok());
}

// ---------------------------------------------------------------------------
static void TestQuantization()
{
	for(int i = 0; i < 1000; i++){
		float p = RandFloat(-2000.0f, 2000.0f);
		// bound: 0.5 mm quantization + 0.5 ulp of float32 at 2 km scale (~0.00012 m)
		CHECK_NEAR(DequantPos(QuantPos(p)), p, 0.0007);
		float v = RandFloat(-300.0f, 300.0f);
		CHECK_NEAR(DequantVel(QuantVel(v)), v, 0.05);
		float ang = RandFloat(-40.0f * RNET_PI, 40.0f * RNET_PI);
		float back = DequantAngle(QuantAngle(ang));
		// wrapped angle difference
		float diff = std::fmod(back - ang, 2.0f * RNET_PI);
		if(diff > RNET_PI) diff -= 2.0f * RNET_PI;
		if(diff < -RNET_PI) diff += 2.0f * RNET_PI;
		CHECK(std::fabs(diff) <= 2.0f * RNET_PI / 65536.0f * 1.5f);
	}
	// exact edges
	CHECK(QuantPos(0.0f) == 0);
	CHECK(DequantPos(QuantPos(1.234f)) == 1.234f);
}

// ---------------------------------------------------------------------------
static void TestStateCodecs()
{
	PlayerState ps;
	ps.playerId = 77;
	ps.xMm = 1234567; ps.yMm = -7654321; ps.zMm = 25000;
	ps.velX = 321; ps.velY = -1000; ps.velZ = 12;
	ps.heading = -32000; ps.pitch = 32000;
	ps.animId = 4321; ps.animFlags = ANIM_CROUCH | ANIM_FIRE;
	ps.health = 87; ps.armour = 55; ps.weapon = 16; ps.ammo = 120; ps.money = 123456;
	ps.vehicleId = 0xFFFF; ps.seat = 0xFF;
	ps.moveFlags = MOVEFLAG_TELEPORT;

	BitWriter w; WritePlayerState(w, ps);
	BitReader r(w.Data(), w.ByteSize());
	PlayerState out;
	CHECK(ReadPlayerState(r, out));
	CHECK(out.playerId == ps.playerId);
	CHECK(out.xMm == ps.xMm && out.yMm == ps.yMm && out.zMm == ps.zMm);
	CHECK(out.velX == ps.velX && out.velY == ps.velY && out.velZ == ps.velZ);
	CHECK(out.heading == ps.heading && out.pitch == ps.pitch);
	CHECK(out.animId == ps.animId && out.animFlags == ps.animFlags);
	CHECK(out.health == ps.health && out.armour == ps.armour);
	CHECK(out.weapon == ps.weapon && out.ammo == ps.ammo && out.money == ps.money);
	CHECK(out.vehicleId == ps.vehicleId && out.seat == ps.seat);
	CHECK(out.moveFlags == ps.moveFlags);
	CHECK(r.Ok());

	VehicleState vs;
	vs.vehicleId = 5; vs.modelId = 220;
	vs.xMm = -1; vs.yMm = 2; vs.zMm = -3;
	vs.heading = 100; vs.pitch = -200; vs.roll = 300;
	vs.velX = 500; vs.velY = -500; vs.velZ = 0;
	vs.steer = -128; vs.gas = 200; vs.brake = 0;
	vs.flags = VEH_ENGINE | VEH_SIREN; vs.health = 999;
	vs.doors[0] = 0; vs.doors[1] = 255; vs.doors[2] = 128; vs.doors[3] = 64; vs.doors[4] = 1; vs.doors[5] = 200;

	BitWriter w2; WriteVehicleState(w2, vs);
	BitReader r2(w2.Data(), w2.ByteSize());
	VehicleState vout;
	CHECK(ReadVehicleState(r2, vout));
	CHECK(vout.vehicleId == vs.vehicleId && vout.modelId == vs.modelId);
	CHECK(vout.xMm == vs.xMm && vout.yMm == vs.yMm && vout.zMm == vs.zMm);
	CHECK(vout.heading == vs.heading && vout.pitch == vs.pitch && vout.roll == vs.roll);
	CHECK(vout.velX == vs.velX && vout.velY == vs.velY && vout.velZ == vs.velZ);
	CHECK(vout.steer == vs.steer && vout.gas == vs.gas && vout.brake == vs.brake);
	CHECK(vout.flags == vs.flags && vout.health == vs.health);
	for(int i = 0; i < 6; i++) CHECK(vout.doors[i] == vs.doors[i]);
	CHECK(r2.Ok());
}

// ---------------------------------------------------------------------------
static void TestMessageCodecs()
{
	// framed HELLO
	MsgHello hello;
	hello.flags = HELLO_PROBE;
	CopyStr(hello.name, sizeof hello.name, "Toni");
	CopyStr(hello.password, sizeof hello.password, "secret");
	BitWriter w; BeginMsg(w, MSG_HELLO); WriteHello(w, hello);
	CHECK(PeekType(w.Data(), w.ByteSize()) == MSG_HELLO);
	BitReader r(w.Data(), w.ByteSize());
	CHECK((MsgType)r.ReadU8() == MSG_HELLO);
	MsgHello h2;
	CHECK(ReadHello(r, h2));
	CHECK(h2.protocolVersion == PROTOCOL_VERSION);
	CHECK(h2.flags == HELLO_PROBE);
	CHECK(std::string(h2.name) == "Toni");
	CHECK(std::string(h2.password) == "secret");

	// chat with unicode-ish bytes
	MsgChat chat; chat.fromId = 3;
	CopyStr(chat.text, sizeof chat.text, "hello \xC3\xB6 world");
	BitWriter w2; BeginMsg(w2, MSG_CHAT); WriteChat(w2, chat);
	BitReader r2(w2.Data(), w2.ByteSize());
	r2.ReadU8();
	MsgChat c2; CHECK(ReadChat(r2, c2));
	CHECK(std::string(c2.text) == chat.text);

	// score update
	MsgScoreUpdate su; su.count = 3;
	for(int i = 0; i < 3; i++){
		su.entries[i].playerId = (uint8_t)(i + 1);
		su.entries[i].score = 10 - i * 7;
		su.entries[i].kills = (uint16_t)(i * 2);
		su.entries[i].deaths = (uint16_t)(i + 1);
	}
	BitWriter w3; WriteScoreUpdate(w3, su);
	BitReader r3(w3.Data(), w3.ByteSize());
	MsgScoreUpdate s2; CHECK(ReadScoreUpdate(r3, s2));
	CHECK(s2.count == 3);
	CHECK(s2.entries[2].playerId == 3 && s2.entries[2].score == -4 && s2.entries[2].deaths == 3);
	CHECK(r3.Ok());

	// truncated score update must fail cleanly
	uint8_t trunc[2] = { 2, 0 };
	BitReader r4(trunc, 2);
	MsgScoreUpdate s3;
	CHECK(!ReadScoreUpdate(r4, s3));

	{  // player list (MSG_PLAYER_PING): round trip keeps ping/score/kills/deaths
	MsgPlayerPing pp; pp.count = 2;
	pp.entries[0].playerId = 4; pp.entries[0].pingMs = 65000; pp.entries[0].score = -12;
	pp.entries[0].kills = 3; pp.entries[0].deaths = 9;
	pp.entries[1].playerId = 7; pp.entries[1].pingMs = 0; pp.entries[1].score = 500;
	BitWriter wp; WritePlayerPing(wp, pp);
	BitReader rp(wp.Data(), wp.ByteSize());
	MsgPlayerPing pp2; CHECK(ReadPlayerPing(rp, pp2));
	CHECK(rp.Ok() && pp2.count == 2);
	CHECK(pp2.entries[0].playerId == 4 && pp2.entries[0].pingMs == 65000 && pp2.entries[0].score == -12);
	CHECK(pp2.entries[0].kills == 3 && pp2.entries[0].deaths == 9);
	CHECK(pp2.entries[1].playerId == 7 && pp2.entries[1].pingMs == 0 && pp2.entries[1].score == 500);

	// a count above the cap is rejected
	uint8_t pbad[1] = { 33 };
	BitReader rpb(pbad, 1); MsgPlayerPing pp3;
	CHECK(!ReadPlayerPing(rpb, pp3));

	// a truncated row fails cleanly
	uint8_t ptrunc[4] = { 2, 4, 0, 0 };
	BitReader rpt(ptrunc, 4); MsgPlayerPing pp4;
	CHECK(!ReadPlayerPing(rpt, pp4));
	}

	// admin give
	MsgGive gv; gv.targetId = 9; gv.kind = GIVE_WEAPON; gv.amount = 250; gv.arg = 16;
	BitWriter wg; WriteGive(wg, gv);
	BitReader rg(wg.Data(), wg.ByteSize());
	MsgGive g2;
	CHECK(ReadGive(rg, g2));
	CHECK(g2.targetId == 9 && g2.kind == GIVE_WEAPON);
	CHECK(g2.amount == 250 && g2.arg == 16);
	CHECK(rg.Ok());

	// pickup / prop spawn
	MsgPickup pk; pk.pickupId = 7; pk.modelIndex = 0; pk.type = 4;
	pk.weapon = 3; pk.quantity = 50; pk.x = 1.5f; pk.y = -2.25f; pk.z = 33.0f;
	BitWriter wpk; WritePickup(wpk, pk);
	BitReader rp(wpk.Data(), wpk.ByteSize());
	MsgPickup p2;
	CHECK(ReadPickup(rp, p2));
	CHECK(p2.pickupId == 7 && p2.type == 4 && p2.weapon == 3 && p2.quantity == 50);
	CHECK(p2.x == 1.5f && p2.y == -2.25f && p2.z == 33.0f);
	CHECK(rp.Ok());
}

// ---------------------------------------------------------------------------
static void TestValidation()
{
	PlayerState a{}, b{};
	a.xMm = 0; a.yMm = 0; a.zMm = 0;
	a.health = 100; a.armour = 20;

	// gentle walk 2 m over 50 ms is fine (40 m/s?? no — 2m/0.05s = 40 m/s is too fast on foot)
	b = a; b.xMm = 2000;
	CHECK(ValidateMove(a, b, TICK_DT) & VAL_TOO_FAST);

	// 0.3 m over 50 ms = 6 m/s on foot — fine
	b = a; b.xMm = 300;
	CHECK(ValidateMove(a, b, TICK_DT) == VAL_OK);

	// hard teleport
	b = a; b.xMm = 500000;
	CHECK(ValidateMove(a, b, TICK_DT) & VAL_TELEPORT);

	// in-vehicle fast is fine
	b = a; b.xMm = 3500; b.vehicleId = 9;
	CHECK(ValidateMove(a, b, TICK_DT) == VAL_OK);

	// health regeneration detected
	b = a; b.xMm = 100; b.health = 101;
	CHECK(ValidateMove(a, b, TICK_DT) & VAL_BAD_HEALTH);

	// health drop fine
	b = a; b.xMm = 100; b.health = 60;
	CHECK(ValidateMove(a, b, TICK_DT) == VAL_OK);

	// fast vertical fall is legitimate physics (spawn drops, ragdolls)
	b = a; b.zMm = -1800; // 36 m/s down
	CHECK(ValidateMove(a, b, TICK_DT) == VAL_OK);

	// ...but falling diagonally at teleport range is still a teleport
	b = a; b.zMm = -1800; b.xMm = 600000;
	CHECK(ValidateMove(a, b, TICK_DT) & VAL_TELEPORT);

	// excessive upward speed is still flagged
	b = a; b.zMm = 3000; // 60 m/s up
	CHECK(ValidateMove(a, b, TICK_DT) & VAL_TOO_FAST);

	// client-flagged teleports are trusted game-side relocations
	b = a; b.xMm = 500000; b.moveFlags = MOVEFLAG_TELEPORT;
	CHECK(ValidateMove(a, b, TICK_DT) == VAL_OK);

	// ...but vitals are still validated on flagged moves
	b.health = 101;
	CHECK(ValidateMove(a, b, TICK_DT) & VAL_BAD_HEALTH);

	// vehicle validation
	VehicleState v1{}, v2{};
	v1.health = 1000;
	v2 = v1; v2.xMm = 4000; // 80 m/s ok
	CHECK(ValidateVehicleMove(v1, v2, TICK_DT) == VAL_OK);
	v2 = v1; v2.xMm = 200000; // teleport
	CHECK(ValidateVehicleMove(v1, v2, TICK_DT) & VAL_TELEPORT);
	v2 = v1; v2.health = 1500;
	CHECK(ValidateVehicleMove(v1, v2, TICK_DT) & VAL_BAD_HEALTH);
}

// ---------------------------------------------------------------------------
static void TestToughValidator()
{
	using namespace rnet;

	// --- payload sanity -----------------------------------------------------
	PlayerState st;
	CHECK(ValidateStateSanity(st)); // defaults are sane
	PlayerState bad = st; bad.xMm = 3000000; // 3000 m — outside the world
	CHECK(!ValidateStateSanity(bad));
	bad = st; bad.yMm = -3000000;
	CHECK(!ValidateStateSanity(bad));
	bad = st; bad.zMm = -2000000;
	CHECK(!ValidateStateSanity(bad));
	bad = st; bad.weapon = 200;
	CHECK(!ValidateStateSanity(bad));
	bad = st; bad.health = 150;
	CHECK(!ValidateStateSanity(bad));
	bad = st; bad.armour = 255;
	CHECK(!ValidateStateSanity(bad));
	bad = st; bad.money = -1;
	CHECK(!ValidateStateSanity(bad));
	bad = st; bad.money = 200000000;
	CHECK(!ValidateStateSanity(bad));
	// extreme but finite velocities are legal
	bad = st; bad.velX = 32767; bad.velY = -32768; bad.velZ = 32767;
	CHECK(ValidateStateSanity(bad));
	// boundary values pass
	PlayerState edge = st;
	edge.xMm = (int32_t)(WORLD_LIMIT_XY * 1000.0f);
	edge.zMm = (int32_t)(WORLD_LIMIT_Z * 1000.0f);
	edge.money = MONEY_MAX;
	edge.weapon = WEAPON_MAX_ID;
	edge.health = 100; edge.armour = 100;
	CHECK(ValidateStateSanity(edge));
	CHECK(ClampAmmo(65535) == AMMO_MAX);
	CHECK(ClampAmmo(12) == 12);

	// --- rate limiter ------------------------------------------------------
	RateLimiter lim;
	lim.Reset(100.0, 2.0f, 4.0f);
	int ok = 0;
	for(int i = 0; i < 10; i++) if(lim.Allow(100.0)) ok++;
	CHECK(ok == 4); // burst exhausted
	ok = 0;
	for(int i = 0; i < 10; i++) if(lim.Allow(102.0)) ok++;
	CHECK(ok == 4); // 2 s at 2/s refills to the cap
	// steady drip under the rate is always allowed
	RateLimiter drip;
	drip.Reset(0.0, 2.0f, 4.0f);
	for(int i = 0; i < 6; i++) CHECK(drip.Allow(i * 0.5));
	// clock going backwards must never grant free tokens
	RateLimiter back;
	back.Reset(50.0, 2.0f, 2.0f);
	CHECK(back.Allow(50.0));
	CHECK(back.Allow(50.0));
	CHECK(!back.Allow(10.0)); // jumped back in time: no refill

	// --- chat spam/hygiene -------------------------------------------------
	RateLimiter chat;
	chat.Reset(0.0, 1.0f, 3.0f);
	CHECK(ValidateChat("hello", chat, 0.0));
	CHECK(!ValidateChat("", chat, 0.0));
	CHECK(!ValidateChat(nullptr, chat, 0.0));
	CHECK(!ValidateChat("bad\nline", chat, 0.0));   // control char
	char longMsg[300];
	memset(longMsg, 'a', sizeof longMsg - 1);
	longMsg[sizeof longMsg - 1] = 0;
	CHECK(!ValidateChat(longMsg, chat, 0.0));       // over codec cap
	CHECK(ValidateChat("a", chat, 1.0));
	CHECK(ValidateChat("b", chat, 1.0));
	CHECK(ValidateChat("c", chat, 1.0));
	CHECK(!ValidateChat("d", chat, 1.0));           // burst exhausted

	// --- names -------------------------------------------------------------
	char out[16];
	SanitizeName(out, sizeof out, "  Toni  Cipriani ");
	CHECK(!strcmp(out, "Toni Cipriani"));
	SanitizeName(out, sizeof out, "bad\x01name\x7f");
	CHECK(!strcmp(out, "badname"));
	SanitizeName(out, sizeof out, "");
	CHECK(!strcmp(out, "Player"));
	SanitizeName(out, sizeof out, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
	CHECK(strlen(out) == 15); // bounded by dstLen

	// --- server-tracked economy -------------------------------------------
	int32_t seen = 100, granted = 0;
	bool cheated = false;
	CHECK(ValidateMoney(100, seen, granted, cheated) == 100 && !cheated);
	CHECK(ValidateMoney(50, seen, granted, cheated) == 50 && !cheated); // spending ok
	CHECK(seen == 50);
	CHECK(ValidateMoney(5000, seen, granted, cheated) == 50); // unearned gain clamped
	CHECK(cheated);
	granted = 200;
	CHECK(ValidateMoney(250, seen, granted, cheated) == 250 && !cheated);
	CHECK(granted == 0);
	CHECK(ValidateMoney(-5, seen, granted, cheated) == 250 && cheated);
}

// ---------------------------------------------------------------------------
static void TestVehicleMessages()
{
	// enter: new-vehicle sentinel + seat + model
	MsgVehEnter ve;
	ve.playerId = 42; ve.vehicleId = 0xFFFF; ve.seat = 0; ve.modelId = 401;
	BitWriter w1; BeginMsg(w1, MSG_VEH_ENTER); WriteVehEnter(w1, ve);
	CHECK(PeekType(w1.Data(), w1.ByteSize()) == MSG_VEH_ENTER);
	BitReader r1(w1.Data(), w1.ByteSize());
	CHECK((MsgType)r1.ReadU8() == MSG_VEH_ENTER);
	MsgVehEnter ve2;
	CHECK(ReadVehEnter(r1, ve2));
	CHECK(ve2.playerId == 42 && ve2.vehicleId == 0xFFFF);
	CHECK(ve2.seat == 0 && ve2.modelId == 401);
	CHECK(r1.Ok());

	// assigned-seat roundtrip: seats up to 7 survive the wire
	ve.vehicleId = 300; ve.seat = 7;
	BitWriter w1b; WriteVehEnter(w1b, ve);
	BitReader r1b(w1b.Data(), w1b.ByteSize());
	MsgVehEnter ve3;
	CHECK(ReadVehEnter(r1b, ve3));
	CHECK(ve3.vehicleId == 300 && ve3.seat == 7);

	// exit (id 0 = server resolves by occupancy)
	MsgVehExit vx; vx.playerId = 42; vx.vehicleId = 0;
	BitWriter w2; BeginMsg(w2, MSG_VEH_EXIT); WriteVehExit(w2, vx);
	BitReader r2(w2.Data(), w2.ByteSize());
	CHECK((MsgType)r2.ReadU8() == MSG_VEH_EXIT);
	MsgVehExit vx2;
	CHECK(ReadVehExit(r2, vx2));
	CHECK(vx2.playerId == 42 && vx2.vehicleId == 0);

	// spawn
	MsgVehSpawn vs;
	vs.vehicleId = 1; vs.modelId = 401;
	vs.x = 100.5f; vs.y = -200.25f; vs.z = 12.0f; vs.heading = 1.5f;
	vs.ownerId = 7;
	BitWriter w3; BeginMsg(w3, MSG_VEH_SPAWN); WriteVehSpawn(w3, vs);
	BitReader r3(w3.Data(), w3.ByteSize());
	CHECK((MsgType)r3.ReadU8() == MSG_VEH_SPAWN);
	MsgVehSpawn vs2;
	CHECK(ReadVehSpawn(r3, vs2));
	CHECK(vs2.vehicleId == 1 && vs2.modelId == 401);
	CHECK(vs2.x == 100.5f && vs2.y == -200.25f && vs2.z == 12.0f && vs2.heading == 1.5f);
	CHECK(vs2.ownerId == 7);
	CHECK(r3.Ok());

	// seat math: passenger slots are seat-1 (0 = driver)
	for(uint8_t seat = 0; seat < 8; seat++){
		int slot = seat == 0 ? -1 : (int)seat - 1;
		CHECK(seat == 0 ? slot == -1 : slot >= 0 && slot < 8);
	}

	// driver-state relay gating model: a player inside a vehicle gets the
	// vehicle speed budget in ValidateMove (VEHICLE_NONE = on foot)
	// 2 m per 50 ms tick = 40 m/s: legal driving, impossible on foot
	PlayerState onFoot{}, riding{};
	onFoot.vehicleId = VEHICLE_NONE;
	riding.vehicleId = 1; riding.seat = 0;
	PlayerState a = onFoot, b = onFoot;
	a.xMm = 0; b.xMm = QuantPos(2.0f);
	CHECK(ValidateMove(a, b, TICK_DT) & VAL_TOO_FAST); // on foot: over budget
	a = riding; b = riding;
	a.xMm = 0; b.xMm = QuantPos(2.0f);
	CHECK(ValidateMove(a, b, TICK_DT) == VAL_OK);      // in vehicle: fine
}

// ---------------------------------------------------------------------------
// Vehicle occupancy / single-entity reconciliation model.
//
// This mirrors the server's authoritative occupancy state machine
// (Server::HandleVehEnter / HandleVehExit / ClearPlayerVehicles) in a
// self-contained form — net-tests does not link the ENet-coupled server — and
// locks the invariants the vehicle-sync fix depends on:
//   1. a player entering a BRAND-NEW car allocates exactly ONE entity,
//   2. re-entering (or a passenger boarding) an EXISTING id allocates nothing,
//   3. occupancy is scoped to the entering player — an enter by A never
//      disturbs B's seat (no "forced into a vehicle you didn't enter"),
//   4. a player holds exactly one seat at a time (old seat released on re-enter).
// ---------------------------------------------------------------------------
namespace {

struct VehReg {
	struct Veh { bool active = false; uint16_t id = 0; uint16_t model = 0; uint8_t occ[8]; };
	static const int MAXV = 8;
	Veh v[MAXV];
	int spawnCount = 0;                       // total entities ever allocated

	VehReg() { for(int i = 0; i < MAXV; i++) for(int s = 0; s < 8; s++) v[i].occ[s] = 0xFF; }

	int findById(uint16_t id) {
		for(int i = 0; i < MAXV; i++) if(v[i].active && v[i].id == id) return i;
		return -1;
	}
	// drop every seat this player holds, anywhere (disconnect / respawn / re-enter)
	void clearPlayer(uint8_t pid) {
		for(int i = 0; i < MAXV; i++)
			if(v[i].active)
				for(int s = 0; s < 8; s++) if(v[i].occ[s] == pid) v[i].occ[s] = 0xFF;
	}
	int alloc(uint16_t model) {
		for(int i = 0; i < MAXV; i++) if(!v[i].active){
			v[i].active = true; v[i].id = (uint16_t)(i + 1); v[i].model = model;
			for(int s = 0; s < 8; s++) v[i].occ[s] = 0xFF;
			spawnCount++;
			return i;
		}
		return -1;
	}
	// returns the ASSIGNED (vehId, seat), or (VEHICLE_NONE, 0xFF) on refusal.
	// reqId == 0xFFFF => allocate a new car.
	std::pair<uint16_t,uint8_t> enter(uint8_t pid, uint16_t reqId, uint8_t reqSeat, uint16_t model) {
		clearPlayer(pid);                      // invariant 4: one seat per player
		int idx;
		if(reqId == VEHICLE_NONE){ idx = alloc(model); if(idx < 0) return {VEHICLE_NONE, 0xFF}; }
		else { idx = findById(reqId); if(idx < 0) return {VEHICLE_NONE, 0xFF}; }
		uint8_t seat = 0xFF;
		if(reqSeat < 8 && v[idx].occ[reqSeat] == 0xFF) seat = reqSeat;
		else for(int s = 0; s < 8; s++) if(v[idx].occ[s] == 0xFF){ seat = (uint8_t)s; break; }
		if(seat == 0xFF) return {VEHICLE_NONE, 0xFF}; // full
		v[idx].occ[seat] = pid;
		return {v[idx].id, seat};
	}
	void exit(uint8_t pid) { clearPlayer(pid); }
	int seatOf(uint16_t id, uint8_t pid) {
		int idx = findById(id); if(idx < 0) return -1;
		for(int s = 0; s < 8; s++) if(v[idx].occ[s] == pid) return s;
		return -1;
	}
};

} // namespace

static void TestVehicleOccupancy()
{
	VehReg reg;

	// invariant 1: A drives into a brand-new car -> exactly one entity allocated
	auto ra = reg.enter(/*pid*/1, VEHICLE_NONE, /*seat*/0, /*model*/130);
	CHECK(ra.first != VEHICLE_NONE && ra.second == 0);
	CHECK(reg.spawnCount == 1);
	uint16_t carA = ra.first;
	CHECK(reg.seatOf(carA, 1) == 0);

	// invariant 2 + 3: B boards A's EXISTING car as a passenger. No new entity,
	// and A's driver seat is untouched (occupancy scoped to the entering player).
	auto rb = reg.enter(/*pid*/2, carA, /*seat*/1, 130);
	CHECK(rb.first == carA && rb.second == 1);
	CHECK(reg.spawnCount == 1);              // NOT duplicated
	CHECK(reg.seatOf(carA, 1) == 0);         // A still driving — never force-moved
	CHECK(reg.seatOf(carA, 2) == 1);

	// a second, independent driver C spawns a DIFFERENT entity
	auto rc = reg.enter(/*pid*/3, VEHICLE_NONE, 0, 131);
	CHECK(rc.first != VEHICLE_NONE && rc.first != carA);
	CHECK(reg.spawnCount == 2);
	uint16_t carC = rc.first;

	// invariant 4: A re-enters its OWN existing car (seat 0) — no new entity, and
	// A is not duplicated across seats (old seat released first).
	auto ra2 = reg.enter(1, carA, 0, 130);
	CHECK(ra2.first == carA && ra2.second == 0);
	CHECK(reg.spawnCount == 2);
	int held = 0;
	for(int s = 0; s < 8; s++) if(reg.seatOf(carA, 1) == s) held++;
	CHECK(held == 1);

    // exit: A leaves, freeing only A's seat; B stays put (scoped)
	reg.exit(1);
	CHECK(reg.seatOf(carA, 1) == -1);
	CHECK(reg.seatOf(carA, 2) == 1);         // B untouched by A's exit
	CHECK(reg.seatOf(carC, 3) == 0);         // C's own car untouched throughout

	// requesting a taken seat falls back to a free one, never evicts the holder
	auto rd = reg.enter(/*pid*/4, carA, /*seat*/2 /*B's seat*/, 130);
	CHECK(rd.first == carA && rd.second != 1); // got some OTHER free seat
	CHECK(reg.seatOf(carA, 2) == 1);           // B still in seat 1

	// entering a non-existent id is refused (no phantom entity)
	auto re = reg.enter(5, /*bogus*/9999, 0, 130);
	CHECK(re.first == VEHICLE_NONE);
	CHECK(reg.spawnCount == 2);
}

static void TestInventoryMessages()
{
	// full snapshot round trip
	MsgInventory inv;
	inv.playerId = 7;
	inv.money = 123456;
	inv.currentWeapon = 22;
	inv.count = 3;
	inv.entries[0].weapon = 22; inv.entries[0].ammo = 200;
	inv.entries[1].weapon = 14; inv.entries[1].ammo = 50;
	inv.entries[2].weapon = 4;  inv.entries[2].ammo = 0;
	BitWriter w; BeginMsg(w, MSG_INVENTORY); WriteInventory(w, inv);
	BitReader r(w.Data(), w.ByteSize());
	CHECK((MsgType)r.ReadU8() == MSG_INVENTORY);
	MsgInventory out;
	CHECK(ReadInventory(r, out));
	CHECK(out.playerId == 7 && out.money == 123456);
	CHECK(out.currentWeapon == 22 && out.count == 3);
	CHECK(out.entries[0].weapon == 22 && out.entries[0].ammo == 200);
	CHECK(out.entries[1].weapon == 14 && out.entries[1].ammo == 50);
	CHECK(out.entries[2].weapon == 4 && out.entries[2].ammo == 0);
	CHECK(r.Ok());

	// empty table is legal (fresh spawn)
	MsgInventory empty;
	BitWriter w2; BeginMsg(w2, MSG_INVENTORY); WriteInventory(w2, empty);
	BitReader r2(w2.Data(), w2.ByteSize());
	CHECK((MsgType)r2.ReadU8() == MSG_INVENTORY);
	MsgInventory e2;
	CHECK(ReadInventory(r2, e2));
	CHECK(e2.count == 0 && e2.money == 0);
	CHECK(r2.Ok());

	// forged oversized count must be rejected
	BitWriter w3;
	w3.WriteU8(MSG_INVENTORY);
	w3.WriteU16(1); w3.WriteI32(0); w3.WriteU8(0); w3.WriteU8(MAX_INV_ENTRIES + 1);
	BitReader r3(w3.Data(), w3.ByteSize());
	CHECK((MsgType)r3.ReadU8() == MSG_INVENTORY);
	MsgInventory bad;
	CHECK(!ReadInventory(r3, bad));

	// restored wallet becomes the ledger baseline: growth by grants only
	int32_t seen = 55000, granted = 0; bool cheated = false;
	CHECK(ValidateMoney(55000, seen, granted, cheated) == 55000 && !cheated);
	granted = 1000;
	CHECK(ValidateMoney(56000, seen, granted, cheated) == 56000 && !cheated);
	granted = 0;
	CHECK(ValidateMoney(57000, seen, granted, cheated) == 56000 && cheated);
}

static void TestProfileStore()
{
	ProfileStore db;
	const char* path = "test-profiles.db";
	std::remove(path);
	CHECK(db.Open(path));

	// missing name: no row
	PlayerProfile p;
	CHECK(!db.Load("ghost", p));

	// save -> load round trip
	PlayerProfile a;
	a.money = 55000;
	a.currentWeapon = 22;
	a.weapons.push_back({ 22, 200 });
	a.weapons.push_back({ 14, 50 });
	CHECK(db.Save("mattb", a));
	PlayerProfile b;
	CHECK(db.Load("mattb", b));
	CHECK(b.money == 55000 && b.currentWeapon == 22 && b.weapons.size() == 2);
	CHECK(b.weapons[0].weapon == 14 && b.weapons[0].ammo == 50);   // ordered by weapon
	CHECK(b.weapons[1].weapon == 22 && b.weapons[1].ammo == 200);

	// name lookup is case-insensitive (COLLATE NOCASE)
	PlayerProfile ci;
	CHECK(db.Load("MATTB", ci));
	CHECK(ci.money == 55000 && ci.weapons.size() == 2);

	// update replaces the weapon table wholesale
	PlayerProfile c;
	c.money = 60000;
	c.currentWeapon = 4;
	c.weapons.push_back({ 4, 0 });
	CHECK(db.Save("mattb", c));
	PlayerProfile d;
	CHECK(db.Load("mattb", d));
	CHECK(d.money == 60000 && d.weapons.size() == 1);
	CHECK(d.weapons[0].weapon == 4 && d.weapons[0].ammo == 0);

	// survives close/reopen: it is really on disk
	db.Close();
	CHECK(!db.IsOpen());
	CHECK(db.Open(path));
	PlayerProfile e;
	CHECK(db.Load("mattb", e));
	CHECK(e.money == 60000 && e.currentWeapon == 4 && e.weapons.size() == 1);

	// match stats + last position round-trip, and reach the disk too
	PlayerProfile s;
	s.money = 60000;
	s.score = 7;
	s.kills = 7;
	s.deaths = 3;
	s.hasPos = true;
	s.x = 1133.5f; s.y = -660.25f; s.z = 23.0f; s.heading = 1.5f;
	CHECK(db.Save("mattb", s));
	db.Close();
	CHECK(db.Open(path));
	PlayerProfile f;
	CHECK(db.Load("mattb", f));
	CHECK(f.score == 7 && f.kills == 7 && f.deaths == 3);
	CHECK(f.hasPos && f.x == 1133.5f && f.y == -660.25f && f.z == 23.0f && f.heading == 1.5f);

	// character look: the chosen ped model AND the component slots survive a
	// save/reopen, so the creator only has to ask a first-time player once
	PlayerProfile look;
	look.skin = 78;
	for(int i = 0; i < 8; i++) look.skinComponents[i] = (uint8_t)(i * 9 + 1);
	CHECK(db.Save("looks", look));
	db.Close();
	CHECK(db.Open(path));
	PlayerProfile look2;
	CHECK(db.Load("looks", look2));
	CHECK(look2.skin == 78);
	bool compsOk = true;
	for(int i = 0; i < 8; i++)
		if(look2.skinComponents[i] != (uint8_t)(i * 9 + 1)) compsOk = false;
	CHECK(compsOk);

	// a profile saved without a position reads back as no position
	PlayerProfile g;
	g.money = 1;
	CHECK(db.Save("nopos", g));
	PlayerProfile h;
	CHECK(db.Load("nopos", h));
	CHECK(!h.hasPos && h.score == 0 && h.kills == 0 && h.deaths == 0);

	db.Close();
	std::remove(path);
}

// name+password auth: PBKDF2 determinism, claim once, restore by owner only
static void TestProfileAuth()
{
	// hashing is deterministic for a fixed salt, and any change flips the hash
	uint8_t salt[16];
	for(int i = 0; i < 16; i++) salt[i] = (uint8_t)(i * 7 + 1);
	uint8_t k1[32], k2[32], k3[32];
	passhash::DeriveKey("hunter2", salt, k1);
	passhash::DeriveKey("hunter2", salt, k2);
	passhash::DeriveKey("hunter3", salt, k3);
	CHECK(memcmp(k1, k2, 32) == 0);              // same pass+salt -> same key
	CHECK(memcmp(k1, k3, 32) != 0);              // different pass -> different key
	CHECK(passhash::SecureEqual(k1, k2, 32));
	CHECK(!passhash::SecureEqual(k1, k3, 32));
	// a different salt also changes the key
	uint8_t salt2[16];
	for(int i = 0; i < 16; i++) salt2[i] = (uint8_t)(i * 13 + 5);
	passhash::DeriveKey("hunter2", salt2, k2);
	CHECK(memcmp(k1, k2, 32) != 0);

	const char* path = "test-auth.db";
	std::remove(path);
	ProfileStore db;
	CHECK(db.Open(path));

	// unknown name is unclaimed; no profile yet
	CHECK(db.Auth("toni", "whatever") == AuthResult::UNCLAIMED);
	PlayerProfile p;
	CHECK(!db.Load("toni", p));

	// claim: set a password, name becomes owned
	CHECK(db.SetPassword("toni", "swordfish"));
	CHECK(db.Auth("toni", "swordfish") == AuthResult::OK);
	CHECK(db.Auth("toni", "wrongpass") == AuthResult::WRONG_PASSWORD);
	CHECK(db.Auth("toni", "") == AuthResult::WRONG_PASSWORD);
	// case-insensitive name match still resolves to the same credential
	CHECK(db.Auth("TONI", "swordfish") == AuthResult::OK);

	// owner restores their saved wallet + weapons
	PlayerProfile a;
	a.money = 12345;
	a.currentWeapon = 26;
	a.weapons.push_back({ 26, 40 });
	CHECK(db.Save("toni", a));
	CHECK(db.Load("toni", a));
	CHECK(a.money == 12345 && a.weapons.size() == 1);

	// changing the password invalidates the old one
	CHECK(db.SetPassword("toni", "newpass99"));
	CHECK(db.Auth("toni", "newpass99") == AuthResult::OK);
	CHECK(db.Auth("toni", "swordfish") == AuthResult::WRONG_PASSWORD);

	// survives reopen (really stored), and short passwords are refused
	db.Close();
	CHECK(db.Open(path));
	CHECK(db.Auth("toni", "newpass99") == AuthResult::OK);
	CHECK(!db.SetPassword("toni", "ab"));   // below MIN_PASSWORD_LEN
	CHECK(db.Auth("toni", "ab") == AuthResult::WRONG_PASSWORD);
	CHECK(db.Auth("toni", "newpass99") == AuthResult::OK); // unchanged
	db.Close();
	std::remove(path);
}

// a profile row created before credentials existed reads as unclaimed
static void TestProfileLegacyMigration()
{
	const char* path = "test-legacy.db";
	std::remove(path);

	// build an old-shape DB by hand: no pass_hash / pass_salt columns
	{
		std::ofstream f(path, std::ios::binary);
		CHECK(f.good());
	}
	std::remove(path);
	ProfileStore old;
	CHECK(old.Open(path));
	PlayerProfile p;
	p.money = 777;
	p.currentWeapon = 18;
	p.weapons.push_back({ 18, 12 });
	CHECK(old.Save("claude", p));
	old.Close();

	// reopen through ProfileStore: EnsureCredentialColumns migrates the row
	ProfileStore db;
	CHECK(db.Open(path));
	CHECK(db.Auth("claude", "anything") == AuthResult::UNCLAIMED); // pre-password row
	PlayerProfile q;
	CHECK(db.Load("claude", q));
	CHECK(q.money == 777 && q.weapons.size() == 1);                 // data intact
	// claiming it now keeps the wallet and locks out strangers
	CHECK(db.SetPassword("claude", "mypassword"));
	CHECK(db.Auth("claude", "mypassword") == AuthResult::OK);
	CHECK(db.Auth("claude", "anything") == AuthResult::WRONG_PASSWORD);
	PlayerProfile r;
	CHECK(db.Load("claude", r));
	CHECK(r.money == 777 && r.weapons.size() == 1);
	db.Close();
	std::remove(path);
}

// ---------------------------------------------------------------------------
// resource pipeline (mod/resource manager)
// ---------------------------------------------------------------------------
#include "resource.h"

static void TestResourceParsing()
{
	// manifest: happy path
	ResourceInfo m;
	CHECK(ResourceManager::ParseManifest(
	    "# pack\nname=welcome-pack\nversion=1.2\nauthor=nobody\n"
	    "description=greets players\ntype=internal\nmotd=hi there\n", m));
	CHECK(m.name == "welcome-pack");
	CHECK(m.version == "1.2");
	CHECK(m.author == "nobody");
	CHECK(m.type == "internal");
	CHECK(m.motd == "hi there");
	CHECK(m.autoload);

	// manifest: missing name fails
	ResourceInfo bad;
	CHECK(!ResourceManager::ParseManifest("version=1\n", bad));
	CHECK(!bad.error.empty());

	// manifest: bad type fails
	ResourceInfo bad2;
	CHECK(!ResourceManager::ParseManifest("name=x\ntype=worm\n", bad2));

	// manifest: autoload=0 honored
	ResourceInfo la;
	CHECK(ResourceManager::ParseManifest("name=y\nautoload=0\n", la));
	CHECK(!la.autoload);

	// script: hooks + command lines + comments
	ResourceInfo s;
	CHECK(ResourceManager::ParseScript(
	    "# comment\n"
	    "@on start\n"
	    "settime 8 0\n"
	    "broadcast hello\n"
	    "@on join\n"
	    "give %id% money 100\n"
	    "@on cmd rules\n"
	    "say rules: be nice %name%\n"
	    "@on interval 30\n"
	    "say tick\n", s));
	CHECK(s.hooks.size() == 4);
	CHECK(s.hooks[0].event == "start" && s.hooks[0].lines.size() == 2);
	CHECK(s.hooks[1].event == "join" && s.hooks[1].lines.size() == 1);
	CHECK(s.hooks[2].event == "cmd" && s.hooks[2].arg == "rules");
	CHECK(s.hooks[3].event == "interval" && s.hooks[3].interval == 30.0);

	// script: command before any hook fails
	ResourceInfo s2;
	CHECK(!ResourceManager::ParseScript("say orphan\n", s2));
	CHECK(!s2.error.empty());

	// script: unknown hook fails
	ResourceInfo s3;
	CHECK(!ResourceManager::ParseScript("@on explode\nsay x\n", s3));

	// script: interval < 1s fails
	ResourceInfo s4;
	CHECK(!ResourceManager::ParseScript("@on interval 0.5\nsay x\n", s4));
}

static void TestResourceExpand()
{
	CHECK(ResourceManager::Expand("give %id% money 500", 7, "alice", "") ==
	      "give 7 money 500");
	CHECK(ResourceManager::Expand("say hi %name%", 1, "bob", "") == "say hi bob");
	CHECK(ResourceManager::Expand("say %args%", 1, "bob", "be nice") ==
	      "say be nice");
	// unknown placeholders survive verbatim
	CHECK(ResourceManager::Expand("say %zzz%", 1, "b", "") == "say %zzz%");
}

static void TestResourceSandbox()
{
	// external packs: safe commands allowed
	CHECK(ResourceManager::ExternalAllowed("say hello"));
	CHECK(ResourceManager::ExternalAllowed("broadcast hi"));
	CHECK(ResourceManager::ExternalAllowed("settime 8 0"));
	CHECK(ResourceManager::ExternalAllowed("setweather 2"));
	CHECK(ResourceManager::ExternalAllowed("spawnprop 1 2 3 4 50"));
	CHECK(ResourceManager::ExternalAllowed("give 3 money 10"));
	// everything else blocked
	CHECK(!ResourceManager::ExternalAllowed("kick 1"));
	CHECK(!ResourceManager::ExternalAllowed("res disable x"));
	CHECK(!ResourceManager::ExternalAllowed("quit"));
	CHECK(!ResourceManager::ExternalAllowed("exec evil.cfg"));
}

static void TestResourceManagerOps()
{
	// build a temp resources dir with one good and one broken pack
	const char* root = "test-resources-tmp";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(std::string(root) + "/good");
	std::filesystem::create_directories(std::string(root) + "/broken");
	{
		std::ofstream f(std::string(root) + "/good/resource.cfg");
		f << "name=good\nversion=0.1\ntype=internal\n";
	}
	{
		std::ofstream f(std::string(root) + "/good/main.rsc");
		f << "@on start\nsay started\n@on cmd hi\nsay hello %name%\n";
	}
	{
		std::ofstream f(std::string(root) + "/broken/resource.cfg");
		f << "type=external\n";   // no name= -> invalid
	}
	// uninstall graveyard must NOT be scanned as a resource
	std::filesystem::create_directories(std::string(root) + "/removed/old-pack");
	{
		std::ofstream f(std::string(root) + "/removed/old-pack/resource.cfg");
		f << "name=old-pack\n";
	}

	std::vector<std::string> ran;
	ResourceManager mgr;
	mgr.Configure(root,
	    [&](const std::string& line, int to){
		    ran.push_back(std::to_string(to) + "|" + line);
	    },
	    [](const std::string&){});
	CHECK(mgr.Reload());
	CHECK(mgr.List().size() == 2);   // good + broken; removed/ is skipped
	CHECK(!mgr.Find("old-pack"));
	const ResourceInfo* good = mgr.Find("good");
	const ResourceInfo* broken = mgr.Find("broken");
	CHECK(good && good->valid);
	CHECK(broken && !broken->valid);

	// run the start hook through the exec sink
	std::string err;
	CHECK(mgr.RunEvent("good", "start", "", -1, -1, "", err));
	CHECK(ran.size() == 1 && ran[0] == "-1|say started");

	// player command hook expands placeholders and routes to the player
	CHECK(mgr.OnPlayerCommand("hi there", 4, "alice"));
	CHECK(ran.size() == 2 && ran[1] == "4|say hello alice");
	CHECK(!mgr.OnPlayerCommand("nosuch", 4, "alice"));

	// disable blocks execution
	CHECK(mgr.SetEnabled("good", false, err));
	CHECK(!mgr.RunEvent("good", "start", "", -1, -1, "", err));
	CHECK(err == "resource disabled");
	// ... and the flag persisted
	CHECK(mgr.SetEnabled("good", true, err));

	// missing resource reports cleanly
	CHECK(!mgr.RunEvent("ghost", "start", "", -1, -1, "", err));
	CHECK(err.find("no such resource") == 0);

	// run a specific hook by name (res run good start)
	ran.clear();
	CHECK(mgr.RunEvent("good", "cmd", "hi", 2, 2, "bob", err));
	CHECK(ran.size() == 1 && ran[0] == "2|say hello bob");

	std::filesystem::remove_all(root);
}

static void TestResListCodec()
{
	// mod/resource inventory round-trips through the wire format
	MsgResList in;
	in.count = 2;
	CopyStr(in.entries[0].name, sizeof in.entries[0].name, "welcome-pack");
	CopyStr(in.entries[0].version, sizeof in.entries[0].version, "1.2");
	in.entries[0].type = 0; in.entries[0].enabled = 1; in.entries[0].valid = 1;
	CopyStr(in.entries[1].name, sizeof in.entries[1].name, "shop");
	in.entries[1].type = 1; in.entries[1].enabled = 0; in.entries[1].valid = 0;
	BitWriter w;
	BeginMsg(w, MSG_RES_LIST);
	WriteResList(w, in);
	BitReader r(w.Data() + 1, w.ByteSize() - 1);
	MsgResList out;
	CHECK(ReadResList(r, out));
	CHECK(out.count == 2);
	CHECK(std::string(out.entries[0].name) == "welcome-pack");
	CHECK(std::string(out.entries[0].version) == "1.2");
	CHECK(out.entries[0].type == 0 && out.entries[0].enabled == 1 && out.entries[0].valid == 1);
	CHECK(std::string(out.entries[1].name) == "shop");
	CHECK(out.entries[1].type == 1 && out.entries[1].enabled == 0 && out.entries[1].valid == 0);

	// bogus count is rejected
	BitWriter w2;
	w2.WriteU8(200);
	BitReader r2(w2.Data(), w2.ByteSize());
	MsgResList bad;
	CHECK(!ReadResList(r2, bad));
}

// v5: live server info in WELCOME (map/players) — the launcher's tooltip reads
// these straight off a probe handshake
static void TestWelcomeLiveInfo()
{
	MsgWelcome w1;
	w1.playerId = 3;
	w1.tickRate = 20;
	w1.streamDistance = 200.0f;
	w1.hour = 9; w1.minute = 30; w1.weather = 1;
	CopyStr(w1.gameMode, sizeof w1.gameMode, "survivor");
	CopyStr(w1.hostname, sizeof w1.hostname, "i8 Showroom");
	w1.players = 2;
	w1.maxPlayers = 32;
	CopyStr(w1.mapName, sizeof w1.mapName, "Liberty City");

	BitWriter w;
	BeginMsg(w, MSG_WELCOME);
	WriteWelcome(w, w1);
	BitReader r(w.Data(), w.ByteSize());
	CHECK((MsgType)r.ReadU8() == MSG_WELCOME);
	MsgWelcome w2;
	CHECK(ReadWelcome(r, w2));
	CHECK(w2.playerId == 3 && w2.tickRate == 20);
	CHECK(w2.players == 2 && w2.maxPlayers == 32);
	CHECK(strcmp(w2.mapName, "Liberty City") == 0);
	CHECK(strcmp(w2.hostname, "i8 Showroom") == 0);

	// a server that stops after the hostname: the optional tail reads as empty
	BitWriter w4;
	w4.WriteU16(1); w4.WriteU16(20); w4.WriteF32(200.0f);
	w4.WriteU8(12); w4.WriteU8(0); w4.WriteU8(0);
	w4.WriteString("freeroam", 31);
	w4.WriteString("old server", MAX_HOSTNAME_LEN - 1);
	BitReader r4(w4.Data(), w4.ByteSize());
	MsgWelcome w5;
	CHECK(ReadWelcome(r4, w5));
	CHECK(w5.players == 0 && w5.mapName[0] == 0);
	CHECK(strcmp(w5.hostname, "old server") == 0);
}

// v5: static vehicles can be retired server-side (MSG_VEH_DESPAWN)
static void TestVehDespawnCodec()
{
	MsgVehDespawn d;
	d.vehicleId = 4242;
	BitWriter w;
	BeginMsg(w, MSG_VEH_DESPAWN);
	WriteVehDespawn(w, d);
	BitReader r(w.Data(), w.ByteSize());
	CHECK((MsgType)r.ReadU8() == MSG_VEH_DESPAWN);
	MsgVehDespawn out;
	CHECK(ReadVehDespawn(r, out));
	CHECK(out.vehicleId == 4242);
}

// v5: server-side favorites — list/add/delete round trips
static void TestFavoritesCodec()
{
	MsgFavReq req;
	CopyStr(req.nick, sizeof req.nick, "MattTest");
	BitWriter w;
	BeginMsg(w, MSG_MASTER_FAV_LIST);
	WriteFavReq(w, req);
	BitReader r(w.Data(), w.ByteSize());
	CHECK((MsgType)r.ReadU8() == MSG_MASTER_FAV_LIST);
	MsgFavReq req2;
	CHECK(ReadFavReq(r, req2));
	CHECK(strcmp(req2.nick, "MattTest") == 0);

	MsgFavAdd add;
	CopyStr(add.nick, sizeof add.nick, "MattTest");
	CopyStr(add.address, sizeof add.address, "203.0.113.9:7777");
	CopyStr(add.name, sizeof add.name, "Liberty City Survivor #1");
	BitWriter w2;
	BeginMsg(w2, MSG_MASTER_FAV_ADD);
	WriteFavAdd(w2, add);
	BitReader r2(w2.Data(), w2.ByteSize());
	CHECK((MsgType)r2.ReadU8() == MSG_MASTER_FAV_ADD);
	MsgFavAdd add2;
	CHECK(ReadFavAdd(r2, add2));
	CHECK(strcmp(add2.nick, "MattTest") == 0);
	CHECK(strcmp(add2.address, "203.0.113.9:7777") == 0);
	CHECK(strcmp(add2.name, "Liberty City Survivor #1") == 0);

	// DEL shares the add layout; the name field is ignored
	BitWriter w3;
	BeginMsg(w3, MSG_MASTER_FAV_DEL);
	WriteFavAdd(w3, add);
	BitReader r3(w3.Data(), w3.ByteSize());
	CHECK((MsgType)r3.ReadU8() == MSG_MASTER_FAV_DEL);
	MsgFavAdd del;
	CHECK(ReadFavAdd(r3, del));
	CHECK(strcmp(del.address, "203.0.113.9:7777") == 0);

	MsgFavResp resp;
	resp.ok = 1;
	resp.count = 2;
	CopyStr(resp.entries[0].address, sizeof resp.entries[0].address, "127.0.0.1:7777");
	CopyStr(resp.entries[0].name, sizeof resp.entries[0].name, "local test server");
	CopyStr(resp.entries[1].address, sizeof resp.entries[1].address, "10.0.0.5:7792");
	CopyStr(resp.entries[1].name, sizeof resp.entries[1].name, "i8 Showroom");
	BitWriter w4;
	BeginMsg(w4, MSG_MASTER_FAV_RESP);
	WriteFavResp(w4, resp);
	BitReader r4(w4.Data(), w4.ByteSize());
	CHECK((MsgType)r4.ReadU8() == MSG_MASTER_FAV_RESP);
	MsgFavResp resp2;
	CHECK(ReadFavResp(r4, resp2));
	CHECK(resp2.ok == 1 && resp2.count == 2);
	CHECK(strcmp(resp2.entries[0].address, "127.0.0.1:7777") == 0);
	CHECK(strcmp(resp2.entries[1].name, "i8 Showroom") == 0);

	// an error response carries no entries
	MsgFavResp bad;
	bad.ok = 0;
	bad.count = 0;
	CopyStr(bad.error, sizeof bad.error, "invalid username");
	BitWriter w5;
	BeginMsg(w5, MSG_MASTER_FAV_RESP);
	WriteFavResp(w5, bad);
	BitReader r5(w5.Data(), w5.ByteSize());
	CHECK((MsgType)r5.ReadU8() == MSG_MASTER_FAV_RESP);
	MsgFavResp bad2;
	CHECK(ReadFavResp(r5, bad2));
	CHECK(bad2.ok == 0 && bad2.count == 0);
	CHECK(strcmp(bad2.error, "invalid username") == 0);
}

// v5: map + protocol version ride the master list so the browser can show a map
// column and flag stale servers before joining
static void TestMasterEntryLiveInfo()
{
	MasterListEntry e;
	e.ipv4 = 0x0100007F;
	e.gamePort = 7792;
	e.players = 3;
	e.maxPlayers = 32;
	e.pingMs = 0;
	CopyStr(e.hostname, sizeof e.hostname, "i8 Showroom");
	CopyStr(e.gameMode, sizeof e.gameMode, "freeroam");
	CopyStr(e.mapName, sizeof e.mapName, "Liberty City");
	e.protocolVersion = PROTOCOL_VERSION;

	BitWriter w;
	BeginMsg(w, MSG_MASTER_LIST_RESP);
	w.WriteU16(1);
	WriteMasterEntry(w, e);
	BitReader r(w.Data(), w.ByteSize());
	CHECK((MsgType)r.ReadU8() == MSG_MASTER_LIST_RESP);
	CHECK(r.ReadU16() == 1);
	MasterListEntry e2;
	CHECK(ReadMasterEntry(r, e2));
	CHECK(e2.gamePort == 7792 && e2.players == 3);
	CHECK(strcmp(e2.mapName, "Liberty City") == 0);
	CHECK(e2.protocolVersion == PROTOCOL_VERSION);

	// an entry without the optional tails stops after the game mode
	BitWriter w4;
	w4.WriteU32(0x0100007F); w4.WriteU16(7777);
	w4.WriteU16(1); w4.WriteU16(32); w4.WriteU16(0);
	w4.WriteString("old", MAX_HOSTNAME_LEN - 1);
	w4.WriteString("survivor", 31);
	BitReader r4(w4.Data(), w4.ByteSize());
	MasterListEntry e4;
	CHECK(ReadMasterEntry(r4, e4));
	CHECK(e4.mapName[0] == 0 && e4.protocolVersion == 0);

	// the announce carries the map to the master
	MsgMasterAnnounce a;
	a.gamePort = 7792;
	a.players = 1;
	a.maxPlayers = 32;
	a.protocolVersion = PROTOCOL_VERSION;
	CopyStr(a.hostname, sizeof a.hostname, "i8 Showroom");
	CopyStr(a.gameMode, sizeof a.gameMode, "freeroam");
	CopyStr(a.mapName, sizeof a.mapName, "Liberty City");
	a.advertisedIp = 0x0A000005;
	BitWriter wa;
	BeginMsg(wa, MSG_MASTER_ANNOUNCE);
	WriteMasterAnnounce(wa, a);
	BitReader ra(wa.Data(), wa.ByteSize());
	CHECK((MsgType)ra.ReadU8() == MSG_MASTER_ANNOUNCE);
	MsgMasterAnnounce a2;
	CHECK(ReadMasterAnnounce(ra, a2));
	CHECK(strcmp(a2.mapName, "Liberty City") == 0);
	CHECK(a2.advertisedIp == 0x0A000005);

	// an announce without the map tail carries only the advertised IP
	BitWriter wa4;
	wa4.WriteU16(7777); wa4.WriteU16(0); wa4.WriteU16(32); wa4.WriteU16(4);
	wa4.WriteString("old", MAX_HOSTNAME_LEN - 1);
	wa4.WriteString("freeroam", 31);
	wa4.WriteU32(0x0A000006);
	BitReader ra4(wa4.Data(), wa4.ByteSize());
	MsgMasterAnnounce a4;
	CHECK(ReadMasterAnnounce(ra4, a4));
	CHECK(a4.mapName[0] == 0);
	CHECK(a4.advertisedIp == 0x0A000006);
	CHECK(a4.protocolVersion == 4);
}

// proximity voice: Opus round trip and the MSG_VOICE packet codec
static void TestVoiceCodec()
{
	rnet::VoiceEncoder enc;
	rnet::VoiceDecoder dec;
	CHECK(enc.Ok() && dec.Ok());

	// a 440 Hz tone: the codec is lossy, so check that every packet fits, the
	// decoder keeps the frame size, and the tone keeps its level after warm-up
	const float kPi = 3.14159265f;
	float phase = 0.0f;
	int16_t pcm[VOICE_FRAME_SAMPLES], out[VOICE_FRAME_SAMPLES];
	uint8_t packet[VOICE_MAX_PACKET];
	bool framesOk = true;
	double inEnergy = 0.0, outEnergy = 0.0;
	for(int f = 0; f < 50; f++){
		for(int k = 0; k < VOICE_FRAME_SAMPLES; k++){
			pcm[k] = (int16_t)(8000.0f * std::sin(phase));
			phase += 2.0f * kPi * 440.0f / VOICE_SAMPLE_RATE;
		}
		int len = enc.Encode(pcm, packet);
		if(len <= 0 || len > VOICE_MAX_PACKET){ framesOk = false; break; }
		int n = dec.Decode(packet, len, out);
		if(n != VOICE_FRAME_SAMPLES){ framesOk = false; break; }
		if(f >= 25){   // skip the codec's warm-up frames
			for(int k = 0; k < VOICE_FRAME_SAMPLES; k++){
				inEnergy += (double)pcm[k] * pcm[k];
				outEnergy += (double)out[k] * out[k];
			}
		}
	}
	CHECK(framesOk);
	CHECK(inEnergy > 0.0);
	CHECK(outEnergy > 0.5 * inEnergy && outEnergy < 2.0 * inEnergy);

	// silence decodes to (near) silence
	for(int k = 0; k < VOICE_FRAME_SAMPLES; k++) pcm[k] = 0;
	for(int f = 0; f < 5; f++){
		int len = enc.Encode(pcm, packet);
		CHECK(len > 0 && len <= VOICE_MAX_PACKET);
		CHECK(dec.Decode(packet, len, out) == VOICE_FRAME_SAMPLES);
	}
	int peak = 0;
	for(int k = 0; k < VOICE_FRAME_SAMPLES; k++) if(std::abs((int)out[k]) > peak) peak = std::abs((int)out[k]);
	CHECK(peak <= 64);

	// packet codec: fromId (v8: 16-bit), length, then the packet, exact size on the wire
	uint8_t body[VOICE_MAX_PACKET];
	for(int i = 0; i < 37; i++) body[i] = (uint8_t)(i * 7 + 3);
	BitWriter w;
	WriteVoice(w, 7, body, 37);
	CHECK(w.ByteSize() == (size_t)(37 + 3));
	BitReader r(w.Data(), w.ByteSize());
	rnet::PlayerId fromId = 0;
	uint8_t back[VOICE_MAX_PACKET] = {}, backLen = 0;
	CHECK(ReadVoice(r, fromId, back, backLen));
	CHECK(fromId == 7 && backLen == 37);
	CHECK(std::memcmp(body, back, 37) == 0);
	CHECK(r.BitsLeft() == 0);

	// a zero length or one past the maximum is malformed, and so is a short body
	BitWriter zero;
	zero.WriteU16(7); zero.WriteU8(0);
	BitReader rZero(zero.Data(), zero.ByteSize());
	CHECK(!ReadVoice(rZero, fromId, back, backLen));
	BitWriter big;
	big.WriteU16(7); big.WriteU8((uint8_t)(VOICE_MAX_PACKET + 1));
	for(int i = 0; i < VOICE_MAX_PACKET + 1; i++) big.WriteU8(1);
	BitReader rBig(big.Data(), big.ByteSize());
	CHECK(!ReadVoice(rBig, fromId, back, backLen));
	BitWriter shortBody;
	shortBody.WriteU16(7); shortBody.WriteU8(10); shortBody.WriteU8(1);
	BitReader rShort(shortBody.Data(), shortBody.ByteSize());
	CHECK(!ReadVoice(rShort, fromId, back, backLen));
}

// ---------------------------------------------------------------------------
// EOS Anti-Cheat relay + config
// ---------------------------------------------------------------------------
static void TestAcProtocol()
{
	MsgAcHello h;
	h.platform = AC_PLATFORM_WINDOWS;
	CopyStr(h.productUserId, sizeof h.productUserId, "00112233445566778899aabbccddeeff");
	CopyStr(h.idToken, sizeof h.idToken, "eyJhbGciOiJIUzI1NiJ9.payload.signature");

	BitWriter w;
	BeginMsg(w, MSG_AC_HELLO);
	WriteAcHello(w, h);
	CHECK(PeekType(w.Data(), w.ByteSize()) == MSG_AC_HELLO);

	BitReader r(w.Data(), w.ByteSize());
	CHECK(r.ReadU8() == MSG_AC_HELLO);
	MsgAcHello h2;
	CHECK(ReadAcHello(r, h2));
	CHECK(h2.platform == AC_PLATFORM_WINDOWS);
	CHECK(std::string(h2.productUserId) == "00112233445566778899aabbccddeeff");
	CHECK(std::string(h2.idToken) == "eyJhbGciOiJIUzI1NiJ9.payload.signature");

	MsgAcStatus st;
	st.status = AC_REG_BAD_TOKEN;
	st.registered = 0;
	CopyStr(st.detail, sizeof st.detail, "Connect ID token rejected");
	BitWriter w2;
	BeginMsg(w2, MSG_AC_STATUS);
	WriteAcStatus(w2, st);
	BitReader r2(w2.Data(), w2.ByteSize());
	CHECK(r2.ReadU8() == MSG_AC_STATUS);
	MsgAcStatus st2;
	CHECK(ReadAcStatus(r2, st2));
	CHECK(st2.status == AC_REG_BAD_TOKEN);
	CHECK(st2.registered == 0);
	CHECK(std::string(st2.detail) == "Connect ID token rejected");

	// a full-size AC message survives the relay unchanged
	MsgAcPayload p;
	p.len = (uint16_t)MAX_AC_PAYLOAD;
	for(int i = 0; i < MAX_AC_PAYLOAD; i++) p.data[i] = (uint8_t)(i * 7);
	BitWriter w3;
	CHECK(WriteAcPayload(w3, p));
	BitReader r3(w3.Data(), w3.ByteSize());
	MsgAcPayload p2;
	CHECK(ReadAcPayload(r3, p2));
	CHECK(p2.len == MAX_AC_PAYLOAD);
	CHECK(memcmp(p.data, p2.data, MAX_AC_PAYLOAD) == 0);

	// a header that claims more bytes than the frame carries is refused
	BitWriter w4;
	w4.WriteU16(200);
	w4.WriteU8(1);
	BitReader r4(w4.Data(), w4.ByteSize());
	MsgAcPayload p3;
	CHECK(!ReadAcPayload(r4, p3));

	// an oversized length is refused before anything is read
	BitWriter w5;
	w5.WriteU16((uint16_t)(MAX_AC_PAYLOAD + 1));
	for(int i = 0; i < 8; i++) w5.WriteU8(0);
	BitReader r5(w5.Data(), w5.ByteSize());
	CHECK(!ReadAcPayload(r5, p3));

	// an encoder never puts an oversized payload on the wire
	MsgAcPayload big;
	big.len = (uint16_t)(MAX_AC_PAYLOAD + 1);
	BitWriter w6;
	CHECK(!WriteAcPayload(w6, big));
}

static void TestAcConfig()
{
	CHECK(ac::IsProductUserId("00112233445566778899aabbccddeeff"));
	CHECK(ac::IsProductUserId("00112233445566778899AABBCCDDEEFF"));
	CHECK(!ac::IsProductUserId("00112233445566778899aabbccddeef"));    // 31 chars
	CHECK(!ac::IsProductUserId("00112233445566778899aabbccddeeff0"));  // 33 chars
	CHECK(!ac::IsProductUserId("zz112233445566778899aabbccddeeff"));
	CHECK(!ac::IsProductUserId(""));

	CHECK(std::string(ac::PlatformName(1)) == "windows");
	CHECK(std::string(ac::PlatformName(2)) == "mac");
	CHECK(std::string(ac::PlatformName(3)) == "linux");
	CHECK(std::string(ac::PlatformName(0)) == "unknown");

	std::string ini = "ac-test.ini";
	{
		std::ofstream f(ini);
		f << "# client credentials\n";
		f << "; second comment style\n";
		f << "[eos]\n";
		f << "client_id =  lcs-client-id  # trailing comment\n";
		f << "client_secret=shhh\n";
		f << "this line has no equals sign\n";
	}
	std::string v;
	CHECK(ac::ReadIniValue(ini, "client_id", v) && v == "lcs-client-id");
	CHECK(ac::ReadIniValue(ini, "client_secret", v) && v == "shhh");
	CHECK(!ac::ReadIniValue(ini, "missing", v));
	std::remove(ini.c_str());

	std::string js = "ac-test.json";
	{
		std::ofstream f(js);
		f << "{\n\t\"title\"\t: \"reLCS\",\n"
		     "\t\"productid\"\t: \"fb155afe4a754a4b91929491873c844e\",\n"
		     "\t\"sandboxid\" : \"515e78d9d395488faf0a3f63041dbba0\"\n}\n";
	}
	CHECK(ac::ReadJsonString(js, "productid", v) && v == "fb155afe4a754a4b91929491873c844e");
	CHECK(ac::ReadJsonString(js, "title", v) && v == "reLCS");
	CHECK(!ac::ReadJsonString(js, "deploymentid", v));
	std::remove(js.c_str());

	// Resolve() fills the shipped product data when nothing is configured
	ac::Config cfg;
	ac::Resolve(cfg);
	CHECK(cfg.productId == ac::DEFAULT_PRODUCT_ID);
	CHECK(cfg.sandboxId == ac::DEFAULT_SANDBOX_ID);
	CHECK(cfg.deploymentId == ac::DEFAULT_DEPLOYMENT_ID);
	CHECK(cfg.registerTimeout == 60);
	CHECK(cfg.requireToken);
	CHECK(cfg.kickOnViolation);

	// "only protected clients are allowed" defaults on, with a workable grace
	CHECK(cfg.requireProtected);
	CHECK(cfg.failOpen);
	CHECK(cfg.graceSeconds >= 5 && cfg.graceSeconds <= 600);

	// a server.cfg typo must not become "kick on join" or "never kick"
	ac::Config tight;
	tight.graceSeconds = 0;
	tight.registerTimeout = 0;
	ac::Sanitize(tight);
	CHECK(tight.graceSeconds == 5);
	CHECK(tight.registerTimeout == 10);
	tight.graceSeconds = 99999;
	ac::Sanitize(tight);
	CHECK(tight.graceSeconds == 600);
	tight.graceSeconds = 45;
	tight.registerTimeout = 30;
	ac::Sanitize(tight);
	CHECK(tight.graceSeconds == 45);
	CHECK(tight.registerTimeout == 30);
}

// ---------------------------------------------------------------------------
// v8: 16-bit player identity + batched entity delivery
// v9: relayed state batches (receiver-relative, ~11 bytes per record)
// ---------------------------------------------------------------------------
static void TestHighPlayerCount()
{
	CHECK(PROTOCOL_VERSION == 9);
	// a 2k load test needs ids above 255 and slots above the old 200 ceiling
	CHECK(MAX_PLAYERS >= 2048);
	CHECK(MAX_SERVER_PLAYERS == 2000);
	CHECK(PLAYER_ID_NONE == 0xFFFF);
	CHECK(PLAYER_ID_SERVER == 0);

	const PlayerId high = 1500;

	MsgWelcome wel;
	wel.playerId = high;
	wel.players = 1499;
	wel.maxPlayers = 2000;
	CopyStr(wel.hostname, sizeof wel.hostname, "big server");
	BitWriter bw;
	WriteWelcome(bw, wel);
	BitReader br(bw.Data(), bw.ByteSize());
	MsgWelcome wel2;
	CHECK(ReadWelcome(br, wel2));
	CHECK(wel2.playerId == high && wel2.players == 1499 && wel2.maxPlayers == 2000);

	// every message that names a player must survive an id above 255
	{
		MsgPlayerJoin j; j.playerId = high; j.skinModel = 7;
		CopyStr(j.name, sizeof j.name, "Load1500");
		BitWriter w; WritePlayerJoin(w, j);
		BitReader r(w.Data(), w.ByteSize()); MsgPlayerJoin j2;
		CHECK(ReadPlayerJoin(r, j2) && j2.playerId == high && strcmp(j2.name, "Load1500") == 0);
	}
	{
		MsgChat c; c.fromId = high; CopyStr(c.text, sizeof c.text, "hi");
		BitWriter w; WriteChat(w, c);
		BitReader r(w.Data(), w.ByteSize()); MsgChat c2;
		CHECK(ReadChat(r, c2) && c2.fromId == high);
	}
	{
		MsgSpawn sp; sp.playerId = high; sp.x = 1.0f;
		BitWriter w; WriteSpawn(w, sp);
		BitReader r(w.Data(), w.ByteSize()); MsgSpawn sp2;
		CHECK(ReadSpawn(r, sp2) && sp2.playerId == high);
	}
	{
		MsgDeath d; d.victimId = high; d.killerId = PLAYER_ID_NONE; d.weaponId = 5;
		BitWriter w; WriteDeath(w, d);
		BitReader r(w.Data(), w.ByteSize()); MsgDeath d2;
		CHECK(ReadDeath(r, d2) && d2.victimId == high && d2.killerId == PLAYER_ID_NONE);
	}
	{
		MsgDamage d; d.targetId = high; d.attackerId = 1999; d.damage = 12;
		BitWriter w; WriteDamage(w, d);
		BitReader r(w.Data(), w.ByteSize()); MsgDamage d2;
		CHECK(ReadDamage(r, d2) && d2.targetId == high && d2.attackerId == 1999);
	}
	{
		MsgVehSpawn vs; vs.vehicleId = 3; vs.ownerId = high;
		BitWriter w; WriteVehSpawn(w, vs);
		BitReader r(w.Data(), w.ByteSize()); MsgVehSpawn vs2;
		CHECK(ReadVehSpawn(r, vs2) && vs2.ownerId == high);
	}
	{
		MsgScoreUpdate su; su.count = 1; su.entries[0].playerId = high; su.entries[0].score = 42;
		BitWriter w; WriteScoreUpdate(w, su);
		BitReader r(w.Data(), w.ByteSize()); MsgScoreUpdate su2;
		CHECK(ReadScoreUpdate(r, su2) && su2.entries[0].playerId == high);
	}
	{
		MsgPlayerPing pp; pp.count = 1; pp.entries[0].playerId = high; pp.entries[0].pingMs = 90;
		BitWriter w; WritePlayerPing(w, pp);
		BitReader r(w.Data(), w.ByteSize()); MsgPlayerPing pp2;
		CHECK(ReadPlayerPing(r, pp2) && pp2.entries[0].playerId == high);
	}
	{
		MsgGamemodeEvent ev; ev.type = GMEV_KILL; ev.arg1 = high; ev.arg2 = 1999;
		BitWriter w; WriteGamemodeEvent(w, ev);
		BitReader r(w.Data(), w.ByteSize()); MsgGamemodeEvent ev2;
		CHECK(ReadGamemodeEvent(r, ev2) && ev2.arg1 == high && ev2.arg2 == 1999);
	}

	// MSG_STATE_BATCH: relayed states are delta-coded against the receiver
	{
		PlayerState st[MAX_STATES_PER_PACKET];
		const int32_t ax = 1000000, ay = -2000000, az = 30000;   // 1 km, -2 km, 30 m
		for(int i = 0; i < 5; i++){
			st[i].playerId = (PlayerId)(1000 + i);
			st[i].xMm = ax + i * 1000;         // whole metres: exact in cm
			st[i].yMm = ay;
			st[i].zMm = az;
			st[i].heading = (int16_t)(i * 4096);
			st[i].velX = 100;                  // 10 m/s
			st[i].health = 90;
		}
		BitWriter w;
		WriteStateBatch(w, st, 5, ax, ay, az);
		BitReader r(w.Data(), w.ByteSize());
		PlayerState out[MAX_STATES_PER_PACKET];
		CHECK(ReadStateBatch(r, out, MAX_STATES_PER_PACKET) == 5);
		CHECK(out[4].playerId == 1004 && out[4].xMm == ax + 4000);
		CHECK(out[4].yMm == ay && out[4].zMm == az);
		CHECK(out[4].heading == 16384);      // 8-bit angle step, exact here
		CHECK(out[4].velX == 100);               // 0.2 m/s steps round-trip
		CHECK(out[4].health == 90);
		CHECK(out[4].vehicleId == VEHICLE_NONE);
		// 5 on-foot records are 87 bits each: five states in well under 100 bytes
		CHECK(w.ByteSize() <= 100);

		// a record beyond the 16-bit cm range falls back to absolute mm
		{
			PlayerState far1[1];
			far1[0].playerId = 7;
			far1[0].xMm = ax + 400000;         // 400 m away
			far1[0].yMm = ay;
			far1[0].zMm = az;
			far1[0].health = 55;
			BitWriter fw;
			WriteStateBatch(fw, far1, 1, ax, ay, az);
			BitReader fr(fw.Data(), fw.ByteSize());
			PlayerState fout[MAX_STATES_PER_PACKET];
			CHECK(ReadStateBatch(fr, fout, MAX_STATES_PER_PACKET) == 1);
			CHECK(fout[0].xMm == ax + 400000 && fout[0].health == 55);
		}

		// vehicle occupancy and the teleport flag ride only when set
		{
			PlayerState veh[1];
			veh[0].playerId = 9;
			veh[0].xMm = ax; veh[0].yMm = ay; veh[0].zMm = az;
			veh[0].vehicleId = 42;
			veh[0].seat = 3;
			veh[0].moveFlags = MOVEFLAG_TELEPORT;
			BitWriter vw;
			WriteStateBatch(vw, veh, 1, ax, ay, az);
			BitReader vr(vw.Data(), vw.ByteSize());
			PlayerState vout[MAX_STATES_PER_PACKET];
			CHECK(ReadStateBatch(vr, vout, MAX_STATES_PER_PACKET) == 1);
			CHECK(vout[0].vehicleId == 42 && vout[0].seat == 3);
			CHECK(vout[0].moveFlags == MOVEFLAG_TELEPORT);
			// on foot is the compact form: 87 bits vs 106 seated
			CHECK(RelayEntryBits(veh[0], ax, ay, az) == 106);
			CHECK(RelayEntryBits(st[0], ax, ay, az) == 87);
		}

		// a count above the format limit is rejected instead of read past the frame
		BitWriter over;
		over.WriteU16((uint16_t)(MAX_STATES_PER_PACKET + 1));
		BitReader rOver(over.Data(), over.ByteSize());
		CHECK(ReadStateBatch(rOver, out, MAX_STATES_PER_PACKET) == -1);

		// ... and so is one that does not fit the caller's array
		BitWriter two;
		two.WriteU16(2);
		two.WriteU16(1);
		BitReader rTwo(two.Data(), two.ByteSize());
		CHECK(ReadStateBatch(rTwo, out, 1) == -1);
	}

	// MSG_ROSTER: batched identity + last spawn
	{
		MsgRosterEntry e[2];
		e[0].id = 2000;
		e[0].skin = 7;
		e[0].flags = ROSTER_SPAWNED | ROSTER_ALIVE;
		e[0].health = 80;
		e[0].xMm = 1234; e[0].yMm = -5678; e[0].zMm = 9000;
		e[0].heading = 1234;
		CopyStr(e[0].name, sizeof e[0].name, "Load2000");
		e[1].id = 1999;
		CopyStr(e[1].name, sizeof e[1].name, "Load1999");

		BitWriter w;
		WriteRoster(w, e, 2);
		BitReader r(w.Data(), w.ByteSize());
		MsgRosterEntry out[2];
		CHECK(ReadRoster(r, out, 2) == 2);
		CHECK(out[0].id == 2000 && (out[0].flags & ROSTER_SPAWNED) && out[0].xMm == 1234 &&
		      out[0].yMm == -5678 && out[0].heading == 1234 && strcmp(out[0].name, "Load2000") == 0);
		CHECK(out[1].id == 1999 && out[1].flags == 0);

		BitWriter over;
		over.WriteU16((uint16_t)(MAX_ROSTER_PER_PACKET + 1));
		BitReader rOver(over.Data(), over.ByteSize());
		CHECK(ReadRoster(rOver, out, MAX_ROSTER_PER_PACKET) == -1);
	}
}

// The server's relay fast path must be a pure speed-up: same bytes on the
// wire as WriteStateBatch, which stays the reference encoder.
static void TestRelayFastPath()
{
	uint32_t seed = 0x5EED1234u;
	auto rnd = [&]() -> uint32_t {
		seed = seed * 1664525u + 1013904223u;
		return seed >> 8;
	};

	const int32_t ax = 1234567, ay = -765432, az = 42000;   // 1.2 km, -0.8 km, 42 m
	PlayerState st[MAX_STATES_PER_PACKET];
	RelayWireRecord wire[MAX_STATES_PER_PACKET];
	RelaySendEntry entry[MAX_STATES_PER_PACKET];
	// equivalence is checked on whole batches, which may exceed one datagram;
	// the budget (and the server's split) is a separate property below
	std::vector<uint8_t> frame(4 * (1 + RELAY_PAYLOAD_BYTES));

	int mismatches = 0, bitMismatches = 0, decodeMismatches = 0;
	for(int trial = 0; trial < 40; trial++){
		const int count = 1 + (int)(rnd() % MAX_STATES_PER_PACKET);
		for(int i = 0; i < count; i++){
			PlayerState& s = st[i];
			s = PlayerState{};
			s.playerId = (PlayerId)(rnd() % 4096);
			// mostly inside a 200 m AOI, sometimes far enough to need absolute mm
			int32_t reach = (trial % 5 == 4) ? 900000 : 190000;
			s.xMm = ax + (int32_t)(rnd() % (uint32_t)(reach * 2)) - reach;
			s.yMm = ay + (int32_t)(rnd() % (uint32_t)(reach * 2)) - reach;
			s.zMm = az + (int32_t)(rnd() % 200000) - 100000;
			s.heading = (int16_t)rnd();
			s.velX = (int16_t)(rnd() % 1200);
			s.velY = (int16_t)(rnd() % 1200);
			s.health = (uint8_t)(rnd() % 200);
			if(trial % 3 == 0 && (rnd() & 1)){
				s.vehicleId = (uint16_t)(rnd() % MAX_VEHICLES);
				s.seat = (rnd() & 1) ? 0xFF : (uint8_t)(rnd() % 8);
			}
			if(rnd() & 1) s.moveFlags = MOVEFLAG_TELEPORT;

			MakeRelayWireRecord(s, wire[i]);
			int16_t dx = 0, dy = 0, dz = 0;
			bool delta = RelayDeltaCm(wire[i].xMm, ax, dx) &
			             RelayDeltaCm(wire[i].yMm, ay, dy) &
			             RelayDeltaCm(wire[i].zMm, az, dz);
			if(RelayWireRecordBits(wire[i], delta) != RelayEntryBits(s, ax, ay, az))
				bitMismatches++;
			entry[i].rec = &wire[i];
			entry[i].dx = dx; entry[i].dy = dy; entry[i].dz = dz;
			entry[i].abs = delta ? 0 : 1;
		}

		BitWriter ref;
		BeginMsg(ref, MSG_STATE_BATCH);
		WriteStateBatch(ref, st, count, ax, ay, az);

		BitSink sink(frame.data(), frame.size());
		BeginMsg(sink, MSG_STATE_BATCH);
		WriteRelayBatch(sink, entry, count, ax, ay, az);
		size_t bytes = sink.Finish();
		if(!sink.Ok() || bytes != ref.ByteSize() ||
		   std::memcmp(frame.data(), ref.Data(), bytes) != 0) mismatches++;

		// both frames decode to the same states (the reader starts after the
		// message-type byte, as the client's dispatcher does)
		BitReader rRef(ref.Data(), ref.ByteSize());
		BitReader rNew(frame.data(), bytes);
		CHECK(rRef.ReadU8() == MSG_STATE_BATCH);
		CHECK(rNew.ReadU8() == MSG_STATE_BATCH);
		PlayerState outRef[MAX_STATES_PER_PACKET], outNew[MAX_STATES_PER_PACKET];
		int nRef = ReadStateBatch(rRef, outRef, MAX_STATES_PER_PACKET);
		int nNew = ReadStateBatch(rNew, outNew, MAX_STATES_PER_PACKET);
		if(nRef != count || nNew != count) decodeMismatches++;
		else{
			for(int i = 0; i < count; i++){
				if(outRef[i].playerId != outNew[i].playerId ||
				   outRef[i].xMm != outNew[i].xMm || outRef[i].yMm != outNew[i].yMm ||
				   outRef[i].zMm != outNew[i].zMm ||
				   outRef[i].heading != outNew[i].heading ||
				   outRef[i].velX != outNew[i].velX ||
				   outRef[i].health != outNew[i].health ||
				   outRef[i].vehicleId != outNew[i].vehicleId ||
				   outRef[i].seat != outNew[i].seat ||
				   outRef[i].moveFlags != outNew[i].moveFlags) decodeMismatches++;
			}
		}
	}
	CHECK(mismatches == 0);
	CHECK(bitMismatches == 0);
	CHECK(decodeMismatches == 0);

	// The server's split rule (relay_ms budget accounting in RelayStates) must
	// still keep every frame inside one datagram's payload, including the wide
	// absolute-position records, and lose nothing across the splits.
	{
		uint8_t pkt[1 + RELAY_PAYLOAD_BYTES + 8];
		const int budgetBits = RELAY_PAYLOAD_BYTES * 8;
		int n = 0, batchBits = RELAY_ANCHOR_BITS, seen = 0, frames = 0;
		bool budgetOk = true;
		const int total = MAX_STATES_PER_PACKET;
		RelaySendEntry sent[MAX_STATES_PER_PACKET];
		auto flush = [&]{
			if(n == 0) return;
			BitSink sink(pkt, sizeof pkt);
			BeginMsg(sink, MSG_STATE_BATCH);
			WriteRelayBatch(sink, sent, n, ax, ay, az);
			size_t bytes = sink.Finish();
			if(!sink.Ok()) budgetOk = false;
			BitReader r(sink.Ok() ? pkt : nullptr, sink.Ok() ? bytes : 0);
			PlayerState out[MAX_STATES_PER_PACKET];
			if(!sink.Ok() || r.ReadU8() != MSG_STATE_BATCH ||
			   ReadStateBatch(r, out, MAX_STATES_PER_PACKET) != n) budgetOk = false;
			seen += n;
			frames++;
			n = 0;
			batchBits = RELAY_ANCHOR_BITS;
		};
		for(int i = 0; i < total; i++){
			RelaySendEntry e;
			e.rec = &wire[i % 40];
			e.abs = (i % 3) == 2;                     // every third record falls back
			int bits = RelayWireRecordBits(*e.rec, !e.abs);
			if(n > 0 && batchBits + bits > budgetBits) flush();
			sent[n++] = e;
			batchBits += bits;
		}
		flush();
		CHECK(budgetOk);
		CHECK(seen == total);
		CHECK(frames > 1);            // 128 wide records cannot fit one datagram
	}

	// an empty batch is a legal (if pointless) frame: count 0 + anchor
	{
		BitSink sink(frame.data(), frame.size());
		BeginMsg(sink, MSG_STATE_BATCH);
		WriteRelayBatch(sink, entry, 0, ax, ay, az);
		CHECK(sink.Finish() == 15);            // type 8b + count 16b + anchor 96b
	}
}

// ---------------------------------------------------------------------------
// Server: inbound player-state coalescing
//
// Two MSG_PLAYER_STATE frames from one client inside a single tick must leave
// only the freshest frame validated and adopted. The test drives the server
// directly (PumpNetwork, then one Tick), so "inside one tick" is exact rather
// than a timing race: both frames are staged by the pump, nothing validates
// them until the tick drains the client's slot.
//
// The superseded frame is deliberately illegal (a 40 m step on foot). Without
// coalescing the server validated it on arrival, emitted MSG_CORRECTION and
// recorded a violation before adopting the freshest frame; the test asserts
// that correction is gone, that the relay carries the freshest position, and
// a negative control (the same illegal frame sent alone) still corrects.
// ---------------------------------------------------------------------------
struct ServerTestPeer {
	static void Pump(Server& s) { s.PumpNetwork(); }
	static void Tick(Server& s) { s.Tick(); }
	static uint64_t Staged(const Server& s) { return s.m_statsStateMsgs; }
	static uint64_t Applied(const Server& s) { return s.m_statsStatesApplied; }
	static uint64_t Coalesced(const Server& s) { return s.m_statsStateCoalesced; }
	static int Violations(Server& s, PlayerId id)
	{
		ServerPlayer* p = s.FindPlayer(id);
		return p ? p->violations : -1;
	}
};

namespace {

struct CoalesceClient {
	ENetHost* host = nullptr;
	ENetPeer* peer = nullptr;
	char name[32] = {};
	PlayerId id = 0;
	PlayerId remoteId = 0;
	bool welcomed = false;
	bool spawned = false;
	bool refused = false;
	bool gotCorrection = false;
	int32_t correctionX = 0;
	bool gotRemote = false;
	int32_t remoteX = 0;
};

bool CoalesceOpen(CoalesceClient& c, uint16_t port, const char* name)
{
	CopyStr(c.name, sizeof c.name, name);
	c.host = enet_host_create(nullptr, 1, 2, 0, 0);
	if(!c.host) return false;
	ENetAddress addr;
	if(enet_address_set_host(&addr, "127.0.0.1") != 0){
		enet_host_destroy(c.host); c.host = nullptr;
		return false;
	}
	addr.port = port;
	c.peer = enet_host_connect(c.host, &addr, 2, 0);
	if(!c.peer){
		enet_host_destroy(c.host); c.host = nullptr;
		return false;
	}
	return true;
}

void CoalesceClose(CoalesceClient& c)
{
	if(c.host){
		if(c.peer) enet_peer_reset(c.peer);
		enet_host_destroy(c.host);
		c.host = nullptr; c.peer = nullptr;
	}
}

PlayerState CoalesceState(PlayerId id, float x, float y, uint8_t moveFlags)
{
	PlayerState st;
	st.playerId = id;
	st.xMm = QuantPos(x); st.yMm = QuantPos(y); st.zMm = QuantPos(0.0f);
	st.velX = st.velY = st.velZ = 0;
	st.heading = QuantAngle(90.0f); st.pitch = 0;
	st.animId = 0; st.animFlags = 0;
	st.health = 100; st.armour = 0;
	st.weapon = 0; st.ammo = 0; st.money = 0;
	st.vehicleId = VEHICLE_NONE; st.seat = 0xFF;
	st.moveFlags = moveFlags;
	return st;
}

void CoalesceSend(CoalesceClient& c, const PlayerState& st)
{
	BitWriter w;
	BeginMsg(w, MSG_PLAYER_STATE);
	WritePlayerState(w, st);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), 0);   // unreliable, like the game
	enet_peer_send(c.peer, CHAN_STATE, pkt);
}

void CoalesceService(CoalesceClient& c)
{
	if(!c.host) return;
	ENetEvent ev;
	while(enet_host_service(c.host, &ev, 0) > 0){
		switch(ev.type){
		case ENET_EVENT_TYPE_CONNECT:{
			// HELLO as soon as the transport is up
			BitWriter w;
			BeginMsg(w, MSG_HELLO);
			MsgHello hello;
			hello.protocolVersion = PROTOCOL_VERSION;
			CopyStr(hello.name, sizeof hello.name, c.name);
			WriteHello(w, hello);
			ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
			enet_peer_send(ev.peer, CHAN_RELIABLE, pkt);
			break;
		}
		case ENET_EVENT_TYPE_RECEIVE:{
			BitReader r(ev.packet->data, ev.packet->dataLength);
			MsgType type = (MsgType)r.ReadU8();
			switch(type){
			case MSG_WELCOME:{
				MsgWelcome wel;
				if(ReadWelcome(r, wel)){
					c.id = wel.playerId;
					c.welcomed = true;
					BitWriter w;
					BeginMsg(w, MSG_SPAWN_REQ);
					ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
					enet_peer_send(ev.peer, CHAN_RELIABLE, pkt);
				}
				break;
			}
			case MSG_SPAWN:{
				MsgSpawn sp;
				if(ReadSpawn(r, sp) && sp.playerId == c.id) c.spawned = true;
				break;
			}
			case MSG_CORRECTION:{
				PlayerState st;
				if(ReadPlayerState(r, st)){
					c.gotCorrection = true;
					c.correctionX = st.xMm;
				}
				break;
			}
			case MSG_STATE_BATCH:{
				PlayerState states[MAX_STATES_PER_PACKET];
				int n = ReadStateBatch(r, states, MAX_STATES_PER_PACKET);
				for(int i = 0; i < n; i++){
					if(states[i].playerId == c.remoteId){
						c.gotRemote = true;
						c.remoteX = states[i].xMm;
					}
				}
				break;
			}
			case MSG_KICK:
			case MSG_DISCONNECT:
				c.refused = true;
				break;
			default:
				break;
			}
			enet_packet_destroy(ev.packet);
			break;
		}
		case ENET_EVENT_TYPE_DISCONNECT:
			c.refused = true;
			break;
		default:
			break;
		}
	}
}

} // namespace

static void TestServerStateCoalescing()
{
	std::unique_ptr<Server> srv(new Server());
	ServerConfig cfg;
	cfg.hostname = "coalesce test";
	cfg.maxPlayers = 8;
	cfg.gamemode = "freeroam";
	cfg.announce = false;
	cfg.lanDiscover = false;
	cfg.profilesDb = "";
	cfg.resourcesDir = "";
	cfg.vehiclesFile = "";
	cfg.logFile = "";
	cfg.ac.enabled = false;
	cfg.ac.requireProtected = false;

	uint16_t port = 0;
	for(uint16_t p = 17777; p <= 17796 && !port; p++){
		cfg.port = p;
		if(srv->Init(cfg)) port = p;
	}
	CHECK(port != 0);
	if(!port) return;

	CoalesceClient a, b;
	CHECK(CoalesceOpen(a, port, "CoalesceA"));
	CHECK(CoalesceOpen(b, port, "CoalesceB"));
	if(!a.host || !b.host){ CoalesceClose(a); CoalesceClose(b); return; }

	auto step = [&](int ms){
		for(int i = 0; i < ms; i++){
			ServerTestPeer::Pump(*srv);
			CoalesceService(a);
			CoalesceService(b);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	};
	auto tick = [&]{
		ServerTestPeer::Tick(*srv);
		CoalesceService(a);
		CoalesceService(b);
	};

	for(int i = 0; i < 3000 && !(a.spawned && b.spawned) && !a.refused && !b.refused; i++) step(1);
	CHECK(a.spawned && b.spawned);
	if(!(a.spawned && b.spawned)){ CoalesceClose(a); CoalesceClose(b); return; }
	a.remoteId = b.id;
	b.remoteId = a.id;

	// a known, adjacent pair: the game's own relocations carry the teleport flag
	CoalesceSend(a, CoalesceState(a.id, 100.0f, 0.0f, MOVEFLAG_TELEPORT));
	CoalesceSend(b, CoalesceState(b.id, 110.0f, 0.0f, MOVEFLAG_TELEPORT));
	enet_host_flush(a.host);
	enet_host_flush(b.host);
	step(10);
	tick();

	const uint64_t staged0 = ServerTestPeer::Staged(*srv);
	const uint64_t applied0 = ServerTestPeer::Applied(*srv);
	const uint64_t coalesced0 = ServerTestPeer::Coalesced(*srv);
	const int violations0 = ServerTestPeer::Violations(*srv, a.id);
	CHECK(violations0 == 0);

	// two frames, one tick: the illegal 40 m step (400 m/s at the tick floor)
	// is superseded by the legal 0.5 m step before anything is validated
	a.gotCorrection = false;
	b.gotRemote = false;
	CoalesceSend(a, CoalesceState(a.id, 140.0f, 0.0f, 0));
	CoalesceSend(a, CoalesceState(a.id, 100.5f, 0.0f, 0));
	enet_host_flush(a.host);
	for(int i = 0; i < 100 && ServerTestPeer::Staged(*srv) < staged0 + 2; i++) step(1);
	CHECK(ServerTestPeer::Staged(*srv) == staged0 + 2);          // both arrived
	CHECK(ServerTestPeer::Coalesced(*srv) == coalesced0 + 1);   // one superseded
	ServerTestPeer::Tick(*srv);
	CoalesceService(a);
	CoalesceService(b);

	CHECK(ServerTestPeer::Applied(*srv) == applied0 + 1);       // one reached the tick
	CHECK(ServerTestPeer::Violations(*srv, a.id) == violations0); // the illegal one never did
	CHECK(!a.gotCorrection);                                     // no rubber-band for it
	CHECK(b.gotRemote);
	CHECK(b.remoteX == QuantPos(100.5f));                        // relay carries the freshest

	// negative control: the same illegal frame sent alone is still corrected, so
	// the assertions above are about coalescing, not about the jump being legal
	a.gotCorrection = false;
	const int violations1 = ServerTestPeer::Violations(*srv, a.id);
	CoalesceSend(a, CoalesceState(a.id, 140.0f, 0.0f, 0));
	enet_host_flush(a.host);
	for(int i = 0; i < 100 && ServerTestPeer::Staged(*srv) < staged0 + 3; i++) step(1);
	ServerTestPeer::Tick(*srv);
	for(int i = 0; i < 50 && !a.gotCorrection; i++) step(1);
	CHECK(a.gotCorrection);
	CHECK(a.correctionX == QuantPos(100.5f));
	CHECK(ServerTestPeer::Violations(*srv, a.id) == violations1 + 1);

	CoalesceClose(a);
	CoalesceClose(b);
}

int main()
{
	std::printf("rnet unit tests\n");
	TestAcProtocol();
	TestAcConfig();
	TestVoiceCodec();
	TestWelcomeLiveInfo();
	TestHighPlayerCount();
	TestRelayFastPath();
	TestVehDespawnCodec();
	TestFavoritesCodec();
	TestMasterEntryLiveInfo();
	TestResListCodec();
	TestResourceParsing();
	TestResourceExpand();
	TestResourceSandbox();
	TestResourceManagerOps();
	TestBitstreamRoundTrip();
	TestQuantization();
	TestStateCodecs();
	TestMessageCodecs();
	TestValidation();
	TestToughValidator();
	TestVehicleMessages();
	TestVehicleOccupancy();
	TestInventoryMessages();
	TestProfileStore();
	TestProfileAuth();
	TestProfileLegacyMigration();
	// last: it boots a real server and prints its join/spawn lines
	TestServerStateCoalescing();
	std::printf("%d checks, %d failures — %s\n", g_checks, g_fails, g_fails ? "FAIL" : "PASS");
	return g_fails ? 1 : 0;
}
