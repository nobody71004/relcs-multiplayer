// src/net/netclient.cpp — NetClient implementation (ENet).
#include "netclient.h"
#include "acclient.h"

#include <enet/enet.h>

#include <chrono>
#include <cstdio>
#include <cstring>

using namespace rnet;

static bool s_enetReady = false;

static double NowSeconds()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// EOS hands AC traffic to a free function, but it has to leave through the one
// live connection: the owning client is registered here.
static NetClient* s_acOwner = nullptr;
static void AcToServer(const void* data, uint32_t len)
{
	if(s_acOwner) s_acOwner->SendAcPayload(data, len);
}

bool NetClient::Connect(const char* host, uint16_t port, const char* nick, const char* password)
{
	if(!s_enetReady){
		if(enet_initialize() != 0) return false;
		s_enetReady = true;
	}

	ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
	if(!client) return false;
	client->maximumPacketSize = 32 * 1024;

	ENetAddress addr;
	if(enet_address_set_host(&addr, host) < 0){
		enet_host_destroy(client);
		return false;
	}
	addr.port = port;
	ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
	if(!peer){
		enet_host_destroy(client);
		return false;
	}

	m_host = client;
	m_peer = peer;
	m_connected = false;
	m_welcomed = false;
	m_spawned = false;
	m_nick = nick ? nick : "Player";
	m_password = password ? password : "";
	m_acHelloSent = false;
	m_acStatus = 0xFF;
	m_acRetryAt = 0.0;
	m_acAttempts = 0;

	// Start the anti-cheat session before the handshake so registration can
	// follow the welcome within the server's grace window.
	s_acOwner = this;
	acclient::SetSendToServer(AcToServer);
	if(!acclient::Init() && acclient::state() != acclient::State::LoggingIn)
		fprintf(stderr, "[AC] anti-cheat unavailable: %s (%s)\n",
		        acclient::StateName(), acclient::Detail());

	fprintf(stderr, "[NET] connecting to %s:%u as %s\n", host, (unsigned)port, nick);
	fflush(stderr);
	return true;
}

void NetClient::Disconnect()
{
	board = rnet::MsgPlayerPing();
	if(m_host && m_peer)
		enet_peer_disconnect((ENetPeer*)m_peer, 0);
	// brief drain so the disconnect reaches the server
	if(m_host){
		ENetEvent ev;
		for(int i = 0; i < 20; i++){
			while(enet_host_service((ENetHost*)m_host, &ev, 0) > 0){
				if(ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
			}
			Sleep(5);
		}
		enet_host_destroy((ENetHost*)m_host);
	}
	m_host = nullptr;
	m_peer = nullptr;
	m_connected = false;
	m_acHelloSent = false;
	m_acAttempts = 0;
	m_acStatus = 0xFF;
	m_acRetryAt = 0.0;
	if(s_acOwner == this) s_acOwner = nullptr;
	// EndSession belongs to the server we just left: keeping it open would hold
	// the kernel driver attached, and the next server needs a fresh session.
	acclient::Shutdown();
}

void NetClient::SendAcHello()
{
	if(!m_host || !m_peer || !m_connected) return;
	MsgAcHello hello;
	hello.platform = AC_PLATFORM_WINDOWS;
	CopyStr(hello.productUserId, sizeof hello.productUserId, acclient::ProductUserId());
	std::string token;
	if(!acclient::CopyIdToken(token)){
		// the login is still finishing (or the token expired): try again shortly
		m_acRetryAt = 0.0;
		return;
	}
	CopyStr(hello.idToken, sizeof hello.idToken, token.c_str());

	BitWriter w;
	BeginMsg(w, MSG_AC_HELLO);
	WriteAcHello(w, hello);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
	enet_host_flush((ENetHost*)m_host);
	m_acHelloSent = true;
	m_acAttempts++;
	fprintf(stderr, "[AC] registering with the server as %s (attempt %d)\n",
	        acclient::ProductUserId(), m_acAttempts);
	fflush(stderr);
}

void NetClient::SendAcPayload(const void* data, uint32_t len)
{
	if(!m_host || !m_peer || !m_connected || !data || len == 0) return;
	if(len > (uint32_t)MAX_AC_PAYLOAD) return;
	MsgAcPayload m;
	m.len = (uint16_t)len;
	memcpy(m.data, data, len);
	BitWriter w;
	BeginMsg(w, MSG_AC_FROM_CLIENT);
	if(!WriteAcPayload(w, m)) return;
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
}

void NetClient::SendState(const PlayerState& s)
{
	if(!m_host || !m_peer || !m_connected) return;
	BitWriter w;
	BeginMsg(w, MSG_PLAYER_STATE);
	WritePlayerState(w, s);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), 0);
	enet_peer_send((ENetPeer*)m_peer, CHAN_STATE, pkt);
}

