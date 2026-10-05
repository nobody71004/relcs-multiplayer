// net/bot/main.cpp — relcs-netbot: scripted headless client for E2E testing.
//
// Usage:
//   relcs-netbot -name BotA -server 127.0.0.1:7777 [-scenario roam|combat|drive|chaos]
//                [-duration 30] [-expect-chat SUBSTR] [-rcon PW] [-seed N] [-verbose]
//
// Prints 1 Hz telemetry lines consumed by the E2E harness:
//   [<unix t>] IAM <x> <y> <z>       — this bot's authoritative position
//   [<unix t>] SEES <id> <x> <y> <z> age=<s> — last relayed state of another
//                                   player + snapshot age at print time
//   (wall-clock timestamps so e2e can align samples across staggered bots)
// and ends with:
//   [BOT] RESULT: PASS|FAIL <reasons>

#include "rnet_protocol.h"
#include <enet/enet.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <string>
#include <map>
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
	std::string rconPassword;
	uint32_t seed = 0;
	bool verbose = false;
	bool discover = false;
	bool masterQuery = false;
	int vehModel = 0; // vehicle model for self-created cars (0 = guess)
};

// ---------------------------------------------------------------------------
// Utility modes (no game session)
// ---------------------------------------------------------------------------
#include <winsock2.h>
#include <ws2tcpip.h>

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
		sockaddr_in from; int flen = sizeof from;
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
		else if(a == "-rcon" && hasNext) cfg.rconPassword = argv[++i];
		else if(a == "-seed" && hasNext) cfg.seed = (uint32_t)atoi(argv[++i]);
		else if(a == "-verbose") cfg.verbose = true;
		else if(a == "-discover") cfg.discover = true;
		else if(a == "-master-query") cfg.masterQuery = true;
		else if(a == "-vehmodel" && hasNext) cfg.vehModel = atoi(argv[++i]);
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
						if(cfg.verbose)
							std::printf("[%.3f] VEH_SPAWN id=%d model=%d\n",
							            elapsed(), (int)vs.vehicleId, (int)vs.modelId);
					}
					break;
				}
				case MSG_VEH_ENTER:{
					MsgVehEnter m;
					if(ReadVehEnter(r, m)){
						if(m.playerId != selfId){
							vehEntersSeen++;
						}else{
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
					if(ReadVehExit(r, m) && m.playerId == selfId){
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
						if(cfg.verbose && vehStatesSeen % 40 == 1)
							std::printf("[%.3f] VEH_STATE veh=%d at %.1f %.1f %.1f\n",
							            elapsed(), (int)vst.vehicleId,
							            vst.xMm / 1000.0f, vst.yMm / 1000.0f, vst.zMm / 1000.0f);
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
		if(t - lastWalk >= 0.05 && alive && t < cfg.duration - 5.0){
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

		// state send at tick rate
		if(t - lastStateSend >= 1.0 / 20.0 && gotSpawn){
			lastStateSend = t;
			self.health = self.health; // authoritative from server
			sendState();
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
				WriteVehEnter(w, ve);
				ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send(peer, CHAN_RELIABLE, pkt);
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
			BitWriter w;
			BeginMsg(w, MSG_VEHICLE_STATE);
			WriteVehicleState(w, vs);
			ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), 0);
			enet_peer_send(peer, CHAN_STATE, pkt);
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

		// rcon follow-up command
		if(rconTimer > 0 && t >= rconTimer){
			rconTimer = -1;
			BitWriter w;
			BeginMsg(w, MSG_RCON_CMD);
			WriteStringMsg(w, "status", 255);
			ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
			enet_peer_send(peer, CHAN_RELIABLE, pkt);
		}

		// 1 Hz telemetry (wall-clock: e2e aligns samples across staggered bots)
		if(t - lastTelemetry >= 1.0){
			lastTelemetry = t;
			double wall = std::chrono::duration_cast<std::chrono::duration<double>>(
				std::chrono::system_clock::now().time_since_epoch()).count();
			std::printf("[%.3f] IAM %.2f %.2f %.2f hp=%d\n",
			            wall, self.xMm / 1000.0f, self.yMm / 1000.0f, self.zMm / 1000.0f,
			            (int)self.health);
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

	bool pass = failReasons.empty();
	std::printf("[BOT] RESULT: %s %s| states=%d corrections=%d deaths=%d spawns=%d "
	            "vehstates=%d vehenters=%d veh=%d\n",
	            pass ? "PASS" : "FAIL", failReasons.c_str(),
	            statesSent, correctionsReceived, deathsSelf, spawnsSeen,
	            vehStatesSeen, vehEntersSeen, (int)myVehicle);
	return pass ? 0 : 1;
}
