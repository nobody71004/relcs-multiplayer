// net/server/server.cpp — reLCS dedicated multiplayer server implementation.
#include "server.h"
#include "gamemode.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <chrono>
#include <algorithm>

#include <ws2tcpip.h>

using namespace rnet;

static double NowSec()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

Server::~Server()
{
	Shutdown();
	if(m_gamemode){ delete m_gamemode; m_gamemode = nullptr; }
	if(m_host){ enet_host_destroy(m_host); m_host = nullptr; }
	if(m_masterClient){ enet_host_destroy(m_masterClient); m_masterClient = nullptr; }
	if(m_discSock != INVALID_SOCKET){ closesocket(m_discSock); m_discSock = INVALID_SOCKET; }
}

// ---------------------------------------------------------------------------
bool Server::Init(const ServerConfig& cfg)
{
	m_cfg = cfg;

	if(enet_initialize() != 0){
		std::printf("[ERR] enet_initialize failed\n");
		return false;
	}

	ENetAddress address;
	address.host = ENET_HOST_ANY;
	address.port = m_cfg.port;
	m_host = enet_host_create(&address, m_cfg.maxPlayers + 16, 2, 0, 0);
	if(!m_host){
		std::printf("[ERR] cannot bind UDP port %u\n", (unsigned)m_cfg.port);
		return false;
	}
	m_host->maximumPacketSize = 32 * 1024;

	if(m_cfg.announce){
		m_masterClient = enet_host_create(nullptr, 1, 2, 0, 0);
	}

	if(m_cfg.lanDiscover)
		InitDiscovery();

	if(m_cfg.gamemode == "survivor")
		m_gamemode = new LibertyCitySurvivorMode();
	else
		m_gamemode = new FreeRoamMode();

	m_startTime = NowSec();
	m_lastTick = 0.0;   // relative clock! (was = m_startTime, which made t-m_lastTick always negative and Tick() never run)
	m_masterLastAnnounce = -100.0;  // connect + announce immediately

	Log("reLCS server '%s' listening on UDP %u | mode=%s | maxplayers=%d",
	    m_cfg.hostname.c_str(), (unsigned)m_cfg.port, m_gamemode->Name(), m_cfg.maxPlayers);
	return true;
}

void Server::Run()
{
	m_running = true;
	while(m_running){
		PumpNetwork();
		PumpDiscovery();

		// console command queue
		for(;;){
			std::string line;
			{
				std::lock_guard<std::mutex> lock(m_cmdMutex);
				if(m_cmdQueue.empty()) break;
				line = m_cmdQueue.front();
				m_cmdQueue.pop_front();
			}
			HandleCommand(line);
		}

		double t = Now();
		double tickDt = 1.0 / std::max(1, m_cfg.tickRate);
		if(t - m_lastTick >= tickDt){
			Tick();
			m_lastTick = t;
		}

		UpdateMasterAnnounce();

		if(m_cfg.netstatsInterval > 0 && t - m_lastNetStats >= m_cfg.netstatsInterval){
			m_lastNetStats = t;
			Log("netstats: sent=%u KB recv=%u KB",
			    (unsigned)(m_host->totalSentData / 1024),
			    (unsigned)(m_host->totalReceivedData / 1024));
			for(int i = 0; i < MAX_PLAYERS; i++){
				ServerPlayer& p = m_players[i];
				if(!p.active || !p.peer) continue;
				Log("netstats: #%d %s rtt=%ums loss=%.1f%%",
				    p.id, p.name.c_str(), (unsigned)p.peer->roundTripTime,
				    p.peer->packetLoss * 100.0f / (float)ENET_PEER_PACKET_LOSS_SCALE);
			}
		}

		Sleep(1);
	}
	Log("server shutting down");
}

void Server::Shutdown()
{
	m_running = false;
}

void Server::PushConsoleLine(const std::string& line)
{
	std::lock_guard<std::mutex> lock(m_cmdMutex);
	m_cmdQueue.push_back(line);
}

double Server::Now() const
{
	return NowSec() - m_startTime;
}

// ---------------------------------------------------------------------------
void Server::SendTo(ENetPeer* peer, const BitWriter& w, uint8_t channel, bool reliable)
{
	if(!peer) return;
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(),
	                                     reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
	enet_peer_send(peer, channel, pkt);
	enet_host_flush(m_host);
}

void Server::Broadcast(const BitWriter& w, uint8_t channel, bool reliable, uint8_t exceptId)
{
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(),
	                                     reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
	for(int i = 0; i < MAX_PLAYERS; i++){
		ServerPlayer& p = m_players[i];
		if(p.active && p.id != exceptId && p.peer)
			enet_peer_send(p.peer, channel, pkt);
	}
	enet_host_flush(m_host);
}