void NetClient::SendVoice(const uint8_t* packet, size_t n)
{
	if(!m_host || !m_peer || !m_connected) return;
	if(n == 0 || n > (size_t)VOICE_MAX_PACKET) return;
	BitWriter w;
	BeginMsg(w, MSG_VOICE);
	WriteVoice(w, m_myId, packet, (uint8_t)n);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), 0);
	enet_peer_send((ENetPeer*)m_peer, CHAN_STATE, pkt);
}

void NetClient::SendChat(const char* text)
{
	if(!m_host || !m_peer || !m_connected) return;
	BitWriter w;
	BeginMsg(w, MSG_CHAT);
	MsgChat c;
	c.fromId = m_myId;
	CopyStr(c.text, sizeof c.text, text);
	WriteChat(w, c);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
	enet_host_flush((ENetHost*)m_host);
}

void NetClient::SendRconAuth(const char* password)
{
	if(!m_host || !m_peer || !m_connected) return;
	BitWriter w;
	BeginMsg(w, MSG_RCON_AUTH);
	WriteStringMsg(w, password, 255);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
	enet_host_flush((ENetHost*)m_host);
}

void NetClient::SendRconCmd(const char* command)
{
	if(!m_host || !m_peer || !m_connected) return;
	BitWriter w;
	BeginMsg(w, MSG_RCON_CMD);
	WriteStringMsg(w, command, 255);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
	enet_host_flush((ENetHost*)m_host);
}

void NetClient::SendVehEnter(uint16_t vehicleId, uint8_t seat, uint16_t modelId)
{
	if(!m_host || !m_peer || !m_connected) return;
	BitWriter w;
	BeginMsg(w, MSG_VEH_ENTER);
	MsgVehEnter m;
	m.playerId = m_myId;
	m.vehicleId = vehicleId;
	m.seat = seat;
	m.modelId = modelId;
	WriteVehEnter(w, m);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
	enet_host_flush((ENetHost*)m_host);
}

void NetClient::SendVehExit(uint16_t vehicleId)
{
	if(!m_host || !m_peer || !m_connected) return;
	BitWriter w;
	BeginMsg(w, MSG_VEH_EXIT);
	MsgVehExit m;
	m.playerId = m_myId;
	m.vehicleId = vehicleId;
	WriteVehExit(w, m);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
	enet_host_flush((ENetHost*)m_host);
}

void NetClient::SendVehState(const VehicleState& s)
{
	if(!m_host || !m_peer || !m_connected) return;
	BitWriter w;
	BeginMsg(w, MSG_VEHICLE_STATE);
	WriteVehicleState(w, s);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), 0);
	enet_peer_send((ENetPeer*)m_peer, CHAN_STATE, pkt);
}

void NetClient::SendInventory(const MsgInventory& m)
{
	if(!m_host || !m_peer || !m_connected) return;
	BitWriter w;
	BeginMsg(w, MSG_INVENTORY);
	WriteInventory(w, m);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
	enet_host_flush((ENetHost*)m_host);
}

void NetClient::SendAppearance(const MsgAppearance& m)
{
	if(!m_host || !m_peer || !m_connected) return;
	BitWriter w;
	BeginMsg(w, MSG_APPEARANCE);
	WriteAppearance(w, m);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
	enet_host_flush((ENetHost*)m_host);
}

void NetClient::SendSpawnRequest()
{
	if(!m_host || !m_peer || !m_connected) return;
	BitWriter w;
	BeginMsg(w, MSG_SPAWN_REQ);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
	enet_host_flush((ENetHost*)m_host);
}

bool NetClient::PopEvent(Event& ev)
{
	if(m_events.empty()) return false;
	ev = m_events.front();
	m_events.pop_front();
	return true;
}

