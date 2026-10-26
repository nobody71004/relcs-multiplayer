// src/net/nethooks.cpp — game glue: connects NetClient to the reLCS engine.
//
// - connect flow : -connect ip:port / -nick name on the command line
// - local state  : sampled from FindPlayerPed() at 20 Hz
// - remote state : kinematic CCivilianPed entities, interpolated, with
//                  idle/walk/run anims driven by relayed velocity
// - overlay      : chat + players via CFont (from CHud::Draw)
#include "nethooks.h"
#include "netclient.h"
#include "rnet_voice.h"
#include "cefhud.h"

#include "common.h"
#include "Lists.h"
#include "PlayerInfo.h"
#include "Pad.h"
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
#include "AnimBlendAssociation.h"
#include "RpAnimBlend.h"
#include "Font.h"
#include "ModelInfo.h"
#include "Collision.h"
#include "Sprite2d.h"
#include "Timer.h"
#include "Frontend.h"
#include "CutsceneMgr.h"
#include "Clock.h"
#include "Weather.h"
#include "Sprite.h"
#include "skeleton.h"
#include "Pickups.h"
#include "Vehicle.h"
#include "Automobile.h"
#include "Camera.h"
#include "VisibilityPlugins.h"
#include "Bike.h"
#include "Boat.h"
#include "CarCtrl.h"
#include "RoadBlocks.h"
#include "Darkel.h"
#include "Script.h"
#include "WeaponInfo.h"
#include "Game.h"

#include <windows.h>
#include <cstdarg>
#include <deque>
#include <mutex>
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
static std::string s_pass;   // claim/restore the server-side profile
static uint16_t s_port = 7777;

static double NowSec()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// puppet -> RenderWare frame sync
//
// Puppets are built with bUsesCollision=false so they can never shove real
// players around (that is what tripped anti-cheat before). The side effect:
// the engine only pushes a moving entity's CMatrix into its RenderWare frame
// inside CPhysical::ProcessCollision / ProcessShift, and that path early-outs
// for collisionless bodies (leaving bIsInSafePosition = true), so
// CWorld::Process's sync loop skips them too. The clump then keeps whatever
// matrix it had when the mesh was created — rendering at the origin or at a
// stale spot and never moving. That is exactly the "the other player's model
// is invisible / does not move" and "the i8 never shows up" report, so every
// place that moves a puppet also has to push the matrix to RenderWare.
// ---------------------------------------------------------------------------
static void SyncPuppetToRw(CEntity* ent)
{
	if(!ent || !ent->m_rwObject)
		return;
	ent->GetMatrix().UpdateRW();
	ent->UpdateRwFrame();
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
	int32 handle = -1;         // pool handle of veh (slot + reuse counter)
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

// Bind a game vehicle to a net id. The pool handle is kept so a pointer the
// engine has since freed (or reused for another vehicle) is never trusted.
static void BindNetVehicle(uint16_t netId, NetVehicle& nv, CVehicle* v)
{
	nv.veh = v;
	nv.handle = v ? CPools::GetVehicleRef(v) : -1;
	if(v) s_vehByPtr[v] = netId;
}

// The engine can free a net vehicle behind our back: puppets built from the
// join snapshot while the game was still starting were wiped by the new-game
// pool reset, and the slots were reused by the world's trains and later by the
// next car anyone spawned. The stale entries then positioned those objects
// every frame — a fresh i8 was dragged onto a train 1 km away and its seated
// driver vanished with it. Drop a stale binding (never delete through it);
// UpdateNetVehicles rebuilds the puppet on the next frame.
static bool NetVehicleStale(uint16_t netId, NetVehicle& nv)
{
	if(!nv.veh) return false;
	// the object must still be ours alone: a newer binding of the same pointer
	// (slot reused by another net vehicle) wins
	auto own = s_vehByPtr.find(nv.veh);
	bool ok = own != s_vehByPtr.end() && own->second == netId &&
	          CPools::GetVehicle(nv.handle) == nv.veh &&
	          nv.veh->GetModelIndex() == (int32)nv.modelId &&
	          (!nv.puppet || (nv.veh->VehicleCreatedBy == MISSION_VEHICLE && !nv.veh->IsTrain()));
	if(ok) return false;
	fprintf(stderr, "[VEHSYNC] net vehicle %d lost its game object (%p) — rebuilding\n",
	        (int)netId, (void*)nv.veh);
	fflush(stderr);
	auto pit = s_vehByPtr.find(nv.veh);
	if(pit != s_vehByPtr.end() && pit->second == netId)
		s_vehByPtr.erase(pit);
	nv.veh = nil;
	nv.handle = -1;
	nv.puppet = false;
	return true;
}

static uint16_t NetIdOfVehicle(CVehicle* v)
{
	auto it = s_vehByPtr.find(v);
	if(it == s_vehByPtr.end()) return rnet::VEHICLE_NONE;
	uint16_t id = it->second;
	auto vit = s_vehicles.find(id);
	if(vit == s_vehicles.end() || vit->second.veh != v){
		// left over from a freed vehicle: must never name a new one
		s_vehByPtr.erase(it);
		return rnet::VEHICLE_NONE;
	}
	if(NetVehicleStale(id, vit->second))   // drops the s_vehByPtr entry itself
		return rnet::VEHICLE_NONE;
	return id;
}

static void UnseatNetPed(CPed* ped)
{
	if(!ped) return;
	CVehicle* veh = ped->m_pMyVehicle;
	if(!veh) return;
	fprintf(stderr, "[NET] UNSEAT-NETPED ped=%p veh=%p model=%d state=%d\n",
	        (void*)ped, (void*)veh, veh ? (int)veh->GetModelIndex() : -1,
	        (int)ped->m_nPedState);
	fflush(stderr);
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
	if(ped->m_rwObject)
		ped->RemoveInCarAnims(); // fade the sit pose out; on-foot anims take over
}

// a seated puppet is static (CWorld::Add, not on the moving-entity list), so
// the world loop never re-anchors it. Put it back in its seat from the car's
// current matrix: called on seat and every frame while the car moves.
static void AnchorSeatedPuppet(CPed* ped)
{
	CVehicle* veh = ped->m_pMyVehicle;
	if(!veh) return;
	// trains seat through their own node alignment — the generic car path
	// assumes car doors/seats that a train clump does not have
	if(veh->IsTrain())
		ped->SetPedPositionInTrain();
	else
		ped->SetPedPositionInCar();
	// a static entity is never re-sectored by the world loop (only
	// ProcessCollision calls RemoveAndAdd), so the puppet stayed listed in the
	// sector where it got in: once the car left the renderer's scan range the
	// player was not drawn at all. Re-sector it with the car.
	ped->RemoveAndAdd();
	SyncPuppetToRw(ped);
}

// the engine's own in-car pose for this seat and vehicle (low cars, bikes and
// boats each have their own sit anim). Also re-applied whenever a seated
// puppet's clump is rebuilt: a fresh clump has no anims and stands upright.
static void PoseSeatedPuppet(CPed* ped)
{
	CVehicle* veh = ped->m_pMyVehicle;
	if(!veh || !ped->m_rwObject) return;
	RpAnimBlendClumpSetBlendDeltas((RpClump*)ped->m_rwObject, ASSOC_PARTIAL, -1000.0f);
	ped->AddInCarAnims(veh, veh->pDriver == ped);
}

// returns false when the seat is already held by someone else (logged by the caller)
static bool SeatNetPed(CPed* ped, CVehicle* veh, uint8 seat)
{
	if(!ped || !veh) return false;
	if(ped->m_pMyVehicle == veh) return true;
	if(ped->m_pMyVehicle) UnseatNetPed(ped);
	if(seat == 0){
		if(veh->pDriver && veh->pDriver != ped) return false; // seat taken locally
		if(!veh->pDriver){
			veh->pDriver = ped;
			ped->RegisterReference((CEntity**)&veh->pDriver);
		}
	}else{
		int i = (int)seat - 1;
		if(i >= 8) return false;
		if(veh->pPassengers[i] && veh->pPassengers[i] != ped) return false;
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
	AnchorSeatedPuppet(ped);
	// a plain CAR_SIT blended over the on-foot anims left the model standing
	// up through the seat. The frame pass keeps the ped in its seat from here.
	PoseSeatedPuppet(ped);
	return true;
}

static bool ModelIsVehicle(int modelId)
{
	CBaseModelInfo* mi = CModelInfo::GetModelInfo(modelId);
	return mi != nil && mi->GetModelType() == MITYPE_VEHICLE;
}

// The world is ±2000 m (WORLD_MIN/MAX in World.h); coordinates outside it have
// no valid sector. A single corrupt state packet must never move a puppet
// there — that is exactly how a net taxi ended up at y=-10880 and crashed the
// client through CPhysical::RemoveAndAdd's sector recycle. NaN-safe range
// test (all comparisons fail for NaN). The rejection log is power-of-two
// throttled: enough to diagnose a generator, never a log flood.
static bool NetPosSane(float x, float y, float z, const char* tag)
{
	if(x > -1900.0f && x < 1900.0f &&
	   y > -1900.0f && y < 1900.0f &&
	   z > -100.0f && z < 800.0f)
		return true;
	static int s_rejects = 0;
	if((++s_rejects & (s_rejects - 1)) == 0){
		fprintf(stderr, "[NET] rejected out-of-world %s position (%.1f, %.1f, %.1f) #%d\n",
		        tag, x, y, z, s_rejects);
		fflush(stderr);
	}
	return false;
}

// Game trains simulate themselves on their tracks (CTrain::UpdateTrains) and a
// CAutomobile built from a train clump has no car nodes — that pairing is the
// "step in train -> crash" regression. For train models adopt the world's
// existing train (already created by CTrain::InitTrains) instead of ever
// constructing a puppet. Returns nil while no matching train exists yet
// (creation is retried from UpdateNetVehicles).
static CVehicle* FindNetableTrain(uint16_t modelId, float x, float y, float z)
{
	CVehicle* best = nil;
	float bestD = 0.0f;
	for(int i = 0; i < CPools::GetVehiclePool()->GetSize(); i++){
		CVehicle* v = CPools::GetVehiclePool()->GetSlot(i);
		if(!v || !v->IsTrain() || v->GetModelIndex() != (int32)modelId)
			continue;
		CVector d = v->GetPosition() - CVector(x, y, z);
		float dd = d.x * d.x + d.y * d.y + d.z * d.z;
		if(!best || dd < bestD){ best = v; bestD = dd; }
	}
	return best;
}

// "vehdiag" console probe: the render state of a vehicle's clump. A model that
// spawns but never shows up on screen almost always reads as renderOn=0 (atomics
// hidden), trisOn=0 (bad geometry) or nilRaster>0 (TXD missing/unsupported).
struct VehDiagStats {
	int atomics = 0, renderOn = 0, tris = 0, trisOn = 0;
	int mats = 0, textured = 0, nilRaster = 0;
};
static RpAtomic* VehDiagAtomicCB(RpAtomic* atomic, void* data)
{
	VehDiagStats* s = (VehDiagStats*)data;
	s->atomics++;
	uint32 fl = RpAtomicGetFlags(atomic);
	bool on = (fl & rpATOMICRENDER) != 0;
	if(on) s->renderOn++;
	RpGeometry* g = RpAtomicGetGeometry(atomic);
	if(g){
		int32 t = RpGeometryGetNumTriangles(g);
		s->tris += t;
		if(on) s->trisOn += t;
		int32 nm = g->matList.numMaterials;
		s->mats += nm;
		for(int32 i = 0; i < nm; i++){
			RpMaterial* m = g->matList.materials[i];
			RwTexture* tex = m ? RpMaterialGetTexture(m) : nil;
			if(tex){
				s->textured++;
				if(!RwTextureGetRaster(tex)) s->nilRaster++;
			}
		}
	}
	return atomic;
}

static CVehicle* CreatePuppetVehicle(uint16_t modelId, float x, float y, float z, float heading)
{
	if(!ModelIsVehicle(modelId))
		return nil; // callers mark the net vehicle rejected (logged once)
	if(CModelInfo::IsTrainModel(modelId))
		return FindNetableTrain(modelId, x, y, z); // never a constructed puppet
	if(!NetPosSane(x, y, z, "vehicle spawn")){
		// fall back to the local player: a spawn near us always streams in, and
		// the first sane state update snaps the puppet to its real position
		CVector me = FindPlayerCoors();
		x = me.x; y = me.y; z = me.z;
	}
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
	SyncPuppetToRw(veh);
	if(!veh->m_rwObject)
		fprintf(stderr, "[NET] WARNING: net vehicle model %d has no rw object\n", (int)modelId);
	else{
		// render state of the new puppet: a model that is "created" but never
		// drawn shows renderOn=0 (atomics hidden), trisOn=0 (no geometry) or
		// nilRaster>0 (texture missing) here
		VehDiagStats ds;
		RpClumpForAllAtomics((RpClump*)veh->m_rwObject, VehDiagAtomicCB, &ds);
		CVector pp = veh->GetPosition();
		CVector me = FindPlayerCoors();
		fprintf(stderr, "[NET] puppet diag model=%d atomics=%d renderOn=%d tris=%d trisOn=%d mats=%d textured=%d nilRaster=%d dist=%.1f z=%.2f groundZ=%.2f\n",
		        (int)modelId, ds.atomics, ds.renderOn, ds.tris, ds.trisOn, ds.mats, ds.textured, ds.nilRaster,
		        std::sqrt((pp.x - me.x) * (pp.x - me.x) + (pp.y - me.y) * (pp.y - me.y) + (pp.z - me.z) * (pp.z - me.z)),
		        pp.z, CWorld::FindGroundZForCoord(pp.x, pp.y));
	}
	fprintf(stderr, "[NET] net vehicle model=%d created at (%.1f, %.1f, %.1f)\n",
	        (int)modelId, x, y, z);
	fflush(stderr);
	return veh;
}

static void ConsolePrint(const char* fmt, ...);

static NetVehicle& NetVehicleAt(uint16_t netId, uint16_t modelId,
                                float x, float y, float z, float heading)
{
	if(!NetPosSane(x, y, z, "vehicle")){
		CVector me = FindPlayerCoors();
		x = me.x; y = me.y; z = me.z; // keep smooth* latch in-world too
	}
	NetVehicle& nv = s_vehicles[netId];
	NetVehicleStale(netId, nv);
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
			// say so in-game too: otherwise the other player just "has no car"
			ConsolePrint("net vehicle %d uses model %d, which this install does not have - it stays invisible here",
			             (int)netId, (int)nv.modelId);
		}
		if(!nv.rejected){
			BindNetVehicle(netId, nv, CreatePuppetVehicle(nv.modelId, nv.smoothX, nv.smoothY, nv.smoothZ, nv.smoothH));
			// an adopted game train is owned by the game — only constructed
			// puppets may be deleted by RemoveNetVehicle
			nv.puppet = nv.veh != nil && nv.veh->VehicleCreatedBy == MISSION_VEHICLE;
		}
	}
	return nv;
}

