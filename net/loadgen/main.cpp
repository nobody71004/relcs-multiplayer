// net/loadgen/main.cpp — relcs-loadgen: N simulated players from one process.
//
// The netbot is a scripted *assertion* client: one bot, one scenario, PASS/FAIL.
// This tool is a *load* client: it packs as many peers as the server will take
// into one ENet host and drives each of them like a real player — handshake,
// spawn request, 20 Hz state, occasional chat, keepalive ping — so a server can
// be measured at 1000 or 2000 players without 2000 machines.
//
// Usage:
//   relcs-loadgen -server 127.0.0.1:7777 -count 2000 -spread 1500 -duration 180
//                 [-ramp 60] [-state-hz 20] [-name-prefix Load] [-cluster 100]
//                 [-chat-pct 0 -chat-sec 60] [-veh-pct 0] [-csv load.csv] [-seed N]
//
// Placement: every bot is assigned a home on a square grid of side -spread
// metres centred on the world origin, and teleports there on its first state
// (flagged with MOVEFLAG_TELEPORT, which the server treats as a reloc). That is
// what makes the AOI fan-out realistic: a server whose players are all inside
// one 200 m bubble is only being measured against its worst case.
//
// Output: one [load] line per second (also CSV with -csv) and a final
// [LOADGEN] RESULT line for scripts.

#include "rnet_protocol.h"
#include "compat.h"
#include <enet/enet.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

using namespace rnet;

static double NowSec()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

struct LoadBot {
	ENetPeer* peer = nullptr;
	PlayerId id = 0;
	bool connected = false;
	bool welcomed = false;
	bool spawned = false;
	bool dead = false;              // kicked or dropped: never revived
	bool sentSpawnReq = false;
	int teleportStates = 0;         // flagged relocations still owed to the server
	float x = 0, y = 0, z = 0;
	float homeX = 0, homeY = 0;
	float heading = 0;
	float speed = 3.0f;
	double lastState = 0, lastChat = 0, lastPing = 0;
	uint32_t rng = 1;
	const char* kickReason = nullptr;
};

struct Config {
	std::string host = "127.0.0.1";
	uint16_t port = DEFAULT_PORT;
	int count = 100;
	float spread = 1500.0f;     // side of the square bots are scattered over
	float cluster = 0.0f;       // >0: put everyone inside this radius instead
	double duration = 60.0;
	double ramp = 30.0;         // seconds over which connections are opened
	double stateHz = 20.0;
	double chatSec = 60.0;
	double chatPct = 0.0;       // % of bots that chat at all
	double vehPct = 0.0;        // % of bots that drive (server caps at MAX_VEHICLES)
	std::string prefix = "Load";
	std::string csv;
	uint32_t seed = 12345;
	bool decode = false;        // parse batch payloads (CPU heavy); counting is enough for load
	int gridBase = 0;           // first slot in the placement grid (sharded fleets)
	int gridTotal = 0;          // total slots across all shards (0 = one unsharded fleet)
};

static uint32_t NextRand(uint32_t& s)
{
	// xorshift32: cheap, and each bot's stream only depends on its own seed
	uint32_t x = s ? s : 1u;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	s = x;
	return x;
}

static float RandUnit(uint32_t& s) { return (float)(NextRand(s) & 0xFFFFFF) / (float)0xFFFFFF; }

static void SendHello(ENetHost* host, LoadBot& b, const Config& cfg, int index)
{
	MsgHello h;
	CopyStr(h.name, sizeof h.name, (cfg.prefix + std::to_string(index)).c_str());
	BitWriter w;
	BeginMsg(w, MSG_HELLO);
	WriteHello(w, h);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send(b.peer, CHAN_RELIABLE, pkt);
	(void)host;
}

static void SendPing(ENetHost* host, LoadBot& b, double t)
{
	BitWriter w;
	BeginMsg(w, MSG_PING);
	w.WriteU32((uint32_t)(t * 1000.0));
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send(b.peer, CHAN_RELIABLE, pkt);
	(void)host;
}

static void SendChat(ENetHost* host, LoadBot& b, const Config& cfg)
{
	char text[MAX_CHAT_LEN];
	snprintf(text, sizeof text, "load %u", (unsigned)b.id);
	BitWriter w;
	BeginMsg(w, MSG_CHAT);
	MsgChat c;
	c.fromId = b.id;
	CopyStr(c.text, sizeof c.text, text);
	WriteChat(w, c);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send(b.peer, CHAN_RELIABLE, pkt);
	(void)cfg;
}