void Server::Log(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	time_t raw = time(nullptr);
	struct tm tmv;
	localtime_s(&tmv, &raw);
	std::printf("[%02d:%02d:%02d] %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, buf);
	std::fflush(stdout);
}

// ---------------------------------------------------------------------------
ServerPlayer* Server::FindPlayer(uint8_t id)
{
	if(id < 1 || id > MAX_PLAYERS) return nullptr;
	ServerPlayer& p = m_players[id - 1];
	return p.active ? &p : nullptr;
}

ServerVehicle* Server::FindVehicle(uint16_t id)
{
	if(id < 1 || id > MAX_VEHICLES) return nullptr;
	ServerVehicle& v = m_vehicles[id - 1];
	return v.active ? &v : nullptr;
}

int Server::ActivePlayerCount() const
{
	int n = 0;
	for(int i = 0; i < MAX_PLAYERS; i++)
		if(m_players[i].active) n++;
	return n;
}

// ---------------------------------------------------------------------------
void Server::PumpNetwork()
{
	ENetEvent ev;
	while(m_host && enet_host_service(m_host, &ev, 0) > 0){
		switch(ev.type){
		case ENET_EVENT_TYPE_CONNECT:
			// player slot is allocated on HELLO
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			OnDisconnect(ev.peer);
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			OnReceive(ev.peer, ev.packet, ev.channelID);
			enet_packet_destroy(ev.packet);
			break;
		default:
			break;
		}
	}

	// master connection maintenance
	if(m_masterClient){
		while(enet_host_service(m_masterClient, &ev, 0) > 0){
			if(ev.type == ENET_EVENT_TYPE_DISCONNECT && ev.peer == m_masterPeer)
				m_masterPeer = nullptr;
			if(ev.type == ENET_EVENT_TYPE_RECEIVE)
				enet_packet_destroy(ev.packet);
		}
	}
}

void Server::OnDisconnect(ENetPeer* peer)
{
	ServerPlayer* p = (ServerPlayer*)peer->data;
	if(!p) return;

	// leave any vehicle (broadcasts MSG_VEH_EXIT so clients unseat the puppet)
	ClearPlayerVehicles(p, true);

	Log("[LEAVE] id=%d name=%s", p->id, p->name.c_str());
	if(m_gamemode) m_gamemode->OnPlayerDisconnect(*this, p->id);

	BitWriter w;
	BeginMsg(w, MSG_PLAYER_QUIT);
	w.WriteU8(p->id);
	Broadcast(w, CHAN_RELIABLE, true);

	peer->data = nullptr;
	*p = ServerPlayer();
}

// ---------------------------------------------------------------------------
void Server::OnReceive(ENetPeer* peer, ENetPacket* packet, uint8_t channel)
{
	if(packet->dataLength < 1) return;
	BitReader r(packet->data, packet->dataLength);
	MsgType type = (MsgType)r.ReadU8();

	ServerPlayer* p = (ServerPlayer*)peer->data;

	// only HELLO is allowed before slot allocation
	if(!p && type != MSG_HELLO)
		return;

	// flood defence: peers burning the packet budget get their burst dropped;
	// persistent floods are kicked
	if(p && !p->msgRate.Allow(Now())){
		if(++p->floodStrikes > 20){
			KickPlayer(p->id, "packet flood");
		}else
			Log("[anticheat] %s packet flood strike %d", p->name.c_str(), p->floodStrikes);
		return;
	}

	switch(type){
	case MSG_HELLO:       HandleHello(p, r, peer, 0); break;
	case MSG_PLAYER_STATE: if(p) HandlePlayerState(p, r); break;
	case MSG_VEHICLE_STATE: if(p) HandleVehicleState(p, r); break;
	case MSG_CHAT:        if(p) HandleChat(p, r); break;
	case MSG_SPAWN_REQ:   if(p) HandleSpawnReq(p); break;
	case MSG_DAMAGE:      if(p) HandleDamage(p, r); break;
	case MSG_VEH_ENTER:   if(p) HandleVehEnter(p, r); break;
	case MSG_VEH_EXIT:    if(p) HandleVehExit(p, r); break;
	case MSG_RCON_AUTH:   if(p) HandleRcon(p, r, true); break;
	case MSG_RCON_CMD:    if(p) HandleRcon(p, r, false); break;
	case MSG_PING: {
		if(!p) break;
		uint32_t stamp = r.ReadU32();
		BitWriter w;
		BeginMsg(w, MSG_PONG);
		w.WriteU32(stamp);
		w.WriteF32((float)(peer->roundTripTime / 2.0));
		SendTo(peer, w, CHAN_RELIABLE, true);
		break;
	}
	case MSG_PONG: break;
	default:
		break;
	}
	(void)channel;
}

// ---------------------------------------------------------------------------
void Server::HandleHello(ServerPlayer* p, BitReader& r, ENetPeer* peer, uint8_t)
{
	if(p) return; // already joined

	MsgHello hello;
	if(!ReadHello(r, hello)) return;

	if(hello.protocolVersion != PROTOCOL_VERSION){
		BitWriter w;
		BeginMsg(w, MSG_DISCONNECT);
		MsgKick k;
		CopyStr(k.reason, sizeof k.reason, "protocol version mismatch");
		WriteKick(w, k);
		SendTo(peer, w, CHAN_RELIABLE, true);
		enet_peer_disconnect_later(peer, 0);
		return;
	}

	// ban check
	for(uint32_t ip : m_bannedIps){
		if(ip == peer->address.host){
			BitWriter w;
			BeginMsg(w, MSG_DISCONNECT);
			MsgKick k;
			CopyStr(k.reason, sizeof k.reason, "banned");
			WriteKick(w, k);
			SendTo(peer, w, CHAN_RELIABLE, true);
			enet_peer_disconnect_later(peer, 0);
			return;
		}
	}

	// latency probe: answer and allocate nothing
	if(hello.flags & HELLO_PROBE){
		BitWriter w;
		BeginMsg(w, MSG_WELCOME);
		MsgWelcome wel;
		wel.playerId = 0xFE;
		wel.tickRate = (uint16_t)m_cfg.tickRate;
		wel.streamDistance = m_cfg.streamDistance;
		wel.hour = m_cfg.hour; wel.minute = m_cfg.minute; wel.weather = m_cfg.weather;
		CopyStr(wel.gameMode, sizeof wel.gameMode, m_gamemode->Name());
		CopyStr(wel.hostname, sizeof wel.hostname, m_cfg.hostname.c_str());
		WriteWelcome(w, wel);
		SendTo(peer, w, CHAN_RELIABLE, true);
		enet_peer_disconnect_later(peer, 0);
		return;
	}

	if(ActivePlayerCount() >= m_cfg.maxPlayers){
		BitWriter w;
		BeginMsg(w, MSG_DISCONNECT);
		MsgKick k;
		CopyStr(k.reason, sizeof k.reason, "server full");
		WriteKick(w, k);
		SendTo(peer, w, CHAN_RELIABLE, true);
		enet_peer_disconnect_later(peer, 0);
		return;
	}

	// allocate slot
	int idx = -1;
	for(int i = 0; i < MAX_PLAYERS; i++){
		if(!m_players[i].active){ idx = i; break; }
	}
	if(idx < 0) return;

	ServerPlayer& np = m_players[idx];
	np = ServerPlayer();
	np.active = true;
	np.id = (uint8_t)(idx + 1);
	np.peer = peer;
	np.ip = peer->address.host;
	np.port = peer->address.port;
	char cleanName[MAX_NAME_LEN];
	SanitizeName(cleanName, sizeof cleanName, hello.name);
	np.name = cleanName;
	// validator buckets for this session
	double nowT = Now();
	np.msgRate.Reset(nowT, 60.0f, 120.0f);
	np.chatRate.Reset(nowT, 2.5f, 5.0f);
	np.damageRate.Reset(nowT, 25.0f, 50.0f);
	np.moneySeen = 0;
	np.moneyGranted = 0;
	np.floodStrikes = 0;
	peer->data = &np;

	Log("[JOIN] id=%d name=%s", np.id, np.name.c_str());

	// welcome
	BitWriter w;
	BeginMsg(w, MSG_WELCOME);
	MsgWelcome wel;
	wel.playerId = np.id;
	wel.tickRate = (uint16_t)m_cfg.tickRate;
	wel.streamDistance = m_cfg.streamDistance;
	wel.hour = m_cfg.hour; wel.minute = m_cfg.minute; wel.weather = m_cfg.weather;
	CopyStr(wel.gameMode, sizeof wel.gameMode, m_gamemode->Name());
	CopyStr(wel.hostname, sizeof wel.hostname, m_cfg.hostname.c_str());
	WriteWelcome(w, wel);
	SendTo(peer, w, CHAN_RELIABLE, true);

	// tell the newcomer about everyone already here
	for(int i = 0; i < MAX_PLAYERS; i++){
		ServerPlayer& other = m_players[i];
		if(other.active && other.id != np.id){
			BitWriter w2;
			BeginMsg(w2, MSG_PLAYER_JOIN);
			MsgPlayerJoin j;
			j.playerId = other.id;
			j.skinModel = other.skin;
			CopyStr(j.name, sizeof j.name, other.name.c_str());
			WritePlayerJoin(w2, j);
			SendTo(peer, w2, CHAN_RELIABLE, true);
		}
	}

	// announce to everyone else
	BitWriter w3;
	BeginMsg(w3, MSG_PLAYER_JOIN);
	MsgPlayerJoin j;
	j.playerId = np.id;
	j.skinModel = np.skin;
	CopyStr(j.name, sizeof j.name, np.name.c_str());
	WritePlayerJoin(w3, j);
	Broadcast(w3, CHAN_RELIABLE, true, np.id);

	// vehicle snapshot: live vehicles + occupants + last driver state, so a
	// late joiner sees the same traffic as everyone else
	for(int i = 0; i < MAX_VEHICLES; i++){
		ServerVehicle& v = m_vehicles[i];
		if(!v.active) continue;
		BitWriter wv;
		BeginMsg(wv, MSG_VEH_SPAWN);
		MsgVehSpawn vs;
		vs.vehicleId = v.id;
		vs.modelId = v.modelId;
		vs.x = v.state.xMm / 1000.0f;
		vs.y = v.state.yMm / 1000.0f;
		vs.z = v.state.zMm / 1000.0f;
		vs.heading = DequantAngle(v.state.heading);
		WriteVehSpawn(wv, vs);
		SendTo(peer, wv, CHAN_RELIABLE, true);

		for(int s = 0; s < 8; s++){
			if(v.occupants[s] == 0xFF) continue;
			BitWriter we;
			BeginMsg(we, MSG_VEH_ENTER);
			MsgVehEnter ve;
			ve.playerId = v.occupants[s];
			ve.vehicleId = v.id;
			ve.seat = (uint8_t)s;
			ve.modelId = v.modelId;
			WriteVehEnter(we, ve);
			SendTo(peer, we, CHAN_RELIABLE, true);
		}

		if(v.lastStateTime > 0.0){
			BitWriter ws;
			BeginMsg(ws, MSG_VEHICLE_STATE);
			WriteVehicleState(ws, v.state);
			SendTo(peer, ws, CHAN_RELIABLE, true);
		}
	}

	if(m_gamemode) m_gamemode->OnPlayerConnect(*this, np.id);
}

// ---------------------------------------------------------------------------
void Server::HandlePlayerState(ServerPlayer* p, BitReader& r)
{
	PlayerState st;
	if(!ReadPlayerState(r, st)) return;

	// rcon-authed admins run admin tools (noclip, teleports, self-granted
	// money) that legitimately trip every anti-cheat budget — exempt them
	// from all penalties while keeping payload sanity as a hard floor
	bool admin = p->rconAuthed;

	// tough payload sanity first: garbage states never touch game logic
	if(!ValidateStateSanity(st)){
		if(admin){
			Log("[anticheat] %s insane state payload (admin — ignored)", p->name.c_str());
			return;
		}
		p->violations += 2;
		Log("[anticheat] %s insane state payload n=%d", p->name.c_str(), p->violations);
		if(p->violations > VIOLATION_KICK)
			KickPlayer(p->id, "invalid state payload");
		return;
	}
	st.ammo = ClampAmmo(st.ammo);

	double t = Now();
	float dt = (float)(t - p->lastStateTime);
	// arrival jitter can compress gaps and make a legal step look faster than
	// it really was: never evaluate a step quicker than the nominal tick, and
	// keep a 2x grace (honest clients pace at >= tick and self-flag anything
	// over budget, so this only blunts speedhacks instead of punishing timing)
	if(dt < TICK_DT) dt = TICK_DT;
	if(dt > 0.5f) dt = 0.5f;
	dt *= 2.0f;

	// client-flagged teleports (game-side respawns/warps) are trusted
	// relocations: log them and rate-limit floods, never anti-cheat kick
	if((st.moveFlags & MOVEFLAG_TELEPORT) && p->lastStateTime > 0.0){
		Log("[teleport] %s (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f)",
		    p->name.c_str(),
		    p->state.xMm/1000.0f, p->state.yMm/1000.0f, p->state.zMm/1000.0f,
		    st.xMm/1000.0f, st.yMm/1000.0f, st.zMm/1000.0f);
		if((float)(t - p->teleportWindowStart) > TELEPORT_WINDOW_SEC){
			p->teleportWindowStart = t;
			p->teleportBurst = 0;
		}
		if(++p->teleportBurst > MAX_TELEPORTS_PER_WINDOW && !admin){
			KickPlayer(p->id, "teleport flood");
			return;
		}
	}

	// admins adopt every move: no validation flags, no rubber-band, no kick
	uint8_t flags = admin ? 0 : ValidateMove(p->state, st, dt);
	if(!admin && p->lastStateTime > 0.0 && (flags & (VAL_TOO_FAST | VAL_TELEPORT))){
		p->violations++;
		Log("violation #%d %s n=%d dt=%.3f (%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f) flags=%02x",
		    p->id, p->name.c_str(), p->violations, dt,
		    p->state.xMm/1000.0f, p->state.yMm/1000.0f, p->state.zMm/1000.0f,
		    st.xMm/1000.0f, st.yMm/1000.0f, st.zMm/1000.0f, (unsigned)flags);
		// rubber-band: push the client back to the last server-accepted state
		BitWriter w;
		BeginMsg(w, MSG_CORRECTION);
		PlayerState fix = p->state;
		fix.health = p->health;
		fix.armour = p->armour;
		WritePlayerState(w, fix);
		SendTo(p->peer, w, CHAN_RELIABLE, true);
		if(p->violations > VIOLATION_KICK){
			KickPlayer(p->id, "movement validation failed");
			return;
		}
	}else if(p->violations > 0){
		p->violations--;
	}

	// server-tracked economy: the wallet may only grow by granted amounts
	// (admins are trusted — give-money and gamemode rewards bypass the ledger)
	if(admin){
		p->moneySeen = st.money;
	}else{
		bool cheated = false;
		st.money = ValidateMoney(st.money, p->moneySeen, p->moneyGranted, cheated);
		if(cheated){
			p->violations += 2;
			Log("[anticheat] %s money edit blocked n=%d", p->name.c_str(), p->violations);
			if(p->violations > VIOLATION_KICK){
				KickPlayer(p->id, "economy exploit");
				return;
			}
		}
	}

	// authoritative vitals
	st.playerId = p->id;
	st.health = p->health;
	st.armour = p->armour;

	// never adopt a failed move: the correction we send points at the last good
	// state, so adopting the bad one would make client and server swap reference
	// frames every tick (a 20 Hz rubber-band oscillation — seen as alternating
	// positions in the violation log)
	if(!(flags & (VAL_TOO_FAST | VAL_TELEPORT))){
		p->state = st;
		p->lastStateTime = t;
	}
}

void Server::HandleVehicleState(ServerPlayer* p, BitReader& r)
{
	VehicleState st;
	if(!ReadVehicleState(r, st)) return;

	ServerVehicle* v = FindVehicle(st.vehicleId);
	if(!v || v->driverId != p->id) return;

	double t = Now();
	float dt = (float)(t - v->lastStateTime);
	if(dt < 0.001f) dt = TICK_DT;
	if(dt > 0.5f) dt = 0.5f;

	if(v->lastStateTime > 0.0 && (ValidateVehicleMove(v->state, st, dt) & VAL_TELEPORT)){
		// don't relay teleports; client will get corrected visually by its own physics
		return;
	}
	v->state = st;
	v->lastStateTime = t;

	// relay the authoritative driver state immediately to everyone else in
	// streaming range (the driver's own client keeps its local simulation)
	BitWriter w;
	BeginMsg(w, MSG_VEHICLE_STATE);
	WriteVehicleState(w, st);
	for(int j = 0; j < MAX_PLAYERS; j++){
		ServerPlayer& q = m_players[j];
		if(!q.active || !q.spawned || !q.peer || q.id == p->id) continue;
		float dist = DistMm(st.xMm, st.yMm, st.zMm, q.state.xMm, q.state.yMm, q.state.zMm);
		if(dist > m_cfg.streamDistance) continue;
		SendTo(q.peer, w, CHAN_STATE, false);
	}
}

void Server::HandleChat(ServerPlayer* p, BitReader& r)
{
	MsgChat chat;
	if(!ReadChat(r, chat)) return;
	// spam/hygiene gate: empty, oversized, control-char or floody chat is dropped
	if(!ValidateChat(chat.text, p->chatRate, Now())){
		Log("[anticheat] %s chat rejected (spam or malformed)", p->name.c_str());
		return;
	}

	// player commands: "/cmd args"
	if(chat.text[0] == '/'){
		std::string cmd = chat.text + 1;
		if(m_gamemode && m_gamemode->OnPlayerCommand(*this, p->id, cmd))
			return;
		if(HandleCommand(cmd, p->id))
			return;
		SendChatTo(p->id, "Unknown command.");
		return;
	}

	if(m_gamemode && !m_gamemode->OnChat(*this, p->id, chat.text))
		return;

	Log("[CHAT] %s: %s", p->name.c_str(), chat.text);
	BitWriter w;
	BeginMsg(w, MSG_CHAT);
	MsgChat out;
	out.fromId = p->id;
	CopyStr(out.text, sizeof out.text, chat.text);
	WriteChat(w, out);
	Broadcast(w, CHAN_RELIABLE, true);
}

void Server::HandleSpawnReq(ServerPlayer* p)
{
	if(m_gamemode) m_gamemode->OnPlayerSpawnRequest(*this, p->id);
}

void Server::HandleDamage(ServerPlayer* p, BitReader& r)
{
	MsgDamage dmg;
	if(!ReadDamage(r, dmg)) return;
	if(!p->damageRate.Allow(Now())) return;

	ServerPlayer* target = FindPlayer(dmg.targetId);
	if(!target || !target->alive || !p->alive || target->id == p->id) return;

	// distance sanity (long-range weapons allowed, generous)
	float dist = DistMm(p->state.xMm, p->state.yMm, p->state.zMm,
	                    target->state.xMm, target->state.yMm, target->state.zMm);
	if(dist > 250.0f) return;

	uint16_t amount = dmg.damage;
	if(amount > 100) amount = 100;

	if(target->armour >= amount){
		target->armour = (uint8_t)(target->armour - amount);
	}else{
		amount = (uint16_t)(amount - target->armour);
		target->armour = 0;
		if(target->health > amount)
			target->health = (uint8_t)(target->health - amount);
		else
			target->health = 0;
	}

	// relay to AOI
	BitWriter w;
	BeginMsg(w, MSG_DAMAGE);
	WriteDamage(w, dmg);
	Broadcast(w, CHAN_RELIABLE, true);

	if(target->health == 0)
		KillPlayer(target->id, p->id, dmg.weaponId);
}

void Server::HandleVehEnter(ServerPlayer* p, BitReader& r)
{
	MsgVehEnter m;
	if(!ReadVehEnter(r, m)) return;

	// claimed model sanity (real clients send the engine model index)
	if(m.modelId == 0 || m.modelId > 20000) return;

	// a player holds exactly one seat: drop any old occupancy (broadcasts exit)
	ClearPlayerVehicles(p, true);

	ServerVehicle* v = nullptr;
	uint16_t vid = m.vehicleId;

	if(vid == 0xFFFF){
		// allocate new vehicle at the player's position
		for(int i = 0; i < MAX_VEHICLES; i++){
			if(!m_vehicles[i].active){
				v = &m_vehicles[i];
				vid = (uint16_t)(i + 1);
				break;
			}
		}
		if(!v) return;
		v->active = true;
		v->id = vid;
		v->modelId = m.modelId;
		v->driverId = 0xFF;
		for(int s = 0; s < 8; s++) v->occupants[s] = 0xFF;
		v->state = VehicleState();
		v->state.vehicleId = vid;
		v->state.modelId = m.modelId;
		v->state.xMm = p->state.xMm;
		v->state.yMm = p->state.yMm;
		v->state.zMm = p->state.zMm;
		v->state.heading = p->state.heading;

		BitWriter w;
		BeginMsg(w, MSG_VEH_SPAWN);
		MsgVehSpawn vs;
		vs.vehicleId = vid;
		vs.modelId = m.modelId;
		vs.x = p->state.xMm / 1000.0f;
		vs.y = p->state.yMm / 1000.0f;
		vs.z = p->state.zMm / 1000.0f;
		vs.heading = DequantAngle(p->state.heading);
		WriteVehSpawn(w, vs);
		Broadcast(w, CHAN_RELIABLE, true);
		Log("[VEH] player %d spawned vehicle id=%d model=%d", p->id, vid, (int)m.modelId);
	}else{
		v = FindVehicle(vid);
		if(!v) return;
	}

	// seat assignment: honour the request when free, otherwise the lowest free
	// seat; a full vehicle rejects the enter
	uint8_t seat = 0xFF;
	if(m.seat < 8 && v->occupants[m.seat] == 0xFF)
		seat = m.seat;
	else
		for(int s = 0; s < 8; s++)
			if(v->occupants[s] == 0xFF){ seat = (uint8_t)s; break; }
	if(seat == 0xFF){
		Log("[VEH] player %d denied: vehicle id=%d full", p->id, vid);
		return;
	}

	v->occupants[seat] = p->id;
	if(seat == 0) v->driverId = p->id;
	p->state.vehicleId = vid;
	p->state.seat = seat;

	BitWriter w;
	BeginMsg(w, MSG_VEH_ENTER);
	MsgVehEnter out;
	out.playerId = p->id;
	out.vehicleId = vid;
	out.seat = seat;   // the ASSIGNED seat (may differ from the request)
	out.modelId = v->modelId;
	WriteVehEnter(w, out);
	Broadcast(w, CHAN_RELIABLE, true);
}

// drop every seat held by this player (disconnect, respawn, re-enter);
// broadcasts MSG_VEH_EXIT so clients unseat/remove the puppet
void Server::ClearPlayerVehicles(ServerPlayer* p, bool broadcast)
{
	for(int i = 0; i < MAX_VEHICLES; i++){
		ServerVehicle& v = m_vehicles[i];
		if(!v.active) continue;
		bool held = false;
		for(int s = 0; s < 8; s++)
			if(v.occupants[s] == p->id){ v.occupants[s] = 0xFF; held = true; }
		if(v.driverId == p->id){ v.driverId = 0xFF; held = true; }
		if(held && broadcast){
			BitWriter w;
			BeginMsg(w, MSG_VEH_EXIT);
			MsgVehExit out;
			out.playerId = p->id;
			out.vehicleId = v.id;
			WriteVehExit(w, out);
			Broadcast(w, CHAN_RELIABLE, true);
		}
	}
	p->state.vehicleId = VEHICLE_NONE;
	p->state.seat = 0xFF;
}

void Server::HandleVehExit(ServerPlayer* p, BitReader& r)
{
	MsgVehExit m;
	if(!ReadVehExit(r, m)) return;

	// occupancy is authoritative: clients may send 0 or a stale id (the bot
	// sends 0), so resolve the vehicle actually holding the player
	auto inVehicle = [&](ServerVehicle* cand){
		if(cand->driverId == p->id) return true;
		for(int s = 0; s < 8; s++)
			if(cand->occupants[s] == p->id) return true;
		return false;
	};
	ServerVehicle* v = FindVehicle(m.vehicleId);
	if(!v || !inVehicle(v)){
		v = nullptr;
		for(int i = 0; i < MAX_VEHICLES; i++)
			if(m_vehicles[i].active && inVehicle(&m_vehicles[i])){ v = &m_vehicles[i]; break; }
	}
	if(!v) return;
	for(int s = 0; s < 8; s++)
		if(v->occupants[s] == p->id) v->occupants[s] = 0xFF;
	if(v->driverId == p->id) v->driverId = 0xFF;
	p->state.vehicleId = VEHICLE_NONE;
	p->state.seat = 0xFF;

	BitWriter w;
	BeginMsg(w, MSG_VEH_EXIT);
	MsgVehExit out;
	out.playerId = p->id;
	out.vehicleId = v->id;
	WriteVehExit(w, out);
	Broadcast(w, CHAN_RELIABLE, true);
}

void Server::HandleRcon(ServerPlayer* p, BitReader& r, bool auth)
{
	char text[256] = {};
	if(!ReadStringMsg(r, text, sizeof text, 255)) return;

	if(auth){
		bool ok = !m_cfg.rconPassword.empty() && m_cfg.rconPassword == text;
		p->rconAuthed = ok;
		BitWriter w;
		BeginMsg(w, ok ? MSG_RCON_OK : MSG_RCON_RESP);
		if(!ok){
			const char* bad = "RCON: wrong password";
			WriteStringMsg(w, bad, 255);
		}
		SendTo(p->peer, w, CHAN_RELIABLE, true);
		Log("[RCON] auth %s from id=%d", ok ? "OK" : "FAILED", p->id);
		return;
	}

	if(!p->rconAuthed){
		BitWriter w;
		BeginMsg(w, MSG_RCON_RESP);
		const char* bad = "RCON: not authenticated";
		WriteStringMsg(w, bad, 255);
		SendTo(p->peer, w, CHAN_RELIABLE, true);
		return;
	}

	Log("[RCON] id=%d: %s", p->id, text);
	HandleCommand(text, p->id);
}

// ---------------------------------------------------------------------------
void Server::Tick()
{
	float dt = (float)(1.0 / std::max(1, m_cfg.tickRate));
	double t = Now();

	// respawns
	for(int i = 0; i < MAX_PLAYERS; i++){
		ServerPlayer& p = m_players[i];
		if(p.active && !p.alive && p.respawnAt > 0.0 && t >= p.respawnAt){
			p.respawnAt = 0.0;
			if(m_gamemode) m_gamemode->OnPlayerSpawnRequest(*this, p.id);
		}
	}

	if(m_gamemode) m_gamemode->OnTick(*this, dt);

	// relay player states within AOI
	for(int i = 0; i < MAX_PLAYERS; i++){
		ServerPlayer& p = m_players[i];
		if(!p.active || !p.spawned || p.lastStateTime <= 0.0) continue;

		for(int j = 0; j < MAX_PLAYERS; j++){
			if(i == j) continue;
			ServerPlayer& q = m_players[j];
			if(!q.active || !q.spawned || !q.peer) continue;

			float dist = DistMm(p.state.xMm, p.state.yMm, p.state.zMm,
			                    q.state.xMm, q.state.yMm, q.state.zMm);
			if(dist > m_cfg.streamDistance) continue;

			BitWriter w;
			BeginMsg(w, MSG_PLAYER_STATE);
			WritePlayerState(w, p.state);
			SendTo(q.peer, w, CHAN_STATE, false);
		}
	}

	// (vehicle states are relayed directly in HandleVehicleState — driver-paced
	//  at 20 Hz, no stale re-broadcasts of parked vehicles)

	// world clock: +1 game minute every 2 real seconds
	if(t - m_lastWorldUpdate > 2.0){
		m_lastWorldUpdate = t;
		m_cfg.minute++;
		if(m_cfg.minute >= 60){ m_cfg.minute = 0; m_cfg.hour = (m_cfg.hour + 1) % 24; }
		BitWriter w;
		BeginMsg(w, MSG_WORLD);
		MsgWorld wrld;
		wrld.hour = m_cfg.hour; wrld.minute = m_cfg.minute; wrld.weather = m_cfg.weather;
		WriteWorld(w, wrld);
		Broadcast(w, CHAN_RELIABLE, true);
	}
}

// ---------------------------------------------------------------------------
void Server::BroadcastChat(const char* text, uint8_t fromId)
{
	BitWriter w;
	BeginMsg(w, MSG_CHAT);
	MsgChat out;
	out.fromId = fromId;
	CopyStr(out.text, sizeof out.text, text);
	WriteChat(w, out);
	Broadcast(w, CHAN_RELIABLE, true);
}

void Server::SendChatTo(uint8_t playerId, const char* text, uint8_t fromId)
{
	ServerPlayer* p = FindPlayer(playerId);
	if(!p || !p->peer) return;
	BitWriter w;
	BeginMsg(w, MSG_CHAT);
	MsgChat out;
	out.fromId = fromId;
	CopyStr(out.text, sizeof out.text, text);
	WriteChat(w, out);
	SendTo(p->peer, w, CHAN_RELIABLE, true);
}

void Server::BroadcastEvent(uint8_t type, uint8_t arg1, uint8_t arg2, const char* text)
{
	BitWriter w;
	BeginMsg(w, MSG_GAMEMODE_EVENT);
	MsgGamemodeEvent ev;
	ev.type = type; ev.arg1 = arg1; ev.arg2 = arg2;
	CopyStr(ev.text, sizeof ev.text, text ? text : "");
	WriteGamemodeEvent(w, ev);
	Broadcast(w, CHAN_RELIABLE, true);
	Log("[GMEV] type=%d %d %d %s", (int)type, (int)arg1, (int)arg2, text ? text : "");
}

void Server::SpawnPlayer(uint8_t playerId, float x, float y, float z, float heading,
                         uint16_t skin, uint8_t health, uint8_t armour)
{
	ServerPlayer* p = FindPlayer(playerId);
	if(!p) return;

	// (re)spawn teleports the player out of any vehicle
	ClearPlayerVehicles(p, true);

	p->spawned = true;
	p->alive = true;
	p->skin = skin;
	p->health = health;
	p->armour = armour;
	p->respawnAt = 0.0;

	PlayerState& st = p->state;
	st.playerId = playerId;
	st.xMm = QuantPos(x); st.yMm = QuantPos(y); st.zMm = QuantPos(z);
	st.velX = st.velY = st.velZ = 0;
	st.heading = QuantAngle(heading); st.pitch = 0;
	st.animId = 0; st.animFlags = 0;
	st.health = health; st.armour = armour;
	st.vehicleId = 0xFFFF; st.seat = 0xFF;
	p->lastStateTime = Now();

	BitWriter w;
	BeginMsg(w, MSG_SPAWN);
	MsgSpawn sp;
	sp.playerId = playerId; sp.skinModel = skin;
	sp.x = x; sp.y = y; sp.z = z; sp.heading = heading;
	sp.health = health; sp.armour = armour;
	WriteSpawn(w, sp);
	Broadcast(w, CHAN_RELIABLE, true);

	Log("[SPAWN] id=%d at (%.1f, %.1f, %.1f)", playerId, x, y, z);
}

void Server::KillPlayer(uint8_t victimId, uint8_t killerId, uint8_t weaponId)
{
	ServerPlayer* victim = FindPlayer(victimId);
	if(!victim || !victim->alive) return;

	victim->alive = false;
	victim->health = 0;
	victim->deaths++;
	victim->respawnAt = Now() + (m_gamemode ? m_gamemode->RespawnDelay() : 3.0f);

	ServerPlayer* killer = FindPlayer(killerId);
	if(killer && killer->id != victimId){
		killer->kills++;
		killer->score++;
	}

	BitWriter w;
	BeginMsg(w, MSG_DEATH);
	MsgDeath d;
	d.victimId = victimId; d.killerId = killerId; d.weaponId = weaponId;
	WriteDeath(w, d);
	Broadcast(w, CHAN_RELIABLE, true);

	Log("[KILL] %s (%d) killed %s (%d) weapon=%d",
	    killer ? killer->name.c_str() : "<none>", (int)killerId,
	    victim->name.c_str(), (int)victimId, (int)weaponId);

	if(m_gamemode) m_gamemode->OnPlayerDeath(*this, victimId, killerId, weaponId);
	BroadcastScores();
}

#include <windows.h>

void Server::SendGive(uint8_t targetId, uint8_t kind, int32_t amount, uint16_t arg)
{
	ServerPlayer* p = FindPlayer(targetId);
	if(!p || !p->peer) return;
	// grant ledger: the client may legally grow its wallet by exactly this
	if(kind == GIVE_MONEY && amount > 0)
		p->moneyGranted += amount;
	BitWriter w;
	BeginMsg(w, MSG_GIVE);
	MsgGive g;
	g.targetId = targetId; g.kind = kind; g.amount = amount; g.arg = arg;
	WriteGive(w, g);
	SendTo(p->peer, w, CHAN_RELIABLE, true);
}

void Server::TeleportPlayer(uint8_t playerId, float x, float y, float z, float heading)
{
	ServerPlayer* p = FindPlayer(playerId);
	if(!p) return;
	p->state.xMm = QuantPos(x); p->state.yMm = QuantPos(y); p->state.zMm = QuantPos(z);
	p->state.heading = QuantAngle(heading);
	p->lastStateTime = Now();

	BitWriter w;
	BeginMsg(w, MSG_CORRECTION);
	PlayerState fix = p->state;
	fix.health = p->health; fix.armour = p->armour;
	WritePlayerState(w, fix);
	SendTo(p->peer, w, CHAN_RELIABLE, true);
}

void Server::SetPlayerVitals(uint8_t playerId, uint8_t health, uint8_t armour)
{
	ServerPlayer* p = FindPlayer(playerId);
	if(!p) return;
	p->health = health;
	p->armour = armour;
}

void Server::BroadcastScores()
{
	MsgScoreUpdate su;
	su.count = 0;
	// simple selection: up to 32 players, sorted by score desc
	struct { uint8_t id; int32_t score; uint16_t kills, deaths; } tmp[MAX_PLAYERS];
	int n = 0;
	for(int i = 0; i < MAX_PLAYERS; i++){
		ServerPlayer& p = m_players[i];
		if(!p.active) continue;
		tmp[n].id = p.id; tmp[n].score = p.score; tmp[n].kills = p.kills; tmp[n].deaths = p.deaths;
		n++;
	}
	std::sort(tmp, tmp + n, [](const auto& a, const auto& b){ return a.score > b.score; });
	for(int i = 0; i < n && i < MAX_SCORE_ENTRIES; i++){
		su.entries[i].playerId = tmp[i].id;
		su.entries[i].score = tmp[i].score;
		su.entries[i].kills = tmp[i].kills;
		su.entries[i].deaths = tmp[i].deaths;
		su.count++;
	}
	BitWriter w;
	BeginMsg(w, MSG_SCORE_UPDATE);
	WriteScoreUpdate(w, su);
	Broadcast(w, CHAN_RELIABLE, true);
}

void Server::KickPlayer(uint8_t playerId, const char* reason)
{
	ServerPlayer* p = FindPlayer(playerId);
	if(!p) return;

	Log("[KICK] id=%d reason=%s", playerId, reason);
	BitWriter w;
	BeginMsg(w, MSG_KICK);
	MsgKick k;
	CopyStr(k.reason, sizeof k.reason, reason);
	WriteKick(w, k);
	SendTo(p->peer, w, CHAN_RELIABLE, true);
	enet_peer_disconnect_later(p->peer, 0);
}

void Server::RestartGameMode()
{
	if(m_gamemode) m_gamemode->Reset(*this);
	for(int i = 0; i < MAX_PLAYERS; i++){
		ServerPlayer& p = m_players[i];
		if(p.active){
			p.score = 0; p.kills = 0; p.deaths = 0;
			p.alive = false; p.respawnAt = Now() + 1.0;
		}
	}
	BroadcastScores();
	Log("[GAMEMODE] %s restarted", m_gamemode ? m_gamemode->Name() : "?");
}

void Server::SetServerVar(const std::string& key, const std::string& value)
{
	if(key == "hostname") m_cfg.hostname = value;
	else if(key == "stream_distance") m_cfg.streamDistance = (float)atof(value.c_str());
	else if(key == "tick_rate") m_cfg.tickRate = atoi(value.c_str());
	else if(key == "weather") m_cfg.weather = (uint8_t)atoi(value.c_str());
	else if(key == "hour") m_cfg.hour = (uint8_t)atoi(value.c_str());
	else if(key == "minute") m_cfg.minute = (uint8_t)atoi(value.c_str());
	else if(key == "lcs_scorelimit" && m_gamemode){
		auto* lcs = dynamic_cast<LibertyCitySurvivorMode*>(m_gamemode);
		if(lcs) lcs->scoreLimit = atoi(value.c_str());
	}
	else if(key == "lcs_roundtime" && m_gamemode){
		auto* lcs = dynamic_cast<LibertyCitySurvivorMode*>(m_gamemode);
		if(lcs) lcs->roundTime = (float)atof(value.c_str());
	}
	else {
		Log("[CFG] unknown server var '%s'", key.c_str());
		return;
	}
	Log("[CFG] %s = %s", key.c_str(), value.c_str());
}

// ---------------------------------------------------------------------------
bool Server::HandleCommand(const std::string& line, int respondTo)
{
	// tokenize
	std::string cmd, rest;
	size_t sp = line.find(' ');
	if(sp == std::string::npos){ cmd = line; }
	else { cmd = line.substr(0, sp); rest = line.substr(sp + 1); while(!rest.empty() && rest[0] == ' ') rest.erase(0, 1); }
	for(auto& c : cmd) c = (char)tolower(c);
	if(cmd.empty()) return false;

	auto respond = [&](const char* fmt, ...){
		char buf[512];
		va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
		if(respondTo < 0){
			std::printf("%s\n", buf);
			std::fflush(stdout);
		}else{
			ServerPlayer* rp = FindPlayer((uint8_t)respondTo);
			if(rp && rp->peer){
				BitWriter w;
				BeginMsg(w, MSG_RCON_RESP);
				WriteStringMsg(w, buf, 255);
				SendTo(rp->peer, w, CHAN_RELIABLE, true);
			}
		}
	};

	if(cmd == "help"){
		respond("commands: help status players say kick ban banip unban setr getr gmx loadmode respawn teleport exec uptime version netstats quit");
		return true;
	}
	if(cmd == "status"){
		respond("hostname=%s players=%d/%d mode=%s uptime=%.0fs tick=%d",
		        m_cfg.hostname.c_str(), ActivePlayerCount(), m_cfg.maxPlayers,
		        m_gamemode ? m_gamemode->Name() : "?", Now(), m_cfg.tickRate);
		return true;
	}
	if(cmd == "players"){
		for(int i = 0; i < MAX_PLAYERS; i++){
			ServerPlayer& p = m_players[i];
			if(!p.active) continue;
			char ipstr[32] = "?";
			ENetAddress a; a.host = p.ip; a.port = p.port;
			enet_address_get_host_ip(&a, ipstr, sizeof ipstr);
			respond("#%d %s ip=%s score=%d k/d=%d/%d ping=%ums%s",
			        p.id, p.name.c_str(), ipstr, p.score, p.kills, p.deaths,
			        p.peer ? (unsigned)p.peer->roundTripTime : 0,
			        p.alive ? "" : " (dead)");
		}
		return true;
	}
	if(cmd == "say"){
		std::string msg = "[SERVER] " + rest;
		BroadcastChat(msg.c_str());
		return true;
	}
	if(cmd == "kick"){
		int id = atoi(rest.c_str());
		std::string reason = "kicked by admin";
		size_t sp2 = rest.find(' ');
		if(sp2 != std::string::npos) reason = rest.substr(sp2 + 1);
		KickPlayer((uint8_t)id, reason.c_str());
		respond("kicked #%d", id);
		return true;
	}
	if(cmd == "ban" || cmd == "banip"){
		uint32_t ip = 0;
		if(cmd == "ban"){
			int id = atoi(rest.c_str());
			ServerPlayer* p = FindPlayer((uint8_t)id);
			if(!p){ respond("no such player"); return true; }
			ip = p->ip;
			KickPlayer((uint8_t)id, "banned");
		}else{
			ENetAddress a;
			if(enet_address_set_host(&a, rest.c_str()) < 0){ respond("bad ip"); return true; }
			ip = a.host;
		}
		m_bannedIps.push_back(ip);
		respond("banned");
		return true;
	}
	if(cmd == "unban"){
		ENetAddress a;
		if(enet_address_set_host(&a, rest.c_str()) == 0){
			m_bannedIps.erase(std::remove(m_bannedIps.begin(), m_bannedIps.end(), a.host),
			                  m_bannedIps.end());
			respond("unbanned");
		}
		return true;
	}
	if(cmd == "setr"){
		size_t sp2 = rest.find(' ');
		if(sp2 == std::string::npos){ respond("usage: setr <key> <value>"); return true; }
		SetServerVar(rest.substr(0, sp2), rest.substr(sp2 + 1));
		return true;
	}
	if(cmd == "getr"){
		respond("hostname=%s stream_distance=%.1f tick_rate=%d hour=%d minute=%d weather=%d",
		        m_cfg.hostname.c_str(), m_cfg.streamDistance, m_cfg.tickRate,
		        (int)m_cfg.hour, (int)m_cfg.minute, (int)m_cfg.weather);
		return true;
	}
	if(cmd == "gmx"){
		RestartGameMode();
		return true;
	}
	if(cmd == "loadmode"){
		delete m_gamemode;
		if(rest == "survivor") m_gamemode = new LibertyCitySurvivorMode();
		else m_gamemode = new FreeRoamMode();
		RestartGameMode();
		respond("gamemode now %s", m_gamemode->Name());
		return true;
	}
	if(cmd == "respawn"){
		int id = atoi(rest.c_str());
		if(m_gamemode) m_gamemode->OnPlayerSpawnRequest(*this, (uint8_t)id);
		return true;
	}
	if(cmd == "teleport"){
		int id = 0; float x = 0, y = 0, z = 0;
		if(sscanf(rest.c_str(), "%d %f %f %f", &id, &x, &y, &z) == 4)
			TeleportPlayer((uint8_t)id, x, y, z, 0.0f);
		else
			respond("usage: teleport <id> <x> <y> <z>");
		return true;
	}
	if(cmd == "exec"){
		FILE* f = fopen(rest.c_str(), "r");
		if(!f){ respond("cannot open %s", rest.c_str()); return true; }
		char buf[512];
		while(fgets(buf, sizeof buf, f)){
			std::string l = buf;
			while(!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
			if(l.empty() || l[0] == '#') continue;
			HandleCommand(l, respondTo);
		}
		fclose(f);
		return true;
	}
	if(cmd == "broadcast" || cmd == "say"){
		if(rest.empty()){ respond("usage: broadcast <text>"); return true; }
		Log("[BCAST] %s", rest.c_str());
		BitWriter w;
		BeginMsg(w, MSG_CHAT);
		MsgChat c;
		c.fromId = 0;
		CopyStr(c.text, sizeof c.text, rest.c_str());
		WriteChat(w, c);
		Broadcast(w, CHAN_RELIABLE, true);
		respond("broadcast sent");
		return true;
	}
	if(cmd == "give"){
		// give <id|name> money <amount> | weapon <type> [ammo] | god [0|1] | health <hp>
		char who[40] = {}, what[24] = {};
		int a1 = 0, a2 = 0;
		int n = sscanf(rest.c_str(), "%39s %23s %d %d", who, what, &a1, &a2);
		if(n < 3){ respond("usage: give <id|name> money|weapon|god|health <value> [ammo]"); return true; }
		ServerPlayer* tp = FindPlayer((uint8_t)atoi(who));
		if(!tp || !tp->active){
			tp = nullptr;
			for(int i = 0; i < MAX_PLAYERS; i++)
				if(m_players[i].active && m_players[i].name == who){ tp = &m_players[i]; break; }
		}
		if(!tp || !tp->active){ respond("no such player: %s", who); return true; }
		if(!strcmp(what, "money")){
			SendGive(tp->id, GIVE_MONEY, a1, 0);
			respond("gave $%d to %s", a1, tp->name.c_str());
		}else if(!strcmp(what, "weapon")){
			SendGive(tp->id, GIVE_WEAPON, a2 > 0 ? a2 : 200, (uint16_t)a1);
			respond("gave weapon %d (%d ammo) to %s", a1, a2 > 0 ? a2 : 200, tp->name.c_str());
		}else if(!strcmp(what, "god")){
			int on = (n >= 4) ? a1 : 1;
			SendGive(tp->id, GIVE_GOD, on, 0);
			respond("godmode %s for %s", on ? "on" : "off", tp->name.c_str());
		}else if(!strcmp(what, "health")){
			SendGive(tp->id, GIVE_HEALTH, a1, 0);
			respond("health set to %d for %s", a1, tp->name.c_str());
		}else
			respond("usage: give <id|name> money|weapon|god|health <value> [ammo]");
		return true;
	}
	if(cmd == "spawnbot"){
		char name[32] = {}, scen[24] = {};
		sscanf(rest.c_str(), "%31s %23s", name, scen);
		if(!name[0]){ respond("usage: spawnbot <name> [roam|combat|drive|chaos]"); return true; }
		if(!scen[0]) snprintf(scen, sizeof scen, "roam");
		char exePath[MAX_PATH] = {};
		GetModuleFileNameA(NULL, exePath, MAX_PATH);
		std::string bot = exePath;
		size_t slash = bot.find_last_of("\\/");
		bot = bot.substr(0, slash + 1) + "relcs-netbot.exe";
		char args[512];
		snprintf(args, sizeof args, "\"%s\" -name %s -server 127.0.0.1:%u -scenario %s -duration 600 -seed %u",
		         bot.c_str(), name, (unsigned)m_cfg.port, scen, (unsigned)(rand() % 10000));
		STARTUPINFOA si; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
		PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
		if(CreateProcessA(NULL, args, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)){
			respond("bot %s spawned (pid %u, %s)", name, (unsigned)pi.dwProcessId, scen);
			CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
		}else
			respond("spawnbot failed (error %u)", (unsigned)GetLastError());
		return true;
	}
	if(cmd == "spawnprop"){
		// spawnprop <x> <y> <z> [weaponType] [qty] — synced prop (weapon crate)
		float x = 0, y = 0, z = 0; int weapon = 4, qty = 50;
		if(sscanf(rest.c_str(), "%f %f %f %d %d", &x, &y, &z, &weapon, &qty) >= 3){
			static uint16_t s_pickupSeq = 0;
			BitWriter w;
			BeginMsg(w, MSG_PICKUP_SPAWN);
			MsgPickup pk;
			pk.pickupId = ++s_pickupSeq;
			pk.modelIndex = 0;          // derive from weapon type
			pk.type = 4;                // PICKUP_ONCE
			pk.weapon = (uint16_t)weapon;
			pk.quantity = (uint32_t)qty;
			pk.x = x; pk.y = y; pk.z = z;
			WritePickup(w, pk);
			Broadcast(w, CHAN_RELIABLE, true);
			respond("prop (weapon %d x%d) at %.1f %.1f %.1f", weapon, qty, x, y, z);
		}else
			respond("usage: spawnprop <x> <y> <z> [weaponType] [qty]");
		return true;
	}
	if(cmd == "inventory" || cmd == "inv"){
		char who[40] = {};
		sscanf(rest.c_str(), "%39s", who);
		ServerPlayer* tp = FindPlayer((uint8_t)atoi(who));
		if(!tp || !tp->active){
			tp = nullptr;
			for(int i = 0; i < MAX_PLAYERS; i++)
				if(m_players[i].active && m_players[i].name == who){ tp = &m_players[i]; break; }
		}
		if(!tp || !tp->active){
			for(int i = 0; i < MAX_PLAYERS; i++)
				if(m_players[i].active)
					respond("#%d %s money=%d hp=%d armour=%d weapon=%d ammo=%d",
					        m_players[i].id, m_players[i].name.c_str(), m_players[i].state.money,
					        (int)m_players[i].state.health, (int)m_players[i].state.armour,
					        (int)m_players[i].state.weapon, (int)m_players[i].state.ammo);
			return true;
		}
		respond("#%d %s money=%d hp=%d armour=%d weapon=%d ammo=%d",
		        tp->id, tp->name.c_str(), tp->state.money,
		        (int)tp->state.health, (int)tp->state.armour,
		        (int)tp->state.weapon, (int)tp->state.ammo);
		return true;
	}
	if(cmd == "uptime"){ respond("up %.0f seconds", Now()); return true; }
	if(cmd == "version"){ respond("reLCS-server protocol=%d game=LCS1", (int)PROTOCOL_VERSION); return true; }
	if(cmd == "netstats"){
		for(int i = 0; i < MAX_PLAYERS; i++){
			ServerPlayer& p = m_players[i];
			if(!p.active || !p.peer) continue;
			respond("#%d %s rtt=%ums loss=%.1f%%",
			        p.id, p.name.c_str(), (unsigned)p.peer->roundTripTime,
			        p.peer->packetLoss * 100.0f / (float)ENET_PEER_PACKET_LOSS_SCALE);
		}
		respond("host totals: sent=%u KB recv=%u KB",
		        (unsigned)(m_host->totalSentData / 1024),
		        (unsigned)(m_host->totalReceivedData / 1024));
		return true;
	}
	if(cmd == "quit" || cmd == "exit"){
		respond("shutting down");
		m_running = false;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
void Server::UpdateMasterAnnounce()
{
	if(!m_cfg.announce || !m_masterClient) return;
	double t = Now();

	if(!m_masterPeer && t - m_masterLastAnnounce > 5.0){
		ENetAddress addr;
		if(enet_address_set_host(&addr, m_cfg.master.c_str()) == 0){
			addr.port = m_cfg.masterPort;
			m_masterPeer = enet_host_connect(m_masterClient, &addr, 2, 0);
			m_masterLastAnnounce = t - 30.0;  // announce as soon as connected
		}else
			m_masterLastAnnounce = t;
	}

	if(m_masterPeer && m_masterPeer->state == ENET_PEER_STATE_CONNECTED &&
	   t - m_masterLastAnnounce >= 30.0){
		BitWriter w;
		BeginMsg(w, MSG_MASTER_ANNOUNCE);
		MsgMasterAnnounce a;
		a.gamePort = m_cfg.port;
		a.players = (uint8_t)ActivePlayerCount();
		a.maxPlayers = (uint8_t)m_cfg.maxPlayers;
		a.protocolVersion = PROTOCOL_VERSION;
		CopyStr(a.hostname, sizeof a.hostname, m_cfg.hostname.c_str());
		CopyStr(a.gameMode, sizeof a.gameMode, m_gamemode ? m_gamemode->Name() : "");
		WriteMasterAnnounce(w, a);
		ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
		if(enet_peer_send(m_masterPeer, 0, pkt) == 0){
			enet_host_flush(m_masterClient);
			m_masterLastAnnounce = t;
		}
		// on failure keep m_masterLastAnnounce as-is -> retried next tick
	}
}

// ---------------------------------------------------------------------------
void Server::InitDiscovery()
{
	m_discSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(m_discSock == INVALID_SOCKET) return;

	BOOL opt = 1;
	setsockopt(m_discSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof opt);
	setsockopt(m_discSock, SOL_SOCKET, SO_BROADCAST, (const char*)&opt, sizeof opt);

	sockaddr_in a;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = ADDR_ANY;
	a.sin_port = htons(DISCOVERY_PORT);
	if(bind(m_discSock, (sockaddr*)&a, sizeof a) != 0){
		std::printf("[WARN] LAN discovery bind failed on %u\n", (unsigned)DISCOVERY_PORT);
		closesocket(m_discSock);
		m_discSock = INVALID_SOCKET;
		return;
	}

	u_long nb = 1;
	ioctlsocket(m_discSock, FIONBIO, &nb);
	Log("LAN discovery listening on UDP %u", (unsigned)DISCOVERY_PORT);
}

void Server::PumpDiscovery()
{
	if(m_discSock == INVALID_SOCKET) return;

	char buf[256];
	sockaddr_in from;
	int flen = sizeof from;
	int n = recvfrom(m_discSock, buf, sizeof buf - 1, 0, (sockaddr*)&from, &flen);
	if(n <= 0) return;
	buf[n] = '\0';

	if(strcmp(buf, LAN_DISCOVER_MAGIC) == 0){
		char resp[512];
		int players = ActivePlayerCount();
		snprintf(resp, sizeof resp, "%s|%s|%d|%d|%s|%d|%u",
		         LAN_RESP_MAGIC, m_cfg.hostname.c_str(), players, m_cfg.maxPlayers,
		         m_gamemode ? m_gamemode->Name() : "", (int)PROTOCOL_VERSION,
		         (unsigned)m_cfg.port);
		sendto(m_discSock, resp, (int)strlen(resp), 0, (sockaddr*)&from, flen);
	}
}
