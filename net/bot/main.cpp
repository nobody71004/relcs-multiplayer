// net/bot/main.cpp — relcs-netbot: scripted headless client for E2E testing.
//
// Usage:
//   relcs-netbot -name BotA -server 127.0.0.1:7777 [-scenario roam|combat|drive|chaos|cruise|watch]
//                [-duration 30] [-expect-chat SUBSTR] [-rcon PW] [-cmd "RCON CMD"] [-seed N] [-verbose]
//
// Vehicle-sync scenarios (late-join / reference-freeze / stream-in coverage):
//   cruise  — spawn a car of our own (VEH_ENTER 0xFFFF, like the game's `ride`
//             console cheat) and drive it as the authoritative driver.
//             -speed M/S         cruise speed (default 12)
//             -jump-at T -jump-dist D [-drop-jump]
//                                at T s relocate the car D m (a scripted warp);
//                                -drop-jump loses the flagged state packets in
//                                transit, exactly like a dropped unreliable packet
//             -drive-to X,Y,Z    drive straight there, park, get out and stay
//   watch   — stand still and observe the other players' cars.
//             -goto-at T -goto X,Y,Z   relocate (flagged teleport) at T s
//             -expect-driver     at the end another player must be seen seated
//                                as driver of a live vehicle that is moving, with
//                                fresh vehicle + player state and a MSG_SPAWN for
//                                every peer we received states from
//             -expect-vehicle-near X,Y,Z,R
//                                at the end a live vehicle must be known within R m
//
// Prints 1 Hz telemetry lines consumed by the E2E harness:
//   [<unix t>] IAM <x> <y> <z>       — this bot's authoritative position
//   [<unix t>] SEES <id> <x> <y> <z> age=<s> — last relayed state of another
//                                   player + snapshot age at print time
//   (wall-clock timestamps so e2e can align samples across staggered bots)
// and ends with:
//   [BOT] RESULT: PASS|FAIL <reasons>

#include "rnet_protocol.h"
#include "rnet_voice.h"
#include <enet/enet.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <string>
#include <map>
#include <set>
#include <random>
#include <algorithm>

using namespace rnet;

static double NowSec()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

struct PeerView {
	PlayerState state;
	double lastSeen = 0.0;
};

struct BotConfig {
	std::string name = "bot";
	std::string host = "127.0.0.1";
	uint16_t port = DEFAULT_PORT;
	std::string scenario = "roam";
	double duration = 30.0;
	std::string expectChat;
	std::string password;        // profile password (claims/restores the name)
	std::string rconPassword;
	std::vector<std::string> rconCmds;   // extra RCON commands after auth (repeatable)
	uint32_t seed = 0;
	bool verbose = false;
	bool discover = false;
	bool masterQuery = false;
	int vehModel = 0; // vehicle model for self-created cars (0 = guess)
	bool voice = false; // -voice: send a 440 Hz tone as voice and report relayed voice frames
	// cruise / watch scenarios (see header)
	float speed = 12.0f;
	double jumpAt = -1.0;
	float jumpDist = 150.0f;
	bool dropJump = false;
	bool driveTo = false;
	float driveX = 0, driveY = 0, driveZ = 0;
	double gotoAt = -1.0;
	float gotoX = 0, gotoY = 0, gotoZ = 0;
	bool expectDriver = false;
	bool expectNear = false;
	float nearX = 0, nearY = 0, nearZ = 0, nearR = 3.0f;
};

// "x,y,z[,w]" -> floats; returns the number parsed
static int ParseFloats(const char* s, float* out, int n)
{
	int got = 0;
	while(got < n && s && *s){
		char* end = nullptr;
		float v = std::strtof(s, &end);
		if(end == s) break;
		out[got++] = v;
		s = (*end == ',') ? end + 1 : end;
	}
	return got;
}

// ---------------------------------------------------------------------------
// Utility modes (no game session)
// ---------------------------------------------------------------------------
#include "compat.h"

static int RunDiscover()
{
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(s == INVALID_SOCKET){ std::printf("[BOT] RESULT: FAIL socket\n"); return 1; }
	BOOL bc = 1;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&bc, sizeof bc);
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);

	const char* magic = LAN_DISCOVER_MAGIC;
	sockaddr_in dst;
	memset(&dst, 0, sizeof dst);
	dst.sin_family = AF_INET;
	dst.sin_port = htons(DISCOVERY_PORT);

	// subnet broadcast + loopback (same-machine test)
	dst.sin_addr.s_addr = INADDR_BROADCAST;
	sendto(s, magic, (int)strlen(magic), 0, (sockaddr*)&dst, sizeof dst);
	inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
	sendto(s, magic, (int)strlen(magic), 0, (sockaddr*)&dst, sizeof dst);

	int found = 0;
	double t0 = NowSec();
	while(NowSec() - t0 < 2.0){
		char buf[512];
		sockaddr_in from; socklen_t flen = sizeof from;
		int n = recvfrom(s, buf, sizeof buf - 1, 0, (sockaddr*)&from, &flen);
		if(n > 0){
			buf[n] = '\0';
			std::printf("[DISC] %s\n", buf);
			if(strncmp(buf, LAN_RESP_MAGIC, strlen(LAN_RESP_MAGIC)) == 0) found++;
		}
		Sleep(10);
	}
	closesocket(s);
	std::printf("[BOT] RESULT: %s discover_responses=%d\n", found ? "PASS" : "FAIL", found);
	return found ? 0 : 1;
}

static int RunMasterQuery(const std::string& host, uint16_t port)
{
	ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
	ENetAddress addr;
	if(enet_address_set_host(&addr, host.c_str()) < 0){
		std::printf("[BOT] RESULT: FAIL bad master host\n"); return 1;
	}
	addr.port = port;
	ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);

	double t0 = NowSec();
	bool connected = false, sentReq = false;
	int entries = 0;
	while(NowSec() - t0 < 5.0){
		ENetEvent ev;
		while(enet_host_service(client, &ev, 0) > 0){
			if(ev.type == ENET_EVENT_TYPE_CONNECT){
				connected = true;
				BitWriter w;
				BeginMsg(w, MSG_MASTER_LIST_REQ);
				ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send(peer, 0, pkt);
				sentReq = true;
			}
			if(ev.type == ENET_EVENT_TYPE_RECEIVE){
				BitReader r(ev.packet->data, ev.packet->dataLength);
				MsgType type = (MsgType)r.ReadU8();
				if(type == MSG_MASTER_LIST_RESP){
					uint16_t count = r.ReadU16();
					for(uint16_t i = 0; i < count; i++){
						MasterListEntry e;
						if(!ReadMasterEntry(r, e)) break;
						entries++;
						char ipstr[32];
						snprintf(ipstr, sizeof ipstr, "%u.%u.%u.%u",
						         e.ipv4 & 0xFF, (e.ipv4 >> 8) & 0xFF,
						         (e.ipv4 >> 16) & 0xFF, (e.ipv4 >> 24) & 0xFF);
						std::printf("[MASTER] %s:%u \"%s\" %d/%d mode=%s\n",
						            ipstr, (unsigned)e.gamePort, e.hostname,
						            (int)e.players, (int)e.maxPlayers, e.gameMode);
					}
				}
				enet_packet_destroy(ev.packet);
			}
		}
		Sleep(5);
	}
	enet_host_destroy(client);
	bool pass = connected && sentReq && entries >= 1;
	std::printf("[BOT] RESULT: %s master_entries=%d\n", pass ? "PASS" : "FAIL", entries);
	return pass ? 0 : 1;
}

