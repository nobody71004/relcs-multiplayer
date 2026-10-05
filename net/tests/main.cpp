// net/tests/main.cpp — unit tests for the rnet protocol layer.
#include "rnet_protocol.h"
#include "rnet_validator.h"
#include "profiles.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>
#include <string>

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
	BitWriter w3; BeginMsg(w3, MSG_VEH_SPAWN); WriteVehSpawn(w3, vs);
	BitReader r3(w3.Data(), w3.ByteSize());
	CHECK((MsgType)r3.ReadU8() == MSG_VEH_SPAWN);
	MsgVehSpawn vs2;
	CHECK(ReadVehSpawn(r3, vs2));
	CHECK(vs2.vehicleId == 1 && vs2.modelId == 401);
	CHECK(vs2.x == 100.5f && vs2.y == -200.25f && vs2.z == 12.0f && vs2.heading == 1.5f);
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
	db.Close();
	std::remove(path);
}

int main()
{
	std::printf("rnet unit tests\n");
	TestBitstreamRoundTrip();
	TestQuantization();
	TestStateCodecs();
	TestMessageCodecs();
	TestValidation();
	TestToughValidator();
	TestVehicleMessages();
	TestInventoryMessages();
	TestProfileStore();
	std::printf("%d checks, %d failures — %s\n", g_checks, g_fails, g_fails ? "FAIL" : "PASS");
	return g_fails ? 1 : 0;
}
