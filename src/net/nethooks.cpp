// src/net/nethooks.cpp — game glue: connects NetClient to the reLCS engine.
//
// - connect flow : -connect ip:port / -nick name on the command line
// - local state  : sampled from FindPlayerPed() at 20 Hz
// - remote state : kinematic CCivilianPed entities, interpolated, with
//                  idle/walk/run anims driven by relayed velocity
// - overlay      : chat + players via CFont (from CHud::Draw)
#include "nethooks.h"
#include "netclient.h"
#include "cefhud.h"

#include "common.h"
#include "Lists.h"
#include "PlayerInfo.h"
#include "PlayerPed.h"
#include "Ped.h"
#include "CivilianPed.h"
#include "Population.h"
#include "Pools.h"
#include "World.h"
#include "Streaming.h"
#include "ModelIndices.h"
#include "AnimManager.h"
#include "AnimationId.h"
#include "Font.h"
#include "ModelInfo.h"
#include "Collision.h"
#include "Sprite2d.h"
#include "Timer.h"
#include "Frontend.h"
#include "Sprite.h"
#include "skeleton.h"
#include "Pickups.h"
#include "Vehicle.h"
#include "Automobile.h"
#include "Bike.h"
#include "Boat.h"

#include <windows.h>
#include <cstdarg>

// winsock1's in_addr macros (s_net/s_host/...) collide with our own globals
#undef s_net
#undef s_host
#undef s_imp
#undef s_lh

#include <cmath>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <chrono>

static NetClient s_net;
static bool s_wantConnect = false;
static bool s_connected = false;
static std::string s_host, s_nick = "Player";
static uint16_t s_port = 7777;

static double NowSec()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// remote vehicle management — net vehicles <-> local CVehicle objects
//
// Ownership model: whoever holds seat 0 (the driver) simulates the vehicle
// locally and streams VehicleState at 20 Hz; every other client materializes
// it as a frozen (kinematic) puppet driven by the relayed state. The server
// assigns the net id; a car entered locally gets "adopted" (bound to the net
// id) instead of being duplicated by a puppet.
// ---------------------------------------------------------------------------
struct NetVehicle {
	CVehicle* veh = nil;
	uint16_t modelId = 0;
	bool puppet = false;       // created by the netcode (vs a local game object)
	bool rejected = false;     // invalid model — drop instead of retrying forever
	bool hasTarget = false;
	rnet::VehicleState target; // latest relayed driver state
	float smoothX = 0, smoothY = 0, smoothZ = 0, smoothH = 0;
};
static std::map<uint16_t, NetVehicle> s_vehicles;
static std::map<CVehicle*, uint16_t> s_vehByPtr;
static uint16_t s_myVehNet = rnet::VEHICLE_NONE;
static uint8_t s_mySeat = 0xFF;
static CVehicle* s_lastMyVeh = nil;
static CVehicle* s_pendingVeh = nil;   // local car waiting for its server id
static uint16_t s_pendingModel = 0;
static std::map<uint8_t, std::pair<uint16_t, uint8_t>> s_pendingSeat; // remote ped -> (veh, seat)

static uint16_t NetIdOfVehicle(CVehicle* v)
{
	auto it = s_vehByPtr.find(v);
	return it == s_vehByPtr.end() ? rnet::VEHICLE_NONE : it->second;
}

static void UnseatNetPed(CPed* ped)
{
	if(!ped) return;
	CVehicle* veh = ped->m_pMyVehicle;
	if(!veh) return;
	if(veh->pDriver == ped){
		veh->pDriver = nil;
		ped->CleanUpOldReference((CEntity**)&veh->pDriver);
	}
	for(int i = 0; i < 8; i++){
		if(veh->pPassengers[i] == ped){
			veh->pPassengers[i] = nil;
			ped->CleanUpOldReference((CEntity**)&veh->pPassengers[i]);
			if(veh->m_nNumPassengers > 0) veh->m_nNumPassengers--;
		}
	}
	ped->m_pMyVehicle = nil;
	veh->CleanUpOldReference((CEntity**)&ped->m_pMyVehicle);
	ped->bInVehicle = false;
	ped->SetPedState(PED_IDLE);
}

static void SeatNetPed(CPed* ped, CVehicle* veh, uint8 seat)
{
	if(!ped || !veh || ped->m_pMyVehicle == veh) return;
	if(ped->m_pMyVehicle) UnseatNetPed(ped);
	if(seat == 0){
		if(veh->pDriver && veh->pDriver != ped) return; // seat taken locally
		if(!veh->pDriver){
			veh->pDriver = ped;
			ped->RegisterReference((CEntity**)&veh->pDriver);
		}
	}else{
		int i = (int)seat - 1;
		if(i >= 8) return;
		if(veh->pPassengers[i] && veh->pPassengers[i] != ped) return;
		if(!veh->pPassengers[i]){
			veh->pPassengers[i] = ped;
			ped->RegisterReference((CEntity**)&veh->pPassengers[i]);
			veh->m_nNumPassengers++;
		}
	}
	ped->m_pMyVehicle = veh;
	veh->RegisterReference((CEntity**)&ped->m_pMyVehicle);
	ped->bInVehicle = true;
	ped->SetPedState(PED_DRIVING);
	ped->SetPedPositionInCar();
	// hold the seated pose; the world loop keeps the ped in its seat from here
	RpClump* clump = (RpClump*)ped->m_rwObject;
	if(clump)
		CAnimManager::BlendAnimation(clump, ASSOCGRP_STD, ANIM_STD_CAR_SIT, 8.0f);
}

static bool ModelIsVehicle(int modelId)
{
	CBaseModelInfo* mi = CModelInfo::GetModelInfo(modelId);
	return mi != nil && mi->GetModelType() == MITYPE_VEHICLE;
}

static CVehicle* CreatePuppetVehicle(uint16_t modelId, float x, float y, float z, float heading)
{
	if(!ModelIsVehicle(modelId))
		return nil; // callers mark the net vehicle rejected (logged once)
	if(CPools::GetVehiclePool()->GetNoOfFreeSpaces() == 0)
		return nil; // retried from UpdateNetVehicles
	CStreaming::RequestModel(modelId, 0);
	CStreaming::LoadAllRequestedModels(false);
	CVehicle* veh;
	if(CModelInfo::IsBikeModel(modelId))      veh = new CBike(modelId, MISSION_VEHICLE);
	else if(CModelInfo::IsBoatModel(modelId)) veh = new CBoat(modelId, MISSION_VEHICLE);
	else                                      veh = new CAutomobile(modelId, MISSION_VEHICLE);
	if(!veh) return nil;
	// netcode-driven: kinematic and inert (bIsFrozen zeroes the applied move/
	// turn speed every frame, so gravity can never drift the puppet)
	veh->bUsesCollision = false;
	veh->bIsFrozen = true;
	veh->SetPosition(CVector(x, y, z));
	veh->SetOrientation(0.0f, 0.0f, heading);
	veh->SetMoveSpeed(0.0f, 0.0f, 0.0f);
	veh->SetTurnSpeed(0.0f, 0.0f, 0.0f);
	CWorld::Add(veh);
	if(!veh->m_rwObject)
		fprintf(stderr, "[NET] WARNING: net vehicle model %d has no rw object\n", (int)modelId);
	fprintf(stderr, "[NET] net vehicle model=%d created at (%.1f, %.1f, %.1f)\n",
	        (int)modelId, x, y, z);
	fflush(stderr);
	return veh;
}

static NetVehicle& NetVehicleAt(uint16_t netId, uint16_t modelId,
                                float x, float y, float z, float heading)
{
	NetVehicle& nv = s_vehicles[netId];
	if(!nv.veh){
		if(nv.modelId == 0){
			nv.modelId = modelId;
			nv.smoothX = x; nv.smoothY = y; nv.smoothZ = z; nv.smoothH = heading;
		}
		if(!nv.rejected && !ModelIsVehicle(nv.modelId)){
			nv.rejected = true;
			fprintf(stderr, "[NET] veh model %d is not a vehicle — net vehicle %d dropped\n",
			        (int)nv.modelId, (int)netId);
			fflush(stderr);
		}
		if(!nv.rejected){
			nv.veh = CreatePuppetVehicle(nv.modelId, nv.smoothX, nv.smoothY, nv.smoothZ, nv.smoothH);
			nv.puppet = nv.veh != nil;
			if(nv.veh) s_vehByPtr[nv.veh] = netId;
		}
	}
	return nv;
}

static void AdoptNetVehicle(uint16_t netId, CVehicle* veh, uint16_t modelId)
{
	NetVehicle& nv = s_vehicles[netId];
	nv.veh = veh;
	nv.modelId = modelId;
	nv.puppet = false;
	CVector p = veh->GetPosition();
	nv.smoothX = p.x; nv.smoothY = p.y; nv.smoothZ = p.z;
	nv.smoothH = std::atan2(-veh->GetForward().x, veh->GetForward().y);
	s_vehByPtr[veh] = netId;
	// our car is a net entity now: keep the population from reclaiming it
	veh->VehicleCreatedBy = MISSION_VEHICLE;
}

static void RemoveNetVehicle(uint16_t netId)
{
	auto it = s_vehicles.find(netId);
	if(it == s_vehicles.end()) return;
	NetVehicle& nv = it->second;
	if(nv.veh){
		s_vehByPtr.erase(nv.veh);
		if(nv.puppet){
			CWorld::Remove(nv.veh);
			delete nv.veh;
		}else{
			// a local game car: hand it back to normal physics
			nv.veh->bIsFrozen = false;
			nv.veh->bUsesCollision = true;
		}
	}
	s_vehicles.erase(it);
}

static void ClearAllVehicles(void)
{
	while(!s_vehicles.empty())
		RemoveNetVehicle(s_vehicles.begin()->first);
	s_pendingSeat.clear();
	s_myVehNet = rnet::VEHICLE_NONE;
	s_mySeat = 0xFF;
	s_pendingVeh = nil;
	s_lastMyVeh = nil;
}