void NetClient::Pump()
{
	// the AC session has to keep ticking even between servers: EOS work is
	// what produces the messages that get relayed
	acclient::Tick();

	if(!m_host) return;

	ENetEvent ev;
	while(enet_host_service((ENetHost*)m_host, &ev, 0) > 0){
		switch(ev.type){
		case ENET_EVENT_TYPE_CONNECT:
			m_connected = true;
			// send HELLO
			{
				BitWriter w;
				BeginMsg(w, MSG_HELLO);
				MsgHello h;
				CopyStr(h.name, sizeof h.name, m_nick.c_str());
				CopyStr(h.password, sizeof h.password, m_password.c_str());
				WriteHello(w, h);
				ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(),
				                                     ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send((ENetPeer*)m_peer, CHAN_RELIABLE, pkt);
				enet_host_flush((ENetHost*)m_host);
			}
			break;
		case ENET_EVENT_TYPE_RECEIVE:
			HandleMessage(ev.packet->data, ev.packet->dataLength);
			enet_packet_destroy(ev.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			m_connected = false;
			{
				Event e;
				e.type = EV_DISCONNECTED;
				e.text = "connection lost";
				m_events.push_back(e);
			}
			break;
		default:
			break;
		}
	}

	// register with this server as soon as the AC session is live (the server
	// only keeps protected clients when it is configured to require them)
	if(m_connected && m_welcomed && !m_acHelloSent){
		const double now = NowSeconds();
		if(acclient::Ready() && now >= m_acRetryAt) SendAcHello();
	}
}

void NetClient::HandleMessage(const uint8_t* data, size_t size)
{
	BitReader r(data, size);
	MsgType type = (MsgType)r.ReadU8();

	switch(type){
	case MSG_WELCOME:{
		MsgWelcome wel;
		if(ReadWelcome(r, wel)){
			m_welcomed = true;
			m_myId = wel.playerId;
			m_hostname = wel.hostname;
			m_gameMode = wel.gameMode;
			m_mapName = wel.mapName;
			m_players = wel.players;
			m_maxPlayers = wel.maxPlayers;
			fprintf(stderr, "[NET] WELCOME id=%d server='%s' mode=%s map=%s players=%d/%d\n",
			        (int)m_myId, wel.hostname, wel.gameMode, wel.mapName,
			        (int)wel.players, (int)wel.maxPlayers);
			fflush(stderr);
			Event e;
			e.type = EV_CONNECTED;
			e.id = m_myId;
			m_events.push_back(e);
			// initial world clock/weather from the welcome
			Event w;
			w.type = EV_WORLD;
			w.kind = wel.hour;
			w.arg = wel.minute;
			w.amount = wel.weather;
			m_events.push_back(w);
		}
		break;
	}
	case MSG_SPAWN:{
		MsgSpawn sp;
		if(ReadSpawn(r, sp)){
			if(sp.playerId == m_myId){
				m_spawned = true;
				myState.playerId = m_myId;
				myState.xMm = QuantPos(sp.x);
				myState.yMm = QuantPos(sp.y);
				myState.zMm = QuantPos(sp.z);
				myState.heading = QuantAngle(sp.heading);
				myState.health = sp.health;
				myState.armour = sp.armour;
				myState.vehicleId = 0xFFFF;
				fprintf(stderr, "[NET] SPAWN at (%.1f, %.1f, %.1f)\n", sp.x, sp.y, sp.z);
				fflush(stderr);
				Event e;
				e.type = EV_SPAWNED;
				e.id = m_myId;
				e.kind = sp.skinModel ? 1 : 0;   // 0 = server has no saved look
				e.state = myState;
				m_events.push_back(e);
			}else{
				NetRemotePlayer& rp = remotes[sp.playerId];
				rp.spawned = true;
				rp.state.playerId = sp.playerId;
				rp.state.xMm = QuantPos(sp.x);
				rp.state.yMm = QuantPos(sp.y);
				rp.state.zMm = QuantPos(sp.z);
				rp.state.heading = QuantAngle(sp.heading);
				Event e;
				e.type = EV_REMOTE_SPAWN;
				e.id = sp.playerId;
				e.state = rp.state;
				m_events.push_back(e);
			}
		}
		break;
	}
	case MSG_PLAYER_STATE:{
		PlayerState st;
		if(ReadPlayerState(r, st) && st.playerId != m_myId){
			NetRemotePlayer& rp = remotes[st.playerId];
			rp.state = st;
			rp.spawned = true;
			rp.lastUpdate = 0.0; // refreshed by Pump caller via wall clock if needed
		}
		break;
	}
	case MSG_VOICE:{
		uint8_t from = 0;
		uint8_t packet[VOICE_MAX_PACKET];
		uint8_t len = 0;
		if(ReadVoice(r, from, packet, len) && r.BitsLeft() == 0 && from != m_myId){
			Event e;
			e.type = EV_VOICE;
			e.id = from;
			e.text.assign((const char*)packet, len);
			m_events.push_back(e);
		}
		break;
	}
	case MSG_CHAT:{
		MsgChat c;
		if(ReadChat(r, c)){
			chat.push_back({ c.fromId, c.text });
			if(chat.size() > 8) chat.pop_front();
			fprintf(stderr, "[NET] CHAT %s\n", c.text);
			fflush(stderr);
		}
		break;
	}
	case MSG_PLAYER_JOIN:{
		MsgPlayerJoin j;
		if(ReadPlayerJoin(r, j)){
			fprintf(stderr, "[NET] JOIN id=%d %s\n", (int)j.playerId, j.name);
			fflush(stderr);
			NetRemotePlayer& rp = remotes[j.playerId];
			rp.name = j.name;
		}
		break;
	}
	case MSG_PLAYER_QUIT:{
		uint8_t id = r.ReadU8();
		fprintf(stderr, "[NET] QUIT id=%d\n", (int)id);
		fflush(stderr);
		remotes.erase(id);
		Event e;
		e.type = EV_REMOTE_QUIT;
		e.id = id;
		m_events.push_back(e);
		break;
	}
	case MSG_DEATH:{
		MsgDeath d;
		if(ReadDeath(r, d)){
			Event e;
			e.type = EV_DEATH;
			e.id = d.victimId;
			e.text = std::to_string((int)d.killerId);
			m_events.push_back(e);
		}
		break;
	}
	case MSG_CORRECTION:{
		PlayerState fix;
		if(ReadPlayerState(r, fix)){
			myState = fix;
			Event e;
			e.type = EV_CORRECTION;
			e.state = fix;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_GIVE:{
		MsgGive g;
		if(ReadGive(r, g) && g.targetId == m_myId){
			Event e;
			e.type = EV_GIVE;
			e.id = g.targetId;
			e.kind = g.kind;
			e.amount = g.amount;
			e.arg = g.arg;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_INVENTORY:{
		MsgInventory m;
		if(ReadInventory(r, m)){
			Event e;
			e.type = EV_INVENTORY;
			e.inv = m;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_APPEARANCE:{
		MsgAppearance m;
		if(ReadAppearance(r, m)){
			// store it on the remote (own id still flows through so the game layer
			// can confirm/apply its own restored look)
			if(m.playerId != m_myId){
				NetRemotePlayer& rp = remotes[(uint8_t)m.playerId];
				rp.appearance = m;
				rp.haveAppearance = true;
			}
			Event e;
			e.type = EV_APPEARANCE;
			e.id = (uint8_t)m.playerId;
			e.appearance = m;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_PICKUP_SPAWN:{
		MsgPickup pk;
		if(ReadPickup(r, pk)){
			Event e;
			e.type = EV_PICKUP;
			e.kind = pk.type;
			e.arg = pk.weapon;
			e.amount = (int32_t)pk.quantity;
			e.model = pk.modelIndex;
			e.state.xMm = QuantPos(pk.x);
			e.state.yMm = QuantPos(pk.y);
			e.state.zMm = QuantPos(pk.z);
			m_events.push_back(e);
		}
		break;
	}
	case MSG_RCON_OK:{
		// the auth packet is answered on the reliable channel before any command
		// we send afterwards, so this flips the admin panel to "logged in"
		Event e;
		e.type = EV_RCON_OK;
		m_events.push_back(e);
		break;
	}
	case MSG_RCON_RESP:{
		char text[256] = {};
		ReadStringMsg(r, text, sizeof text, 255);
		Event e;
		e.type = EV_RCON_RESP;
		e.text = text;
		m_events.push_back(e);
		break;
	}
	case MSG_VEHICLE_STATE:{
		VehicleState vs;
		if(ReadVehicleState(r, vs)){
			Event e;
			e.type = EV_VEH_STATE;
			e.arg = vs.vehicleId;
			e.model = vs.modelId;
			e.veh = vs;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_VEH_SPAWN:{
		MsgVehSpawn vs;
		if(ReadVehSpawn(r, vs)){
			fprintf(stderr, "[NET] VEH_SPAWN id=%d model=%d\n", (int)vs.vehicleId, (int)vs.modelId);
			fflush(stderr);
			Event e;
			e.type = EV_VEH_SPAWN;
			e.id = vs.ownerId;
			e.arg = vs.vehicleId;
			e.model = vs.modelId;
			e.state.xMm = QuantPos(vs.x);
			e.state.yMm = QuantPos(vs.y);
			e.state.zMm = QuantPos(vs.z);
			e.state.heading = QuantAngle(vs.heading);
			m_events.push_back(e);
		}
		break;
	}
	case MSG_VEH_ENTER:{
		MsgVehEnter m;
		if(ReadVehEnter(r, m)){
			fprintf(stderr, "[NET] VEH_ENTER player=%d veh=%d seat=%d\n",
			        (int)m.playerId, (int)m.vehicleId, (int)m.seat);
			fflush(stderr);
			Event e;
			e.type = EV_VEH_ENTER;
			e.id = m.playerId;
			e.arg = m.vehicleId;
			e.kind = m.seat;
			e.model = m.modelId;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_VEH_EXIT:{
		MsgVehExit m;
		if(ReadVehExit(r, m)){
			fprintf(stderr, "[NET] VEH_EXIT player=%d veh=%d\n", (int)m.playerId, (int)m.vehicleId);
			fflush(stderr);
			Event e;
			e.type = EV_VEH_EXIT;
			e.id = m.playerId;
			e.arg = m.vehicleId;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_VEH_DESPAWN:{
		MsgVehDespawn m;
		if(ReadVehDespawn(r, m)){
			fprintf(stderr, "[NET] VEH_DESPAWN veh=%d\n", (int)m.vehicleId);
			fflush(stderr);
			Event e;
			e.type = EV_VEH_DESPAWN;
			e.arg = m.vehicleId;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_KICK:
	case MSG_DISCONNECT:{
		MsgKick k;
		ReadKick(r, k);
		fprintf(stderr, "[NET] KICKED: %s (code %u)\n", k.reason, (unsigned)k.code);
		fflush(stderr);
		Event e;
		e.type = EV_DISCONNECTED;
		e.kind = k.code;   // JoinRefused, so the game can show an auth-specific hint
		e.text = k.reason;
		m_events.push_back(e);
		break;
	}
	case MSG_DAMAGE:{
		MsgDamage d;
		if(ReadDamage(r, d) && d.targetId == m_myId){
			uint16_t dmg = d.damage;
			if(myState.armour >= dmg) myState.armour = (uint8_t)(myState.armour - dmg);
			else {
				dmg = (uint16_t)(dmg - myState.armour);
				myState.armour = 0;
				myState.health = (uint8_t)(myState.health > dmg ? myState.health - dmg : 0);
			}
		}
		break;
	}
	case MSG_WORLD:{
		// authoritative server world clock / weather
		MsgWorld wrld;
		if(ReadWorld(r, wrld)){
			Event e;
			e.type = EV_WORLD;
			e.kind = wrld.hour;
			e.arg = wrld.minute;
			e.amount = wrld.weather;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_RES_LIST:{
		// mod/resource inventory for the admin panel
		MsgResList list;
		if(ReadResList(r, list)){
			resList = list;
			Event e;
			e.type = EV_RES_LIST;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_SCORE_UPDATE:
		break;   // scores ride their own message; never parse as gamemode text
	case MSG_PLAYER_PING:{
		rnet::MsgPlayerPing pp;
		if(ReadPlayerPing(r, pp)) board = pp;
		break;
	}
	case MSG_GAMEMODE_EVENT:{
		// server announcements (broadcast / round events) -> large banner
		MsgGamemodeEvent ev;
		if(ReadGamemodeEvent(r, ev) && ev.text[0] &&
		   (ev.type == GMEV_TEXT || ev.type == GMEV_ROUND_START || ev.type == GMEV_ROUND_END)){
			Event e;
			e.type = EV_BANNER;
			e.text = ev.text;
			m_events.push_back(e);
		}
		break;
	}
	case MSG_AC_TO_CLIENT:{
		// opaque EOS anti-cheat traffic: never parsed, only handed to the SDK
		MsgAcPayload m;
		if(ReadAcPayload(r, m)) acclient::OnServerMessage(m.data, m.len);
		break;
	}
	case MSG_AC_STATUS:{
		MsgAcStatus st;
		if(ReadAcStatus(r, st)){
			m_acStatus = st.status;
			const char* why = st.detail[0] ? st.detail : "no detail";
			fprintf(stderr, "[AC] %s: %s\n", st.registered ? "registered" : "not registered", why);
			fflush(stderr);
			// a refusal is worth one more try (token not ready, EOS still
			// booting); the server's grace window bounds how long that lasts
			if(!st.registered && m_acAttempts < 5) m_acRetryAt = NowSeconds() + 5.0;
			Event e;
			e.type = EV_AC_STATUS;
			e.kind = st.status;
			e.text = why;
			m_events.push_back(e);
		}
		break;
	}
	default:
		break;
	}
}
