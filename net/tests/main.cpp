// net/tests/main.cpp — unit tests for the rnet protocol layer.
#include "rnet_protocol.h"
#include "rnet_validator.h"
#include "rnet_voice.h"
#include "profiles.h"
#include "passhash.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
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

	// a v4 server stops after the hostname: the v5 tail must read as empty
	BitWriter w4;
	w4.WriteU8(1); w4.WriteU16(20); w4.WriteF32(200.0f);
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

	// a v4 entry stops after the game mode
	BitWriter w4;
	w4.WriteU32(0x0100007F); w4.WriteU16(7777);
	w4.WriteU8(1); w4.WriteU8(32); w4.WriteU16(0);
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

	// a v4 announce has only the advertised IP tail
	BitWriter wa4;
	wa4.WriteU16(7777); wa4.WriteU8(0); wa4.WriteU8(32); wa4.WriteU16(4);
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

	// packet codec: fromId, length, then the packet, exact size on the wire
	uint8_t body[VOICE_MAX_PACKET];
	for(int i = 0; i < 37; i++) body[i] = (uint8_t)(i * 7 + 3);
	BitWriter w;
	WriteVoice(w, 7, body, 37);
	CHECK(w.ByteSize() == (size_t)(37 + 2));
	BitReader r(w.Data(), w.ByteSize());
	uint8_t fromId = 0, back[VOICE_MAX_PACKET] = {}, backLen = 0;
	CHECK(ReadVoice(r, fromId, back, backLen));
	CHECK(fromId == 7 && backLen == 37);
	CHECK(std::memcmp(body, back, 37) == 0);
	CHECK(r.BitsLeft() == 0);

	// a zero length or one past the maximum is malformed, and so is a short body
	BitWriter zero;
	zero.WriteU8(7); zero.WriteU8(0);
	BitReader rZero(zero.Data(), zero.ByteSize());
	CHECK(!ReadVoice(rZero, fromId, back, backLen));
	BitWriter big;
	big.WriteU8(7); big.WriteU8((uint8_t)(VOICE_MAX_PACKET + 1));
	for(int i = 0; i < VOICE_MAX_PACKET + 1; i++) big.WriteU8(1);
	BitReader rBig(big.Data(), big.ByteSize());
	CHECK(!ReadVoice(rBig, fromId, back, backLen));
	BitWriter shortBody;
	shortBody.WriteU8(7); shortBody.WriteU8(10); shortBody.WriteU8(1);
	BitReader rShort(shortBody.Data(), shortBody.ByteSize());
	CHECK(!ReadVoice(rShort, fromId, back, backLen));
}

int main()
{
	std::printf("rnet unit tests\n");
	TestVoiceCodec();
	TestWelcomeLiveInfo();
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
	TestInventoryMessages();
	TestProfileStore();
	TestProfileAuth();
	TestProfileLegacyMigration();
	std::printf("%d checks, %d failures — %s\n", g_checks, g_fails, g_fails ? "FAIL" : "PASS");
	return g_fails ? 1 : 0;
}