// per-frame: puppet creation retries, clump rebuild, interpolation, and
// freeze/unfreeze based on who drives
static void UpdateNetVehicles(float dt)
{
	for(auto& kv : s_vehicles){
		NetVehicle& nv = kv.second;
		if(!nv.veh){
			if(nv.rejected) continue;
			nv.veh = CreatePuppetVehicle(nv.modelId, nv.smoothX, nv.smoothY, nv.smoothZ, nv.smoothH);
			if(!nv.veh) continue;
			nv.puppet = true;
			s_vehByPtr[nv.veh] = kv.first;
		}
		CVehicle* veh = nv.veh;
		// belt & braces: streaming eviction kills the clump — rebuild it
		if(!veh->m_rwObject){
			CStreaming::RequestModel(nv.modelId, 0);
			CStreaming::LoadAllRequestedModels(false);
			veh->SetModelIndex(nv.modelId);
		}
		if(kv.first == s_myVehNet && s_mySeat == 0){
			// we drive: the local simulation is authoritative — real physics
			veh->bIsFrozen = false;
			veh->bUsesCollision = true;
			continue;
		}
		// remote-driven or parked: kinematic, glued to the relayed state
		veh->bIsFrozen = true;
		veh->bUsesCollision = false;
		if(nv.hasTarget){
			float tx = nv.target.xMm / 1000.0f, ty = nv.target.yMm / 1000.0f, tz = nv.target.zMm / 1000.0f;
			float th = rnet::DequantAngle(nv.target.heading);
			float k = dt * 12.0f;
			if(k > 1.0f) k = 1.0f;
			float dx = tx - nv.smoothX, dy = ty - nv.smoothY, dz = tz - nv.smoothZ;
			if(dx*dx + dy*dy + dz*dz > 400.0f)
				k = 1.0f; // teleport-ish gap: snap instead of gliding
			nv.smoothX += (tx - nv.smoothX) * k;
			nv.smoothY += (ty - nv.smoothY) * k;
			nv.smoothZ += (tz - nv.smoothZ) * k;
			float dh = th - nv.smoothH;
			while(dh > rnet::RNET_PI)  dh -= 2.0f * rnet::RNET_PI;
			while(dh < -rnet::RNET_PI) dh += 2.0f * rnet::RNET_PI;
			nv.smoothH += dh * k;
		}
		veh->SetPosition(CVector(nv.smoothX, nv.smoothY, nv.smoothZ));
		veh->SetOrientation(0.0f, 0.0f, nv.smoothH);
		veh->SetMoveSpeed(0.0f, 0.0f, 0.0f);
		veh->SetTurnSpeed(0.0f, 0.0f, 0.0f);
	}
}

// ---------------------------------------------------------------------------
// remote player entity management
// ---------------------------------------------------------------------------
struct RemotePed {
	CPed* ped = nullptr;
	int lastAnim = -1;
	float smoothX = 0, smoothY = 0, smoothZ = 0;
	bool init = false;
};
static std::map<uint8_t, RemotePed> s_remotes;

static void RemoveRemote(uint8_t id)
{
	auto it = s_remotes.find(id);
	if(it != s_remotes.end()){
		if(it->second.ped){
			UnseatNetPed(it->second.ped); // no-op when on foot
			CWorld::Remove(it->second.ped);
			delete it->second.ped;
		}
		s_remotes.erase(it);
	}
	s_pendingSeat.erase(id);
}

static void SpawnRemote(uint8_t id, float x, float y, float z, float heading)
{
	if(!s_connected)
		return;
	RemoveRemote(id);
	CStreaming::RequestModel(MI_MALE01, STREAMFLAGS_KEEP_IN_MEMORY);
	CStreaming::LoadAllRequestedModels(false);

	// CPed::operator new returns nil when the ped pool is full and the
	// constructor would crash on a null 'this'; defer and retry next frame
	if(CPools::GetPedPool()->GetNoOfFreeSpaces() == 0){
		static bool s_warnedPoolFull = false;
		if(!s_warnedPoolFull){
			fprintf(stderr, "[NET] remote ped id=%d deferred: ped pool full\n", (int)id);
			s_warnedPoolFull = true;
		}
		return;
	}

	CCivilianPed* ped = new CCivilianPed(PEDTYPE_CIVMALE, MI_MALE01);
	if(!ped)
		return;
	// puppets are positioned directly each frame; letting them collide makes
	// the physics shove real players around (which then trips anti-cheat)
	ped->bUsesCollision = false;
	// network avatars are inert puppets driven only by relayed state:
	//  - UNK_CHAR makes CCivilianPed::ProcessControl bail out (no autonomous
	//    AI, so no wandering/fighting on a kinematic body)
	//  - and makes CanBeDeleted() false, so CPopulation culling
	//    (RemovePed -> delete) can never free the ped out from under us
	ped->CharCreatedBy = UNK_CHAR;
	ped->bRespondsToThreats = false;
	CVector pos(x, y, z);
	ped->SetPosition(pos);
	ped->SetOrientation(0.0f, 0.0f, heading);
	CWorld::Add(ped);
	if(!ped->m_rwObject)
		fprintf(stderr, "[NET] WARNING: remote ped id=%d has no rw object (model %d not loaded?)\n",
		        (int)id, (int)MI_MALE01);

	RemotePed& rp = s_remotes[id];
	rp.ped = ped;
	rp.smoothX = x; rp.smoothY = y; rp.smoothZ = z;
	rp.init = true;
	fprintf(stderr, "[NET] remote ped id=%d created at (%.1f, %.1f, %.1f)\n", (int)id, x, y, z);
	fflush(stderr);
}

static void ApplyRemote(uint8_t id, const rnet::PlayerState& st, float dt)
{
	RemotePed& rp = s_remotes[id];
	if(!rp.ped || !rp.init){
		SpawnRemote(id, st.xMm / 1000.0f, st.yMm / 1000.0f, st.zMm / 1000.0f,
		            rnet::DequantAngle(st.heading));
		return;
	}
	CPed* ped = rp.ped;

	// belt & braces: if streaming ever evicted the model the clump dies and
	// the puppet silently renders as nothing (nametag floats "in the air") —
	// rebuild it in place
	if(!ped->m_rwObject){
		CStreaming::RequestModel(MI_MALE01, STREAMFLAGS_KEEP_IN_MEMORY);
		CStreaming::LoadAllRequestedModels(false);
		ped->SetModelIndex(MI_MALE01);
		rp.lastAnim = -1;
		fprintf(stderr, "[NET] remote ped id=%d clump rebuilt\n", (int)id);
		fflush(stderr);
	}

	// seated in a net vehicle: the world loop holds the ped in its seat
	// (SetPedPositionInCar) — don't fight it with on-foot interpolation
	if(ped->bInVehicle && ped->m_pMyVehicle){
		CVector p = ped->GetPosition();
		rp.smoothX = p.x; rp.smoothY = p.y; rp.smoothZ = p.z;
		return;
	}

	// interpolate toward the relayed position
	float tx = st.xMm / 1000.0f, ty = st.yMm / 1000.0f, tz = st.zMm / 1000.0f;
	if(st.moveFlags & rnet::MOVEFLAG_TELEPORT){
		// remote was relocated by its own game (respawn/warp): snap, don't glide
		rp.smoothX = tx; rp.smoothY = ty; rp.smoothZ = tz;
	}else{
		float k = dt * 12.0f;
		if(k > 1.0f) k = 1.0f;
		rp.smoothX += (tx - rp.smoothX) * k;
		rp.smoothY += (ty - rp.smoothY) * k;
		rp.smoothZ += (tz - rp.smoothZ) * k;
	}

	CVector pos(rp.smoothX, rp.smoothY, rp.smoothZ);
	ped->SetPosition(pos);
	ped->SetOrientation(0.0f, 0.0f, rnet::DequantAngle(st.heading));
	ped->SetMoveSpeed(0.0f, 0.0f, 0.0f);
	ped->SetTurnSpeed(0.0f, 0.0f, 0.0f);

	// anim from relayed speed
	float speed = std::sqrt(rnet::DequantVel(st.velX) * rnet::DequantVel(st.velX) +
	                        rnet::DequantVel(st.velY) * rnet::DequantVel(st.velY));
	int anim = speed < 0.5f ? 0 : (speed < 2.5f ? 1 : 2);
	// don't call ped->GetClump() here: its assert aborts when m_rwObject is nil
	RpClump* clump = (RpClump*)ped->m_rwObject;
	if(anim != rp.lastAnim && clump){
		AnimationId ids[3] = { ANIM_STD_IDLE, ANIM_STD_WALK, ANIM_STD_RUN };
		CAnimManager::BlendAnimation(clump, ASSOCGRP_STD, ids[anim], 8.0f);
		rp.lastAnim = anim;
	}
}

// ---------------------------------------------------------------------------
// local client features: fps cap, admin panel, inventory, chat input, noclip
// ---------------------------------------------------------------------------
static std::string s_rconPw;
static bool s_god = false;
static bool s_noclip = false;
static CVector s_ncPos(0.0f, 0.0f, 0.0f);
static double s_welcomeUntil = 0.0;
static int s_fpsCap = 30;
static bool s_panelOpen = false;
static int s_panelRow = 0;
static int s_panelWeapon = 0;
static int s_panelFpsIdx = 0;
static bool s_invOpen = false;
static bool s_chatOpen = false;
static char s_chatBuf[96] = {};
static int s_chatLen = 0;
static char s_chatMode = 'c'; // 'c' = public chat, 'b' = broadcast (admin)
static char s_lastRcon[96] = {};

static const int kFpsPresets[] = { 30, 60, 120, 240, 360, 0 }; // 0 = unlimited
static const char* kFpsNames[] = { "30", "60", "120", "240", "360", "UNLIMITED" };
static const int kNumFps = 6;

static const eWeaponType kGiveWeapons[] = {
	WEAPONTYPE_COLT45, WEAPONTYPE_SHOTGUN, WEAPONTYPE_TEC9, WEAPONTYPE_RUGER,
	WEAPONTYPE_SNIPERRIFLE, WEAPONTYPE_FLAMETHROWER, WEAPONTYPE_KATANA,
	WEAPONTYPE_BASEBALLBAT, WEAPONTYPE_MOLOTOV, WEAPONTYPE_PYTHON,
};
static const char* kGiveWeaponNames[] = {
	"Colt45", "Shotgun", "Tec9", "Ruger", "Sniper", "Flamethrower",
	"Katana", "Bat", "Molotov", "Python",
};
static const int kNumGiveWeapons = 10;