static void AdoptNetVehicle(uint16_t netId, CVehicle* veh, uint16_t modelId)
{
	NetVehicle& nv = s_vehicles[netId];
	nv.modelId = modelId;
	nv.puppet = false;
	BindNetVehicle(netId, nv, veh);
	CVector p = veh->GetPosition();
	nv.smoothX = p.x; nv.smoothY = p.y; nv.smoothZ = p.z;
	nv.smoothH = std::atan2(-veh->GetForward().x, veh->GetForward().y);
	// our car is a net entity now: keep the population from reclaiming it
	veh->VehicleCreatedBy = MISSION_VEHICLE;
}

// cleanly detach the local player from any vehicle. Deleting a car under
// the player flags them for deferred world-removal, and a dangling
// PED_DRIVING state freezes the game: the character model is hidden (it
// renders inside cars) and input steers a car that no longer exists.
static void UnseatPlayerForWorld(CPlayerPed* me)
{
	if(!me) return;
	fprintf(stderr, "[NET] UNSEAT-FOR-WORLD veh=%p model=%d state=%d\n",
	        (void*)me->m_pMyVehicle,
	        me->m_pMyVehicle ? (int)me->m_pMyVehicle->GetModelIndex() : -1,
	        (int)me->m_nPedState);
	fflush(stderr);
	if(me->EnteringCar())
		me->QuitEnteringCar();
	if(me->m_pMyVehicle){
		CVehicle* v = me->m_pMyVehicle;
		if(v->pDriver == me) v->pDriver = nil;
		else v->RemovePassenger(me);
		me->m_pMyVehicle = nil;
	}
	me->bInVehicle = false;
	if(me->Driving() || me->EnteringCar())
		me->SetPedState(PED_IDLE);
	me->bRemoveFromWorld = false;
}

