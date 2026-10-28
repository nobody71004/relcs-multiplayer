// src/net/netclient.h — engine-independent ENet client core for reLCS multiplayer.
// Used by the game (via nethooks) and self-contained: no game headers.
#pragma once

#include "rnet_protocol.h"

#include <string>
#include <map>
#include <deque>

struct NetChatLine {
	rnet::PlayerId fromId = 0;
	std::string text;
};

struct NetRemotePlayer {
	rnet::PlayerState state;
	std::string name;
	bool spawned = false;
	double lastUpdate = 0.0;
	rnet::MsgAppearance appearance;   // latest known look (model + component slots)
	bool haveAppearance = false;      // a MSG_APPEARANCE has arrived for this player
};

class NetClient {
public:
	enum EventType {
		EV_NONE = 0,
		EV_CONNECTED,      // welcome received; id valid
		EV_SPAWNED,        // own spawn (state = spawn state)
		EV_REMOTE_SPAWN,   // another player spawned (id/state)
		EV_REMOTE_QUIT,
		EV_CHAT,
		EV_DEATH,          // id = victim, text = killer id
		EV_CORRECTION,     // own authoritative state
		EV_DISCONNECTED,   // text = reason
		EV_GIVE,           // admin effect: kind/amount/arg for own player
		EV_INVENTORY,      // profile restore from server: inv = wallet + weapon table
		EV_APPEARANCE,     // a player's look: id = playerId, appearance = model + components
		EV_RCON_OK,        // the server accepted our rcon password
		EV_RCON_RESP,      // text = rcon response line
		EV_PICKUP,         // synced prop: kind=type amount=qty arg=weapon, pos in state
		// vehicle events: arg = vehicleId, model = modelId, id = playerId,
		// kind = seat, veh = VehicleState (EV_VEH_STATE / EV_VEH_SPAWN pos)
		EV_VEH_SPAWN,      // a net vehicle exists (state carries spawn pos/heading, id = owner)
		EV_VEH_ENTER,      // a player took a seat (kind = seat)
		EV_VEH_EXIT,       // a player left a vehicle
		EV_VEH_STATE,      // driver state update (veh)
		EV_VEH_DESPAWN,    // server retired a vehicle: drop the puppet (arg = id)
		EV_BANNER,         // large center-screen announcement (text)
		EV_WORLD,          // server world clock/weather: kind=hour arg=minute amount=weather
		EV_RES_LIST,       // mod/resource inventory updated (see resList)
		EV_VOICE,          // voice frame: id = speaker, text = one Opus packet
		EV_AC_STATUS,      // anti-cheat registration answer: kind = AcRegisterResult, text = detail
	};

	struct Event {
		int type = EV_NONE;
		rnet::PlayerId id = 0;   // player this event is about (16-bit since v8)
		uint8_t kind = 0;
		int32_t amount = 0;
		uint16_t arg = 0;
		uint16_t model = 0;
		std::string text;
		rnet::PlayerState state;
		rnet::VehicleState veh;
		rnet::MsgInventory inv;
		rnet::MsgAppearance appearance;
	};

	bool Connect(const char* host, uint16_t port, const char* nick, const char* password = nullptr);
	void Disconnect();
	void Pump();                                 // call every frame
	void SendState(const rnet::PlayerState& s);  // unreliable, 20 Hz
	void SendVoice(const uint8_t* packet, size_t n); // unreliable, one 20 ms Opus packet
	void SendChat(const char* text);
	void SendSpawnRequest();
	void SendRconAuth(const char* password);
	void SendRconCmd(const char* command);
	void SendVehEnter(uint16_t vehicleId, uint8_t seat, uint16_t modelId); // 0xFFFF = new
	void SendVehExit(uint16_t vehicleId);
	void SendVehState(const rnet::VehicleState& s); // unreliable, 20 Hz, driver only
	void SendInventory(const rnet::MsgInventory& m); // reliable snapshot (profile sync)
	void SendAppearance(const rnet::MsgAppearance& m); // reliable: "this is my look"

	// --- EOS anti-cheat ---
	// MSG_AC_HELLO (PUID + Connect ID token + platform) then the relay. The
	// session itself lives in acclient; this only moves bytes over the peer.
	void SendAcHello();
	void SendAcPayload(const void* data, uint32_t len);
	bool AcRegistered() const { return m_acStatus == rnet::AC_REG_OK; }
	uint8_t AcStatus() const { return m_acStatus; }

	bool Connected() const { return m_connected; }
	bool Spawned() const { return m_spawned; }
	rnet::PlayerId MyId() const { return m_myId; }
	const char* Hostname() const { return m_hostname.c_str(); }
	const char* GameMode() const { return m_gameMode.c_str(); }
	const char* MapName() const { return m_mapName.c_str(); }
	uint16_t Players() const { return m_players; }
	uint16_t MaxPlayers() const { return m_maxPlayers; }
	const char* MyName() const { return m_nick.c_str(); }

	bool PopEvent(Event& ev);

	std::map<rnet::PlayerId, NetRemotePlayer> remotes;
	std::deque<NetChatLine> chat;   // ring; game layer draws it
	rnet::PlayerState myState;
	rnet::MsgResList resList;       // server mod/resource inventory (admin panel)
	rnet::MsgPlayerPing board;      // latest MSG_PLAYER_PING: top players (scoreboard)

private:
	void HandleMessage(const uint8_t* data, size_t size);

	void* m_host = nullptr;   // ENetHost*
	void* m_peer = nullptr;   // ENetPeer*
	bool m_connected = false;
	bool m_welcomed = false;
	bool m_spawned = false;
	rnet::PlayerId m_myId = 0;
	std::string m_nick;
	std::string m_password;   // claim/restore the server-side profile (sent in MSG_HELLO)
	std::string m_hostname, m_gameMode, m_mapName;
	uint16_t m_players = 0, m_maxPlayers = 0;
	std::deque<Event> m_events;
	bool m_acHelloSent = false;
	uint8_t m_acStatus = 0xFF;      // 0xFF = no answer yet, else AcRegisterResult
	double m_acRetryAt = 0.0;
	int m_acAttempts = 0;
};