void NetGame_SetFpsLimit(int n)
{
	s_fpsCap = n;
	RsGlobal.maxFPS = (n <= 0) ? 1000000 : n;
	// caps above a typical display refresh need vsync out of the way
	if(n <= 0 || n > 60)
		FrontEndMenuManager.m_PrefsVsync = 0;
	fprintf(stderr, "[NET] fps limit set to %d (0 = unlimited)\n", n);
	fflush(stderr);
}

// one-shot key edge detector (edges are computed once per frame and cached,
// so every caller sees the same answer — the old consuming version meant a
// keybind key (T=chat, I=inventory) could never be typed into a text field)
static bool KeyEdge(int vk)
{
	static bool prev[256] = {};
	static bool cur[256] = {};
	static uint32 s_frame = 0xFFFFFFFFu;
	uint32 frame = CTimer::GetFrameCounter();
	if(s_frame != frame){
		s_frame = frame;
		for(int k = 0; k < 256; k++){
			bool now = (GetAsyncKeyState(k) & 0x8000) != 0;
			cur[k] = now && !prev[k];
			prev[k] = now;
		}
	}
	return cur[vk & 255];
}

// ---------------------------------------------------------------------------
// configurable keybinds — keybinds.ini next to the exe
// ---------------------------------------------------------------------------
struct KeyBinds {
	int chat = 'T';
	int admin = VK_F6;
	int inventory = 'I';
	int console = VK_F7;
	int console2 = VK_OEM_3; // `~
};
static KeyBinds s_keys;