// One real-player state tick: walk, occasionally turn, clamp inside a small
// area around home, and report at the negotiated rate.
static void SendState(ENetHost* host, LoadBot& b, double t, double dt, bool teleport, bool reliable)
{
	if(RandUnit(b.rng) < 0.02f)
		b.heading += (RandUnit(b.rng) - 0.5f) * 1.2f;

	float speed = b.speed * (0.6f + RandUnit(b.rng) * 0.8f);
	float nx = b.x + std::cos(b.heading) * speed * (float)dt;
	float ny = b.y + std::sin(b.heading) * speed * (float)dt;
	if(std::fabs(nx - b.homeX) > 40.0f || std::fabs(ny - b.homeY) > 40.0f)
		b.heading += 3.14159265f;
	else { b.x = nx; b.y = ny; }

	PlayerState st;
	st.playerId = b.id;
	st.xMm = QuantPos(b.x);
	st.yMm = QuantPos(b.y);
	st.zMm = QuantPos(b.z);
	st.heading = QuantAngle(b.heading);
	st.velX = QuantVel(std::cos(b.heading) * speed);
	st.velY = QuantVel(std::sin(b.heading) * speed);
	st.health = 100;
	st.moveFlags = teleport ? MOVEFLAG_TELEPORT : 0;

	BitWriter w;
	BeginMsg(w, MSG_PLAYER_STATE);
	WritePlayerState(w, st);
	// A relocation must not ride the unreliable channel: at 2k the server sees
	// ~25% packet loss, and one dropped flag turns the next state into a
	// 1.5 km "teleport" the anti-cheat correctly rejects. Real clients send
	// a warp reliably (and repeat it) for exactly this reason.
	enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), flags);
	enet_peer_send(b.peer, CHAN_STATE, pkt);
	(void)host; (void)t;
}

static void Usage()
{
	std::printf(
	    "relcs-loadgen -server HOST:PORT -count N [-spread M] [-cluster R] [-duration S]\n"
	    "             [-ramp S] [-state-hz H] [-chat-pct P] [-chat-sec S] [-veh-pct P]\n"
	    "             [-name-prefix P] [-seed N] [-csv PATH]\n"
	    "             [-decode 0|1] [-grid-base N] [-grid-total N]\n"
	    "  -decode 1   parse every relayed state (protocol check); default counts only\n"
	    "  -grid-*     shard fleets: each process owns a slice of one placement grid\n");
}