static void RemoveNetVehicle(uint16_t netId)
{
	fprintf(stderr, "[NET] REMOVE-NETVEHICLE id=%d\n", (int)netId);
	fflush(stderr);
	auto it = s_vehicles.find(netId);
	if(it == s_vehicles.end()) return;
	NetVehicle& nv = it->second;
	NetVehicleStale(netId, nv);   // never delete through a freed pointer
	if(nv.veh){
		s_vehByPtr.erase(nv.veh);
		if(nv.puppet){
			// ~CVehicle flags its occupants for deferred removal — never let
			// that land on the local player
			CPlayerPed* me = FindPlayerPed();
			if(me && me->m_pMyVehicle == nv.veh)
				UnseatPlayerForWorld(me);
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

// Door sync: the driver streams each car door's open ratio and a remote puppet
// applies it straight to the door frame. This is the pose math of
// CAutomobile::OpenDoor without its sounds and body hide/show side effects.
static void ApplyNetDoors(CAutomobile* car, const uint8_t* ratios)
{
	// eDoors order: bonnet, boot, front left, front right, rear left, rear right
	static const int32 s_comps[6] = { CAR_BONNET, CAR_BOOT, CAR_DOOR_LF, CAR_DOOR_RF, CAR_DOOR_LR, CAR_DOOR_RR };
	for(int i = 0; i < 6; i++){
		RwFrame* node = car->m_aCarNodes[s_comps[i]];
		if(!node) continue; // this model has no such door
		CDoor& door = car->Doors[i];
		door.Open(ratios[i] / 255.0f);
		float axes[3] = { 0.0f, 0.0f, 0.0f };
		axes[door.m_nAxis] = door.m_fAngle;
		CMatrix mat(RwFrameGetMatrix(node));
		CVector pos = mat.GetPosition();
		mat.SetRotate(axes[0], axes[1], axes[2]);
		mat.Translate(pos);
		mat.UpdateRW();
	}
}

// per-frame: puppet creation retries, clump rebuild, interpolation, and
// freeze/unfreeze based on who drives
static void UpdateNetVehicles(float dt)
{
	// render-state probe for remote-driven cars, every 2 s: where the puppet
	// is, how far from the camera, its fade alpha and its driver's state —
	// enough to tell "not drawn" apart from "drawn somewhere else"
	static double s_lastVehDiag = 0.0;
	bool diag = NowSec() - s_lastVehDiag >= 2.0;
	if(diag) s_lastVehDiag = NowSec();
	for(auto& kv : s_vehicles){
		NetVehicleStale(kv.first, kv.second);
		if(diag && kv.second.veh && kv.first != s_myVehNet){
			CVehicle* v = kv.second.veh;
			CVector p = v->GetPosition();
			CVector c = TheCamera.GetPosition();
			float cd = (p - c).Magnitude();
			int alpha = -1;
#ifdef VIS_DISTANCE_ALPHA
			if(v->m_rwObject) alpha = CVisibilityPlugins::GetObjectDistanceAlpha(v->m_rwObject);
#endif
			fprintf(stderr, "[VEHDIAG] veh=%d model=%d puppet=%d pos=(%.1f,%.1f,%.1f) camDist=%.1f rw=%d visible=%d alpha=%d fade=%d static=%d driver=%p drvState=%d\n",
			        (int)kv.first, (int)kv.second.modelId, (int)kv.second.puppet, p.x, p.y, p.z, cd,
			        (int)(v->m_rwObject != nil), (int)v->bIsVisible, alpha, (int)v->bDistanceFade,
			        (int)v->GetIsStatic(), (void*)v->pDriver, v->pDriver ? (int)v->pDriver->m_nPedState : -1);
		}
	}
	for(auto& kv : s_vehicles){
		NetVehicle& nv = kv.second;
		if(!nv.veh){
			if(nv.rejected) continue;
			BindNetVehicle(kv.first, nv, CreatePuppetVehicle(nv.modelId, nv.smoothX, nv.smoothY, nv.smoothZ, nv.smoothH));
			if(!nv.veh) continue;
			nv.puppet = nv.veh->VehicleCreatedBy == MISSION_VEHICLE; // trains adopted
		}
		CVehicle* veh = nv.veh;
		// belt & braces: streaming eviction kills the clump — rebuild it
		if(!veh->m_rwObject){
			CStreaming::RequestModel(nv.modelId, 0);
			CStreaming::LoadAllRequestedModels(false);
			veh->SetModelIndex(nv.modelId);
			SyncPuppetToRw(veh);
		}
		if(kv.first == s_myVehNet && s_mySeat == 0){
			// we drive: the local simulation is authoritative — real physics
			veh->bIsFrozen = false;
			veh->bUsesCollision = true;
			// keep the kinematic latch on the live car: the moment we step out
			// it is glued to smooth*/target, which otherwise still hold where
			// we got in (the car snapped back there on exit)
			CVector p = veh->GetPosition();
			nv.smoothX = p.x; nv.smoothY = p.y; nv.smoothZ = p.z;
			nv.smoothH = std::atan2(-veh->GetForward().x, veh->GetForward().y);
			nv.target.xMm = rnet::QuantPos(p.x);
			nv.target.yMm = rnet::QuantPos(p.y);
			nv.target.zMm = rnet::QuantPos(p.z);
			nv.target.heading = rnet::QuantAngle(nv.smoothH);
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
		// CAutomobile::ProcessControl marks a frozen car bIsInSafePosition, so
		// the world loop skips its ProcessCollision — the only place a
		// collisionless car is re-sectored. The puppet then stayed listed in its
		// spawn sector: once it drove off, the renderer never found it, and its
		// seated driver (drawn as part of the car) vanished with it.
		veh->RemoveAndAdd();
		SyncPuppetToRw(veh);
		if(veh->IsCar() && nv.hasTarget)
			ApplyNetDoors((CAutomobile*)veh, nv.target.doors);
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
	int  model = MI_MALE01;   // the ped model this remote should render as (appearance sync)
	float seatMismatch = 0.0f; // seconds the relayed seat has disagreed with the puppet
};
static std::map<uint8_t, RemotePed> s_remotes;

// A player's chosen look may arrive (MSG_APPEARANCE) before the ped exists, or
// be changed mid-session. Remember the model they asked for -- the RAW id, not
// the resolved one -- so a custom ped that only streams in later still wins:
// resolving at store time would freeze the fallback in place forever.
static std::map<uint8_t, uint16_t> s_remoteModel;

// Only ped models are valid avatars; a vehicle/other model would crash the ped
// clump build. Fall back to the default civilian if the id is not a ped model.
static int ResolvePedModel(uint16_t model)
{
	if(model == 0) return MI_MALE01;
	CBaseModelInfo* mi = CModelInfo::GetModelInfo((int)model);
	if(mi && mi->GetModelType() == MITYPE_PED)
		return (int)model;
	return MI_MALE01;
}

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
	// note: s_remoteModel is intentionally NOT erased here — the clump is often
	// torn down and rebuilt (streaming eviction, re-skin) while the player stays,
	// and keeping the last known look lets SpawnRemote restore it. It is cleared
	// on EV_REMOTE_QUIT instead.
}

static void SpawnRemote(uint8_t id, float x, float y, float z, float heading)
{
	if(!s_connected)
		return;
	if(!NetPosSane(x, y, z, "ped spawn"))
		return; // ApplyRemote retries on the next sane state
	// the look the server told us this player uses (default until MSG_APPEARANCE)
	int model = MI_MALE01;
	{
		auto mit = s_remoteModel.find(id);
		if(mit != s_remoteModel.end()) model = ResolvePedModel(mit->second);
	}
	// RemoveRemote drops the pending seat; keep it so the frame pass can seat
	// the ped once it (and its car) exist, whichever arrives last
	auto pit = s_pendingSeat.find(id);
	bool hadSeat = pit != s_pendingSeat.end();
	auto pendingSeat = hadSeat ? pit->second : std::make_pair((uint16_t)rnet::VEHICLE_NONE, (uint8_t)0xFF);
	RemoveRemote(id);
	if(hadSeat) s_pendingSeat[id] = pendingSeat;
	CStreaming::RequestModel(model, STREAMFLAGS_KEEP_IN_MEMORY);
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

	CCivilianPed* ped = new CCivilianPed(PEDTYPE_CIVMALE, model);
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
	SyncPuppetToRw(ped);
	if(!ped->m_rwObject)
		fprintf(stderr, "[NET] WARNING: remote ped id=%d has no rw object (model %d not loaded?)\n",
		        (int)id, (int)MI_MALE01);

	RemotePed& rp = s_remotes[id];
	rp.ped = ped;
	rp.smoothX = x; rp.smoothY = y; rp.smoothZ = z;
	rp.init = true;
	rp.model = model;
	fprintf(stderr, "[NET] remote ped id=%d created at (%.1f, %.1f, %.1f) model=%d\n",
	        (int)id, x, y, z, model);
	fflush(stderr);
}

// A player's look arrived/changed (MSG_APPEARANCE). Remember the model for the
// id, and if the ped already exists re-skin it in place: SetModelIndex rebuilds
// the RenderWare clump, so the puppet instantly renders as the new character.
// (LCS renders a ped by model index; the component slots are carried for mods.)
static void ApplyRemoteAppearance(uint8_t id, const rnet::MsgAppearance& ap)
{
	int model = ResolvePedModel(ap.model);
	s_remoteModel[id] = ap.model;   // raw: ResolvePedModel runs again once it loads
	auto it = s_remotes.find(id);
	if(it == s_remotes.end() || !it->second.ped){
		// ped not built yet: SpawnRemote will pick up the stored model
		return;
	}
	RemotePed& rp = it->second;
	if(rp.model == model)
		return;   // already wearing this look
	CStreaming::RequestModel(model, STREAMFLAGS_KEEP_IN_MEMORY);
	CStreaming::LoadAllRequestedModels(false);
	rp.ped->SetModelIndex(model);   // rebuilds the clump
	SyncPuppetToRw(rp.ped);
	rp.model = model;
	rp.lastAnim = -1;               // re-blend the idle/walk anim on the new clump
	PoseSeatedPuppet(rp.ped);       // a seated look change keeps sitting
	fprintf(stderr, "[NET] remote ped id=%d re-skinned to model %d\n", (int)id, model);
	fflush(stderr);
}

static void ApplyRemote(uint8_t id, const rnet::PlayerState& st, float dt)
{
	float tx = st.xMm / 1000.0f, ty = st.yMm / 1000.0f, tz = st.zMm / 1000.0f;
	// never move or materialize a puppet from an out-of-world position
	if(!NetPosSane(tx, ty, tz, "ped"))
		return;
	RemotePed& rp = s_remotes[id];
	if(!rp.ped || !rp.init){
		SpawnRemote(id, tx, ty, tz, rnet::DequantAngle(st.heading));
		return;
	}
	CPed* ped = rp.ped;

	// belt & braces: if streaming ever evicted the model the clump dies and
	// the puppet silently renders as nothing (nametag floats "in the air") —
	// rebuild it in place
	if(!ped->m_rwObject){
		CStreaming::RequestModel(rp.model, STREAMFLAGS_KEEP_IN_MEMORY);
		CStreaming::LoadAllRequestedModels(false);
		ped->SetModelIndex(rp.model);
		SyncPuppetToRw(ped);
		rp.lastAnim = -1;
		PoseSeatedPuppet(ped);   // no-op on foot
		fprintf(stderr, "[NET] remote ped id=%d clump rebuilt (model %d)\n", (int)id, rp.model);
		fflush(stderr);
	}

	// seat reconciliation: every relayed state carries the server's occupancy
	// for this player. The reliable enter/exit events normally get there first;
	// when one was missed or raced (late join, ped built after the enter), a
	// disagreement that outlives any in-flight event is repaired from the state.
	{
		bool wantSeat = st.vehicleId != rnet::VEHICLE_NONE && st.vehicleId != rnet::VEHICLE_UNSYNCED;
		bool seated = ped->bInVehicle && ped->m_pMyVehicle;
		bool agree = wantSeat ? (seated && NetIdOfVehicle(ped->m_pMyVehicle) == st.vehicleId)
		                      : !seated;
		if(agree || s_pendingSeat.count(id))
			rp.seatMismatch = 0.0f;
		else if((rp.seatMismatch += dt) > 0.75f){
			rp.seatMismatch = 0.0f;
			fprintf(stderr, "[VEHSYNC] remote player %d seat repaired from state: veh=%d seat=%d (was %s)\n",
			        (int)id, (int)st.vehicleId, (int)st.seat, seated ? "seated" : "on foot");
			fflush(stderr);
			if(wantSeat)
				s_pendingSeat[id] = std::make_pair(st.vehicleId, st.seat); // frame pass seats it
			else
				UnseatNetPed(ped);
		}
	}

	// seated in a net vehicle: the world loop holds the ped in its seat
	// (SetPedPositionInCar) — don't fight it with on-foot interpolation
	if(ped->bInVehicle && ped->m_pMyVehicle){
		CVector p = ped->GetPosition();
		rp.smoothX = p.x; rp.smoothY = p.y; rp.smoothZ = p.z;
		rp.lastAnim = -1; // re-blend the on-foot anim once it steps out
		return;
	}

	// interpolate toward the relayed position
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
	ped->RemoveAndAdd();   // keep the world sector current (see AnchorSeatedPuppet)
	SyncPuppetToRw(ped);   // collisionless puppets are skipped by the engine's sync

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
static bool s_rconAuthed = false;   // the server accepted our rcon password
static bool s_god = false;
static bool s_noclip = false;
static CVector s_ncPos(0.0f, 0.0f, 0.0f);
static float s_ncSpeed = 20.0f;   // noclip base speed m/s (nocspeed cmd, [ ] adjust)
static double s_welcomeUntil = 0.0;
static std::string s_bannerText;   // server broadcast / round announcement
static bool s_voiceMuted = false;  // voicemute key: our mic is not sent
static double s_bannerUntil = 0.0;
static int s_worldHour = 12, s_worldMinute = 0;   // server-synced world clock
static int s_worldWeather = -1;
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

// GetAsyncKeyState is global: it reports keys typed into ANY window. Without
// this gate a second client on the same PC ran every console command typed into
// the first one ("ride 295" spawned a duplicate car and warped that client's
// player into it). The foreground window counts as ours when it belongs to this
// process, so the CEF overlay/panels (same process) keep working.
static bool GameHasFocus(void)
{
	HWND fg = GetForegroundWindow();
	if(!fg) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

// GetAsyncKeyState held-test, false while another window has focus
static bool KeyDown(int vk)
{
	return GameHasFocus() && (GetAsyncKeyState(vk) & 0x8000) != 0;
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
		bool focused = GameHasFocus();
		for(int k = 0; k < 256; k++){
			bool now = (GetAsyncKeyState(k) & 0x8000) != 0;
			// unfocused: track the key state but never report an edge, so a key
			// still held when focus returns does not fire either
			cur[k] = focused && now && !prev[k];
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
	int voicemute = VK_F9;   // proximity voice mic on/off
	int scoreboard = VK_TAB; // hold: player list + ping/score board
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
			      "chat=T\nadmin=F6\ninventory=I\nconsole=F7\nconsole2=GRAVE\nvoicemute=F9\nscoreboard=TAB\n", f);
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
		else if(!_stricmp(key, "voicemute")) s_keys.voicemute = vk;
		else if(!_stricmp(key, "scoreboard")) s_keys.scoreboard = vk;
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

// door-chime (implemented below, near NetGame_Frame)
static void PlayDoorChime(void);
static void StopDoorChime(void);
static void PlayRemoteDoorChime(float x, float y, float z);
static bool s_doorChime = true;

// --- appearance sync: the local player's chosen look (model + components) ----
// Default = MI_MALE01. The player customizes via the `model` console command;
// the server restores a saved look on join (EV_APPEARANCE for our own id).
static rnet::MsgAppearance s_myAppearance;
static bool   s_appearanceDirty = false;   // needs sending to the server
static bool   s_appearanceServerKnown = false; // the server has echoed our look at least once

// queue our look for sending (console command / server restore -> re-send)
static void ApplyLocalAppearance(bool force);

static bool   s_creatorFirst = false;  // opened because this player has no saved look

static void SetMyAppearance(uint16_t model, const uint8_t* comps /*8 or null*/)
{
	// Ignore a no-op: the overlay re-sends the current selection whenever it
	// reloads (every lcs:// request navigates it), and a redundant forced
	// re-skin rebuilds the ped clump — visible flicker plus streaming churn.
	uint16_t m = model != 0 ? model : s_myAppearance.model;
	uint8_t c[rnet::MAX_APPEARANCE_COMPONENTS];
	for(int i = 0; i < rnet::MAX_APPEARANCE_COMPONENTS; i++)
		c[i] = comps ? comps[i] : s_myAppearance.components[i];
	if(m == s_myAppearance.model &&
	   memcmp(c, s_myAppearance.components, sizeof c) == 0){
		s_myAppearance.playerId = s_net.MyId();
		if(comps) s_creatorFirst = false;   // they already have this look
		return;
	}
	s_myAppearance.playerId = s_net.MyId();
	s_myAppearance.model = m;
	memcpy(s_myAppearance.components, c, sizeof c);
	if(m != MI_MALE01 && ResolvePedModel(m) == MI_MALE01)
		ConsolePrint("model %d is not a ped model — open `creator` for the valid list", (int)m);
	s_appearanceDirty = true;
	s_creatorFirst = false;              // a picked character ends "first join"
	ApplyLocalAppearance(true);
}

// Put our own chosen look on our own body. LCS builds the local player ped as
// MI_PLAYER, which is why a chosen model used to show up only on other clients'
// screens: the sender never saw his own character. Re-skinning the local ped
// keeps what we see and what everyone else sees identical. A custom ped that is
// not resident yet keeps the current body instead of snapping to the default.
static bool   s_creatorShown = false;   // first-join creator opened this session
static bool   s_creatorWanted = false;  // waiting for the CEF overlay to come up
static double s_localAppearanceRetry = 0.0;
static void ApplyLocalAppearance(bool force)
{
	CPlayerPed* me = FindPlayerPed();
	if(!me || !s_connected || s_myAppearance.model == 0)
		return;
	int model = ResolvePedModel(s_myAppearance.model);
	if(model == MI_MALE01 && s_myAppearance.model != MI_MALE01)
		return;                       // custom ped not streamed yet - try later
	// already wearing it (and the clump is intact): nothing to rebuild
	if(me->GetModelIndex() == model && me->m_rwObject)
		return;
	if(!force){
		double now = NowSec();
		if(now - s_localAppearanceRetry < 1.0) return;
		s_localAppearanceRetry = now;
	}
	CStreaming::RequestModel(model, STREAMFLAGS_KEEP_IN_MEMORY);
	CStreaming::LoadAllRequestedModels(false);
	me->SetModelIndex(model);
	SyncPuppetToRw(me);
	fprintf(stderr, "[NET] local ped re-skinned to model %d\n", model);
	fflush(stderr);
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
		ConsolePrint("commands: help say broadcast(bc) give spawnbot spawnprop teleport(tp) ride model creator");
		ConsolePrint("          fps god noclip nocspeed players worldstats ground status settime setweather res doorchime");
		ConsolePrint("          rcon rconauth(login) keybinds clear quit");
	}else if(is("say")){
		if(rest[0]) s_net.SendChat(rest);
		else ConsolePrint("usage: say <text>");
	}else if(is("broadcast") || is("bc")){
		if(rest[0]) SendRconFmt("broadcast %s", rest);
		else ConsolePrint("usage: broadcast <text>");
	}else if(is("give")){
		// accept both "give money 5000" (self) and "give <id|name> money 5000"
		char first[24] = {};
		sscanf(rest, "%23s", first);
		bool kindFirst = !_stricmp(first, "money") || !_stricmp(first, "weapon") ||
		                 !_stricmp(first, "god") || !_stricmp(first, "health");
		if(rest[0]){
			if(kindFirst) SendRconFmt("give %d %s", myId, rest);
			else          SendRconFmt("give %s", rest);
		}else
			ConsolePrint("usage: give [id|name] money|weapon|god|health <value> [ammo]");
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
				// the engine's own warp (what the mission scripts use): seats the
				// driver, hands the car to the player and applies the in-car sit
				// anims. Setting the seat fields by hand skipped the anims, so the
				// character stood upright through the seat.
				me->SetObjective(OBJECTIVE_ENTER_CAR_AS_DRIVER, car);
				me->WarpPedIntoCar(car);
				me->RestorePreviousObjective();
				me->SetPedPositionInCar();
				// a stale enter-car objective (left behind by a failed enter
				// attempt) makes the ped AI call SetExitCar the moment the player
				// sits in a car that is not m_carInObjective — "spawn the car and
				// the character gets out". Drop the objective cleanly; for a
				// seated driver ClearObjective is otherwise a no-op.
				me->ClearObjective();
				ConsolePrint("ride: spawned model %d at (%.1f, %.1f, %.1f)", model, p.x, p.y, p.z);
			}
		}
	}else if(is("vehdiag")){
		// render-state probe: the car we sit in, else the nearest net vehicle
		CVehicle* v = nil;
		if(me && me->m_pMyVehicle) v = me->m_pMyVehicle;
		if(!v){
			CVector ref = me ? me->GetPosition() : FindPlayerCoors();
			float bestD = 0.0f;
			for(auto& kv : s_vehicles){
				CVehicle* cand = kv.second.veh;
				if(!cand) continue;
				CVector d = cand->GetPosition() - ref;
				float dd = d.MagnitudeSqr();
				if(!v || dd < bestD){ v = cand; bestD = dd; }
			}
		}
		if(!v)
			ConsolePrint("vehdiag: no vehicle around");
		else if(!v->m_rwObject)
			ConsolePrint("vehdiag model=%d has NO rw object", v->GetModelIndex());
		else if(RwObjectGetType(v->m_rwObject) != rpCLUMP)
			ConsolePrint("vehdiag model=%d rw object is not a clump", v->GetModelIndex());
		else{
			VehDiagStats st;
			RpClumpForAllAtomics((RpClump*)v->m_rwObject, VehDiagAtomicCB, &st);
			CVector p = v->GetPosition();
			ConsolePrint("vehdiag model=%d pos=(%.1f,%.1f,%.1f) atomics=%d renderOn=%d tris=%d trisOn=%d mats=%d textured=%d nilRaster=%d",
			             v->GetModelIndex(), p.x, p.y, p.z, st.atomics, st.renderOn,
			             st.tris, st.trisOn, st.mats, st.textured, st.nilRaster);
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
		ConsolePrint("noclip %s (speed %.0f m/s — nocspeed <n>, hold Ctrl=x4 Alt=x0.25, [ / ] adjust)",
		             s_noclip ? "ON" : "OFF", s_ncSpeed);
	}else if(is("nocspeed")){
		float v = 0.0f;
		if(sscanf(rest, "%f", &v) == 1 && v >= 1.0f && v <= 500.0f){
			s_ncSpeed = v;
			ConsolePrint("noclip speed -> %.0f m/s", s_ncSpeed);
		}else
			ConsolePrint("usage: nocspeed <1-500 m/s> (current %.0f)", s_ncSpeed);
	}else if(is("worldstats")){
		// read-only diagnostic: proves the multiplayer world rules are live
		int pedTotal = 0, pedRandom = 0, vehTotal = 0, vehRandom = 0, vehNet = 0;
		auto* pp = CPools::GetPedPool();
		for(int i = pp->GetSize()-1; i >= 0; i--){
			CPed* p = pp->GetSlot(i);
			if(!p) continue;
			pedTotal++;
			if(p->CharCreatedBy == RANDOM_CHAR && !p->IsPlayer()) pedRandom++;
		}
		auto* vp = CPools::GetVehiclePool();
		for(int i = vp->GetSize()-1; i >= 0; i--){
			CVehicle* v = vp->GetSlot(i);
			if(!v) continue;
			vehTotal++;
			if(v->VehicleCreatedBy == RANDOM_VEHICLE) vehRandom++;
			if(NetIdOfVehicle(v) != rnet::VEHICLE_NONE) vehNet++;
		}
		int activeScripts = 0, missionScripts = 0;
		for(CRunningScript* s = CTheScripts::pActiveScripts; s; s = s->next){
			activeScripts++;
			if(s->m_bIsMissionScript) missionScripts++;
		}
		int pickupActive = 0;
		for(int i = 0; i < NUMPICKUPS; i++)
			if(CPickups::aPickUps[i].m_eType != PICKUP_NONE) pickupActive++;
		CVector pos = me ? me->GetPosition() : CVector(0, 0, 0);
		ConsolePrint("worldstats pos=(%.0f,%.0f,%.0f) level=%d money=$%d pickups=%d time=%02d:%02d weather=%d",
		             pos.x, pos.y, pos.z, (int)CGame::currLevel,
		             CWorld::Players[CWorld::PlayerInFocus].m_nMoney, pickupActive,
		             s_worldHour, s_worldMinute, s_worldWeather);
		ConsolePrint("worldstats ped: state=%d inveh=%d removed=%d",
		             me ? (int)me->GetPedState() : -1,
		             me && me->bInVehicle ? 1 : 0,
		             me && me->bRemoveFromWorld ? 1 : 0);
		ConsolePrint("worldstats vis: model=%d rw=%d hp=%.0f cutscene=%d menu=%d",
		             me ? me->GetModelIndex() : -1,
		             me && me->m_rwObject ? 1 : 0,
		             me ? (double)me->m_fHealth : 0.0,
		             CCutsceneMgr::IsCutsceneProcessing() ? 1 : 0,
		             FrontEndMenuManager.m_bMenuActive ? 1 : 0);
		ConsolePrint("worldstats peds: total=%d world-npc=%d | cars: total=%d traffic=%d net=%d",
		             pedTotal, pedRandom, vehTotal, vehRandom, vehNet);
		ConsolePrint("worldstats scripts: active=%d mission=%d frenzy=%s rules=%s paused=%d",
		             activeScripts, missionScripts,
		             CDarkel::FrenzyOnGoing() ? "ON" : "off",
		             NetGame_RulesActive() ? "ON" : "off",
		             CTimer::GetIsPaused() ? 1 : 0);
	}else if(is("ground")){
		// collision probe: reports the ground Z under a point — a valid Z on a
		// far island proves that island's collision is loaded and collidable
		float x = 0, y = 0, z = 100.0f;
		if(sscanf(rest, "%f %f %f", &x, &y, &z) >= 2){
			bool found = false;
			float gz = CWorld::FindGroundZFor3DCoord(x, y, z, &found);
			ConsolePrint("ground (%.0f,%.0f,%.0f) -> %s z=%.1f", x, y, z,
			             found ? "HIT" : "MISS", gz);
		}else ConsolePrint("usage: ground <x> <y> [z]");
	}else if(is("players")){
		ConsolePrint("you (#%d) %s", myId, s_nick.c_str());
		for(auto& kv : s_net.remotes)
			ConsolePrint("#%d %s", (int)kv.first, kv.second.name.c_str());
	}else if(is("status")){
		SendRconFmt("status");
	}else if(is("settime") || is("setweather") || is("res") || is("resources")){
		// server-side commands forwarded verbatim (admin panel uses these)
		if(rest[0]) SendRconFmt("%s %s", cmd, rest);
		else        SendRconFmt("%s", cmd);
	}else if(is("rconauth") || is("login")){
		// admin panel login: the server checks the password and answers with
		// MSG_RCON_OK (or a "wrong password" rcon response)
		if(rest[0]){
			s_rconPw = rest;
			s_rconAuthed = false;
			s_net.SendRconAuth(rest);
			ConsolePrint("rcon: authenticating...");
		}else ConsolePrint("usage: rconauth <server rcon password>");
	}else if(is("rcon")){
		if(rest[0]) s_net.SendRconCmd(rest);
		else ConsolePrint("usage: rcon <server command>");
	}else if(is("creator") || is("character") || is("customize")){
		// reopen the character creator at any time (first join opens it once)
		CefHud_Show("creator");
		ConsolePrint("character creator opened");
	}else if(is("model") || is("appearance") || is("skin")){
		// customization entry point: choose the character/outfit the other
		// players see you as. "model <pedModelId> [c0 c1 ...]" sets the ped model
		// (and optional clothing/component slots); "model" alone prints the current.
		if(!rest[0]){
			ConsolePrint("your model=%d (components %d %d %d %d %d %d %d %d)",
			             (int)s_myAppearance.model,
			             s_myAppearance.components[0], s_myAppearance.components[1],
			             s_myAppearance.components[2], s_myAppearance.components[3],
			             s_myAppearance.components[4], s_myAppearance.components[5],
			             s_myAppearance.components[6], s_myAppearance.components[7]);
			ConsolePrint("usage: model <pedModelId> [c0..c7]  (e.g. model 9=taxi-driver, 11=criminal, 78=gang)");
		}else{
			int mdl = 0, c[rnet::MAX_APPEARANCE_COMPONENTS] = {};
			int n = sscanf(rest, "%d %d %d %d %d %d %d %d %d",
			               &mdl, &c[0], &c[1], &c[2], &c[3], &c[4], &c[5], &c[6], &c[7]);
			if(n < 1 || mdl <= 0){
				ConsolePrint("usage: model <pedModelId> [c0..c7]");
			}else{
				uint8_t comps[rnet::MAX_APPEARANCE_COMPONENTS];
				for(int i = 0; i < rnet::MAX_APPEARANCE_COMPONENTS; i++)
					comps[i] = (uint8_t)(c[i] & 0xFF);
				SetMyAppearance((uint16_t)mdl, comps);
				ConsolePrint("appearance set to model %d — sending to server", mdl);
			}
		}
	}else if(is("clear")){
		s_consoleLog.clear();
	}else if(is("doorchime")){
		if(rest[0] && !_stricmp(rest, "off")){ s_doorChime = false; StopDoorChime(); ConsolePrint("doorchime off"); }
		else if(rest[0] && !_stricmp(rest, "on")){ s_doorChime = true; ConsolePrint("doorchime on"); }
		else { PlayDoorChime(); ConsolePrint("doorchime: playing (mods/doorchime.wav or SFX/doorchime.wav)"); }
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

// one key=value pair out of a &-separated query string (the overlay sends
// several: model plus the component slots)
static std::string CefArg(const std::string& args, const char* key)
{
	std::string k = std::string(key) + "=";
	size_t pos = 0;
	while(pos <= args.size()){
		if(args.compare(pos, k.size(), k) == 0){
			size_t end = args.find('&', pos);
			if(end == std::string::npos) end = args.size();
			return args.substr(pos + k.size(), end - (pos + k.size()));
		}
		size_t amp = args.find('&', pos);
		if(amp == std::string::npos) break;
		pos = amp + 1;
	}
	return std::string();
}

// CEF query args arrive percent-encoded (JS encodeURIComponent); decode %XX
static std::string UrlDecode(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for(size_t i = 0; i < s.size(); i++){
		if(s[i] == '%' && i + 2 < s.size()){
			auto hex = [](char c) -> int {
				if(c >= '0' && c <= '9') return c - '0';
				if(c >= 'a' && c <= 'f') return c - 'a' + 10;
				if(c >= 'A' && c <= 'F') return c - 'A' + 10;
				return -1;
			};
			int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
			if(hi >= 0 && lo >= 0){
				out += (char)((hi << 4) | lo);
				i += 2;
				continue;
			}
		}
		if(s[i] == '+'){ out += ' '; continue; }
		out += s[i];
	}
	return out;
}

// CEF delivers lcs:// navigations on Chromium's IO/UI thread, never on the
// game's main thread. Applying them there (re-skinning a ped, streaming a
// model, running the console) touches RenderWare and the streaming manager
// from a foreign thread: picking a character froze the whole game. Keep the
// callback trivial and drain the queue from NetGame_Frame instead.
static std::mutex s_cefQueueLock;
static std::deque<std::string> s_cefQueue;

static void ProcessCefRequest(const std::string& req)
{
	size_t q = req.find('?');
	std::string cmd = req.substr(0, q);
	std::string args = q == std::string::npos ? "" : req.substr(q + 1);
	auto arg = [&](const char* key) -> std::string {
		std::string k = std::string(key) + "=";
		if(args.rfind(k, 0) != 0) return "";
		return UrlDecode(args.substr(k.size()));
	};
	if(cmd == "chat"){
		std::string text = arg("text");
		if(!text.empty()) s_net.SendChat(text.c_str());
	}else if(cmd == "model" || cmd == "appearance"){
		// UI customization entry point: ?model=<pedModelId>[&c0=..&c7=..]
		std::string m = CefArg(args, "model");
		if(!m.empty()){
			int mdl = atoi(m.c_str());
			if(mdl > 0){
				uint8_t comps[rnet::MAX_APPEARANCE_COMPONENTS];
				bool haveComps = false;
				for(int i = 0; i < rnet::MAX_APPEARANCE_COMPONENTS; i++){
					char key[8];
					snprintf(key, sizeof key, "c%d", i);
					std::string v = CefArg(args, key);
					comps[i] = v.empty() ? 0 : (uint8_t)atoi(v.c_str());
					if(!v.empty()) haveComps = true;
				}
				fprintf(stderr, "[CEF] appearance request: model=%d comps=%d,%d,%d,%d,%d,%d,%d,%d\n",
				        mdl, comps[0], comps[1], comps[2], comps[3], comps[4], comps[5], comps[6], comps[7]);
				fflush(stderr);
				SetMyAppearance((uint16_t)mdl, haveComps ? comps : nullptr);
			}
		}
	}else if(cmd == "console"){
		std::string c = arg("cmd");
		if(!c.empty()) ConsoleExec(c.c_str());
	}else if(cmd == "close"){
		s_cefPanel.clear();
		CefHud_Show("");
	}else{
		fprintf(stderr, "[CEF] %s\n", req.c_str());
		fflush(stderr);
	}
}

static void CefQueryHandler(const char* request, void*)
{
	if(!request) return;
	std::lock_guard<std::mutex> lock(s_cefQueueLock);
	// a stalled frame must not let the overlay grow the queue without bound
	if(s_cefQueue.size() >= 256) s_cefQueue.pop_front();
	s_cefQueue.push_back(request);
}

static void DrainCefRequests(void)
{
	std::deque<std::string> queued;
	{
		std::lock_guard<std::mutex> lock(s_cefQueueLock);
		queued.swap(s_cefQueue);
	}
	for(std::string& req : queued) ProcessCefRequest(req);
}

static double s_lastCefPush = 0.0;
// scoreboard overlay: true while the scoreboard key is held (see NetGame_PollUi)
static bool s_boardHeld = false;

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
	// both flags at once: the admin panel reads this string to label the
	// Godmode/NoClip buttons, and an if/else chain made them exclusive —
	// turning NoClip on relabelled Godmode as OFF (and vice versa)
	char modeBuf[48];
	if(s_god && s_noclip) snprintf(modeBuf, sizeof modeBuf, "GOD NOCLIP %.0fm/s", s_ncSpeed);
	else if(s_god)        snprintf(modeBuf, sizeof modeBuf, "GOD");
	else if(s_noclip)     snprintf(modeBuf, sizeof modeBuf, "NOCLIP %.0fm/s", s_ncSpeed);
	else modeBuf[0] = 0;
	// player list rows from the server (MSG_PLAYER_PING); names come from joins
	std::string board = "[";
	{
		bool first = true;
		const rnet::MsgPlayerPing& bp = s_net.board;
		for(int i = 0; i < bp.count && i < rnet::MAX_PING_ENTRIES; i++){
			const rnet::PingEntry& e = bp.entries[i];
			bool isMe = e.playerId == s_net.MyId();
			std::string name;
			if(isMe) name = s_net.MyName();
			else{
				auto it = s_net.remotes.find(e.playerId);
				name = it != s_net.remotes.end() ? it->second.name : "Player " + std::to_string((int)e.playerId);
			}
			char row[512];
			snprintf(row, sizeof row,
			         "%s{\"id\":%d,\"name\":\"%s\",\"ping\":%d,\"score\":%d,"
			         "\"kills\":%d,\"deaths\":%d,\"me\":%d}",
			         first ? "" : ",", (int)e.playerId, jesc(name).c_str(), (int)e.pingMs,
			         (int)e.score, (int)e.kills, (int)e.deaths, isMe ? 1 : 0);
			board += row;
			first = false;
		}
	}
	board += "]";

	// 32 KB: 32 board rows plus the chat/console/resource lists and the ped
	// catalog (the creator's model picker) all have to fit
	char buf[32768];
	std::string banner = "\"\"";
	if(NowSec() < s_bannerUntil && !s_bannerText.empty())
		banner = "\"" + jesc(s_bannerText) + "\"";
	char wtime[8];
	snprintf(wtime, sizeof wtime, "%02d:%02d", s_worldHour, s_worldMinute);
	// mod/resource inventory for the admin panel
	std::string res = "[";
	{
		bool first = true;
		const rnet::MsgResList& rl = s_net.resList;
		for(int i = 0; i < rl.count && i < rnet::MAX_RES_ENTRIES; i++){
			const rnet::MsgResEntry& e = rl.entries[i];
			char ent[192];
			snprintf(ent, sizeof ent,
			         "%s{\"name\":\"%s\",\"ver\":\"%s\",\"type\":\"%s\","
			         "\"on\":%d,\"ok\":%d}",
			         first ? "" : ",", jesc(e.name).c_str(), jesc(e.version).c_str(),
			         e.type ? "external" : "internal", (int)e.enabled, (int)e.valid);
			res += ent;
			first = false;
		}
	}
	res += "]";

	// character creator: every ped model the game knows, built once. The overlay
	// filters/searches it and the player's pick is a normal MSG_APPEARANCE.
	static std::string pedJson;
	if(pedJson.empty()){
		pedJson = "[";
		bool firstPed = true;
		int nPeds = 0;
		int total = CModelInfo::GetNumModelInfos();
		for(int i = 1; i < total && i < 20000; i++){
			CBaseModelInfo* mi = CModelInfo::GetModelInfo(i);
			if(!mi || mi->GetModelType() != MITYPE_PED) continue;
			const char* nm = mi->GetModelName();
			if(!nm || !nm[0]) continue;
			char ent[128];
			snprintf(ent, sizeof ent, "%s{\"id\":%d,\"name\":\"%s\"}",
			         firstPed ? "" : ",", i, jesc(nm).c_str());
			pedJson += ent;
			firstPed = false;
			nPeds++;
		}
		pedJson += "]";
		fprintf(stderr, "[NET] ped catalog: %d models for the character creator\n", nPeds);
		fflush(stderr);
	}
	std::string compsJson = "[";
	for(int i = 0; i < rnet::MAX_APPEARANCE_COMPONENTS; i++){
		char e[8];
		snprintf(e, sizeof e, "%s%d", i ? "," : "", (int)s_myAppearance.components[i]);
		compsJson += e;
	}
	compsJson += "]";
	snprintf(buf, sizeof buf,
	         "{\"server\":\"%s\",\"players\":%zu,\"fps\":\"%s\",\"mode\":\"%s\","
	         "\"money\":%d,\"hp\":%d,\"armour\":%d,\"welcome\":%d,\"banner\":%s,"
	         "\"wtime\":\"%s\",\"wweather\":%d,"
	         "\"weapons\":%s,\"chat\":%s,\"console\":%s,\"resources\":%s,"
	         "\"showBoard\":%s,\"board\":%s,"
	         "\"rcon\":%d,\"rconmsg\":\"%s\","
	         "\"creator\":{\"model\":%d,\"comps\":%s,\"first\":%s},\"peds\":%s}",
	         jesc(s_net.Hostname()).c_str(),
	         s_net.remotes.size() + (s_net.Spawned() ? 1 : 0),
	         fpsName, modeBuf,
	         CWorld::Players[CWorld::PlayerInFocus].m_nMoney,
	         me ? (int)me->m_fHealth : 0, me ? (int)me->m_fArmour : 0,
	         (int)(NowSec() < s_welcomeUntil), banner.c_str(),
	         wtime, s_worldWeather,
	         weapons.c_str(), chat.c_str(), con.c_str(), res.c_str(),
	         s_boardHeld ? "true" : "false", board.c_str(),
	         s_rconAuthed ? 2 : (s_rconPw.empty() ? 0 : 1), jesc(s_lastRcon).c_str(),
	         (int)s_myAppearance.model, compsJson.c_str(),
	         s_creatorFirst ? "true" : "false", pedJson.c_str());
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
			if(want){
				// The engine hides the cursor with ShowCursor(FALSE) (and the count
				// can already be below zero), so normalise the display counter
				// instead of a single balanced call — otherwise the arrow never
				// comes back and the panel is unclickable until the window is
				// re-activated.
				while(ShowCursor(TRUE) < 0){}
				// a captured/clipped cursor is the other half of "mouse is dead":
				// a click on the panel used to arm SetCapture in the game's WndProc
				if(GetCapture() == GetActiveWindow()) ReleaseCapture();
				ClipCursor(nullptr);
			}else{
				while(ShowCursor(FALSE) >= 0){}
			}
			SetCursor(LoadCursor(nullptr, want ? IDC_ARROW : IDC_CROSS));
		}
		// re-assert every frame: WM_SETCURSOR from the engine can clear it
		if(want) SetCursor(LoadCursor(nullptr, IDC_ARROW));
	}

	// The engine reads its own pad state, so swallowing keys in this handler is
	// not enough: Enter (console submit) and Space leaked into the game as
	// enter/exit-vehicle and jump, ejecting the driver right after `ride`.
	// While any UI owns the keyboard, disable gameplay pad input.
	{
		static bool s_uiLockedPad = false;
		bool uiOwnsKeys = s_consoleOpen || s_chatOpen || s_invOpen || s_panelOpen || !s_cefPanel.empty();
		// CPad resets DisablePlayerControls in its own Clear() path, so the lock
		// is re-asserted every frame while a UI panel owns the keyboard.
		if(uiOwnsKeys)
			CPad::GetPad(0)->SetDisablePlayerControls(PLAYERCONTROL_UNK8);
		else if(s_uiLockedPad)
			CPad::GetPad(0)->SetEnablePlayerControls(PLAYERCONTROL_UNK8);
		s_uiLockedPad = uiOwnsKeys;
	}

	// scoreboard: the key is passive (no focus change); it is ignored while a
	// UI panel owns the keyboard so typing TAB never flashes the board
	{
		bool anyUi = s_consoleOpen || s_chatOpen || s_invOpen || s_panelOpen || !s_cefPanel.empty();
		bool held = s_net.Connected() && !anyUi && KeyDown(s_keys.scoreboard);
		if(held != s_boardHeld){
			s_boardHeld = held;
			s_lastCefPush = -1e9;   // push the change now instead of within 200 ms
		}
	}

	// poll every handled key first so edge state stays coherent
	bool kAdmin = KeyEdge(s_keys.admin), kInv = KeyEdge(s_keys.inventory),
	     kChat = KeyEdge(s_keys.chat), kMute = KeyEdge(s_keys.voicemute),
	     kConsole = KeyEdge(s_keys.console) ||
	                (s_keys.console2 && s_keys.console2 != s_keys.console && KeyEdge(s_keys.console2)),
	     kEsc = KeyEdge(VK_ESCAPE), kUp = KeyEdge(VK_UP), kDown = KeyEdge(VK_DOWN),
	     kLeft = KeyEdge(VK_LEFT), kRight = KeyEdge(VK_RIGHT),
	     kRet = KeyEdge(VK_RETURN), kBack = KeyEdge(VK_BACK), kSpace = KeyEdge(VK_SPACE);
	if(kMute){
		s_voiceMuted = !s_voiceMuted;
		s_bannerText = s_voiceMuted ? "VOICE MUTED (F9)" : "VOICE ON (F9)";
		s_bannerUntil = NowSec() + 2.0;
	}
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

	// CEF overlay takes all input while a panel is open (or CEF has focus)
	if(CefHud_Active() && (CefHud_Focused() || !s_cefPanel.empty())){
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
		bool lmb = KeyDown(VK_LBUTTON);
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

// The launcher starts the game without a console, so every [NET]/[VEHSYNC]
// line was lost and sync bugs reported from real sessions had no evidence.
// When stderr is not redirected anywhere, write it to reLCS-net-<pid>.log.
static void EnsureNetLogFile(void)
{
	HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
	if(h != nil && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN)
		return;   // already captured (pipe, file or console)
	char name[64];
	snprintf(name, sizeof name, "reLCS-net-%lu.log", GetCurrentProcessId());
	if(freopen(name, "w", stderr))
		setvbuf(stderr, nil, _IONBF, 0);
}

bool NetGame_OnCommandLine(const char* arg)
{
	if(s_pendingArg == "-connect"){
		EnsureNetLogFile();
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
	if(s_pendingArg == "-password"){
		s_pass = arg;
		s_pendingArg.clear();
		return true;
	}
	if(!strcmp(arg, "-connect") || !strcmp(arg, "-nick") ||
	   !strcmp(arg, "-fps") || !strcmp(arg, "-rcon") ||
	   !strcmp(arg, "-password")){
		s_pendingArg = arg;
		return true;
	}
	return false;
}

bool NetGame_IsActive(void)
{
	return s_connected;
}

bool NetGame_RulesActive(void)
{
	// cover the connect window too: the new-game pipeline starts spawning
	// world actors before the handshake completes
	return s_wantConnect || s_connected;
}

// ---------------------------------------------------------------------------
// world rules — enforced for every multiplayer client, every frame:
//   * no world NPCs and no traffic (population knobs zeroed, stock purged)
//   * no missions, kill frenzies, cutscenes or script roadblocks (gated in
//     the script VM) and the whole map stays unlocked (Streaming/ColStore)
// ---------------------------------------------------------------------------

// a traffic car someone is halfway through entering must survive this cycle
static bool VehicleReferencedByAnyone(CVehicle* v)
{
	auto* pp = CPools::GetPedPool();
	for(int i = pp->GetSize()-1; i >= 0; i--){
		CPed* p = pp->GetSlot(i);
		if(p && !p->bInVehicle && p->m_pMyVehicle == v)
			return true;
	}
	return false;
}

static void PurgeWorldActors(void)
{
	CPlayerPed* me = FindPlayerPed();
	CVehicle* meVeh = me ? me->m_pMyVehicle : nil;

	// traffic vehicles first: ~CVehicle flags any occupants for deferred,
	// engine-managed removal, so no occupant can be left dangling
	auto* vp = CPools::GetVehiclePool();
	for(int i = vp->GetSize()-1; i >= 0; i--){
		CVehicle* v = vp->GetSlot(i);
		if(!v || v->VehicleCreatedBy != RANDOM_VEHICLE)
			continue;
		// never touch multiplayer cars or the car we are using
		if(v == meVeh || v == s_pendingVeh || NetIdOfVehicle(v) != rnet::VEHICLE_NONE)
			continue;
		// skip cars carrying anything other than plain world NPCs
		bool skip = VehicleReferencedByAnyone(v);
		for(int s = -1; !skip && s < v->m_nNumMaxPassengers; s++){
			CPed* occ = s < 0 ? v->pDriver : v->pPassengers[s];
			if(occ && (occ->IsPlayer() || occ->CharCreatedBy != RANDOM_CHAR))
				skip = true;
		}
		if(skip)
			continue;
		CWorld::Remove(v);
		delete v;
	}

	// world civilians/cops on foot (plus any occupant the dtors just detached)
	auto* pp = CPools::GetPedPool();
	for(int i = pp->GetSize()-1; i >= 0; i--){
		CPed* p = pp->GetSlot(i);
		if(!p || p->CharCreatedBy != RANDOM_CHAR || p->IsPlayer())
			continue;
		CPopulation::RemovePed(p);
	}
}

static void NetGame_WorldRules(void)
{
	static bool s_logged = false;
	if(!s_logged){
		s_logged = true;
		fprintf(stderr, "[NET] world rules active: no NPCs/traffic, no missions, full map\n");
		fflush(stderr);
	}

	// hard-zero the population knobs every frame: mission cleanup resets them
	CPopulation::PedDensityMultiplier = 0.0f;
	CPopulation::MaxNumberOfPedsInUse = 0;
	CPopulation::MaxNumberOfPedsInUseInterior = 0;
	CCarCtrl::CarDensityMultiplier = 0.0f;
	CCarCtrl::MaxNumberOfCarsInUse = 0;
	// scripted roadblocks (bridge barriers) may never exist in multiplayer
	CRoadBlocks::ClearScriptRoadBlocks();

	// heal player states that silently freeze the game: a vanished car leaves
	// PED_DRIVING with no m_pMyVehicle (invisible, unresponsive), and a stale
	// deferred-removal flag must never survive a frame
	{
		CPlayerPed* me = FindPlayerPed();
		if(me){
			if(me->bRemoveFromWorld)
				me->bRemoveFromWorld = false;
			if((me->Driving() || me->EnteringCar()) && !me->m_pMyVehicle)
				UnseatPlayerForWorld(me);
		}
	}

	static double s_lastPurge = -100.0;
	double now = NowSec();
	if(now - s_lastPurge >= 1.0){
		s_lastPurge = now;
		PurgeWorldActors();
	}
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

// --- persistent profile sync: wallet + weapon table -------------------------
static rnet::MsgInventory s_invLast;
static bool   s_invHave = false;
// admin give-money that the server has not echoed back in an inventory snapshot
// yet. Without this, a snapshot generated before the server accepted our higher
// wallet reverted the grant a few frames later (the cash never stuck).
static int32_t s_moneyPending = 0;
static double s_invLastSend = -100.0;
static bool   s_invForce = false;     // send on next frame regardless of throttle
static rnet::MsgInventory s_pendingInv; // restore waiting for the player ped
static bool   s_havePending = false;
static rnet::MsgInventory s_restore;   // last server restore (re-apply after game wipes)
static bool   s_haveRestore = false;
static double s_restoreAt = 0.0;
static int    s_restoreReapplies = 0;
static CPlayerPed* s_restorePed = nullptr;

static rnet::MsgInventory BuildInventory()
{
	rnet::MsgInventory m;
	m.money = CWorld::Players[CWorld::PlayerInFocus].m_nMoney;
	CPlayerPed* me = FindPlayerPed();
	if(me){
		m.currentWeapon = (uint8_t)me->GetWeapon()->m_eWeaponType;
		for(int i = 0; i < TOTAL_WEAPON_SLOTS && m.count < rnet::MAX_INV_ENTRIES; i++){
			if(!me->HasWeaponSlot((uint8)i)) continue;
			CWeapon& w = me->GetWeapon((uint8)i);
			uint8_t type = (uint8_t)w.m_eWeaponType;
			if(type == 0) continue;
			int32_t ammo = w.m_nAmmoTotal;
			m.entries[m.count].weapon = type;
			m.entries[m.count].ammo = (uint16_t)(ammo < 0 ? 0 : (ammo > 65535 ? 65535 : ammo));
			m.count++;
		}
	}
	return m;
}

static bool InvDiffers(const rnet::MsgInventory& a, const rnet::MsgInventory& b)
{
	if(a.money != b.money || a.count != b.count || a.currentWeapon != b.currentWeapon)
		return true;
	for(int i = 0; i < a.count; i++)
		if(a.entries[i].weapon != b.entries[i].weapon || a.entries[i].ammo != b.entries[i].ammo)
			return true;
	return false;
}

// apply a server profile restore absolutely: wallet + full weapon table.
// applyWallet=false is for the game-wipe re-apply, which must rebuild the
// weapon table only — a stale snapshot must never roll the wallet back.
static void ApplyInventory(const rnet::MsgInventory& m, bool applyWallet = true)
{
	CPlayerPed* me = FindPlayerPed();
	if(!me) return;
	if(applyWallet){
		int32_t wallet = CWorld::Players[CWorld::PlayerInFocus].m_nMoney;
		if(s_moneyPending > 0 && m.money < wallet){
			// a snapshot that predates our grant: keep the cash, spend the grant
			int32_t diff = wallet - m.money;
			s_moneyPending -= diff;
			if(s_moneyPending < 0) s_moneyPending = 0;
		}else{
			if(s_moneyPending > 0) s_moneyPending = 0;   // server caught up
			CWorld::Players[CWorld::PlayerInFocus].m_nMoney = m.money;
		}
	}
	me->ClearWeapons();
	int selSlot = 0;
	for(int i = 0; i < m.count; i++){
		int slot = me->GiveWeapon((eWeaponType)m.entries[i].weapon, (uint32)m.entries[i].ammo, true);
		if(m.entries[i].weapon == m.currentWeapon) selSlot = slot;
	}
	me->SetCurrentWeapon(selSlot);
	s_invLast = m;
	s_invLast.money = CWorld::Players[CWorld::PlayerInFocus].m_nMoney;
	s_invHave = true;
	s_invForce = false;
	// remember the restore: the game's own spawn pipeline (new-game init,
	// cutscene relocations) re-initializes the loadout shortly after join and
	// would otherwise wipe the restored weapons
	s_restore = m;
	s_haveRestore = true;
	s_restoreAt = NowSec();
	s_restorePed = me;
	fprintf(stderr, "[NET] profile restored: $%d + %d weapons\n", (int)m.money, (int)m.count);
	fflush(stderr);
}
static bool s_haveLastSent = false;
static double s_lastTeleportLog = 0.0;

// The server's spawn arrives right after the hello handshake — often while the
// game is still loading and FindPlayerPed() is still nil. Dropping it strands
// the player at the single-player start position (far from the showroom i8s)
// until someone types teleport, so hold the spawn and apply it as soon as the
// ped exists — same pattern as the inventory restore (s_pendingInv).
static rnet::PlayerState s_pendingSpawn;
static bool s_havePendingSpawn = false;
// the spawn the server last gave us: re-asserted when the game's own start-up
// (new-game script) drags the player somewhere else right after it
static rnet::PlayerState s_spawnState;
static double s_spawnAt = -100.0;
static int s_spawnReasserts = 0;

// A relocation (spawn, server teleport) usually lands in a part of the map that
// is not streamed in yet. Dropping a ped or car there lets it fall through the
// unloaded ground until the engine's rescue puts it on some path node far away.
// Load the destination first and stand the entity on the real ground under the
// requested point. A short hop inside the loaded area skips the (blocking) load.
static CVector NetArrivalPos(CEntity* ent, CVector pos)
{
	CVector d = ent->GetPosition() - pos;
	if(d.x * d.x + d.y * d.y > 30.0f * 30.0f){
		CStreaming::LoadScene(pos);
		CStreaming::LoadSceneCollision(pos);
	}
	bool found = false;
	float ground = CWorld::FindGroundZFor3DCoord(pos.x, pos.y, pos.z + 2.0f, &found);
	if(!found)
		ground = CWorld::FindGroundZForCoord(pos.x, pos.y); // asked for a point below the map
	pos.z = ground + ent->GetDistanceFromCentreOfMassToBaseOfModel();
	return pos;
}

static void ApplySpawnState(const rnet::PlayerState& st, bool fresh = true)
{
	CPlayerPed* me = FindPlayerPed();
	if(!me || !NetPosSane(st.xMm / 1000.0f, st.yMm / 1000.0f,
	                      st.zMm / 1000.0f, "spawn"))
		return;
	if(fresh){
		s_spawnState = st;
		s_spawnAt = NowSec();
		s_spawnReasserts = 0;
	}
	// a gamemode respawn can land mid-drive: detach from the car
	// first or the ped stays hidden and steers a phantom vehicle
	UnseatPlayerForWorld(me);
	CVector pos = NetArrivalPos(me, CVector(st.xMm / 1000.0f, st.yMm / 1000.0f, st.zMm / 1000.0f));
	// Teleport (not SetPosition): it clears bIsStanding so the ped
	// settles on the ground instead of hovering mid-air forever
	me->Teleport(pos);
	me->SetOrientation(0.0f, 0.0f, rnet::DequantAngle(st.heading));
	s_lastSentPos = pos;
	s_haveLastSent = true;
}

// ---------------------------------------------------------------------------
// vehicle door chime (BMW i8-style) — plays when the local player opens a car
// door (enter/exit transition).  Sound file resolution:
//   mods/doorchime.wav  — user-supplied recording (own the rights to it!)
//   SFX/doorchime.wav   — generated original fallback (tools/gen_doorchime.js)
// ---------------------------------------------------------------------------
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

static int s_lastPedState = 0;   // s_doorChime is declared above (console needs it)
static bool s_lastInVeh = false;
static double s_lastChimeAt = -100.0;

static void DoorChimeFile(char* full)
{
	char path[MAX_PATH];
	GetModuleFileNameA(NULL, path, MAX_PATH);
	char* slash = strrchr(path, '\\');
	if(slash) *slash = 0;
	snprintf(full, MAX_PATH, "%s\\mods\\doorchime.wav", path);
	DWORD attr = GetFileAttributesA(full);
	if(attr == INVALID_FILE_ATTRIBUTES)
		snprintf(full, MAX_PATH, "%s\\SFX\\doorchime.wav", path);
}

static bool s_chimeRinging = false;

// one-shot play (console test)
static void PlayDoorChime(void)
{
	if(!s_doorChime) return;
	char full[MAX_PATH];
	DoorChimeFile(full);
	// SND_ASYNC: never block the game loop on audio
	if(!PlaySoundA(full, NULL, SND_ASYNC | SND_FILENAME | SND_NODEFAULT))
		fprintf(stderr, "[NET] doorchime: cannot play %s\n", full);
}

// loop while the door stands open; StopDoorChime silences it the moment the
// door is shut (SND_ASYNC playback is process-global, so PlaySound(NULL) ends
// whichever chime instance is running)
static void StartDoorChimeLoop(void)
{
	if(!s_doorChime) return;
	char full[MAX_PATH];
	DoorChimeFile(full);
	if(!PlaySoundA(full, NULL, SND_ASYNC | SND_FILENAME | SND_NODEFAULT | SND_LOOP))
		fprintf(stderr, "[NET] doorchime: cannot play %s\n", full);
	s_chimeRinging = true;
}

static void StopDoorChime(void)
{
	if(!s_chimeRinging) return;
	PlaySoundA(NULL, NULL, 0);
	s_chimeRinging = false;
}

// a neighbour opened or left a car: one chime, only when they are close enough
// to hear it. winmm PlaySound has no panning, so this is a 2D one-shot; the
// range gate stands in for distance falloff. It never interrupts a local chime
// that is already ringing, and it respects the local `doorchime off` setting.
static const float REMOTE_CHIME_RANGE = 60.0f;
static void PlayRemoteDoorChime(float x, float y, float z)
{
	if(!s_doorChime || s_chimeRinging) return;
	if(!FindPlayerPed()) return;
	CVector p = FindPlayerCoors();
	float dx = p.x - x, dy = p.y - y, dz = p.z - z;
	if(dx * dx + dy * dy + dz * dz > REMOTE_CHIME_RANGE * REMOTE_CHIME_RANGE) return;
	char full[MAX_PATH];
	DoorChimeFile(full);
	PlaySoundA(full, NULL, SND_ASYNC | SND_FILENAME | SND_NODEFAULT);
}

static void NetGame_DoorChimeTick(void)
{
	CPlayerPed* me = FindPlayerPed();
	if(!me){ StopDoorChime(); s_lastPedState = 0; s_lastInVeh = false; return; }
	int st = me->m_nPedState;
	// the car door stands open exactly while the ped is in a door state
	// (enter / exit / jacking) — the chime must track that edge-to-edge:
	// ring while the door is open, stop the moment it is shut (driving or
	// back on foot), never keep ringing over the drive
	bool doorOpen = (st == PED_ENTER_CAR || st == PED_CARJACK || st == PED_EXIT_CAR ||
	                 st == PED_OPEN_DOOR);
	if(doorOpen && !s_chimeRinging){
		fprintf(stderr, "[NET] doorchime: door open (state=%d)\n", st);
		fflush(stderr);
		StartDoorChimeLoop();
	}else if(!doorOpen && s_chimeRinging){
		fprintf(stderr, "[NET] doorchime: door shut (state=%d)\n", st);
		fflush(stderr);
		StopDoorChime();
	}
	if(!s_doorChime && s_chimeRinging) StopDoorChime();
	s_lastPedState = st;
	s_lastInVeh = me->InVehicle();
}

// ---------------------------------------------------------------------------
// proximity voice chat
//   capture : winmm waveIn, 8 kHz mono 16-bit, 20 ms blocks, energy-gated;
//             speech blocks (plus a short hangover) are Opus encoded and sent;
//             F9 (keybinds.ini: voicemute) toggles our mic off and on
//   playback: one OpenAL source per nearby speaker (pool of VOICE_SRC_MAX),
//             gain falls to 0 at VOICE_RANGE_M, pan follows the speaker's
//             bearing relative to the local player's body
// No microphone or no OpenAL context means voice stays silent; nothing else
// in multiplayer depends on it.
// ---------------------------------------------------------------------------
#include <AL/al.h>
#include <AL/alc.h>

using rnet::VOICE_SAMPLE_RATE;
using rnet::VOICE_FRAME_SAMPLES;
using rnet::VOICE_MAX_PACKET;

static const int   VOICE_SRC_MAX    = 8;       // speakers audible at once
static const int   VOICE_BUFS       = 8;       // queued frames per speaker (160 ms max backlog)
static const int   VOICE_PRIME      = 3;       // frames queued before a speaker starts (jitter)
static const int   VOICE_MIC_BLOCKS = 4;       // waveIn blocks in flight
static const float VOICE_VAD_RMS    = 350.0f;  // speech threshold (int16 RMS)
static const int   VOICE_HANGOVER   = 10;      // frames (200 ms) sent after speech stops
static const float VOICE_VOLUME     = 1.0f;

struct VoiceSource {
	rnet::VoiceDecoder dec;       // Opus state of the current speaker
	bool used = false;
	uint8_t speaker = 0;
	ALuint src = 0;
	ALuint bufs[VOICE_BUFS] = {};
	bool busy[VOICE_BUFS] = {};   // buffer is queued on the source
	bool primed = false;
	ULONGLONG lastFrame = 0;      // GetTickCount64 ms
};

static bool s_voiceInitDone = false;
static bool s_micOk = false;
static bool s_outOk = false;
static HWAVEIN s_waveIn = nullptr;
static WAVEHDR s_micHdr[VOICE_MIC_BLOCKS];
static int16_t s_micPcm[VOICE_MIC_BLOCKS][VOICE_FRAME_SAMPLES];
static VoiceSource s_vsrc[VOICE_SRC_MAX];
static int s_hangover = 0;
static rnet::VoiceEncoder s_enc;

static void VoiceInit(void)
{
	s_voiceInitDone = true;

	// playback: sources and buffers
	alGetError();
	s_outOk = true;
	for(int i = 0; i < VOICE_SRC_MAX; i++){
		VoiceSource& vs = s_vsrc[i];
		alGenSources(1, &vs.src);
		alGenBuffers(VOICE_BUFS, vs.bufs);
		if(alGetError() != AL_NO_ERROR){ s_outOk = false; break; }
		alSourcef(vs.src, AL_ROLLOFF_FACTOR, 0.0f);   // no distance model: gain and pan are ours
		alSourcei(vs.src, AL_SOURCE_RELATIVE, AL_TRUE);
		alSource3f(vs.src, AL_POSITION, 0.0f, 0.0f, 0.0f);
	}

	// capture: default recording device, 8 kHz mono 16-bit
	WAVEFORMATEX fmt = {};
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = 1;
	fmt.nSamplesPerSec = VOICE_SAMPLE_RATE;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = 2;
	fmt.nAvgBytesPerSec = VOICE_SAMPLE_RATE * 2;
	if(waveInOpen(&s_waveIn, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR){
		s_micOk = true;
		for(int i = 0; i < VOICE_MIC_BLOCKS; i++){
			WAVEHDR& h = s_micHdr[i];
			memset(&h, 0, sizeof h);
			h.lpData = (LPSTR)s_micPcm[i];
			h.dwBufferLength = VOICE_FRAME_SAMPLES * 2;
			waveInPrepareHeader(s_waveIn, &h, sizeof h);
			waveInAddBuffer(s_waveIn, &h, sizeof h);
		}
		waveInStart(s_waveIn);
	}

	fprintf(stderr, "[VOICE] init: mic=%d playback=%d\n", (int)s_micOk, (int)s_outOk);
	fflush(stderr);
}

static void VoiceReleaseSource(VoiceSource& vs)
{
	alSourceStop(vs.src);
	ALint processed = 0;
	alGetSourcei(vs.src, AL_BUFFERS_PROCESSED, &processed);
	while(processed-- > 0){
		ALuint b = 0;
		alSourceUnqueueBuffers(vs.src, 1, &b);
		for(int j = 0; j < VOICE_BUFS; j++) if(vs.bufs[j] == b) vs.busy[j] = false;
	}
	vs.used = false;
	vs.primed = false;
}

static void VoiceShutdown(void)
{
	if(s_waveIn){
		waveInStop(s_waveIn);
		waveInReset(s_waveIn);
		for(int i = 0; i < VOICE_MIC_BLOCKS; i++)
			if(s_micHdr[i].dwFlags & WHDR_PREPARED)
				waveInUnprepareHeader(s_waveIn, &s_micHdr[i], sizeof(WAVEHDR));
		waveInClose(s_waveIn);
		s_waveIn = nullptr;
	}
	s_micOk = false;
	if(s_outOk && alcGetCurrentContext()){
		for(int i = 0; i < VOICE_SRC_MAX; i++){
			VoiceReleaseSource(s_vsrc[i]);
			alDeleteSources(1, &s_vsrc[i].src);
			alDeleteBuffers(VOICE_BUFS, s_vsrc[i].bufs);
		}
	}
	s_outOk = false;
	s_voiceInitDone = false;
}

// a speaker's source: the one already playing them, else a free one
static VoiceSource* VoiceSourceFor(uint8_t speaker, ULONGLONG now)
{
	for(int i = 0; i < VOICE_SRC_MAX; i++)
		if(s_vsrc[i].used && s_vsrc[i].speaker == speaker) return &s_vsrc[i];
	for(int i = 0; i < VOICE_SRC_MAX; i++){
		if(!s_vsrc[i].used){
			s_vsrc[i].used = true;
			s_vsrc[i].speaker = speaker;
			s_vsrc[i].primed = false;
			s_vsrc[i].lastFrame = now;
			s_vsrc[i].dec.Reset();      // fresh decoder state for the new speaker
			return &s_vsrc[i];
		}
	}
	return nullptr;
}

// incoming frame from the server (EV_VOICE): decode and queue on the speaker's source
static void VoiceOnFrame(uint8_t speaker, const std::string& packet)
{
	if(!s_outOk || packet.empty() || packet.size() > (size_t)VOICE_MAX_PACKET) return;
	if(s_net.remotes.find(speaker) == s_net.remotes.end()) return;   // no position: cannot place it
	ULONGLONG now = GetTickCount64();
	VoiceSource* vs = VoiceSourceFor(speaker, now);
	if(!vs) return;

	int slot = -1;
	for(int j = 0; j < VOICE_BUFS; j++) if(!vs->busy[j]){ slot = j; break; }
	if(slot < 0) return;   // backlog full: drop the frame (bounds latency)

	int16_t pcm[VOICE_FRAME_SAMPLES];
	int n = vs->dec.Decode((const uint8_t*)packet.data(), (int)packet.size(), pcm);
	if(n <= 0) return;   // corrupt packet: drop it, the decoder keeps going
	alBufferData(vs->bufs[slot], AL_FORMAT_MONO16, pcm, n * (ALsizei)sizeof(int16_t), VOICE_SAMPLE_RATE);
	alSourceQueueBuffers(vs->src, 1, &vs->bufs[slot]);
	vs->busy[slot] = true;
	vs->lastFrame = now;
}

// per-tick bookkeeping: recycle played buffers, jitter start, state
static void VoiceServiceSource(VoiceSource& vs)
{
	ALint processed = 0;
	alGetSourcei(vs.src, AL_BUFFERS_PROCESSED, &processed);
	while(processed-- > 0){
		ALuint b = 0;
		alSourceUnqueueBuffers(vs.src, 1, &b);
		for(int j = 0; j < VOICE_BUFS; j++) if(vs.bufs[j] == b) vs.busy[j] = false;
	}
	int queued = 0;
	for(int j = 0; j < VOICE_BUFS; j++) if(vs.busy[j]) queued++;
	if(queued == 0) vs.primed = false;                       // drained: wait for jitter again
	else if(!vs.primed && queued >= VOICE_PRIME) vs.primed = true;

	ALint state = 0;
	alGetSourcei(vs.src, AL_SOURCE_STATE, &state);
	if(vs.primed && state != AL_PLAYING) alSourcePlay(vs.src);
}

// gain from the 3D distance to the local player; pan from the bearing in the
// local player's frame (GTA heading: forward = (-sin h, cos h), right = (fy, -fx))
static void VoiceUpdateSpatial(VoiceSource& vs)
{
	auto rit = s_net.remotes.find(vs.speaker);
	if(rit == s_net.remotes.end()){
		alSourcef(vs.src, AL_GAIN, 0.0f);
		return;
	}
	const rnet::PlayerState& sp = rit->second.state;
	const rnet::PlayerState& me = s_net.myState;
	float dx = (sp.xMm - me.xMm) / 1000.0f;
	float dy = (sp.yMm - me.yMm) / 1000.0f;
	float dz = (sp.zMm - me.zMm) / 1000.0f;
	float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
	float gain = 1.0f - dist / rnet::VOICE_RANGE_M;
	if(gain < 0.0f) gain = 0.0f;
	if(gain > 1.0f) gain = 1.0f;

	float h = rnet::DequantAngle(me.heading);
	float fx = -std::sin(h), fy = std::cos(h);
	float rx = fy, ry = -fx;
	float hd = std::sqrt(dx * dx + dy * dy);
	float right = 0.0f, fwd = 0.0f;
	if(hd > 0.01f){
		float ux = dx / hd, uy = dy / hd;
		right = ux * rx + uy * ry;
		fwd = ux * fx + uy * fy;
	}
	// OpenAL listener space: +x right, -z forward
	alSource3f(vs.src, AL_POSITION, right, 0.0f, -fwd);
	alSourcef(vs.src, AL_GAIN, gain * VOICE_VOLUME);
}

// once per frame while connected: capture, then service every speaker
static void VoiceTick(void)
{
	if(!s_voiceInitDone){
		if(!alcGetCurrentContext()) return;   // audio not up yet
		VoiceInit();
	}
	ULONGLONG now = GetTickCount64();

	if(s_micOk){
		for(int i = 0; i < VOICE_MIC_BLOCKS; i++){
			WAVEHDR& h = s_micHdr[i];
			if(!(h.dwFlags & WHDR_DONE)) continue;
			int n = (int)(h.dwBytesRecorded / 2);
			if(n > VOICE_FRAME_SAMPLES) n = VOICE_FRAME_SAMPLES;
			const int16_t* pcm = s_micPcm[i];
			double sum = 0.0;
			for(int k = 0; k < n; k++) sum += (double)pcm[k] * (double)pcm[k];
			float rms = n > 0 ? (float)std::sqrt(sum / n) : 0.0f;
			if(rms > VOICE_VAD_RMS) s_hangover = VOICE_HANGOVER;
			else if(s_hangover > 0) s_hangover--;
			if(n == VOICE_FRAME_SAMPLES){
				// encode every full frame, even silence, so the Opus state stays continuous
				uint8_t packet[VOICE_MAX_PACKET];
				int len = s_enc.Encode(pcm, packet);
				if(len > 0 && s_hangover > 0 && !s_voiceMuted && s_net.Spawned())
					s_net.SendVoice(packet, (size_t)len);
			}
			waveInAddBuffer(s_waveIn, &h, sizeof h);
		}
	}

	if(s_outOk){
		for(int i = 0; i < VOICE_SRC_MAX; i++){
			VoiceSource& vs = s_vsrc[i];
			if(!vs.used) continue;
			if(now - vs.lastFrame > 1000){       // silent for 1 s: free the source
				VoiceReleaseSource(vs);
				continue;
			}
			VoiceServiceSource(vs);
			VoiceUpdateSpatial(vs);
		}
	}
}

void NetGame_Frame(void)
{
	NetGame_DoorChimeTick();

	// chromium overlay: init on first frame, pump + push state every frame
	CefHud_Ensure();
	if(CefHud_Active()){
		static bool s_cefHooked = false;
		if(!s_cefHooked){ s_cefHooked = true; CefHud_SetQueryHandler(CefQueryHandler); }
		DrainCefRequests();
		CefHud_Tick();
		CefPushState();
	}

	// enforce multiplayer world rules (also while still connecting)
	if(NetGame_RulesActive())
		NetGame_WorldRules();

	// connect once the player exists (i.e. world loaded)
	if(s_wantConnect && !s_connected && FindPlayerPed()){
		if(s_net.Connect(s_host.c_str(), s_port, s_nick.c_str(), s_pass.c_str())){
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
	VoiceTick();

	float dt = CTimer::GetTimeStep() / 50.0f; // frames -> seconds-ish (50 fps units)
	if(dt <= 0.0f || dt > 0.5f) dt = 0.02f;

	// drop bindings to vehicles the engine freed before any event handler
	// (seat, state, despawn) can touch them
	for(auto& kv : s_vehicles)
		NetVehicleStale(kv.first, kv.second);

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
			// The server spawns a player whose profile holds no look with skin 0.
			// That is a first join: let him pick a character before he starts,
			// instead of silently playing as the default civilian.
			// kind == 0 means the server spawned us with no saved look: a first
			// join. The overlay may not be initialised yet, so flag it and let
			// the frame pass open the panel once CEF is up.
			if(ev.kind == 0 && !s_creatorShown){
				s_creatorShown = true;
				s_creatorWanted = true;
				s_creatorFirst = true;
				fprintf(stderr, "[NET] first join: opening the character creator\n");
				fflush(stderr);
			}
			if(FindPlayerPed())
				ApplySpawnState(ev.state);
			else{
				// still loading — apply from the pump loop once the ped exists
				s_pendingSpawn = ev.state;
				s_havePendingSpawn = true;
			}
			s_welcomeUntil = NowSec() + 6.0;
			// push the first inventory snapshot right after spawning
			s_invForce = true;
			break;
		}
		case NetClient::EV_INVENTORY:
			// profile restore from the server (sent at hello) — apply as soon as
			// the player ped exists
			if(FindPlayerPed())
				ApplyInventory(ev.inv);
			else{
				s_pendingInv = ev.inv;
				s_havePending = true;
			}
			break;
		case NetClient::EV_CORRECTION:{
			CPlayerPed* me = FindPlayerPed();
			if(me && NetPosSane(ev.state.xMm / 1000.0f, ev.state.yMm / 1000.0f,
			                    ev.state.zMm / 1000.0f, "correction")){
				CVector pos(ev.state.xMm / 1000.0f, ev.state.yMm / 1000.0f, ev.state.zMm / 1000.0f);
				if(me->bInVehicle && me->m_pMyVehicle){
					// a seated ped is re-anchored to its car every frame: moving
					// the ped alone changes nothing, so move the car we sit in
					pos = NetArrivalPos(me->m_pMyVehicle, pos);
					me->m_pMyVehicle->Teleport(pos);
					me->m_pMyVehicle->SetMoveSpeed(0.0f, 0.0f, 0.0f);
					me->m_pMyVehicle->SetTurnSpeed(0.0f, 0.0f, 0.0f);
				}else{
					pos = NetArrivalPos(me, pos);
					me->Teleport(pos);   // clears bIsStanding — see EV_SPAWNED
					me->SetOrientation(0.0f, 0.0f, rnet::DequantAngle(ev.state.heading));
				}
				me->SetMoveSpeed(0.0f, 0.0f, 0.0f);
				s_lastSentPos = pos;
				s_haveLastSent = true;
			}
			break;
		}
		case NetClient::EV_GIVE:{
			// Money is wallet state, not ped state: apply it even while dead, in a
			// menu, or before the player ped exists, otherwise an admin grant sent
			// during a respawn is silently dropped and the cash never arrives.
			if(ev.kind == rnet::GIVE_MONEY){
				CWorld::Players[CWorld::PlayerInFocus].m_nMoney += ev.amount;
				s_moneyPending += ev.amount;
				if(s_haveRestore) s_restore.money = CWorld::Players[CWorld::PlayerInFocus].m_nMoney;
				// The state tick re-asserts s_invLast.money over the wallet (the
				// server's validated economy). Without moving that baseline too,
				// an admin grant was applied here and then reverted on the very
				// next tick, so give-money never showed up on the client.
				if(s_invHave) s_invLast.money = CWorld::Players[CWorld::PlayerInFocus].m_nMoney;
				s_invForce = true;   // report the new wallet immediately
				snprintf(s_lastRcon, sizeof s_lastRcon, "give money +%d -> $%d", ev.amount,
				        (int)CWorld::Players[CWorld::PlayerInFocus].m_nMoney);
				ConsolePrint("$%d received (wallet $%d)", ev.amount,
				             (int)CWorld::Players[CWorld::PlayerInFocus].m_nMoney);
				fprintf(stderr, "[NET] admin give money +%d -> %d\n", ev.amount,
				        (int)CWorld::Players[CWorld::PlayerInFocus].m_nMoney);
				fflush(stderr);
				break;
			}
			CPlayerPed* me = FindPlayerPed();
			if(me){
				switch(ev.kind){
				case rnet::GIVE_MONEY: break;   // handled above
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
		case NetClient::EV_RCON_OK:
			s_rconAuthed = true;
			snprintf(s_lastRcon, sizeof s_lastRcon, "%s", "RCON: authenticated");
			s_bannerText = "RCON: ADMIN AUTHENTICATED";
			s_bannerUntil = NowSec() + 2.5;
			ConsolePrint("rcon: authenticated — admin panel enabled");
			fprintf(stderr, "[NET] rcon authenticated\n");
			fflush(stderr);
			break;
		case NetClient::EV_RCON_RESP:
			// surface the server's answer in the admin panel console — a silent
			// rcon failure looks exactly like a dead button otherwise
			snprintf(s_lastRcon, sizeof s_lastRcon, "%s", ev.text.c_str());
			if(ev.text.find("wrong password") != std::string::npos ||
			   ev.text.find("not authenticated") != std::string::npos)
				s_rconAuthed = false;
			ConsolePrint("rcon: %s", ev.text.c_str());
			break;
		case NetClient::EV_BANNER:
			s_bannerText = ev.text;
			s_bannerUntil = NowSec() + 8.0;
			fprintf(stderr, "[NET] banner: %s\n", ev.text.c_str());
			fflush(stderr);
			break;
		case NetClient::EV_WORLD:{
			// authoritative world clock: snap on drift so the local minute tick
			// stays smooth; weather applies immediately when it changes
			int h = ev.kind & 0xFF, m = ev.arg % 60, w = ev.amount;
			int diff = (h * 60 + m) - ((int)CClock::ms_nGameClockHours * 60 + (int)CClock::ms_nGameClockMinutes);
			if(diff > 720) diff -= 1440;
			if(diff < -720) diff += 1440;
			if(diff >= 2 || diff <= -2){
				CClock::SetGameClock((uint8)h, (uint8)m);
				if(diff >= 15 || diff <= -15){
					fprintf(stderr, "[NET] time -> %02d:%02d\n", h, m);
					fflush(stderr);
				}
			}
			if(w >= 0 && w != s_worldWeather){
				s_worldWeather = w;
				CWeather::ForceWeatherNow((int16)w);
				fprintf(stderr, "[NET] weather -> %d\n", w);
				fflush(stderr);
			}
			s_worldHour = h; s_worldMinute = m;
			break;
		}
		case NetClient::EV_PICKUP:{
			if(!NetPosSane(ev.state.xMm / 1000.0f, ev.state.yMm / 1000.0f,
			               ev.state.zMm / 1000.0f, "pickup"))
				break;
			CVector pos(ev.state.xMm / 1000.0f, ev.state.yMm / 1000.0f, ev.state.zMm / 1000.0f);
			// stream the pickup's models in BEFORE the object is created — an
			// unloaded model makes the prop invisible (or crashes SetModelIndex)
			int32 model = ev.model != 0 ? (int32)ev.model
			                          : CPickups::ModelForWeapon((eWeaponType)ev.arg);
			if(model > 0 && CModelInfo::GetModelInfo(model)){
				CStreaming::RequestModel(model, 0);
				if(CModelInfo::GetModelInfo(model)->GetModelType() == MITYPE_WEAPON){
					CWeaponInfo* wi = CWeaponInfo::GetWeaponInfo((eWeaponType)ev.arg);
					if(wi && wi->m_nModel2Id > 0)
						CStreaming::RequestModel(wi->m_nModel2Id, 0);
				}
				CStreaming::LoadAllRequestedModels(false);
			}
			if(ev.model != 0)
				CPickups::GenerateNewOne(pos, ev.model, ev.kind, (uint32)ev.amount);
			else
				CPickups::GenerateNewOne_WeaponType(pos, (eWeaponType)ev.arg, ev.kind, (uint32)ev.amount);
			fprintf(stderr, "[NET] prop spawned at (%.1f, %.1f, %.1f) model=%d\n",
			        pos.x, pos.y, pos.z, model);
			fflush(stderr);
			break;
		}
		case NetClient::EV_REMOTE_SPAWN:
			SpawnRemote(ev.id, ev.state.xMm / 1000.0f, ev.state.yMm / 1000.0f,
			            ev.state.zMm / 1000.0f, rnet::DequantAngle(ev.state.heading));
			break;
		case NetClient::EV_VOICE:
			VoiceOnFrame(ev.id, ev.text);
			break;
		case NetClient::EV_REMOTE_QUIT:
			RemoveRemote(ev.id);
			s_remoteModel.erase(ev.id);   // forget the departed player's look
			break;
		case NetClient::EV_APPEARANCE:
			if(ev.id == s_net.MyId()){
				// the server restored our saved look (join) or echoed our change.
				// Adopt it as our baseline so we don't immediately re-send/fight it;
				// LCS keeps the local PlayerPed as MI_PLAYER, so we don't re-skin self.
				s_myAppearance = ev.appearance;
				s_myAppearance.playerId = s_net.MyId();
				s_appearanceServerKnown = true;
				s_appearanceDirty = false;
				s_creatorFirst = false;   // a saved look: no creator gate needed
				fprintf(stderr, "[NET] own appearance from server: model=%d\n",
				        (int)ev.appearance.model);
				fflush(stderr);
			}else{
				// a remote player's look: re-skin their puppet (or store for spawn)
				ApplyRemoteAppearance(ev.id, ev.appearance);
			}
			break;
		case NetClient::EV_VEH_SPAWN:{
			float vx = ev.state.xMm / 1000.0f, vy = ev.state.yMm / 1000.0f, vz = ev.state.zMm / 1000.0f;
			float vh = rnet::DequantAngle(ev.state.heading);
			// a spawn for an id we already track is a duplicate echo: never build
			// a second puppet over an entity we already hold
			if(s_vehicles.find(ev.arg) != s_vehicles.end())
				break;
			if(s_pendingVeh && ev.id == s_net.MyId()){
				// this spawn is OUR enter being allocated (owner-tagged by the
				// server): bind our local car. The owner check is what stops us
				// adopting someone else's same-model car parked next to ours.
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
			fprintf(stderr, "[VEHSYNC] spawn veh=%d model=%d owner=%d me=%d known=%d at (%.1f,%.1f,%.1f) -> puppet\n",
			        (int)ev.arg, (int)ev.model, (int)ev.id, (int)s_net.MyId(),
			        (int)(s_vehicles.find(ev.arg) != s_vehicles.end()), vx, vy, vz);
			fflush(stderr);
			NetVehicleAt(ev.arg, ev.model, vx, vy, vz, vh);
			break;
		}
		case NetClient::EV_VEH_ENTER:
			if(ev.id == s_net.MyId()){
				s_myVehNet = ev.arg;
				s_mySeat = ev.kind;
				// bind our local car object if the spawn didn't match it: prefer
				// s_pendingVeh (the exact car we got into), else our current/last car
				CVehicle* pending = s_pendingVeh;
				s_pendingVeh = nil;
				bool bound = false;
				CPlayerPed* me2 = FindPlayerPed();
				CVehicle* local = pending;
				if(!local) local = (me2 && me2->bInVehicle) ? me2->m_pMyVehicle : s_lastMyVeh;
				if(local && NetIdOfVehicle(local) == rnet::VEHICLE_NONE){
					RemoveNetVehicle(ev.arg); // drop any half-made puppet for this id
					AdoptNetVehicle(ev.arg, local, (uint16_t)local->GetModelIndex());
					bound = true;
				}
				fprintf(stderr, "[VEHSYNC] self SEATED veh=%d seat=%d local-car-bound=%d\n",
				        (int)ev.arg, (int)ev.kind, (int)bound);
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
					bool ok = SeatNetPed(it->second.ped, veh, ev.kind);
					fprintf(stderr, "[VEHSYNC] remote player %d %s veh=%d seat=%d (ped=%p veh-known=%d)\n",
					        (int)ev.id, ok ? "SEATED" : "REJECTED seat taken",
					        (int)ev.arg, (int)ev.kind, (void*)it->second.ped, (int)(vit != s_vehicles.end()));
					fflush(stderr);
					if(ok){
						CVector vp = veh->GetPosition();
						PlayRemoteDoorChime(vp.x, vp.y, vp.z);
					}
				}else{
					fprintf(stderr, "[VEHSYNC] remote player %d PENDING veh=%d seat=%d (ped=%d car=%d)\n",
					        (int)ev.id, (int)ev.arg, (int)ev.kind,
					        (int)(it != s_remotes.end() && it->second.ped != nil), (int)(veh != nil));
					fflush(stderr);
					s_pendingSeat[ev.id] = std::make_pair(ev.arg, ev.kind);
				}
			}
			break;
		case NetClient::EV_VEH_EXIT:
			if(ev.id == s_net.MyId()){
				fprintf(stderr, "[VEHSYNC] self EXIT veh=%d\n", (int)ev.arg);
				fflush(stderr);
				s_myVehNet = rnet::VEHICLE_NONE;
				s_mySeat = 0xFF;
				s_pendingVeh = nil;
			}else{
				auto it = s_remotes.find(ev.id);
				if(it != s_remotes.end() && it->second.ped){
					CVehicle* leaving = it->second.ped->m_pMyVehicle;
					if(leaving){
						CVector lp = leaving->GetPosition();
						PlayRemoteDoorChime(lp.x, lp.y, lp.z);
					}
					fprintf(stderr, "[VEHSYNC] remote player %d EXIT veh=%d was-seated=%d\n",
					        (int)ev.id, (int)ev.arg, (int)(it->second.ped->m_pMyVehicle != nil));
					fflush(stderr);
					UnseatNetPed(it->second.ped);
				}
				s_pendingSeat.erase(ev.id);
			}
			break;
		case NetClient::EV_VEH_DESPAWN:{
			// the server retired this vehicle (admin delcar, static spawn cleanup)
			if(ev.arg == s_myVehNet){
				s_myVehNet = rnet::VEHICLE_NONE;
				s_mySeat = 0xFF;
				s_pendingVeh = nil;
			}
			RemoveNetVehicle(ev.arg);
			break;
		}
		case NetClient::EV_VEH_STATE:{
			if(ev.arg == s_myVehNet && s_mySeat == 0)
				break; // our own authoritative echo
			// reject insane targets before they latch into smooth*/target —
			// one bad packet must not teleport a puppet out of the world
			if(!NetPosSane(ev.veh.xMm / 1000.0f, ev.veh.yMm / 1000.0f,
			               ev.veh.zMm / 1000.0f, "veh state"))
				break;
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
			if(ev.kind == rnet::REFUSE_AUTH)
				ConsolePrint("join refused: this name is claimed — set the right password in the launcher (or use -password)");
			else if(ev.kind == rnet::REFUSE_PASSWORD)
				ConsolePrint("join refused: %s", ev.text.c_str());
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

	// seat remote players whose ped and car arrived in either order
	for(auto it = s_pendingSeat.begin(); it != s_pendingSeat.end(); ){
		auto rit = s_remotes.find(it->first);
		auto vit = s_vehicles.find(it->second.first);
		if(rit != s_remotes.end() && rit->second.ped &&
		   vit != s_vehicles.end() && vit->second.veh){
			bool ok = SeatNetPed(rit->second.ped, vit->second.veh, it->second.second);
			fprintf(stderr, "[VEHSYNC] pending seat applied player=%d veh=%d seat=%d ok=%d\n",
			        (int)it->first, (int)it->second.first, (int)it->second.second, (int)ok);
			fflush(stderr);
			it = s_pendingSeat.erase(it);
		}else
			++it;
	}
	// keep every seated remote player in its seat as its net vehicle moves
	for(auto& kv : s_remotes){
		CPed* ped = kv.second.ped;
		if(ped && ped->bInVehicle && ped->m_pMyVehicle)
			AnchorSeatedPuppet(ped);
	}

	// sample + send local state at 20 Hz
	double now = NowSec();
	CPlayerPed* me = FindPlayerPed();

	// local vehicle enter/exit streaming
	if(me){
		static int s_lastLoggedPedState = -1;
		if((int)me->m_nPedState != s_lastLoggedPedState){
			fprintf(stderr, "[NET] pedstate %d -> %d (inVeh=%d veh=%p)\n",
			        s_lastLoggedPedState, (int)me->m_nPedState,
			        (int)me->bInVehicle, (void*)me->m_pMyVehicle);
			fflush(stderr);
			s_lastLoggedPedState = (int)me->m_nPedState;
		}
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
				fprintf(stderr, "[VEHSYNC] self ENTER model=%d seat=%d netId=%d (%s)\n",
				        (int)cur->GetModelIndex(), (int)seat, (int)s_myVehNet,
				        existing != rnet::VEHICLE_NONE ? "existing car" : "new car, server spawns it");
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
		float sp = s_ncSpeed * dt;
		if(KeyDown(VK_CONTROL)) sp *= 4.0f;      // speed boost
		else if(KeyDown(VK_MENU)) sp *= 0.25f;   // fine control
		// [ / ] adjust the base speed — but never while typing in a UI
		bool uiUp = !s_cefPanel.empty() || s_consoleOpen || s_chatOpen || s_panelOpen;
		if(!uiUp && KeyEdge(VK_OEM_6)) s_ncSpeed = s_ncSpeed * 1.5f > 500.0f ? 500.0f : s_ncSpeed * 1.5f;
		if(!uiUp && KeyEdge(VK_OEM_4)) s_ncSpeed = s_ncSpeed / 1.5f < 1.0f ? 1.0f : s_ncSpeed / 1.5f;
		CVector fwd = me->GetForward();
		CVector right = me->GetRight();
		if(KeyDown('W')) s_ncPos += fwd * sp;
		if(KeyDown('S')) s_ncPos -= fwd * sp;
		if(KeyDown('D')) s_ncPos += right * sp;
		if(KeyDown('A')) s_ncPos -= right * sp;
		if(KeyDown(VK_SPACE)) s_ncPos.z += sp;
		if(KeyDown(VK_SHIFT)) s_ncPos.z -= sp;
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
			// right after a spawn that jump is the game's own start-up moving
			// the player off the server's spawn point: put them back on it
			// (a long jump that also left the spawn far behind — walking or a
			// small physics hop near the spawn is the player's own movement)
			float sx = pos.x - s_spawnState.xMm / 1000.0f, sy = pos.y - s_spawnState.yMm / 1000.0f;
			if(st.moveFlags && !inVehicle && s_spawnReasserts < 3 && now - s_spawnAt < 10.0 &&
			   h > 40.0f && sx * sx + sy * sy > 40.0f * 40.0f){
				s_spawnReasserts++;
				fprintf(stderr, "[NET] spawn re-asserted after game-side relocation (%d)\n", s_spawnReasserts);
				fflush(stderr);
				ApplySpawnState(s_spawnState, false);
				pos = me->GetPosition();
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
		// server-authoritative economy: report the wallet the server last agreed
		// on, not whatever the game's own new-game / script pipeline wrote into
		// the player struct.  That local re-init differs from the server ledger
		// and used to be reported as a money edit on every tick, so the economy
		// anti-cheat kicked the client seconds after it finished joining.
		if(s_invHave){
			CWorld::Players[CWorld::PlayerInFocus].m_nMoney = s_invLast.money;
			st.money = s_invLast.money;
		}else{
			st.money = CWorld::Players[CWorld::PlayerInFocus].m_nMoney;
		}
		st.animId = 0;
		st.animFlags = 0;
		st.vehicleId = inVehicle ? (s_myVehNet != rnet::VEHICLE_NONE ? s_myVehNet : rnet::VEHICLE_UNSYNCED)
		                         : rnet::VEHICLE_NONE;
		st.seat = inVehicle ? s_mySeat : 0xFF;
		s_lastSentPos = pos;
		s_haveLastSent = true;
		s_net.SendState(st);

	// a spawn/restore that arrived before the ped existed: apply it now
	if(s_havePendingSpawn && FindPlayerPed()){
		s_havePendingSpawn = false;
		ApplySpawnState(s_pendingSpawn);
	}
	if(s_havePending && FindPlayerPed()){
		s_havePending = false;
		ApplyInventory(s_pendingInv);
	}

	// the game re-initializing the player shortly after join (new-game /
	// cutscene pipeline) wipes the restored loadout — re-apply it a few times
	// inside a grace window instead of reporting the wipe back to the profile DB
	if(s_haveRestore && s_restore.count > 0 && s_restoreReapplies < 4 &&
	   now - s_restoreAt < 60.0){
		CPlayerPed* cur = FindPlayerPed();
		bool wiped = cur && (cur != s_restorePed || BuildInventory().count == 0);
		if(wiped){
			s_restoreReapplies++;
			ApplyInventory(s_restore, false);
			fprintf(stderr, "[NET] restore re-applied after game-side wipe (%d)\n", s_restoreReapplies);
			fflush(stderr);
		}
	}

	// persistent profile: report the wallet + weapon table when it changes
	// (throttled — the server stores the latest snapshot in SQLite)
	rnet::MsgInventory inv = BuildInventory();
		if((s_invForce || now - s_invLastSend >= 0.5) && (!s_invHave || InvDiffers(s_invLast, inv))){
			inv.playerId = s_net.MyId();
			s_net.SendInventory(inv);
			s_invLast = inv;
			s_invHave = true;
			s_invLastSend = now;
			s_invForce = false;
		}

		// appearance: send our chosen look whenever it changed (the `model`
		// console command sets s_appearanceDirty). The server persists it and
		// rebroadcasts so every other client re-skins our puppet.
		if(s_creatorWanted && CefHud_Active()){
			s_creatorWanted = false;
			CefHud_Show("creator");
			fprintf(stderr, "[NET] character creator opened\n");
			fflush(stderr);
		}

		// respawns, cutscenes and streaming rebuilds can put the default body
		// back; re-apply ours so the character we picked stays ours
		ApplyLocalAppearance(false);

		if(s_appearanceDirty){
			s_appearanceDirty = false;
			s_myAppearance.playerId = s_net.MyId();
			s_net.SendAppearance(s_myAppearance);
			fprintf(stderr, "[NET] sent appearance model=%d\n", (int)s_myAppearance.model);
			fflush(stderr);
		}

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
			if(cv->IsCar()){
				CAutomobile* car = (CAutomobile*)cv;
				for(int i = 0; i < 6; i++){
					float r = car->Doors[i].GetAngleOpenRatio();
					if(r < 0.0f) r = 0.0f;
					if(r > 1.0f) r = 1.0f;
					vs.doors[i] = (uint8_t)(r * 255.0f + 0.5f);
				}
			}
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

	// server broadcast / round banner (large, center screen)
	if(noww < s_bannerUntil && !s_bannerText.empty()){
		float fade = (float)(s_bannerUntil - noww) / 1.5f;
		if(fade > 1.0f) fade = 1.0f;
		HudText(SCREEN_SCALE_X(320.0f), SCREEN_SCALE_Y(105.0f), 1.0f,
		        s_bannerText.c_str(),
		        CRGBA(255, 90, 60, (uint8)(fade * 255.0f)), true);
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
	{
		std::lock_guard<std::mutex> lock(s_cefQueueLock);
		s_cefQueue.clear();
	}
	CefHud_Shutdown();
	VoiceShutdown();
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
	// a CEF panel owns the mouse as soon as it is OPEN — waiting for CEF focus
	// first was a chicken-and-egg (you cannot click to focus without a cursor)
	return (CefHud_Active() && CefHud_Focused()) || s_panelOpen || !s_cefPanel.empty();
}