static int KeyFromName(const char* name)
{
	if(!name || !name[0]) return 0;
	char up[24];
	size_t n = 0;
	for(; name[n] && n < sizeof up - 1; n++)
		up[n] = (char)toupper((unsigned char)name[n]);
	up[n] = 0;
	if(up[1] == 0 && ((up[0] >= 'A' && up[0] <= 'Z') || (up[0] >= '0' && up[0] <= '9')))
		return up[0];
	static const struct { const char* n; int vk; } kTbl[] = {
		{"ESC", VK_ESCAPE}, {"ESCAPE", VK_ESCAPE}, {"TAB", VK_TAB},
		{"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN},
		{"BACKSPACE", VK_BACK}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
		{"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"SHIFT", VK_SHIFT},
		{"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"ALT", VK_MENU},
		{"GRAVE", VK_OEM_3}, {"TILDE", VK_OEM_3}, {"BACKTICK", VK_OEM_3},
		{"SEMICOLON", VK_OEM_1}, {"PLUS", VK_OEM_PLUS}, {"MINUS", VK_OEM_MINUS},
		{"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD}, {"SLASH", VK_OEM_2},
		{"LBRACKET", VK_OEM_4}, {"RBRACKET", VK_OEM_6}, {"QUOTE", VK_OEM_7},
		{"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
		{"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
		{"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
	};
	for(const auto& e : kTbl)
		if(!strcmp(up, e.n)) return e.vk;
	return 0;
}

static void LoadKeyBinds(void)
{
	const char* path = "keybinds.ini";
	FILE* f = fopen(path, "r");
	if(!f){
		// first run: write the defaults so users have something to edit
		f = fopen(path, "w");
		if(f){
			fputs("; LCS Online keybinds — values: A-Z 0-9 F1-F12 ESC TAB SPACE ENTER\n"
			      "; BACKSPACE UP DOWN LEFT RIGHT SHIFT CTRL ALT GRAVE SEMICOLON PLUS\n"
			      "; MINUS COMMA PERIOD SLASH LBRACKET RBRACKET QUOTE\n"
			      "chat=T\nadmin=F6\ninventory=I\nconsole=F7\nconsole2=GRAVE\n", f);
			fclose(f);
		}
		return;
	}
	char line[128];
	while(fgets(line, sizeof line, f)){
		char* p = line;
		while(*p == ' ' || *p == '\t') p++;
		if(*p == ';' || *p == '#' || *p == '\n' || !*p) continue;
		char* eq = strchr(p, '=');
		if(!eq) continue;
		*eq = 0;
		char* key = p;
		char* val = eq + 1;
		key[strcspn(key, " \t\r\n")] = 0;
		val[strcspn(val, " \t\r\n")] = 0;
		int vk = KeyFromName(val);
		if(!vk) continue;
		if(!_stricmp(key, "chat")) s_keys.chat = vk;
		else if(!_stricmp(key, "admin")) s_keys.admin = vk;
		else if(!_stricmp(key, "inventory")) s_keys.inventory = vk;
		else if(!_stricmp(key, "console")) s_keys.console = vk;
		else if(!_stricmp(key, "console2")) s_keys.console2 = vk;
	}
	fclose(f);
	fprintf(stderr, "[NET] keybinds.ini loaded\n");
	fflush(stderr);
}

// ---------------------------------------------------------------------------
// developer console
// ---------------------------------------------------------------------------
static bool s_consoleOpen = false;
static char s_consoleBuf[192] = {};
static int s_consoleLen = 0;
static std::vector<std::string> s_consoleLog;
static std::vector<std::string> s_consoleHist;
static int s_consoleHistPos = -1;

static void ConsolePrint(const char* fmt, ...)
{
	char buf[256];
	va_list ap; va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	s_consoleLog.push_back(buf);
	if(s_consoleLog.size() > 80) s_consoleLog.erase(s_consoleLog.begin());
	fprintf(stderr, "[CONSOLE] %s\n", buf);
	fflush(stderr);
}

static void SendRconFmt(const char* fmt, ...)
{
	char buf[160];
	va_list ap; va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if(!s_rconPw.empty()){
		s_net.SendRconCmd(buf);
	}else{
		// never fail silently: admin commands (broadcast/give/...) need the
		// server's rcon password — set it in the launcher or via -rcon PW
		ConsolePrint("rcon not configured — '%s' NOT sent (set RCON in launcher settings or -rcon PW)", buf);
	}
	snprintf(s_lastRcon, sizeof s_lastRcon, "%s", s_rconPw.empty() ? "no -rcon password" : buf);
}

static void ConsoleExec(const char* line)
{
	char cmd[32] = {}, rest[160] = {};
	sscanf(line, "%31s %159[^\n]", cmd, rest);
	if(!cmd[0]) return;
	auto is = [&](const char* c){ return _stricmp(cmd, c) == 0; };
	CPlayerPed* me = FindPlayerPed();
	int myId = (int)s_net.MyId();
	if(is("help")){
		ConsolePrint("commands: help say broadcast(bc) give spawnbot spawnprop teleport(tp) ride");
		ConsolePrint("          fps god noclip players status rcon keybinds clear quit");
	}else if(is("say")){
		if(rest[0]) s_net.SendChat(rest);
		else ConsolePrint("usage: say <text>");
	}else if(is("broadcast") || is("bc")){
		if(rest[0]) SendRconFmt("broadcast %s", rest);
		else ConsolePrint("usage: broadcast <text>");
	}else if(is("give")){
		if(rest[0]) SendRconFmt("give %d %s", myId, rest);
		else ConsolePrint("usage: give money|weapon|god|health <value> [ammo]");
	}else if(is("spawnbot")){
		if(!rest[0]) snprintf(rest, sizeof rest, "NPC%03d roam", (int)(NowSec() * 1000.0) % 1000);
		SendRconFmt("spawnbot %s", rest);
	}else if(is("spawnprop")){
		if(me){
			CVector p = me->GetPosition();
			if(rest[0]) SendRconFmt("spawnprop %.1f %.1f %.1f %s", p.x, p.y, p.z, rest);
			else SendRconFmt("spawnprop %.1f %.1f %.1f 4 50", p.x, p.y, p.z);
		}
	}else if(is("teleport") || is("tp")){
		float x = 0, y = 0, z = 0;
		if(sscanf(rest, "%f %f %f", &x, &y, &z) == 3)
			SendRconFmt("teleport %d %.1f %.1f %.1f", myId, x, y, z);
		else ConsolePrint("usage: teleport <x> <y> <z>");
	}else if(is("ride")){
		int model = 151;
		sscanf(rest, "%d", &model);
		if(!me)
			ConsolePrint("no player");
		else if(!ModelIsVehicle(model))
			ConsolePrint("model %d is not a vehicle (taxi=151 cheetah=146 sentinel=136)", model);
		else{
			// dev/cheat: spawn a car and hop in as driver — the normal enter
			// streaming (seat assignment, state relay) picks it up from here
			if(me->InVehicle())
				UnseatNetPed(me);
			CVector fwd = me->GetForward();
			CVector p = me->GetPosition() + fwd * 4.0f;
			CStreaming::RequestModel(model, 0);
			CStreaming::LoadAllRequestedModels(false);
			CVehicle* car;
			if(CModelInfo::IsBikeModel(model))      car = new CBike(model, MISSION_VEHICLE);
			else if(CModelInfo::IsBoatModel(model)) car = new CBoat(model, MISSION_VEHICLE);
			else                                    car = new CAutomobile(model, MISSION_VEHICLE);
			if(!car){
				ConsolePrint("vehicle create failed (vehicle pool full?)");
			}else{
				car->SetPosition(p);
				car->SetOrientation(0.0f, 0.0f, std::atan2(-fwd.x, fwd.y));
				CWorld::Add(car);
				me->m_pMyVehicle = car;
				car->RegisterReference((CEntity**)&me->m_pMyVehicle);
				me->bInVehicle = true;
				me->bUsesCollision = false;
				me->SetPedState(PED_DRIVING);
				car->SetDriver(me);
				car->SetStatus(STATUS_PLAYER);
				car->bEngineOn = true;
				me->SetPedPositionInCar();
				ConsolePrint("ride: spawned model %d at (%.1f, %.1f, %.1f)", model, p.x, p.y, p.z);
			}
		}
	}else if(is("fps")){
		int n = 30;
		sscanf(rest, "%d", &n);
		NetGame_SetFpsLimit(n);
		ConsolePrint("fps limit -> %d (0 = unlimited)", n);
	}else if(is("god")){
		s_god = !s_god;
		SendRconFmt("give %d god %d", myId, s_god ? 1 : 0);
		ConsolePrint("godmode %s", s_god ? "ON" : "OFF");
	}else if(is("noclip")){
		s_noclip = !s_noclip;
		if(me){
			if(s_noclip){ s_ncPos = me->GetPosition(); me->bUsesCollision = false; }
			else me->bUsesCollision = true;
		}
		ConsolePrint("noclip %s", s_noclip ? "ON" : "OFF");
	}else if(is("players")){
		ConsolePrint("you (#%d) %s", myId, s_nick.c_str());
		for(auto& kv : s_net.remotes)
			ConsolePrint("#%d %s", (int)kv.first, kv.second.name.c_str());
	}else if(is("status")){
		SendRconFmt("status");
	}else if(is("rcon")){
		if(rest[0]) s_net.SendRconCmd(rest);
		else ConsolePrint("usage: rcon <server command>");
	}else if(is("clear")){
		s_consoleLog.clear();
	}else if(is("keybinds")){
		ConsolePrint("chat=admin=inventory=console=console2= — edit keybinds.ini");
	}else if(is("quit")){
		PostQuitMessage(0);
	}else{
		ConsolePrint("unknown command: %s (try: help)", cmd);
	}
}

// ---------------------------------------------------------------------------
// CEF overlay glue: JS -> host commands and host -> JS state push
// ---------------------------------------------------------------------------
static std::string s_cefPanel; // which overlay panel is open ("" = none)

static void CefQueryHandler(const char* request, void*)
{
	if(!request) return;
	std::string req(request);
	size_t q = req.find('?');
	std::string cmd = req.substr(0, q);
	std::string args = q == std::string::npos ? "" : req.substr(q + 1);
	auto arg = [&](const char* key) -> std::string {
		std::string k = std::string(key) + "=";
		if(args.rfind(k, 0) != 0) return "";
		return args.substr(k.size());
	};
	if(cmd == "chat"){
		std::string text = arg("text");
		if(!text.empty()) s_net.SendChat(text.c_str());
	}else if(cmd == "console"){
		std::string c = arg("cmd");
		if(!c.empty()) ConsoleExec(c.c_str());
	}else if(cmd == "close"){
		s_cefPanel.clear();
		CefHud_Show("");
	}else{
		fprintf(stderr, "[CEF] %s\n", request);
		fflush(stderr);
	}
}

static double s_lastCefPush = 0.0;
static void CefPushState(void)
{
	if(!CefHud_Active()) return;
	double now = NowSec();
	if(now - s_lastCefPush < 0.2) return;
	s_lastCefPush = now;

	auto jesc = [](const std::string& s){
		std::string o;
		for(char c : s){
			if(c == '"' || c == '\\'){ o += '\\'; o += c; }
			else if((unsigned char)c < 0x20) continue;
			else o += c;
		}
		return o;
	};

	CPlayerPed* me = FindPlayerPed();
	std::string weapons = "[";
	if(me){
		bool first = true;
		for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
			CWeapon& wpn = me->GetWeapon((uint8)slot);
			if(wpn.m_eWeaponType == WEAPONTYPE_UNARMED) continue;
			char w[112];
			snprintf(w, sizeof w, "%s{\"name\":\"slot %d — type %d\",\"ammo\":%d}",
			         first ? "" : ",", slot, (int)wpn.m_eWeaponType, (int)wpn.m_nAmmoTotal);
			weapons += w;
			first = false;
		}
	}
	weapons += "]";

	std::string chat = "[";
	{
		bool first = true;
		size_t n = s_net.chat.size(), start = n > 16 ? n - 16 : 0;
		for(size_t i = start; i < n; i++){
			chat += (first ? "\"" : ",\"") + jesc(s_net.chat[i].text) + "\"";
			first = false;
		}
	}
	chat += "]";
	std::string con = "[";
	{
		bool first = true;
		size_t n = s_consoleLog.size(), start = n > 30 ? n - 30 : 0;
		for(size_t i = start; i < n; i++){
			con += (first ? "\"" : ",\"") + jesc(s_consoleLog[i]) + "\"";
			first = false;
		}
	}
	con += "]";

	const char* fpsName = "30";
	for(int i = 0; i < kNumFps; i++) if(kFpsPresets[i] == s_fpsCap) fpsName = kFpsNames[i];
	char buf[4608];
	snprintf(buf, sizeof buf,
	         "{\"server\":\"%s\",\"players\":%zu,\"fps\":\"%s\",\"mode\":\"%s\","
	         "\"money\":%d,\"hp\":%d,\"armour\":%d,\"welcome\":%d,"
	         "\"weapons\":%s,\"chat\":%s,\"console\":%s}",
	         jesc(s_net.Hostname()).c_str(),
	         s_net.remotes.size() + (s_net.Spawned() ? 1 : 0),
	         fpsName, s_god ? "GOD" : (s_noclip ? "NOCLIP" : ""),
	         CWorld::Players[CWorld::PlayerInFocus].m_nMoney,
	         me ? (int)me->m_fHealth : 0, me ? (int)me->m_fArmour : 0,
	         (int)(NowSec() < s_welcomeUntil),
	         weapons.c_str(), chat.c_str(), con.c_str());
	CefHud_PushState(buf);
}

enum {
	PR_BROADCAST = 0, PR_NOCLIP, PR_GOD, PR_MONEY, PR_WEAPON,
	PR_SPAWNBOT, PR_SPAWNPROP, PR_FPS, PR_COUNT
};

static void NetGame_PollUi(void)
{
	static bool s_bindsLoaded = false;
	if(!s_bindsLoaded){ s_bindsLoaded = true; LoadKeyBinds(); }

	// OS cursor while a panel owns the mouse: win.cpp's WM_SETCURSOR draws the
	// arrow; balance the ShowCursor display counter once per transition (the
	// game hides the cursor via ShowCursor(FALSE) on its first WM_SETCURSOR)
	{
		static bool s_cursorShown = false;
		bool want = NetGame_WantMouse();
		if(want != s_cursorShown){
			s_cursorShown = want;
			ShowCursor(want ? TRUE : FALSE);
			SetCursor(LoadCursor(nullptr, want ? IDC_ARROW : IDC_CROSS));
		}
	}

	// poll every handled key first so edge state stays coherent
	bool kAdmin = KeyEdge(s_keys.admin), kInv = KeyEdge(s_keys.inventory),
	     kChat = KeyEdge(s_keys.chat),
	     kConsole = KeyEdge(s_keys.console) ||
	                (s_keys.console2 && s_keys.console2 != s_keys.console && KeyEdge(s_keys.console2)),
	     kEsc = KeyEdge(VK_ESCAPE), kUp = KeyEdge(VK_UP), kDown = KeyEdge(VK_DOWN),
	     kLeft = KeyEdge(VK_LEFT), kRight = KeyEdge(VK_RIGHT),
	     kRet = KeyEdge(VK_RETURN), kBack = KeyEdge(VK_BACK), kSpace = KeyEdge(VK_SPACE);
	int letter = 0, digit = 0, punct = 0;
	for(int k = '0'; k <= '9'; k++) if(KeyEdge(k)) digit = k;
	for(int k = 'A'; k <= 'Z'; k++) if(KeyEdge(k)) letter = k;
	static const struct { int vk; char ch; } kPunct[] = {
		{VK_OEM_PERIOD, '.'}, {VK_OEM_COMMA, ','}, {VK_OEM_2, '/'}, {VK_OEM_MINUS, '-'},
		{VK_OEM_1, ';'}, {VK_OEM_7, '\''}, {VK_OEM_4, '['}, {VK_OEM_6, ']'},
		{VK_OEM_PLUS, '='}, {VK_OEM_3, '`'},
	};
	for(const auto& e : kPunct) if(KeyEdge(e.vk)) punct = e.ch;
	auto typeChar = [&]() -> int {
		if(kSpace) return ' ';
		if(digit) return digit;
		if(letter) return letter;
		return punct;
	};

	auto cefPanel = [&](const char* name){
		s_cefPanel = (s_cefPanel == name) ? "" : name;
		CefHud_Show(s_cefPanel.c_str());
	};

	// CEF overlay takes all input while a panel is open
	if(CefHud_Active() && CefHud_Focused()){
		if(kEsc){ s_cefPanel.clear(); CefHud_Show(""); return; }
		// non-typeable keys still switch panels while typing
		if(kConsole){ cefPanel("console"); return; }
		if(kAdmin){ cefPanel("admin"); return; }
		// control keys: real keydown events (the page listens on keydown) +
		// the matching char event for text fields. Order matters within a
		// frame: text first, then Backspace, then Enter — otherwise Enter can
		// submit before the last typed character lands (dropped trailing char)
		if(kUp)   CefHud_Key(1, VK_UP, 0, 0);
		if(kDown) CefHud_Key(1, VK_DOWN, 0, 0);
		if(letter){
			// respect shift/caps so typed text keeps its case
			bool upper = ((GetKeyState(VK_SHIFT) & 0x8000) != 0) ^
			             ((GetKeyState(VK_CAPITAL) & 1) != 0);
			int ch = upper ? letter : letter + ('a' - 'A');
			CefHud_Key(1, letter, ch, (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0);
		}
		if(digit)  CefHud_Key(1, digit, digit, 0);
		if(kSpace) CefHud_Key(1, VK_SPACE, ' ', 0);
		if(punct)  CefHud_Key(1, 0, punct, 0);
		if(kBack){ CefHud_Key(1, VK_BACK, 0, 0);   CefHud_Key(1, 0, '\b', 0); }
		if(kRet){  CefHud_Key(1, VK_RETURN, 0, 0); CefHud_Key(1, 0, '\r', 0); }
		POINT pt;
		GetCursorPos(&pt);
		ScreenToClient(GetActiveWindow(), &pt);
		static bool s_lmbPrev = false;
		bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		CefHud_Mouse(0, pt.x, pt.y, 0, 0);
		if(lmb && !s_lmbPrev) CefHud_Mouse(1, pt.x, pt.y, 0, 0);
		if(!lmb && s_lmbPrev) CefHud_Mouse(2, pt.x, pt.y, 0, 0);
		s_lmbPrev = lmb;
		return;
	}

	if(kConsole){
		if(CefHud_Active()){ cefPanel("console"); return; }
		s_consoleOpen = !s_consoleOpen;
		if(s_consoleOpen){ s_panelOpen = false; s_chatOpen = false; s_invOpen = false; }
	}

	if(s_consoleOpen){
		if(kEsc) s_consoleOpen = false;
		else if(kRet){
			s_consoleBuf[s_consoleLen] = 0;
			if(s_consoleLen > 0){
				s_consoleHist.push_back(s_consoleBuf);
				if(s_consoleHist.size() > 24) s_consoleHist.erase(s_consoleHist.begin());
				ConsoleExec(s_consoleBuf);
			}
			s_consoleHistPos = -1;
			s_consoleLen = 0;
		}else if(kUp){
			if(!s_consoleHist.empty()){
				if(s_consoleHistPos < 0) s_consoleHistPos = (int)s_consoleHist.size();
				if(s_consoleHistPos > 0) s_consoleHistPos--;
				snprintf(s_consoleBuf, sizeof s_consoleBuf, "%s", s_consoleHist[s_consoleHistPos].c_str());
				s_consoleLen = (int)strlen(s_consoleBuf);
			}
		}else if(kDown){
			if(s_consoleHistPos >= 0 && s_consoleHistPos + 1 < (int)s_consoleHist.size()){
				s_consoleHistPos++;
				snprintf(s_consoleBuf, sizeof s_consoleBuf, "%s", s_consoleHist[s_consoleHistPos].c_str());
			}else{
				s_consoleHistPos = -1;
				s_consoleBuf[0] = 0;
			}
			s_consoleLen = (int)strlen(s_consoleBuf);
		}else{
			if(kBack && s_consoleLen > 0) s_consoleLen--;
			int ch = typeChar();
			if(ch && s_consoleLen < (int)sizeof(s_consoleBuf) - 1)
				s_consoleBuf[s_consoleLen++] = (char)ch;
		}
		return; // swallow gameplay keys while the console is up
	}

	if(kAdmin){
		if(CefHud_Active()){ cefPanel("admin"); return; }
		s_panelOpen = !s_panelOpen;
		if(s_panelOpen){ s_invOpen = false; s_chatOpen = false; }
	}
	if(kInv){
		if(CefHud_Active()){ cefPanel("inventory"); return; }
		s_invOpen = !s_invOpen;
	}

	if(s_chatOpen){
		if(kEsc) s_chatOpen = false;
		else if(kRet){
			s_chatBuf[s_chatLen] = 0;
			if(s_chatLen > 0){
				if(s_chatMode == 'b'){
					char cmd[128];
					snprintf(cmd, sizeof cmd, "broadcast %s", s_chatBuf);
					s_net.SendRconCmd(cmd);
				}else
					s_net.SendChat(s_chatBuf);
			}
			s_chatOpen = false;
			s_chatLen = 0;
		}else{
			if(kBack && s_chatLen > 0) s_chatLen--;
			int ch = typeChar();
			if(ch && s_chatLen < (int)sizeof(s_chatBuf) - 1)
				s_chatBuf[s_chatLen++] = (char)ch;
		}
		return; // swallow gameplay keys while typing
	}

	if(kChat && !s_panelOpen){
		if(CefHud_Active()){ cefPanel("chat"); return; }
		s_chatOpen = true; s_chatMode = 'c'; s_chatLen = 0;
	}
	if(!s_panelOpen) return;
	if(kEsc){ s_panelOpen = false; return; }
	if(kUp) s_panelRow = (s_panelRow + PR_COUNT - 1) % PR_COUNT;
	if(kDown) s_panelRow = (s_panelRow + 1) % PR_COUNT;

	bool admin = !s_rconPw.empty();		auto rcon = [&](const char* fmt, ...){
			char buf[160]; va_list ap; va_start(ap, fmt);
			vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
		if(admin) s_net.SendRconCmd(buf);
		snprintf(s_lastRcon, sizeof s_lastRcon, "%s", admin ? buf : "no -rcon password");
	};

	if(kLeft || kRight){
		int d = kRight ? 1 : -1;
		if(s_panelRow == PR_WEAPON)
			s_panelWeapon = (s_panelWeapon + kNumGiveWeapons + d) % kNumGiveWeapons;
		if(s_panelRow == PR_FPS){
			s_panelFpsIdx = (s_panelFpsIdx + kNumFps + d) % kNumFps;
			NetGame_SetFpsLimit(kFpsPresets[s_panelFpsIdx]);
		}
	}
	if(!kRet) return;

	int myId = (int)s_net.MyId();
	CPlayerPed* me = FindPlayerPed();
	switch(s_panelRow){
	case PR_BROADCAST:
		s_chatOpen = true; s_chatMode = 'b'; s_chatLen = 0;
		break;
	case PR_NOCLIP:
		s_noclip = !s_noclip;
		if(me){
			if(s_noclip){
				s_ncPos = me->GetPosition();
				me->bUsesCollision = false;
			}else
				me->bUsesCollision = true;
		}
		break;
	case PR_GOD:
		rcon("give %d god %d", myId, s_god ? 0 : 1);
		break;
	case PR_MONEY:
		rcon("give %d money 10000", myId);
		break;
	case PR_WEAPON:
		rcon("give %d weapon %d 200", myId, (int)kGiveWeapons[s_panelWeapon]);
		break;
	case PR_SPAWNBOT:
		rcon("spawnbot NPC%03d roam", (int)(NowSec() * 1000.0) % 1000);
		break;
	case PR_SPAWNPROP:
		if(me){
			CVector p = me->GetPosition();
			rcon("spawnprop %.1f %.1f %.1f 4 50", p.x, p.y, p.z);
		}
		break;
	case PR_FPS:
		s_panelFpsIdx = (s_panelFpsIdx + 1) % kNumFps;
		NetGame_SetFpsLimit(kFpsPresets[s_panelFpsIdx]);
		break;
	}
}

// ---------------------------------------------------------------------------
// command line
// ---------------------------------------------------------------------------
static std::string s_pendingArg;

bool NetGame_OnCommandLine(const char* arg)
{
	if(s_pendingArg == "-connect"){
		std::string v = arg;
		size_t c = v.find(':');
		s_host = v.substr(0, c);
		if(c != std::string::npos) s_port = (uint16_t)atoi(v.c_str() + c + 1);
		s_wantConnect = true;
		s_pendingArg.clear();
		return true;
	}
	if(s_pendingArg == "-nick"){
		s_nick = arg;
		s_pendingArg.clear();
		return true;
	}
	if(s_pendingArg == "-fps"){
		NetGame_SetFpsLimit(atoi(arg)); // 0 or "unlimited" = uncapped
		s_pendingArg.clear();
		return true;
	}
	if(s_pendingArg == "-rcon"){
		s_rconPw = arg;
		s_pendingArg.clear();
		return true;
	}
	if(!strcmp(arg, "-connect") || !strcmp(arg, "-nick") ||
	   !strcmp(arg, "-fps") || !strcmp(arg, "-rcon")){
		s_pendingArg = arg;
		return true;
	}
	return false;
}

bool NetGame_IsActive(void)
{
	return s_connected;
}

// ---------------------------------------------------------------------------
// frontend auto-start: a -connect launch must never depend on menu navigation
// ---------------------------------------------------------------------------
static bool s_autoStartDone = false;
static double s_autoStartAt = 0.0;

void NetGame_FrontendTick(void)
{
	if(!s_wantConnect || s_autoStartDone)
		return;
	if(FindPlayerPed()){
		// game already running; NetGame_Frame will handle the connect
		s_autoStartDone = true;
		return;
	}
	double now = NowSec();
	if(s_autoStartAt == 0.0){
		// give the frontend a moment to finish fading in before yanking it away
		s_autoStartAt = now + 2.0;
		fprintf(stderr, "[NET] -connect: auto-starting game in 2s (server %s:%u)\n",
		        s_host.c_str(), (unsigned)s_port);
		fflush(stderr);
		return;
	}
	if(now < s_autoStartAt)
		return;
	s_autoStartDone = true;
	fprintf(stderr, "[NET] -connect: starting new game\n");
	fflush(stderr);
	// same code path as the frontend's New Game button
	FrontEndMenuManager.DoSettingsBeforeStartingAGame();
}

// ---------------------------------------------------------------------------
// per-frame
// ---------------------------------------------------------------------------
static double s_lastSend = 0.0;
static double s_lastLog = 0.0;
// the game relocates the player on its own (new-game init race, death and
// hospital respawn, scripted warps); don't fight it — report the jump as a
// flagged teleport and the server resets its movement reference instead of
// anti-cheat kicking us (snap-back used to ping-pong with the game at 20 Hz)
static CVector s_lastSentPos(0.0f, 0.0f, 0.0f);
static bool s_haveLastSent = false;
static double s_lastTeleportLog = 0.0;

void NetGame_Frame(void)
{
	// chromium overlay: init on first frame, pump + push state every frame
	CefHud_Ensure();
	if(CefHud_Active()){
		static bool s_cefHooked = false;
		if(!s_cefHooked){ s_cefHooked = true; CefHud_SetQueryHandler(CefQueryHandler); }
		CefHud_Tick();
		CefPushState();
	}

	// connect once the player exists (i.e. world loaded)
	if(s_wantConnect && !s_connected && FindPlayerPed()){
		if(s_net.Connect(s_host.c_str(), s_port, s_nick.c_str())){
			s_connected = true;
			s_wantConnect = false;
		}else{
			fprintf(stderr, "[NET] connect failed to %s:%u\n", s_host.c_str(), (unsigned)s_port);
			fflush(stderr);
			s_wantConnect = false;
		}
	}
	if(!s_connected)
		return;

	s_net.Pump();

	float dt = CTimer::GetTimeStep() / 50.0f; // frames -> seconds-ish (50 fps units)
	if(dt <= 0.0f || dt > 0.5f) dt = 0.02f;

	// events
	NetClient::Event ev;
	while(s_net.PopEvent(ev)){
		switch(ev.type){
		case NetClient::EV_CONNECTED:
			s_net.SendSpawnRequest();
			if(!s_rconPw.empty())
				s_net.SendRconAuth(s_rconPw.c_str());
			break;
		case NetClient::EV_SPAWNED:{
			CPlayerPed* me = FindPlayerPed();
			if(me){
				CVector pos(ev.state.xMm / 1000.0f, ev.state.yMm / 1000.0f, ev.state.zMm / 1000.0f);
				me->SetPosition(pos);
				me->SetOrientation(0.0f, 0.0f, rnet::DequantAngle(ev.state.heading));
				s_lastSentPos = pos;
				s_haveLastSent = true;
			}
			s_welcomeUntil = NowSec() + 6.0;
			break;
		}
		case NetClient::EV_CORRECTION:{
			CPlayerPed* me = FindPlayerPed();
			if(me){
				CVector pos(ev.state.xMm / 1000.0f, ev.state.yMm / 1000.0f, ev.state.zMm / 1000.0f);
				me->SetPosition(pos);
				me->SetOrientation(0.0f, 0.0f, rnet::DequantAngle(ev.state.heading));
				me->SetMoveSpeed(0.0f, 0.0f, 0.0f);
				s_lastSentPos = pos;
				s_haveLastSent = true;
			}
			break;
		}
		case NetClient::EV_GIVE:{
			CPlayerPed* me = FindPlayerPed();
			if(me){
				switch(ev.kind){
				case rnet::GIVE_MONEY:
					CWorld::Players[CWorld::PlayerInFocus].m_nMoney += ev.amount;
					break;
				case rnet::GIVE_WEAPON:
					me->SetCurrentWeapon(me->GiveWeapon((eWeaponType)ev.arg, (uint32)ev.amount, true));
					break;
				case rnet::GIVE_GOD:
					s_god = ev.amount != 0;
					break;
				case rnet::GIVE_HEALTH:
					me->m_fHealth = (float)ev.amount;
					break;
				}
				fprintf(stderr, "[NET] admin give kind=%d amount=%d arg=%d\n",
				        (int)ev.kind, ev.amount, (int)ev.arg);
				fflush(stderr);
			}
			break;
		}
		case NetClient::EV_RCON_RESP:
			snprintf(s_lastRcon, sizeof s_lastRcon, "%s", ev.text.c_str());
			fprintf(stderr, "[NET] rcon: %s\n", ev.text.c_str());
			break;
		case NetClient::EV_PICKUP:{
			CVector pos(ev.state.xMm / 1000.0f, ev.state.yMm / 1000.0f, ev.state.zMm / 1000.0f);
			if(ev.model != 0)
				CPickups::GenerateNewOne(pos, ev.model, ev.kind, (uint32)ev.amount);
			else
				CPickups::GenerateNewOne_WeaponType(pos, (eWeaponType)ev.arg, ev.kind, (uint32)ev.amount);
			fprintf(stderr, "[NET] prop spawned at (%.1f, %.1f, %.1f)\n", pos.x, pos.y, pos.z);
			fflush(stderr);
			break;
		}
		case NetClient::EV_REMOTE_SPAWN:
			SpawnRemote(ev.id, ev.state.xMm / 1000.0f, ev.state.yMm / 1000.0f,
			            ev.state.zMm / 1000.0f, rnet::DequantAngle(ev.state.heading));
			// seat arrived before the ped existed — apply it now
			{
				auto pit = s_pendingSeat.find(ev.id);
				if(pit != s_pendingSeat.end()){
					auto vit = s_vehicles.find(pit->second.first);
					auto rit = s_remotes.find(ev.id);
					if(vit != s_vehicles.end() && vit->second.veh &&
					   rit != s_remotes.end() && rit->second.ped)
						SeatNetPed(rit->second.ped, vit->second.veh, pit->second.second);
					s_pendingSeat.erase(pit);
				}
			}
			break;
		case NetClient::EV_REMOTE_QUIT:
			RemoveRemote(ev.id);
			break;
		case NetClient::EV_VEH_SPAWN:{
			float vx = ev.state.xMm / 1000.0f, vy = ev.state.yMm / 1000.0f, vz = ev.state.zMm / 1000.0f;
			float vh = rnet::DequantAngle(ev.state.heading);
			if(s_pendingVeh){
				// this spawn is OUR enter being allocated: bind our local car
				CVector pp = s_pendingVeh->GetPosition();
				bool match = (ev.model == s_pendingModel) &&
				             std::fabs(pp.x - vx) < 30.0f && std::fabs(pp.y - vy) < 30.0f;
				if(match){
					AdoptNetVehicle(ev.arg, s_pendingVeh, s_pendingModel);
					s_pendingVeh = nil;
					fprintf(stderr, "[NET] local car adopted as net vehicle %d\n", (int)ev.arg);
					fflush(stderr);
					break;
				}
			}
			NetVehicleAt(ev.arg, ev.model, vx, vy, vz, vh);
			break;
		}
		case NetClient::EV_VEH_ENTER:
			if(ev.id == s_net.MyId()){
				s_myVehNet = ev.arg;
				s_mySeat = ev.kind;
				s_pendingVeh = nil;
				// bind our local car object if the spawn didn't match it
				{
					CPlayerPed* me2 = FindPlayerPed();
					CVehicle* local = (me2 && me2->bInVehicle) ? me2->m_pMyVehicle : s_lastMyVeh;
					if(local && NetIdOfVehicle(local) == rnet::VEHICLE_NONE){
						RemoveNetVehicle(ev.arg); // drop any half-made puppet
						AdoptNetVehicle(ev.arg, local, (uint16_t)local->GetModelIndex());
					}
				}
				fprintf(stderr, "[NET] we are in net vehicle %d seat %d\n", (int)ev.arg, (int)ev.kind);
				fflush(stderr);
			}else{
				// remote player took a seat
				CVehicle* veh = nil;
				auto vit = s_vehicles.find(ev.arg);
				if(vit != s_vehicles.end()) veh = vit->second.veh;
				if(!veh){
					// enter raced the spawn — materialize on the spot
					NetRemotePlayer& rp = s_net.remotes[ev.id];
					veh = NetVehicleAt(ev.arg, ev.model,
					                   rp.state.xMm / 1000.0f, rp.state.yMm / 1000.0f,
					                   rp.state.zMm / 1000.0f,
					                   rnet::DequantAngle(rp.state.heading)).veh;
				}
				auto it = s_remotes.find(ev.id);
				if(it != s_remotes.end() && it->second.ped && veh){
					SeatNetPed(it->second.ped, veh, ev.kind);
					fprintf(stderr, "[NET] remote player %d seated in veh %d seat %d\n",
					        (int)ev.id, (int)ev.arg, (int)ev.kind);
					fflush(stderr);
				}else
					s_pendingSeat[ev.id] = std::make_pair(ev.arg, ev.kind);
			}
			break;
		case NetClient::EV_VEH_EXIT:
			if(ev.id == s_net.MyId()){
				s_myVehNet = rnet::VEHICLE_NONE;
				s_mySeat = 0xFF;
				s_pendingVeh = nil;
			}else{
				auto it = s_remotes.find(ev.id);
				if(it != s_remotes.end() && it->second.ped)
					UnseatNetPed(it->second.ped);
				s_pendingSeat.erase(ev.id);
			}
			break;
		case NetClient::EV_VEH_STATE:{
			if(ev.arg == s_myVehNet && s_mySeat == 0)
				break; // our own authoritative echo
			NetVehicle& nv = NetVehicleAt(ev.arg, ev.model,
			                            ev.veh.xMm / 1000.0f, ev.veh.yMm / 1000.0f,
			                            ev.veh.zMm / 1000.0f,
			                            rnet::DequantAngle(ev.veh.heading));
			nv.target = ev.veh;
			if(!nv.hasTarget){
				nv.smoothX = ev.veh.xMm / 1000.0f;
				nv.smoothY = ev.veh.yMm / 1000.0f;
				nv.smoothZ = ev.veh.zMm / 1000.0f;
				nv.smoothH = rnet::DequantAngle(ev.veh.heading);
				nv.hasTarget = true;
			}
			{
				static int s_vehStateLog = 0;
				if(++s_vehStateLog % 100 == 1){
					fprintf(stderr, "[NET] applying veh %d state at (%.1f, %.1f, %.1f) #%d\n",
					        (int)ev.arg, ev.veh.xMm / 1000.0f, ev.veh.yMm / 1000.0f,
					        ev.veh.zMm / 1000.0f, s_vehStateLog);
					fflush(stderr);
				}
			}
			break;
		}
		case NetClient::EV_DISCONNECTED:
			fprintf(stderr, "[NET] disconnected: %s\n", ev.text.c_str());
			fflush(stderr);
			while(!s_remotes.empty())
				RemoveRemote(s_remotes.begin()->first);
			ClearAllVehicles();
			s_connected = false;
			break;
		default:
			break;
		}
	}

	// apply remote states
	for(auto& kv : s_net.remotes)
		if(kv.second.spawned)
			ApplyRemote(kv.first, kv.second.state, dt);

	// vehicles: interpolation, freeze/unfreeze, puppet retries
	UpdateNetVehicles(dt);

	// sample + send local state at 20 Hz
	double now = NowSec();
	CPlayerPed* me = FindPlayerPed();

	// local vehicle enter/exit streaming
	if(me){
		CVehicle* cur = (me->bInVehicle && me->m_pMyVehicle != nil) ? me->m_pMyVehicle : nil;
		if(cur != s_lastMyVeh){
			if(s_lastMyVeh){
				// left the old vehicle (id 0: the server resolves by occupancy)
				uint16_t id = NetIdOfVehicle(s_lastMyVeh);
				s_net.SendVehExit(id != rnet::VEHICLE_NONE ? id : 0);
				s_myVehNet = rnet::VEHICLE_NONE;
				s_mySeat = 0xFF;
				s_pendingVeh = nil;
			}
			if(cur){
				uint8_t seat = 0;
				if(me != cur->pDriver){
					seat = 0xFF;
					for(int i = 0; i < 8; i++)
						if(cur->pPassengers[i] == me){ seat = (uint8_t)(i + 1); break; }
					if(seat == 0xFF) seat = 0;
				}
				s_mySeat = seat;
				uint16_t existing = NetIdOfVehicle(cur);
				if(existing != rnet::VEHICLE_NONE){
					// entering a vehicle that is already a net entity (someone's car)
					s_myVehNet = existing;
					s_net.SendVehEnter(existing, seat, (uint16_t)cur->GetModelIndex());
				}else{
					// new net vehicle: the server allocates the id
					s_pendingVeh = cur;
					s_pendingModel = (uint16_t)cur->GetModelIndex();
					s_net.SendVehEnter(rnet::VEHICLE_NONE, seat, s_pendingModel);
				}
				fprintf(stderr, "[NET] entered vehicle model=%d seat=%d\n",
				        (int)cur->GetModelIndex(), (int)seat);
				fflush(stderr);
			}
			s_lastMyVeh = cur;
		}
	}

	// admin UI + local admin states
	NetGame_PollUi();
	if(s_god && me){
		me->m_fHealth = 100.0f;
		me->m_fArmour = 100.0f;
	}
	if(s_noclip && me){
		me->bUsesCollision = false;
		me->SetMoveSpeed(0.0f, 0.0f, 0.0f);
		float sp = 20.0f * dt;
		CVector fwd = me->GetForward();
		CVector right = me->GetRight();
		if(GetAsyncKeyState('W') & 0x8000) s_ncPos += fwd * sp;
		if(GetAsyncKeyState('S') & 0x8000) s_ncPos -= fwd * sp;
		if(GetAsyncKeyState('D') & 0x8000) s_ncPos += right * sp;
		if(GetAsyncKeyState('A') & 0x8000) s_ncPos -= right * sp;
		if(GetAsyncKeyState(VK_SPACE) & 0x8000) s_ncPos.z += sp;
		if(GetAsyncKeyState(VK_SHIFT) & 0x8000) s_ncPos.z -= sp;
		me->SetPosition(s_ncPos);
	}

	if(me && s_net.Spawned() && now - s_lastSend >= 0.05){
		float tickDt = (float)(now - s_lastSend);
		if(tickDt > 0.5f) tickDt = 0.5f;
		s_lastSend = now;
		rnet::PlayerState& st = s_net.myState;
		CVector pos = me->GetPosition();
		bool inVehicle = me->bInVehicle && me->m_pMyVehicle != nil;

		// self-validate: anything the server would movement-flag (game-side
		// relocations, scripted/cutscene rides, physics launches) is reported
		// as a flagged move instead — the server resets its movement reference
		// (snap-back used to ping-pong with the game at 20 Hz)
		st.moveFlags = 0;
		if(s_haveLastSent){
			float dx = pos.x - s_lastSentPos.x, dy = pos.y - s_lastSentPos.y, dz = pos.z - s_lastSentPos.z;
			// mirror ValidateMove's movement rule with our precise local dt
			float maxH = (inVehicle ? rnet::MAX_SPEED_VEHICLE : rnet::MAX_SPEED_ONFOOT) * tickDt;
			float maxDown = (inVehicle ? rnet::MAX_SPEED_VEHICLE : rnet::MAX_SPEED_FALL) * tickDt;
			float h = std::sqrt(dx*dx + dy*dy);
			if(h > maxH || dz > maxH || -dz > maxDown)
				st.moveFlags = rnet::MOVEFLAG_TELEPORT;
			if(st.moveFlags && now - s_lastTeleportLog > 1.0){
				s_lastTeleportLog = now;
				fprintf(stderr, "[NET] game-driven move (%.1f, %.1f, %.1f)->(%.1f, %.1f, %.1f) — flagged\n",
				        s_lastSentPos.x, s_lastSentPos.y, s_lastSentPos.z, pos.x, pos.y, pos.z);
				fflush(stderr);
			}
		}
		st.xMm = rnet::QuantPos(pos.x);
		st.yMm = rnet::QuantPos(pos.y);
		st.zMm = rnet::QuantPos(pos.z);
		CVector fwd = me->GetForward();
		// GTA heading convention: z with forward = (-sin z, cos z), i.e. the
		// inverse of SetOrientation(0,0,z) (was atan2(y,x) — 90° off on remotes)
		st.heading = rnet::QuantAngle(std::atan2(-fwd.x, fwd.y));
		st.pitch = 0;
		CVector spd = me->GetMoveSpeed();
		st.velX = rnet::QuantVel(spd.x);
		st.velY = rnet::QuantVel(spd.y);
		st.velZ = rnet::QuantVel(spd.z);
		st.health = (uint8_t)(me->m_fHealth > 100.0f ? 100 : me->m_fHealth);
		st.armour = (uint8_t)(me->m_fArmour > 100.0f ? 100 : me->m_fArmour);
		st.weapon = (uint8_t)me->GetWeapon()->m_eWeaponType;
		st.ammo = (uint16_t)me->GetWeapon()->m_nAmmoTotal;
		st.money = CWorld::Players[CWorld::PlayerInFocus].m_nMoney;
		st.animId = 0;
		st.animFlags = 0;
		st.vehicleId = inVehicle ? (s_myVehNet != rnet::VEHICLE_NONE ? s_myVehNet : rnet::VEHICLE_UNSYNCED)
		                         : rnet::VEHICLE_NONE;
		st.seat = inVehicle ? s_mySeat : 0xFF;
		s_lastSentPos = pos;
		s_haveLastSent = true;
		s_net.SendState(st);

		// the driver streams the vehicle (seat 0 is authoritative for the car)
		if(inVehicle && s_myVehNet != rnet::VEHICLE_NONE && s_mySeat == 0 && me->m_pMyVehicle){
			CVehicle* cv = me->m_pMyVehicle;
			rnet::VehicleState vs;
			vs.vehicleId = s_myVehNet;
			vs.modelId = (uint16_t)cv->GetModelIndex();
			CVector vpos = cv->GetPosition();
			vs.xMm = rnet::QuantPos(vpos.x);
			vs.yMm = rnet::QuantPos(vpos.y);
			vs.zMm = rnet::QuantPos(vpos.z);
			CVector vf = cv->GetForward(), vr = cv->GetRight(), vu = cv->GetUp();
			vs.heading = rnet::QuantAngle(std::atan2(-vf.x, vf.y));
			float fp = vf.z;
			if(fp > 1.0f) fp = 1.0f;
			if(fp < -1.0f) fp = -1.0f;
			vs.pitch = rnet::QuantAngle(std::asin(fp));
			vs.roll = rnet::QuantAngle(std::atan2(-vr.z, vu.z));
			CVector spd = cv->GetMoveSpeed();
			vs.velX = rnet::QuantVel(spd.x);
			vs.velY = rnet::QuantVel(spd.y);
			vs.velZ = rnet::QuantVel(spd.z);
			vs.steer = (int8_t)(cv->m_fSteerAngle * 100.0f);
			vs.gas = (uint8_t)(cv->m_fGasPedal * 100.0f);
			vs.brake = (uint8_t)(cv->m_fBrakePedal * 100.0f);
			vs.flags = cv->bEngineOn ? rnet::VEH_ENGINE : 0;
			if(cv->bLightsOn) vs.flags |= rnet::VEH_LIGHTS;
			vs.health = (uint16_t)(cv->m_fHealth < 0.0f ? 0 : cv->m_fHealth);
			s_net.SendVehState(vs);
		}
	}

	if(now - s_lastLog > 2.0){
		s_lastLog = now;
		fprintf(stderr, "[NET] remotes=%zu pos=(%.1f, %.1f, %.1f)\n",
		        s_net.remotes.size(),
		        me ? me->GetPosition().x : 0.0f,
		        me ? me->GetPosition().y : 0.0f,
		        me ? me->GetPosition().z : 0.0f);
		fflush(stderr);
	}
}

// ---------------------------------------------------------------------------
// overlay: nametags, welcome splash, chat, inventory, admin panel
// ---------------------------------------------------------------------------
static void HudText(float x, float y, float scale, const char* text, CRGBA col, bool centered = false)
{
	wchar buffer[256];
	char ascii[256];
	snprintf(ascii, sizeof ascii, "%s", text);
	AsciiToUnicode(ascii, buffer);
	CFont::SetScale(SCREEN_SCALE_X(scale), SCREEN_SCALE_Y(scale * 1.5f));
	CFont::SetColor(col);
	if(centered) CFont::SetCentreOn(); else CFont::SetCentreOff();
	CFont::PrintString(x, y, buffer);
	CFont::SetCentreOff();
}

void NetGame_DrawHud(void)
{
	if(!s_connected)
		return;

	CFont::SetBackgroundOff();
	CFont::SetRightJustifyOff();
	CFont::SetBackGroundOnlyTextOff();
	CFont::SetFontStyle(FONT_HEADING);
	CFont::SetPropOff();
	CFont::SetDropShadowPosition(2);
	CFont::SetDropColor(CRGBA(0, 0, 0, 255));

	// nametags anchored at the model's true head height (engine convention:
	// entity pos + colModel boundingBox.max.z = top of model — see Cam.cpp)
	CPlayerPed* me = FindPlayerPed();
	CVector myPos = me ? me->GetPosition() : CVector(0.0f, 0.0f, 0.0f);
	auto tagHeight = [](CPed* ped) -> float {
		CColModel* col = CModelInfo::GetColModel(ped->GetModelIndex());
		return ped->GetPosition().z + (col ? col->boundingBox.max.z + 0.12f : 1.9f);
	};
	for(auto& kv : s_remotes){
		if(!kv.second.ped) continue;
		CVector p = kv.second.ped->GetPosition();
		if((p - myPos).Magnitude() > 80.0f) continue;
		p.z = tagHeight(kv.second.ped);
		CVector scr; float w, h;
		if(CSprite::CalcScreenCoors(p, &scr, &w, &h, true)){
			auto it = s_net.remotes.find(kv.first);
			char name[32];
			snprintf(name, sizeof name, "%s",
			         (it != s_net.remotes.end() && !it->second.name.empty()) ? it->second.name.c_str() : "?");
			HudText(scr.x, scr.y, 0.42f, name, CRGBA(255, 255, 255, 220), true);
		}
	}
	// own nametag above the local player model
	if(me && s_net.Spawned()){
		CVector p = me->GetPosition();
		p.z = tagHeight(me);
		CVector scr; float w, h;
		if(CSprite::CalcScreenCoors(p, &scr, &w, &h, true))
			HudText(scr.x, scr.y, 0.42f, s_nick.c_str(), CRGBA(120, 220, 120, 230), true);
	}

	// With the chromium overlay live, it owns ALL 2D UI (top bar, splash,
	// chat, console, inventory, admin). Only the world-anchored nametags stay
	// native — and only while CEF is absent do the native fallbacks draw.
	if(!CefHud_Active()){
	// welcome splash on world enter (centered, scaled to fit any screen)
	double noww = NowSec();
	if(noww < s_welcomeUntil){
		float fade = (float)(s_welcomeUntil - noww) / 1.5f;
		if(fade > 1.0f) fade = 1.0f;
		HudText(SCREEN_SCALE_X(320.0f), SCREEN_SCALE_Y(150.0f), 0.6f,
		        "Welcome to Liberty City Stories Online",
		        CRGBA(255, 210, 80, (uint8)(fade * 255.0f)), true);
	}

	// server HUD (top center)
	{
		char line[160];
		size_t players = s_net.remotes.size() + (me && s_net.Spawned() ? 1 : 0);
		snprintf(line, sizeof line, "%s   players online: %zu", s_net.Hostname(), players);
		HudText(SCREEN_SCALE_X(320.0f), SCREEN_SCALE_Y(25.0f), 0.55f, line, CRGBA(120, 220, 120, 255), true);
		const char* fpsName = "30";
		for(int i = 0; i < kNumFps; i++) if(kFpsPresets[i] == s_fpsCap) fpsName = kFpsNames[i];
		snprintf(line, sizeof line, "%s%s  fps: %s",
		         s_god ? "GOD  " : "", s_noclip ? "NOCLIP  " : "", fpsName);
		if(s_god || s_noclip || s_fpsCap != 30)
			HudText(SCREEN_SCALE_X(320.0f), SCREEN_SCALE_Y(40.0f), 0.45f, line, CRGBA(255, 120, 120, 255), true);
	}

	// chat log (center screen — keeps the minimap clear)
	{
		const size_t maxLines = 8;
		size_t total = s_net.chat.size();
		size_t start = total > maxLines ? total - maxLines : 0;
		if(total > start){
			CSprite2d::DrawRect(
				CRect(SCREEN_SCALE_X(70.0f), SCREEN_SCALE_Y(212.0f),
				      SCREEN_SCALE_X(570.0f), SCREEN_SCALE_Y(220.0f + 14.0f * (float)(total - start) + 8.0f)),
				CRGBA(0, 0, 0, 85));
		}
		float y = SCREEN_SCALE_Y(220.0f);
		for(size_t i = start; i < total; i++){
			HudText(SCREEN_SCALE_X(320.0f), y, 0.55f, s_net.chat[i].text.c_str(), CRGBA(255, 255, 255, 255), true);
			y += SCREEN_SCALE_Y(14.0f);
		}
	}

	// chat / broadcast input line (center screen, below the log)
	if(s_chatOpen){
		char line[112];
		s_chatBuf[s_chatLen] = 0;
		snprintf(line, sizeof line, "%s %s_",
		         s_chatMode == 'b' ? "[BROADCAST]" : "[CHAT]", s_chatBuf);
		HudText(SCREEN_SCALE_X(320.0f), SCREEN_SCALE_Y(332.0f), 0.55f, line, CRGBA(255, 230, 120, 255), true);
	}

	// inventory + personal HUD (right side)
	if(s_invOpen && me){
		char line[96];
		float y = SCREEN_SCALE_Y(120.0f);
		HudText(SCREEN_SCALE_X(400.0f), y, 0.55f, "INVENTORY (I)", CRGBA(255, 210, 80, 255));
		y += SCREEN_SCALE_Y(15.0f);
		snprintf(line, sizeof line, "Money: $%d", CWorld::Players[CWorld::PlayerInFocus].m_nMoney);
		HudText(SCREEN_SCALE_X(400.0f), y, 0.45f, line, CRGBA(230, 230, 230, 255));
		y += SCREEN_SCALE_Y(12.0f);
		snprintf(line, sizeof line, "Health: %d   Armour: %d",
		         (int)me->m_fHealth, (int)me->m_fArmour);
		HudText(SCREEN_SCALE_X(400.0f), y, 0.45f, line, CRGBA(230, 230, 230, 255));
		y += SCREEN_SCALE_Y(14.0f);
		for(int slot = 0; slot < TOTAL_WEAPON_SLOTS; slot++){
			CWeapon& wpn = me->GetWeapon((uint8)slot);
			if(wpn.m_eWeaponType == WEAPONTYPE_UNARMED) continue;
			snprintf(line, sizeof line, "  slot %d: type %d  ammo %d",
			         slot, (int)wpn.m_eWeaponType, (int)wpn.m_nAmmoTotal);
			HudText(SCREEN_SCALE_X(400.0f), y, 0.4f, line, CRGBA(190, 190, 190, 255));
			y += SCREEN_SCALE_Y(11.0f);
		}
	}

	// developer console (configurable keybind)
	if(s_consoleOpen){
		CSprite2d::DrawRect(CRect(SCREEN_SCALE_X(20.0f), SCREEN_SCALE_Y(60.0f),
		                      SCREEN_SCALE_X(620.0f), SCREEN_SCALE_Y(302.0f)), CRGBA(0, 0, 0, 175));
		HudText(SCREEN_SCALE_X(320.0f), SCREEN_SCALE_Y(66.0f), 0.5f,
		        "DEVELOPER CONSOLE — type help for commands", CRGBA(120, 220, 120, 255), true);
		const size_t maxLines = 11;
		size_t total = s_consoleLog.size();
		size_t start = total > maxLines ? total - maxLines : 0;
		float y = SCREEN_SCALE_Y(88.0f);
		for(size_t i = start; i < total; i++){
			HudText(SCREEN_SCALE_X(34.0f), y, 0.42f, s_consoleLog[i].c_str(), CRGBA(215, 215, 215, 255));
			y += SCREEN_SCALE_Y(16.0f);
		}
		char line[224];
		s_consoleBuf[s_consoleLen] = 0;
		snprintf(line, sizeof line, "> %s_", s_consoleBuf);
		HudText(SCREEN_SCALE_X(34.0f), SCREEN_SCALE_Y(276.0f), 0.45f, line, CRGBA(255, 230, 120, 255));
	}

	// server admin panel (F6)
	if(s_panelOpen){
		char line[112];
		float y = SCREEN_SCALE_Y(120.0f);
		HudText(SCREEN_SCALE_X(60.0f), y, 0.55f, "SERVER ADMIN PANEL (F6)", CRGBA(255, 210, 80, 255));
		y += SCREEN_SCALE_Y(15.0f);
		for(int row = 0; row < PR_COUNT; row++){
			const char* label = ""; char value[40] = "";
			switch(row){
			case PR_BROADCAST: label = "Broadcast message"; break;
			case PR_NOCLIP:    label = "NoClip"; snprintf(value, sizeof value, "%s", s_noclip ? "ON" : "OFF"); break;
			case PR_GOD:       label = "Godmode"; snprintf(value, sizeof value, "%s", s_god ? "ON" : "OFF"); break;
			case PR_MONEY:     label = "Give $10000"; break;
			case PR_WEAPON:    label = "Give weapon"; snprintf(value, sizeof value, "%s", kGiveWeaponNames[s_panelWeapon]); break;
			case PR_SPAWNBOT:  label = "Spawn NPC bot"; break;
			case PR_SPAWNPROP: label = "Spawn prop here"; break;
			case PR_FPS:       label = "FPS limit"; snprintf(value, sizeof value, "%s", kFpsNames[s_panelFpsIdx]); break;
			}
			snprintf(line, sizeof line, "%s %-18s %s", row == s_panelRow ? ">" : " ", label, value);
			HudText(SCREEN_SCALE_X(60.0f), y, 0.45f, line,
			        row == s_panelRow ? CRGBA(255, 255, 255, 255) : CRGBA(170, 170, 170, 255));
			y += SCREEN_SCALE_Y(12.0f);
		}
		snprintf(line, sizeof line, "rcon: %s", s_rconPw.empty() ? "not configured (-rcon pw)" : "ok");
		HudText(SCREEN_SCALE_X(60.0f), y, 0.4f, line, CRGBA(140, 140, 255, 255));
		if(s_lastRcon[0]){
			y += SCREEN_SCALE_Y(11.0f);
			snprintf(line, sizeof line, "last: %s", s_lastRcon);
			HudText(SCREEN_SCALE_X(60.0f), y, 0.4f, line, CRGBA(140, 140, 255, 255));
		}
	}
	} // !CefHud_Active()

	// chromium overlay on top
	CefHud_Draw();
}

void NetGame_Shutdown(void)
{
	CefHud_Shutdown();
	if(s_connected){
		s_net.Disconnect();
		s_connected = false;
	}
	for(auto it = s_remotes.begin(); it != s_remotes.end(); ){
		uint8_t id = it->first;
		++it;
		RemoveRemote(id);
	}
	ClearAllVehicles();
}

// ---------------------------------------------------------------------------
// UI mouse mode: while a panel wants clicks (F6 admin, console, chat) the OS
// cursor is free and gameplay input ignores the mouse (see Pad.cpp / win.cpp)
// ---------------------------------------------------------------------------
bool NetGame_WantMouse(void)
{
	return (CefHud_Active() && CefHud_Focused()) || s_panelOpen;
}