int main(int argc, char** argv)
{
	Config cfg;
	for(int i = 1; i < argc; i++){
		std::string a = argv[i];
		bool hasNext = (i + 1 < argc);
		if(a == "-server" && hasNext){
			std::string s = argv[++i];
			size_t c = s.find(':');
			cfg.host = s.substr(0, c);
			if(c != std::string::npos) cfg.port = (uint16_t)atoi(s.c_str() + c + 1);
		}
		else if(a == "-count" && hasNext) cfg.count = atoi(argv[++i]);
		else if(a == "-spread" && hasNext) cfg.spread = (float)atof(argv[++i]);
		else if(a == "-cluster" && hasNext) cfg.cluster = (float)atof(argv[++i]);
		else if(a == "-duration" && hasNext) cfg.duration = atof(argv[++i]);
		else if(a == "-ramp" && hasNext) cfg.ramp = atof(argv[++i]);
		else if(a == "-state-hz" && hasNext) cfg.stateHz = atof(argv[++i]);
		else if(a == "-chat-pct" && hasNext) cfg.chatPct = atof(argv[++i]);
		else if(a == "-chat-sec" && hasNext) cfg.chatSec = atof(argv[++i]);
		else if(a == "-veh-pct" && hasNext) cfg.vehPct = atof(argv[++i]);
		else if(a == "-name-prefix" && hasNext) cfg.prefix = argv[++i];
		else if(a == "-seed" && hasNext) cfg.seed = (uint32_t)atoi(argv[++i]);
		else if(a == "-csv" && hasNext) cfg.csv = argv[++i];
		else if(a == "-decode" && hasNext) cfg.decode = (atoi(argv[++i]) != 0);
		else if(a == "-grid-base" && hasNext) cfg.gridBase = atoi(argv[++i]);
		else if(a == "-grid-total" && hasNext) cfg.gridTotal = atoi(argv[++i]);
		else if(a == "-h" || a == "--help"){ Usage(); return 0; }
	}
	if(cfg.count < 1 || cfg.count > rnet::MAX_SERVER_PLAYERS){
		std::printf("[LOADGEN] RESULT: FAIL count must be 1..%d\n", MAX_SERVER_PLAYERS);
		return 2;
	}
	if(cfg.stateHz < 0.1 || cfg.stateHz > 60.0) cfg.stateHz = 20.0;
	if(cfg.spread < 1.0f) cfg.spread = 1.0f;

	if(enet_initialize() != 0){
		std::printf("[LOADGEN] RESULT: FAIL enet_init\n");
		return 1;
	}

	ENetHost* host = enet_host_create(nullptr, (size_t)cfg.count + 16, 2, 0, 0);
	if(!host){
		std::printf("[LOADGEN] RESULT: FAIL host_create\n");
		return 1;
	}
	host->maximumPacketSize = 32 * 1024;
	// A thousand peers receive tens of MB/s of relay traffic; the default
	// socket buffer would drop bursts before this process can drain them and
	// the run would report client-side loss as if it were the server's.
	enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF, 8 * 1024 * 1024);
	enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF, 2 * 1024 * 1024);

	ENetAddress addr;
	if(enet_address_set_host(&addr, cfg.host.c_str()) < 0){
		std::printf("[LOADGEN] RESULT: FAIL bad host %s\n", cfg.host.c_str());
		return 1;
	}
	addr.port = cfg.port;

	// Placement: square grid, or a dense cluster for the worst-case AOI test.
	std::vector<LoadBot> bots((size_t)cfg.count);
	// A shard places its bots in a slice of the *global* grid, so two processes
	// driving 500 bots each cover 1000 distinct homes instead of overlapping.
	int total = cfg.gridTotal > 0 ? cfg.gridTotal : cfg.count;
	if(total < cfg.gridBase + cfg.count) total = cfg.gridBase + cfg.count;
	int side = (int)std::ceil(std::sqrt((double)total));
	float step = cfg.spread / (float)(side > 1 ? side - 1 : 1);
	for(int i = 0; i < cfg.count; i++){
		LoadBot& b = bots[(size_t)i];
		int slot = cfg.gridBase + i;
		b.rng = cfg.seed + (uint32_t)slot * 2654435761u;
		if(cfg.cluster > 0.0f){
			float ang = RandUnit(b.rng) * 6.2831853f;
			float rad = std::sqrt(RandUnit(b.rng)) * cfg.cluster;
			b.homeX = std::cos(ang) * rad;
			b.homeY = std::sin(ang) * rad;
		}else{
			b.homeX = -cfg.spread * 0.5f + step * (float)(slot % side);
			b.homeY = -cfg.spread * 0.5f + step * (float)(slot / side);
		}
		b.x = b.homeX;
		b.y = b.homeY;
		b.z = 0.0f;
		b.heading = RandUnit(b.rng) * 6.2831853f;
		b.speed = 2.0f + RandUnit(b.rng) * 6.0f;
	}

	FILE* csv = nullptr;
	if(!cfg.csv.empty()){
		csv = std::fopen(cfg.csv.c_str(), "w");
		if(csv){
			std::fprintf(csv, "t,connected,welcomed,spawned,dead,states_out,recv_pkts,"
			                  "bytes_out,bytes_in,rtt_avg_ms,rtt_max_ms\n");
			std::fflush(csv);
		}
	}

	std::printf("[LOADGEN] target=%s:%u count=%d spread=%.0f cluster=%.0f ramp=%.0fs "
	            "state_hz=%.1f duration=%.0fs\n",
	            cfg.host.c_str(), (unsigned)cfg.port, cfg.count, cfg.spread, cfg.cluster,
	            cfg.ramp, cfg.stateHz, cfg.duration);

	const double t0 = NowSec();
	double nextConnect = 0.0;
	double connectEvery = cfg.ramp > 0.0 ? cfg.ramp / (double)cfg.count : 0.0;
	int nextIndex = 0;
	int refused = 0, dropped = 0;
	uint64_t statesOut = 0, statesIn = 0, stateBatchesIn = 0;
	uint64_t lastSent = host->totalSentData, lastRecv = host->totalReceivedData;
	uint64_t lastPkRecv = host->totalReceivedPackets;
	double lastReport = 0.0;
	char lastKick[128] = {};

	auto elapsed = [&]{ return NowSec() - t0; };

	while(elapsed() < cfg.duration){
		// ramp: open connections over -ramp seconds so a join wave stays realistic
		if(nextIndex < cfg.count && elapsed() >= nextConnect){
			LoadBot& b = bots[(size_t)nextIndex];
			ENetPeer* peer = enet_host_connect(host, &addr, 2, 0);
			if(peer){
				peer->data = (void*)(intptr_t)(nextIndex + 1);   // 1-based index
				b.peer = peer;
			}
			nextIndex++;
			nextConnect = elapsed() + connectEvery;
		}

		ENetEvent ev;
		while(enet_host_service(host, &ev, 0) > 0){
			switch(ev.type){
			case ENET_EVENT_TYPE_CONNECT:{
				intptr_t idx = (intptr_t)ev.peer->data;
				if(idx <= 0) break;
				LoadBot& b = bots[(size_t)(idx - 1)];
				b.connected = true;
				SendHello(host, b, cfg, (int)(idx - 1));
				break;
			}
			case ENET_EVENT_TYPE_RECEIVE:{
				BitReader r(ev.packet->data, ev.packet->dataLength);
				MsgType type = (MsgType)r.ReadU8();
				intptr_t idx = (intptr_t)ev.peer->data;
				// messages that do not name a player only need counting
				if(type == MSG_STATE_BATCH) stateBatchesIn++;
				if(idx <= 0){ enet_packet_destroy(ev.packet); break; }
				LoadBot& b = bots[(size_t)(idx - 1)];
				switch(type){
				case MSG_WELCOME:{
					MsgWelcome wel;
					if(ReadWelcome(r, wel)){
						b.welcomed = true;
						b.id = wel.playerId;
						if(!b.sentSpawnReq){
							b.sentSpawnReq = true;
							BitWriter w;
							BeginMsg(w, MSG_SPAWN_REQ);
							ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(),
							                                     ENET_PACKET_FLAG_RELIABLE);
							enet_peer_send(b.peer, CHAN_RELIABLE, pkt);
						}
					}
					break;
				}
				case MSG_SPAWN:{
					MsgSpawn sp;
					if(ReadSpawn(r, sp)){
						if(sp.playerId == b.id){
							b.spawned = true;
							// the server spawn point is shared; walk home and flag
							// the relocation so it is accepted as a legit teleport
							b.x = b.homeX;
							b.y = b.homeY;
							b.z = 0.0f;
							// Repeat the flag for ~1 s: the first state goes reliable,
							// the rest are cheap insurance against an order inversion
							// while the reliable one is still in flight (the flood guard
							// allows 200 flagged teleports per 5 s).
							b.teleportStates = 20;
						}
					}
					break;
				}
				case MSG_STATE_BATCH:{
					// The point of a load client is server egress, not the client's
					// scene graph: decoding every relayed state costs more CPU than
					// the server spends producing it, and a saturated load host then
					// under-sends and skews the run. -decode 1 keeps full parsing
					// for protocol checks.
					if(cfg.decode){
						PlayerState st[MAX_STATES_PER_PACKET];
						int n = ReadStateBatch(r, st, MAX_STATES_PER_PACKET);
						if(n > 0) statesIn += (uint64_t)n;
					}else{
						uint16_t n = r.ReadU16();
						if(r.Ok() && n <= MAX_STATES_PER_PACKET) statesIn += n;
					}
					break;
				}
				case MSG_ROSTER:{
					MsgRosterEntry es[MAX_ROSTER_PER_PACKET];
					ReadRoster(r, es, MAX_ROSTER_PER_PACKET);
					break;
				}
				case MSG_KICK:
				case MSG_DISCONNECT:{
					MsgKick k; ReadKick(r, k);
					CopyStr(lastKick, sizeof lastKick, k.reason);
					if(type == MSG_KICK) refused++; else dropped++;
					b.dead = true;
					break;
				}
				default: break;
				}
				enet_packet_destroy(ev.packet);
				break;
			}
			case ENET_EVENT_TYPE_DISCONNECT:{
				intptr_t idx = (intptr_t)ev.peer->data;
				if(idx > 0){
					LoadBot& b = bots[(size_t)(idx - 1)];
					if(!b.dead){ dropped++; b.dead = true; }
				}
				break;
			}
			default: break;
			}
		}

		// per-bot traffic
		double t = elapsed();
		double dt = 1.0 / cfg.stateHz;
		for(int i = 0; i < cfg.count; i++){
			LoadBot& b = bots[(size_t)i];
			if(!b.peer || !b.welcomed || b.dead) continue;
			if(!b.spawned) continue;

			if(t - b.lastState >= dt){
				double tickDt = b.lastState > 0 ? (t - b.lastState) : dt;
				// A stalled load host can be a whole second late; moving the bot
				// the full elapsed distance in one step is a spike no real client
				// produces and would trip the server's burst-arrival buffer.
				if(tickDt > 2.0 * dt) tickDt = 2.0 * dt;
				b.lastState = t;
				bool tp = b.teleportStates > 0;
				bool reliable = (b.teleportStates == 20);   // first flagged state
				if(tp) b.teleportStates--;
				SendState(host, b, t, tickDt, tp, reliable);
				statesOut++;
			}
			if(cfg.chatPct > 0.0 && (double)(i % 100) < cfg.chatPct &&
			   t - b.lastChat >= cfg.chatSec){
				b.lastChat = t;
				SendChat(host, b, cfg);
			}
			if(t - b.lastPing >= 5.0){
				b.lastPing = t;
				SendPing(host, b, t);
			}
			(void)cfg.vehPct;   // vehicle load is driven by the netbot -scenario drive
		}

		// 1 Hz report + CSV sample
		if(t - lastReport >= 1.0){
			double win = t - lastReport;
			if(win <= 0.0) win = 1.0;
			int conn = 0, wel = 0, sp = 0, dead = 0;
			uint32_t rttSum = 0, rttMax = 0, rttN = 0;
			for(int i = 0; i < cfg.count; i++){
				LoadBot& b = bots[(size_t)i];
				if(b.connected) conn++;
				if(b.welcomed) wel++;
				if(b.spawned) sp++;
				if(b.dead) dead++;
				if(b.peer && b.welcomed && !b.dead){
					rttSum += b.peer->roundTripTime;
					if(b.peer->roundTripTime > rttMax) rttMax = b.peer->roundTripTime;
					rttN++;
				}
			}
			uint64_t totalSent = host->totalSentData, totalRecv = host->totalReceivedData;
			uint64_t pkRecv = host->totalReceivedPackets;
			double outKB = (double)(totalSent - lastSent) / win / 1024.0;
			double inKB = (double)(totalRecv - lastRecv) / win / 1024.0;
			uint64_t pkIn = pkRecv - lastPkRecv;
			lastSent = totalSent; lastRecv = totalRecv; lastPkRecv = pkRecv;

			std::printf("[load] t=%.1f conn=%d/%d wel=%d spawned=%d dead=%d states/s=%.0f "
			            "states_in/s=%.0f batches_in/s=%.0f recv_pkts/s=%llu "
			            "out=%.1fKB/s in=%.1fKB/s rtt_avg=%ums rtt_max=%ums\n",
			            t, conn, cfg.count, wel, sp, dead,
			            (double)statesOut / win, (double)statesIn / win,
			            (double)stateBatchesIn / win, (unsigned long long)pkIn,
			            outKB, inKB, (unsigned)(rttN ? rttSum / rttN : 0), (unsigned)rttMax);
			std::fflush(stdout);
			if(csv){
				std::fprintf(csv, "%.2f,%d,%d,%d,%d,%llu,%llu,%llu,%llu,%u,%u\n",
				             t, conn, wel, sp, dead,
				             (unsigned long long)statesOut,
				             (unsigned long long)pkIn,
				             (unsigned long long)totalSent,
				             (unsigned long long)totalRecv,
				             (unsigned)(rttN ? rttSum / rttN : 0), (unsigned)rttMax);
				std::fflush(csv);
			}
			lastReport = t;
		}

		Sleep(0);
	}

	// graceful shutdown: a disconnect lets the server free slots and profiles
	for(int i = 0; i < cfg.count; i++){
		if(bots[(size_t)i].peer && !bots[(size_t)i].dead)
			enet_peer_disconnect(bots[(size_t)i].peer, 0);
	}
	enet_host_flush(host);
	double tEnd = NowSec();
	while(NowSec() - tEnd < 1.5){
		ENetEvent ev;
		while(enet_host_service(host, &ev, 0) > 0){
			if(ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
		}
		Sleep(5);
	}

	int wel = 0, sp = 0, dead = 0;
	for(int i = 0; i < cfg.count; i++){
		LoadBot& b = bots[(size_t)i];
		if(b.welcomed) wel++;
		if(b.spawned) sp++;
		if(b.dead) dead++;
	}
	int live = wel - dead;
	// A run only counts as setup-OK when (nearly) everyone got in and spawned;
	// throughput numbers from a half-admitted fleet say nothing about the server.
	int missing = cfg.count - wel;
	bool pass = live > 0 && missing <= cfg.count / 100 && sp >= live - (live / 100);
	std::printf("[LOADGEN] RESULT: %s fleet=%d welcomed=%d spawned=%d dropped=%d "
	            "refused=%d states_out=%llu states_in=%llu batches_in=%llu last_kick=\"%s\"\n",
	            pass ? "PASS" : "FAIL", cfg.count, wel, sp, dropped, refused,
	            (unsigned long long)statesOut, (unsigned long long)statesIn,
	            (unsigned long long)stateBatchesIn, lastKick);

	if(csv) std::fclose(csv);
	enet_host_destroy(host);
	enet_deinitialize();
	return pass ? 0 : 1;
}