int main(int argc, char** argv)
{
	BotConfig cfg;
	for(int i = 1; i < argc; i++){
		std::string a = argv[i];
		bool hasNext = (i + 1 < argc);
		if(a == "-name" && hasNext) cfg.name = argv[++i];
		else if(a == "-server" && hasNext){
			std::string s = argv[++i];
			size_t c = s.find(':');
			cfg.host = s.substr(0, c);
			if(c != std::string::npos) cfg.port = (uint16_t)atoi(s.c_str() + c + 1);
		}
		else if(a == "-scenario" && hasNext) cfg.scenario = argv[++i];
		else if(a == "-duration" && hasNext) cfg.duration = atof(argv[++i]);
		else if(a == "-expect-chat" && hasNext) cfg.expectChat = argv[++i];
		else if(a == "-password" && hasNext) cfg.password = argv[++i];
		else if(a == "-rcon" && hasNext) cfg.rconPassword = argv[++i];
		else if(a == "-cmd" && hasNext) cfg.rconCmds.push_back(argv[++i]);
		else if(a == "-seed" && hasNext) cfg.seed = (uint32_t)atoi(argv[++i]);
		else if(a == "-verbose") cfg.verbose = true;
		else if(a == "-discover") cfg.discover = true;
		else if(a == "-master-query") cfg.masterQuery = true;
		else if(a == "-vehmodel" && hasNext) cfg.vehModel = atoi(argv[++i]);
		else if(a == "-voice") cfg.voice = true;
		else if(a == "-speed" && hasNext) cfg.speed = (float)atof(argv[++i]);
		else if(a == "-jump-at" && hasNext) cfg.jumpAt = atof(argv[++i]);
		else if(a == "-jump-dist" && hasNext) cfg.jumpDist = (float)atof(argv[++i]);
		else if(a == "-drop-jump") cfg.dropJump = true;
		else if(a == "-drive-to" && hasNext){
			float v[3];
			cfg.driveTo = ParseFloats(argv[++i], v, 3) == 3;
			cfg.driveX = v[0]; cfg.driveY = v[1]; cfg.driveZ = v[2];
		}
		else if(a == "-goto-at" && hasNext) cfg.gotoAt = atof(argv[++i]);
		else if(a == "-goto" && hasNext){
			float v[3];
			if(ParseFloats(argv[++i], v, 3) == 3){ cfg.gotoX = v[0]; cfg.gotoY = v[1]; cfg.gotoZ = v[2]; }
		}
		else if(a == "-expect-driver") cfg.expectDriver = true;
		else if(a == "-expect-vehicle-near" && hasNext){
			float v[4] = { 0, 0, 0, 3.0f };
			cfg.expectNear = ParseFloats(argv[++i], v, 4) >= 3;
			cfg.nearX = v[0]; cfg.nearY = v[1]; cfg.nearZ = v[2]; cfg.nearR = v[3];
		}
	}
	if(cfg.seed == 0) cfg.seed = (uint32_t)std::hash<std::string>{}(cfg.name);
	std::mt19937 rng(cfg.seed);

	if(enet_initialize() != 0){ std::printf("[BOT] RESULT: FAIL enet_init\n"); return 1; }

	if(cfg.discover) return RunDiscover();
	if(cfg.masterQuery) return RunMasterQuery(cfg.host, cfg.port);

	ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
	if(!client){ std::printf("[BOT] RESULT: FAIL host_create\n"); return 1; }
	client->maximumPacketSize = 32 * 1024;

	ENetAddress addr;
	if(enet_address_set_host(&addr, cfg.host.c_str()) < 0){
		std::printf("[BOT] RESULT: FAIL bad host %s\n", cfg.host.c_str());
		return 1;
	}
	addr.port = cfg.port;
	ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
	if(!peer){ std::printf("[BOT] RESULT: FAIL connect\n"); return 1; }

	// --- assertion state ---
	bool gotWelcome = false, gotSpawn = false, kicked = false, disconnected = false;
	bool gotChatExpect = cfg.expectChat.empty();
	bool sawPeer = false, sawDeath = false, rconOk = cfg.rconPassword.empty();
	bool rconCmdOk = cfg.rconPassword.empty();
	uint8_t selfId = 0;
	int statesSent = 0, correctionsReceived = 0, deathsSelf = 0, spawnsSeen = 0;
	int vehStatesSeen = 0, vehEntersSeen = 0; // relayed traffic from other drivers
	std::string failReasons;
	std::map<uint8_t, PeerView> peers;
	PlayerState self;
	self.health = 100;
	bool alive = false;

	// spawn origin for the random walk
	float homeX = 0, homeY = 0, homeZ = 0;
	float heading = 0.0f;
	uint16_t animId = 0;

	double t0 = NowSec();
	double lastStateSend = 0, lastWalk = 0, lastChat = 0, lastDamage = 0, lastTelemetry = 0,
	       lastPing = 0, lastVehCycle = 0, rconTimer = -1.0;
	int chatSeq = 0, vehPhase = 0;
	// vehicle session (drive scenario): the real id/seat arrive with the
	// server's VEH_ENTER echo; seenVeh* tracks vehicles other players created
	uint16_t myVehicle = 0xFFFF;
	uint8_t mySeat = 0xFF;
	uint16_t myModel = 0;
	double lastVehState = 0;
	uint16_t seenVehId = 0xFFFF, seenVehModel = 0; // last vehicle that appeared
	uint16_t ownVehId = 0xFFFF;                    // last car WE drove (seat 0)

	// --- single-entity / occupancy integrity (vehicle-sync regression guards) ---
	// live vehicle set: SPAWN adds, DESPAWN removes. A correctly behaving server
	// gives us exactly ONE entity per real car — never a duplicate. We also count
	// how many distinct ids were ever spawned and whether any single id was
	// spawned twice without an intervening despawn (the "car gets duplicated" bug).
	std::set<uint16_t> liveVehicles;
	std::map<uint16_t, int> vehSpawnCount;         // id -> times SPAWN seen while already live
	size_t maxLiveVehicles = 0;
	int dupSpawns = 0;                             // SPAWN for an id already live (duplicate)
	// occupancy integrity: a VEH_ENTER naming US must be one WE asked for. We set
	// expectSelfEnter just before sending our own enter; any self-enter arriving
	// while that flag is clear means the server put us in a car we never entered.
	bool expectSelfEnter = false;
	int unexpectedSelfEnters = 0;
	// how many DISTINCT other players we saw seated via VEH_ENTER (not us). Used to
	// confirm occupancy stays scoped to the player who actually entered.
	std::map<uint8_t, uint16_t> otherOccupant;     // playerId -> vehicleId they entered

	// --- watch scenario: what this client knows about the world's vehicles ---
	struct VehView {
		bool hasPos = false;
		float x = 0, y = 0, z = 0;
		double lastState = -1.0;   // elapsed() of the last MSG_VEHICLE_STATE
		int states = 0;
		float minX = 0, maxX = 0, minY = 0, maxY = 0;
	};
	std::map<uint16_t, VehView> vehView;
	std::map<uint8_t, std::pair<uint16_t, uint8_t>> seatOf; // other player -> (vehicle, seat) per VEH_ENTER/EXIT
	std::set<uint8_t> peerSpawnSeen;                // other players we got a MSG_SPAWN for
	std::set<uint8_t> peerStateSeen;                // other players we got relayed states for

	// --- cruise scenario: our own car ---
	bool carRequested = false;
	double spawnedAt = -1.0;
	float circleCX = 0, circleCY = 0, circleA = 0, circleR = 25.0f;
	bool jumped = false, dropState = false, dropVehState = false;
	bool parked = false;
	uint16_t parkedId = 0xFFFF;
	float parkX = 0, parkY = 0, parkZ = 0;
	int correctionsIgnored = 0;
	bool gone = false;                               // watch: -goto relocation done

	// proximity voice (-voice): tone frames out, relayed frames in
	double lastVoice = 0.0;
	int voiceSent = 0, voiceOutOfRange = 0;
	float voicePhase = 0.0f;
	rnet::VoiceEncoder voiceEnc;
	struct Heard { int frames = 0; float maxDist = 0.0f; };
	std::map<uint8_t, Heard> heard;

	auto elapsed = [&]{ return NowSec() - t0; };

	// connect handshake
	bool connected = false;
	while(elapsed() < 5.0 && !connected){
		ENetEvent ev;
		while(enet_host_service(client, &ev, 0) > 0){
			if(ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
			if(ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
		}
		Sleep(5);
	}
	if(!connected){
		std::printf("[BOT] RESULT: FAIL connect_timeout\n");
		return 1;
	}

	// send HELLO
	{
		BitWriter w;
		BeginMsg(w, MSG_HELLO);
		MsgHello h;
		CopyStr(h.name, sizeof h.name, cfg.name.c_str());
		CopyStr(h.password, sizeof h.password, cfg.password.c_str());
		WriteHello(w, h);
		ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
		enet_peer_send(peer, CHAN_RELIABLE, pkt);
		enet_host_flush(client);
	}

	std::printf("[%.3f] START %s scenario=%s target=%s:%u\n",
	            elapsed(), cfg.name.c_str(), cfg.scenario.c_str(), cfg.host.c_str(), (unsigned)cfg.port);

	auto sendChat = [&](const char* text){
		BitWriter w;
		BeginMsg(w, MSG_CHAT);
		MsgChat c;
		c.fromId = selfId;
		CopyStr(c.text, sizeof c.text, text);
		WriteChat(w, c);
		ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
		enet_peer_send(peer, CHAN_RELIABLE, pkt);
	};

	// self-validate like the real game client: any step over the movement
	// budget (scenario teleports, respawns) is reported as a flagged
	// relocation so the server resets its reference instead of rubber-banding
	double lastSentT = -1.0;
	float sentX = 0, sentY = 0, sentZ = 0;

	auto sendState = [&](){
		self.moveFlags = 0;
		float px = self.xMm / 1000.0f, py = self.yMm / 1000.0f, pz = self.zMm / 1000.0f;
		if(lastSentT >= 0.0){
			float ddt = (float)(elapsed() - lastSentT);
			if(ddt < TICK_DT) ddt = TICK_DT;
			bool inVehicle = (self.vehicleId != VEHICLE_NONE);
			float maxH = (inVehicle ? MAX_SPEED_VEHICLE : MAX_SPEED_ONFOOT) * ddt;
			float maxDown = (inVehicle ? MAX_SPEED_VEHICLE : MAX_SPEED_FALL) * ddt;
			float dx = px - sentX, dy = py - sentY, dz = pz - sentZ;
			float h = std::sqrt(dx * dx + dy * dy);
			if(h > maxH || dz > maxH || -dz > maxDown){
				self.moveFlags = MOVEFLAG_TELEPORT;
				double jw = std::chrono::duration_cast<std::chrono::duration<double>>(
					std::chrono::system_clock::now().time_since_epoch()).count();
				std::printf("[%.3f] JUMP %.2f %.2f %.2f\n", jw, px, py, pz);
			}
		}
		lastSentT = elapsed();
		sentX = px; sentY = py; sentZ = pz;
		if(dropState){
			// simulated loss: the client believes it sent this state (its own
			// reference moves on) but the packet never reaches the server
			dropState = false;
			statesSent++;
			return;
		}
		BitWriter w;
		BeginMsg(w, MSG_PLAYER_STATE);
		self.playerId = selfId;
		WritePlayerState(w, self);
		ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), 0);
		enet_peer_send(peer, CHAN_STATE, pkt);
		statesSent++;
	};

	// ---------------- main loop ----------------
	while(elapsed() < cfg.duration && !kicked && !disconnected){
		ENetEvent ev;
		while(enet_host_service(client, &ev, 0) > 0){
			switch(ev.type){
			case ENET_EVENT_TYPE_RECEIVE:{
				BitReader r(ev.packet->data, ev.packet->dataLength);
				MsgType type = (MsgType)r.ReadU8();
				switch(type){
				case MSG_WELCOME:{
					MsgWelcome wel;
					if(ReadWelcome(r, wel)){
						gotWelcome = true;
						selfId = wel.playerId;
						std::printf("[%.3f] WELCOME id=%d mode=%s host=%s\n",
						            elapsed(), (int)selfId, wel.gameMode, wel.hostname);
						// request spawn
						BitWriter w;
						BeginMsg(w, MSG_SPAWN_REQ);
						ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
						enet_peer_send(peer, CHAN_RELIABLE, pkt);
						if(!cfg.rconPassword.empty()){
							BitWriter w2;
							BeginMsg(w2, MSG_RCON_AUTH);
							WriteStringMsg(w2, cfg.rconPassword.c_str(), 255);
							ENetPacket* p2 = enet_packet_create(w2.Data(), w2.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
							enet_peer_send(peer, CHAN_RELIABLE, p2);
							rconTimer = elapsed() + 1.0;
						}
					}
					break;
				}
				case MSG_SPAWN:{
					MsgSpawn sp;
					if(!ReadSpawn(r, sp)) break;
					if(sp.playerId == selfId){
						gotSpawn = true;
						spawnsSeen++;
						alive = true;
						self.health = sp.health;
						self.armour = sp.armour;
						self.xMm = QuantPos(sp.x); self.yMm = QuantPos(sp.y); self.zMm = QuantPos(sp.z);
						self.heading = QuantAngle(sp.heading);
						homeX = sp.x; homeY = sp.y; homeZ = sp.z;
						heading = sp.heading;
						spawnedAt = elapsed();
						{ // position discontinuity marker for the e2e convergence check
							double jw = std::chrono::duration_cast<std::chrono::duration<double>>(
								std::chrono::system_clock::now().time_since_epoch()).count();
							std::printf("[%.3f] JUMP %.2f %.2f %.2f\n", jw, sp.x, sp.y, sp.z);
						}
						if(cfg.verbose)
							std::printf("[%.3f] SPAWN id=%d at %.1f %.1f %.1f\n",
							            elapsed(), (int)sp.playerId, sp.x, sp.y, sp.z);
					}else{
						// another player spawned — never touch our own position for it
						PeerView& pv = peers[sp.playerId];
						pv.state.playerId = sp.playerId;
						pv.state.xMm = QuantPos(sp.x);
						pv.state.yMm = QuantPos(sp.y);
						pv.state.zMm = QuantPos(sp.z);
						pv.lastSeen = elapsed();
						sawPeer = true;
						peerSpawnSeen.insert(sp.playerId);
						if(cfg.verbose)
							std::printf("[%.3f] SPAWN-other id=%d at %.1f %.1f %.1f\n",
							            elapsed(), (int)sp.playerId, sp.x, sp.y, sp.z);
					}
					break;
				}
				case MSG_PLAYER_STATE:{
					PlayerState st;
					if(ReadPlayerState(r, st)){
						if(st.playerId != selfId){
							PeerView& pv = peers[st.playerId];
							pv.state = st;
							pv.lastSeen = elapsed();
							sawPeer = true;
							peerStateSeen.insert(st.playerId);
						}
					}
					break;
				}
				case MSG_CHAT:{
					MsgChat c;
					if(ReadChat(r, c)){
						if(cfg.verbose)
							std::printf("[%.3f] CHAT from=%d %s\n", elapsed(), (int)c.fromId, c.text);
						if(c.fromId != selfId && !cfg.expectChat.empty() &&
						   std::string(c.text).find(cfg.expectChat) != std::string::npos)
							gotChatExpect = true;
					}
					break;
				}
				case MSG_GIVE:{
					MsgGive g;
					if(ReadGive(r, g) && g.targetId == selfId){
						switch(g.kind){
						case GIVE_MONEY:  self.money += g.amount; break;
						case GIVE_WEAPON: self.weapon = (uint8_t)g.arg; self.ammo = (uint16_t)g.amount; break;
						case GIVE_HEALTH: self.health = (uint8_t)g.amount; break;
						default: break;
						}
						if(cfg.verbose)
							std::printf("[%.3f] GIVE kind=%d amount=%d arg=%d\n",
							            elapsed(), (int)g.kind, g.amount, (int)g.arg);
					}
					break;
				}
				case MSG_DEATH:{
					MsgDeath d;
					if(ReadDeath(r, d)){
						sawDeath = true;
						if(d.victimId == selfId){
							alive = false;
							deathsSelf++;
						}
						if(cfg.verbose)
							std::printf("[%.3f] DEATH victim=%d killer=%d\n",
							            elapsed(), (int)d.victimId, (int)d.killerId);
					}
					break;
				}
				case MSG_DAMAGE:{
					MsgDamage d;
					if(ReadDamage(r, d) && d.targetId == selfId && alive){
						uint16_t dmg = d.damage;
						if(self.armour >= dmg) self.armour = (uint8_t)(self.armour - dmg);
						else {
							dmg = (uint16_t)(dmg - self.armour);
							self.armour = 0;
							self.health = (uint8_t)(self.health > dmg ? self.health - dmg : 0);
						}
					}
					break;
				}
				case MSG_CORRECTION:{
					PlayerState fix;
					if(ReadPlayerState(r, fix)){
						correctionsReceived++;
						// the game client cannot act on a correction while seated:
						// CPed::Teleport moves the ped, then the world loop puts it
						// straight back into its seat (the car never moves)
						if(cfg.scenario == "cruise" && self.vehicleId != VEHICLE_NONE){
							correctionsIgnored++;
							break;
						}
						self.xMm = fix.xMm; self.yMm = fix.yMm; self.zMm = fix.zMm;
						self.heading = fix.heading;
						{ // position discontinuity marker for the e2e convergence check
							double jw = std::chrono::duration_cast<std::chrono::duration<double>>(
								std::chrono::system_clock::now().time_since_epoch()).count();
							std::printf("[%.3f] JUMP %.2f %.2f %.2f\n", jw,
							            fix.xMm / 1000.0f, fix.yMm / 1000.0f, fix.zMm / 1000.0f);
						}
					}
					break;
				}
				case MSG_PLAYER_JOIN:{
					MsgPlayerJoin j;
					if(ReadPlayerJoin(r, j) && cfg.verbose)
						std::printf("[%.3f] JOIN id=%d %s\n", elapsed(), (int)j.playerId, j.name);
					break;
				}
				case MSG_PLAYER_QUIT:{
					uint8_t id = r.ReadU8();
					peers.erase(id);
					seatOf.erase(id);
					break;
				}
				case MSG_KICK:
				case MSG_DISCONNECT:{
					MsgKick k; ReadKick(r, k);
					kicked = (type == MSG_KICK);
					disconnected = true;
					std::printf("[%.3f] KICKED/DISC: %s\n", elapsed(), k.reason);
					failReasons += "kicked ";
					break;
				}
				case MSG_RCON_OK:{
					rconOk = true;
					break;
				}
				case MSG_RCON_RESP:{
					char text[256] = {};
					ReadStringMsg(r, text, sizeof text, 255);
					if(cfg.verbose) std::printf("[%.3f] RCON: %s\n", elapsed(), text);
					if(std::string(text).find("hostname=") != std::string::npos)
						rconCmdOk = true;
					break;
				}
				case MSG_VEH_SPAWN:{
					MsgVehSpawn vs;
					if(ReadVehSpawn(r, vs)){
						seenVehId = vs.vehicleId;
						seenVehModel = vs.modelId;
						// single-entity guard: a SPAWN for a car already in our live
						// set is a duplicate (the bug under test). Count it, don't
						// grow the set past one entry for that id.
						if(liveVehicles.count(vs.vehicleId)){
							dupSpawns++;
							vehSpawnCount[vs.vehicleId]++;
						}else{
							liveVehicles.insert(vs.vehicleId);
						}
						if(liveVehicles.size() > maxLiveVehicles)
							maxLiveVehicles = liveVehicles.size();
						VehView& vv = vehView[vs.vehicleId];
						if(!vv.hasPos){
							vv.hasPos = true;
							vv.x = vs.x; vv.y = vs.y; vv.z = vs.z;
							vv.minX = vv.maxX = vs.x; vv.minY = vv.maxY = vs.y;
						}
						if(cfg.verbose)
							std::printf("[%.3f] VEH_SPAWN id=%d model=%d live=%d\n",
							            elapsed(), (int)vs.vehicleId, (int)vs.modelId,
							            (int)liveVehicles.size());
					}
					break;
				}
				case MSG_VEH_DESPAWN:{
					MsgVehDespawn vd;
					if(ReadVehDespawn(r, vd)){
						liveVehicles.erase(vd.vehicleId);
						vehView.erase(vd.vehicleId);
						if(cfg.verbose)
							std::printf("[%.3f] VEH_DESPAWN id=%d live=%d\n",
							            elapsed(), (int)vd.vehicleId, (int)liveVehicles.size());
					}
					break;
				}
				case MSG_VEH_ENTER:{
					MsgVehEnter m;
					if(ReadVehEnter(r, m)){
						if(m.playerId != selfId){
							vehEntersSeen++;
							// occupancy stays scoped to the entering player: record
							// who the server seated and where
							otherOccupant[m.playerId] = m.vehicleId;
							seatOf[m.playerId] = std::make_pair(m.vehicleId, m.seat);
						}else{
							// a self-enter we did NOT initiate = the server forced us
							// into a car (the "other clients get force-spawned into a
							// duplicate" bug). expectSelfEnter is armed only around our
							// own enter request.
							if(!expectSelfEnter)
								unexpectedSelfEnters++;
							expectSelfEnter = false;
							myVehicle = m.vehicleId;
							mySeat = m.seat;
							myModel = m.modelId;
							if(m.seat == 0) ownVehId = m.vehicleId;
							self.vehicleId = m.vehicleId;
							self.seat = m.seat;
							std::printf("[%.3f] VEH_ENTER self veh=%d seat=%d model=%d\n",
							            elapsed(), (int)m.vehicleId, (int)m.seat, (int)m.modelId);
						}
					}
					break;
				}
				case MSG_VEH_EXIT:{
					MsgVehExit m;
					if(ReadVehExit(r, m) && m.playerId != selfId)
						seatOf.erase(m.playerId);
					else if(r.Ok() && m.playerId == selfId){
						myVehicle = 0xFFFF;
						mySeat = 0xFF;
						self.vehicleId = VEHICLE_NONE;
						self.seat = 0xFF;
						std::printf("[%.3f] VEH_EXIT self\n", elapsed());
					}
					break;
				}
				case MSG_VEHICLE_STATE:{
					VehicleState vst;
					if(ReadVehicleState(r, vst)){
						vehStatesSeen++;
						VehView& vv = vehView[vst.vehicleId];
						vv.x = vst.xMm / 1000.0f; vv.y = vst.yMm / 1000.0f; vv.z = vst.zMm / 1000.0f;
						if(!vv.hasPos){
							vv.minX = vv.maxX = vv.x; vv.minY = vv.maxY = vv.y;
						}
						vv.hasPos = true;
						vv.lastState = elapsed();
						vv.states++;
						vv.minX = std::min(vv.minX, vv.x); vv.maxX = std::max(vv.maxX, vv.x);
						vv.minY = std::min(vv.minY, vv.y); vv.maxY = std::max(vv.maxY, vv.y);
						if(cfg.verbose && vehStatesSeen % 40 == 1)
							std::printf("[%.3f] VEH_STATE veh=%d at %.1f %.1f %.1f\n",
							            elapsed(), (int)vst.vehicleId,
							            vst.xMm / 1000.0f, vst.yMm / 1000.0f, vst.zMm / 1000.0f);
					}
					break;
				}
				case MSG_VOICE:{
					uint8_t from = 0;
					uint8_t packet[VOICE_MAX_PACKET];
					uint8_t len = 0;
					if(cfg.voice && ReadVoice(r, from, packet, len) && r.BitsLeft() == 0 && from != selfId){
						Heard& hd = heard[from];
						hd.frames++;
						auto pit = peers.find(from);
						if(pit != peers.end()){
							float dx = (pit->second.state.xMm - self.xMm) / 1000.0f;
							float dy = (pit->second.state.yMm - self.yMm) / 1000.0f;
							float dz = (pit->second.state.zMm - self.zMm) / 1000.0f;
							float d = std::sqrt(dx * dx + dy * dy + dz * dz);
							if(d > hd.maxDist) hd.maxDist = d;
							// the server relays only within VOICE_RANGE_M (+ slack for stale peer state)
							if(d > VOICE_RANGE_M + 5.0f) voiceOutOfRange++;
						}
					}
					break;
				}
				case MSG_PONG: break;
				default: break;
				}
				enet_packet_destroy(ev.packet);
				break;
			}
			case ENET_EVENT_TYPE_DISCONNECT:
				disconnected = true;
				break;
			default:
				break;
			}
		}

		double t = elapsed();

		// random walk — settle (park) for the last 5 s so the harness can compare
		// final positions exactly across bots
		bool walks = cfg.scenario != "cruise" && cfg.scenario != "watch";
		if(walks && t - lastWalk >= 0.05 && alive && t < cfg.duration - 5.0){
			lastWalk = t;
			std::uniform_real_distribution<float> turn(-0.6f, 0.6f);
			heading += turn(rng);
			float speed = (cfg.scenario == "chaos") ? 8.0f : 3.0f;
			if(cfg.scenario == "chaos"){
				std::uniform_real_distribution<float> jump(-40.0f, 40.0f);
				if(rng() % 40 == 0){ // occasional wild move — expects server correction
					self.xMm = QuantPos(homeX + jump(rng));
					self.yMm = QuantPos(homeY + jump(rng));
				}
			}
			float nx = self.xMm / 1000.0f + std::cos(heading) * speed * 0.05f;
			float ny = self.yMm / 1000.0f + std::sin(heading) * speed * 0.05f;
			// stay near home
			if(std::fabs(nx - homeX) > 45.0f || std::fabs(ny - homeY) > 45.0f)
				heading += 3.14159f;
			else {
				self.xMm = QuantPos(nx);
				self.yMm = QuantPos(ny);
			}
			self.heading = QuantAngle(heading);
			self.velX = QuantVel(std::cos(heading) * speed);
			self.velY = QuantVel(std::sin(heading) * speed);
		}

		// cruise: take a car of our own (like the game's `ride` cheat: the car
		// exists locally first, the server learns about it from VEH_ENTER 0xFFFF)
		// and drive it as the authoritative driver
		if(cfg.scenario == "cruise" && gotSpawn && alive && !carRequested && !parked &&
		   spawnedAt >= 0.0 && t - spawnedAt >= 1.0){
			carRequested = true;
			// the car appears 4 m ahead and the player is seated in it
			self.xMm = QuantPos(self.xMm / 1000.0f + std::cos(heading) * 4.0f);
			self.yMm = QuantPos(self.yMm / 1000.0f + std::sin(heading) * 4.0f);
			self.vehicleId = VEHICLE_UNSYNCED;   // riding, no net id yet
			self.seat = 0;
			circleA = heading - 1.5707963f;      // drive tangentially from here
			circleCX = self.xMm / 1000.0f - std::cos(circleA) * circleR;
			circleCY = self.yMm / 1000.0f - std::sin(circleA) * circleR;
			BitWriter w;
			BeginMsg(w, MSG_VEH_ENTER);
			MsgVehEnter ve;
			ve.playerId = selfId;
			ve.vehicleId = 0xFFFF;
			ve.seat = 0;
			ve.modelId = (uint16_t)(cfg.vehModel > 0 ? cfg.vehModel : 151);
			ve.x = self.xMm / 1000.0f; ve.y = self.yMm / 1000.0f; ve.z = self.zMm / 1000.0f;
			ve.heading = heading;
			WriteVehEnter(w, ve);
			ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
			enet_peer_send(peer, CHAN_RELIABLE, pkt);
			expectSelfEnter = true;
			myVehicle = 0x1234; // provisional until the VEH_ENTER echo
			double jw = std::chrono::duration_cast<std::chrono::duration<double>>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			std::printf("[%.3f] CAR %.2f %.2f %.2f\n", jw,
			            self.xMm / 1000.0f, self.yMm / 1000.0f, self.zMm / 1000.0f);
		}
		if(cfg.scenario == "cruise" && carRequested && !parked && self.vehicleId != VEHICLE_NONE &&
		   t - lastWalk >= 0.05){
			float step = cfg.speed * (float)(t - lastWalk);
			if(step > cfg.speed * 0.1f) step = cfg.speed * 0.1f;
			lastWalk = t;
			float px = self.xMm / 1000.0f, py = self.yMm / 1000.0f, pz = self.zMm / 1000.0f;
			float nx, ny, nz = pz;
			if(cfg.driveTo){
				float dx = cfg.driveX - px, dy = cfg.driveY - py, dz = cfg.driveZ - pz;
				float d = std::sqrt(dx * dx + dy * dy + dz * dz);
				heading = std::atan2(dy, dx);
				if(d <= step){
					nx = cfg.driveX; ny = cfg.driveY; nz = cfg.driveZ;
				}else{
					nx = px + dx / d * step; ny = py + dy / d * step; nz = pz + dz / d * step;
				}
			}else{
				circleA += step / circleR;
				nx = circleCX + std::cos(circleA) * circleR;
				ny = circleCY + std::sin(circleA) * circleR;
				heading = circleA + 1.5707963f;
			}
			if(cfg.jumpAt >= 0.0 && !jumped && t >= cfg.jumpAt){
				// scripted relocation of the car (and us in it)
				jumped = true;
				circleCX += cfg.jumpDist;
				nx += cfg.jumpDist;
				dropState = dropVehState = cfg.dropJump;
				std::printf("[%.3f] RELOCATE +%.0f m%s\n", elapsed(), cfg.jumpDist,
				            cfg.dropJump ? " (flagged packets lost)" : "");
			}
			self.velX = QuantVel((nx - px) / std::max(0.001f, (float)TICK_DT));
			self.velY = QuantVel((ny - py) / std::max(0.001f, (float)TICK_DT));
			self.xMm = QuantPos(nx); self.yMm = QuantPos(ny); self.zMm = QuantPos(nz);
			self.heading = QuantAngle(heading);
			if(cfg.driveTo && nx == cfg.driveX && ny == cfg.driveY && nz == cfg.driveZ && myVehicle != 0x1234 &&
			   myVehicle != 0xFFFF){
				// arrived: park, get out, stay next to the car
				parked = true;
				parkedId = myVehicle;
				parkX = nx; parkY = ny; parkZ = nz;
				self.velX = self.velY = 0;
				BitWriter w;
				BeginMsg(w, MSG_VEH_EXIT);
				MsgVehExit vx;
				vx.playerId = selfId;
				vx.vehicleId = myVehicle;
				WriteVehExit(w, vx);
				ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send(peer, CHAN_RELIABLE, pkt);
				myVehicle = 0xFFFF;
				mySeat = 0xFF;
				self.vehicleId = VEHICLE_NONE;
				self.seat = 0xFF;
				std::printf("[%.3f] PARKED %d %.2f %.2f %.2f\n", elapsed(), (int)parkedId, parkX, parkY, parkZ);
			}
		}

		// watch: optional relocation (a flagged teleport, like an admin warp)
		if(cfg.scenario == "watch" && cfg.gotoAt >= 0.0 && !gone && gotSpawn && t >= cfg.gotoAt){
			gone = true;
			self.xMm = QuantPos(cfg.gotoX); self.yMm = QuantPos(cfg.gotoY); self.zMm = QuantPos(cfg.gotoZ);
			std::printf("[%.3f] GOTO %.1f %.1f %.1f\n", elapsed(), cfg.gotoX, cfg.gotoY, cfg.gotoZ);
		}

		// state send at tick rate
		if(t - lastStateSend >= 1.0 / 20.0 && gotSpawn){
			lastStateSend = t;
			self.health = self.health; // authoritative from server
			sendState();
		}

		// proximity voice (-voice): one 20 ms Opus tone frame every 20 ms
		if(cfg.voice && gotSpawn && t - lastVoice >= 0.02){
			lastVoice = t;
			int16_t pcm[VOICE_FRAME_SAMPLES];
			for(int k = 0; k < VOICE_FRAME_SAMPLES; k++){
				pcm[k] = (int16_t)(8000.0f * std::sin(voicePhase));
				voicePhase += 2.0f * 3.14159265f * 440.0f / VOICE_SAMPLE_RATE;
				if(voicePhase >= 6.2831853f) voicePhase -= 6.2831853f;
			}
			uint8_t packet[VOICE_MAX_PACKET];
			int len = voiceEnc.Encode(pcm, packet);
			if(len > 0){
				BitWriter w;
				BeginMsg(w, MSG_VOICE);
				WriteVoice(w, selfId, packet, (uint8_t)len);
				ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), 0);
				enet_peer_send(peer, CHAN_STATE, pkt);
				voiceSent++;
			}
		}

		// chat every 3s
		if(t - lastChat >= 3.0 && gotSpawn){
			lastChat = t;
			char msg[128];
			snprintf(msg, sizeof msg, "BOTMSG %s %d", cfg.name.c_str(), ++chatSeq);
			sendChat(msg);
		}

		// combat: damage a random other player every 0.5s
		if(cfg.scenario == "combat" && t - lastDamage >= 0.5 && alive && !peers.empty()){
			lastDamage = t;
			auto it = peers.begin();
			std::uniform_int_distribution<size_t> d(0, peers.size() - 1);
			std::advance(it, d(rng));
			BitWriter w;
			BeginMsg(w, MSG_DAMAGE);
			MsgDamage dmg;
			dmg.targetId = it->first;
			dmg.attackerId = selfId;
			dmg.weaponId = 0;
			dmg.damage = 12;
			WriteDamage(w, dmg);
			ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
			enet_peer_send(peer, CHAN_RELIABLE, pkt);
		}

		// drive: cycle enter/drive/exit — join a vehicle someone else created
		// when one exists (passenger flow), otherwise spawn our own and drive
		if(cfg.scenario == "drive" && gotSpawn && t - lastVehCycle >= 8.0){
			lastVehCycle = t;
			if(myVehicle == 0xFFFF){
				BitWriter w;
				BeginMsg(w, MSG_VEH_ENTER);					MsgVehEnter ve;
					ve.playerId = selfId;
					if(seenVehId != 0xFFFF && seenVehId != ownVehId){
						ve.vehicleId = seenVehId; // someone else's car: passenger
						ve.seat = 1;
					}else if(ownVehId != 0xFFFF){
						ve.vehicleId = ownVehId; // back into our own car: drive
						ve.seat = 0;
					}else{
						ve.vehicleId = 0xFFFF;  // new car: we drive
						ve.seat = 0;
					}
					ve.modelId = seenVehModel ? seenVehModel
					             : (uint16_t)(cfg.vehModel > 0 ? cfg.vehModel : 151);
					// v6: where the car is (the bot's car rides on its own position)
					ve.x = self.xMm / 1000.0f; ve.y = self.yMm / 1000.0f; ve.z = self.zMm / 1000.0f;
					ve.heading = heading;
				WriteVehEnter(w, ve);
				ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send(peer, CHAN_RELIABLE, pkt);
				expectSelfEnter = true; // the server's VEH_ENTER echo for us is expected
				myVehicle = 0x1234; // provisional; the server's VEH_ENTER echo confirms
			}else{
				BitWriter w;
				BeginMsg(w, MSG_VEH_EXIT);
				MsgVehExit vx;
				vx.playerId = selfId;
				vx.vehicleId = (myVehicle == 0x1234) ? 0 : myVehicle; // 0 = resolve by occupancy
				WriteVehExit(w, vx);
				ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send(peer, CHAN_RELIABLE, pkt);
				myVehicle = 0xFFFF;
				mySeat = 0xFF;
				self.vehicleId = VEHICLE_NONE;
				self.seat = 0xFF;
			}
		}

		// stream vehicle state while driving (seat 0 is authoritative driver)
		if(myVehicle != 0xFFFF && myVehicle != 0x1234 && mySeat == 0 && gotSpawn &&
		   t - lastVehState >= 1.0 / 20.0){
			lastVehState = t;
			VehicleState vs;
			vs.vehicleId = myVehicle;
			vs.modelId = myModel ? myModel : (uint16_t)cfg.vehModel;
			vs.xMm = self.xMm; vs.yMm = self.yMm; vs.zMm = self.zMm;
			vs.heading = self.heading;
			vs.velX = self.velX; vs.velY = self.velY;
			vs.flags = VEH_ENGINE;
			vs.health = 1000;
			if(dropVehState){
				dropVehState = false;   // lost in transit (cruise -drop-jump)
			}else{
				BitWriter w;
				BeginMsg(w, MSG_VEHICLE_STATE);
				WriteVehicleState(w, vs);
				ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), 0);
				enet_peer_send(peer, CHAN_STATE, pkt);
			}
		}

		// ping every 5s
		if(t - lastPing >= 5.0){
			lastPing = t;
			BitWriter w;
			BeginMsg(w, MSG_PING);
			w.WriteU32((uint32_t)(t * 1000));
			ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
			enet_peer_send(peer, CHAN_RELIABLE, pkt);
		}

		// rcon follow-up command(s)
		if(rconTimer > 0 && t >= rconTimer){
			rconTimer = -1;
			BitWriter w;
			BeginMsg(w, MSG_RCON_CMD);
			WriteStringMsg(w, "status", 255);
			ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
			enet_peer_send(peer, CHAN_RELIABLE, pkt);
			for(size_t i = 0; i < cfg.rconCmds.size(); i++){
				BitWriter wc;
				BeginMsg(wc, MSG_RCON_CMD);
				WriteStringMsg(wc, cfg.rconCmds[i].c_str(), 255);
				ENetPacket* pc = enet_packet_create(wc.Data(), wc.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send(peer, CHAN_RELIABLE, pc);
				if(cfg.verbose)
					std::printf("[%.3f] RCON CMD: %s\n", elapsed(), cfg.rconCmds[i].c_str());
			}
		}

		// 1 Hz telemetry (wall-clock: e2e aligns samples across staggered bots)
		if(t - lastTelemetry >= 1.0){
			lastTelemetry = t;
			double wall = std::chrono::duration_cast<std::chrono::duration<double>>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			std::printf("[%.3f] IAM %.2f %.2f %.2f hp=%d\n",
			            wall, self.xMm / 1000.0f, self.yMm / 1000.0f, self.zMm / 1000.0f,            (int)self.health);
			if(cfg.scenario == "cruise"){
				if(myVehicle != 0xFFFF && myVehicle != 0x1234 && mySeat == 0)
					std::printf("[%.3f] IAMVEH %d %.2f %.2f %.2f\n", wall, (int)myVehicle,
					            self.xMm / 1000.0f, self.yMm / 1000.0f, self.zMm / 1000.0f);
				else if(parked)
					std::printf("[%.3f] IAMVEH %d %.2f %.2f %.2f\n", wall, (int)parkedId, parkX, parkY, parkZ);
			}
			if(cfg.scenario == "watch"){
				for(auto& kv : vehView){
					if(!kv.second.hasPos) continue;
					std::printf("[%.3f] SEESVEH %d %.2f %.2f %.2f age=%.2f states=%d\n", wall, (int)kv.first,
					            kv.second.x, kv.second.y, kv.second.z,
					            kv.second.lastState >= 0.0 ? (float)(t - kv.second.lastState) : -1.0f,
					            kv.second.states);
				}
			}
			if(cfg.voice){
				for(auto& kv : heard)
					std::printf("[%.3f] HEARD %d frames=%d maxdist=%.2f\n", wall, (int)kv.first,
					            kv.second.frames, kv.second.maxDist);
			}
			for(auto& kv : peers){
				if(kv.second.lastSeen > t - 5.0)
					std::printf("[%.3f] SEES %d %.2f %.2f %.2f age=%.2f\n", wall, (int)kv.first,
					            kv.second.state.xMm / 1000.0f,
					            kv.second.state.yMm / 1000.0f,
					            kv.second.state.zMm / 1000.0f,
					            (float)(t - kv.second.lastSeen));
			}
		}

		Sleep(1);
	}

	// --- watch assertions: evaluated on the live view, before disconnecting ---
	std::string watchSummary;
	if(cfg.scenario == "watch" && (cfg.expectDriver || cfg.expectNear)){
		double endT = elapsed();
		char line[512];
		if(cfg.expectDriver){
			// a peer seated as driver (seat 0) of a vehicle we know, per the
			// relayed player state or per VEH_ENTER bookkeeping
			int driver = -1, vehicle = -1;
			bool byState = false, byEnter = false;
			for(auto& kv : peers){
				const PlayerState& st = kv.second.state;
				if(st.vehicleId != VEHICLE_NONE && st.vehicleId != VEHICLE_UNSYNCED && st.seat == 0 &&
				   vehView.count(st.vehicleId)){
					driver = kv.first; vehicle = st.vehicleId; byState = true;
				}
			}
			for(auto& kv : seatOf){
				if(kv.second.second == 0 && vehView.count(kv.second.first)){
					if(driver < 0){ driver = kv.first; vehicle = kv.second.first; }
					if(driver == kv.first && vehicle == kv.second.first) byEnter = true;
				}
			}
			if(driver < 0){
				failReasons += "no_driver_seen ";
				snprintf(line, sizeof line, "driver=none peers=%d vehicles=%d",
				         (int)peers.size(), (int)vehView.size());
			}else{
				const VehView& vv = vehView[(uint16_t)vehicle];
				const PeerView& pv = peers[(uint8_t)driver];
				float span = std::max(vv.maxX - vv.minX, vv.maxY - vv.minY);
				float vehAge = vv.lastState >= 0.0 ? (float)(endT - vv.lastState) : 999.0f;
				float pedAge = peers.count((uint8_t)driver) ? (float)(endT - pv.lastSeen) : 999.0f;
				float dx = pv.state.xMm / 1000.0f - vv.x, dy = pv.state.yMm / 1000.0f - vv.y,
				      dz = pv.state.zMm / 1000.0f - vv.z;
				float gap = std::sqrt(dx * dx + dy * dy + dz * dz);
				if(!liveVehicles.count((uint16_t)vehicle)) failReasons += "vehicle_never_spawned ";
				if(vv.states < 20 || span < 5.0f) failReasons += "vehicle_not_moving ";
				if(vehAge > 2.0f) failReasons += "vehicle_state_stale ";
				if(pedAge > 2.0f) failReasons += "driver_state_stale ";
				if(gap > 6.0f) failReasons += "driver_not_in_vehicle ";
				snprintf(line, sizeof line,
				         "driver=%d veh=%d by_state=%d by_enter=%d veh_states=%d span=%.1f veh_age=%.2f ped_age=%.2f ped_veh_gap=%.2f",
				         driver, vehicle, (int)byState, (int)byEnter, vv.states, span, vehAge, pedAge, gap);
			}
			watchSummary += line;
			int missing = 0;
			for(uint8_t id : peerStateSeen)
				if(!peerSpawnSeen.count(id) && peers.count(id)) missing++;
			if(missing) failReasons += "peer_spawn_missing ";
			snprintf(line, sizeof line, " peers_without_spawn=%d", missing);
			watchSummary += line;
		}
		if(cfg.expectNear){
			float best = 1e9f;
			int bestId = -1;
			for(auto& kv : vehView){
				if(!kv.second.hasPos) continue;
				float dx = kv.second.x - cfg.nearX, dy = kv.second.y - cfg.nearY, dz = kv.second.z - cfg.nearZ;
				float d = std::sqrt(dx * dx + dy * dy + dz * dz);
				if(d < best){ best = d; bestId = kv.first; }
			}
			if(best > cfg.nearR) failReasons += "no_vehicle_near_expected ";
			snprintf(line, sizeof line, " nearest_veh=%d dist=%.2f (want <= %.1f)", bestId, best, cfg.nearR);
			watchSummary += line;
		}
		std::printf("[BOT] WATCH %s\n", watchSummary.c_str());
	}
	if(cfg.scenario == "cruise")
		std::printf("[BOT] CRUISE veh=%d parked=%d corrections_ignored=%d\n",
		            (int)(parked ? parkedId : myVehicle), (int)parked, correctionsIgnored);

	enet_peer_disconnect(peer, 0);
	{
		ENetEvent ev;
		double tEnd = NowSec();
		while(NowSec() - tEnd < 1.0){
			while(enet_host_service(client, &ev, 0) > 0)
				if(ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
			Sleep(5);
		}
	}
	enet_host_destroy(client);
	enet_deinitialize();

	// ---------------- assertions ----------------
	if(!gotWelcome) failReasons += "no_welcome ";
	if(!gotSpawn) failReasons += "no_spawn ";
	if(!gotChatExpect) failReasons += "no_expected_chat ";
	if(!sawPeer && cfg.scenario != "solo") failReasons += "no_peer_state ";
	if(cfg.scenario == "combat" && !sawDeath) failReasons += "no_death ";
	if(cfg.scenario == "combat" && deathsSelf == 0 && !failReasons.empty()) {}
	if(!rconOk) failReasons += "rcon_auth_failed ";
	if(!rconCmdOk) failReasons += "rcon_cmd_failed ";
	if(statesSent < (int)(cfg.duration * 10)) failReasons += "too_few_states ";
	if(kicked) failReasons += "kicked ";

	// --- vehicle-sync integrity assertions ---
	// no car was ever duplicated (same id spawned twice without a despawn)
	if(dupSpawns > 0) failReasons += "duplicate_vehicle_spawn ";
	// we were never forced into a car we did not enter
	if(unexpectedSelfEnters > 0) failReasons += "forced_into_vehicle ";
	// the drive scenario must have actually exercised an enter-and-drive cycle
	if(cfg.scenario == "drive"){
		if(ownVehId == 0xFFFF && vehStatesSeen == 0 && statesSent > 0)
			failReasons += "drive_never_entered ";
	}
	// machine-readable integrity line for the e2e harness (single-entity check)
	std::printf("[BOT] VEHSUMMARY live_now=%d max_live=%d dup_spawns=%d "
	            "forced_self_enters=%d other_occupants=%d own_veh=%d\n",
	            (int)liveVehicles.size(), (int)maxLiveVehicles, dupSpawns,
	            unexpectedSelfEnters, (int)otherOccupant.size(), (int)ownVehId);
	if(cfg.voice){
		if(voiceSent == 0) failReasons += "no_voice_sent ";
		if(voiceOutOfRange > 0) failReasons += "voice_out_of_range ";
		std::printf("[BOT] VOICE sent=%d heard_speakers=%d out_of_range=%d\n",
		            voiceSent, (int)heard.size(), voiceOutOfRange);
	}

	bool pass = failReasons.empty();
	std::printf("[BOT] RESULT: %s %s| states=%d corrections=%d deaths=%d spawns=%d "
	            "vehstates=%d vehenters=%d veh=%d\n",
	            pass ? "PASS" : "FAIL", failReasons.c_str(),
	            statesSent, correctionsReceived, deathsSelf, spawnsSeen,
	            vehStatesSeen, vehEntersSeen, (int)myVehicle);
	return pass ? 0 : 1;
}
