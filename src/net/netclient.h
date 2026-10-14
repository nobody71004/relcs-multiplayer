// src/net/netclient.h — engine-independent ENet client core for reLCS multiplayer.
// Used by the game (via nethooks) and self-contained: no game headers.
#pragma once

#include "rnet_protocol.h"

#include <string>
#include <map>
#include <deque>

struct NetChatLine {
	uint8_t fromId = 0;
	std::string text;
};

struct NetRemotePlayer {
	rnet::PlayerState state;
	std::string name;
	bool spawned = false;
	bool relayed = false;    // state came from MSG_PLAYER_STATE: vehicleId/seat are the server's
	double lastUpdate = 0.0;
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
		EV_RCON_RESP,      // text = rcon response line
		EV_PICKUP,         // synced prop: kind=type amount=qty arg=weapon, pos in state
		// vehicle events: arg = vehicleId, model = modelId, id = playerId,
		// kind = seat, veh = VehicleState (EV_VEH_STATE), state = position
		// (EV_VEH_SPAWN, and EV_VEH_ENTER: where the server has the vehicle)
		EV_VEH_SPAWN,      // a net vehicle exists (state carries spawn pos/heading, id = owner)
		EV_VEH_ENTER,      // a player took a seat (kind = seat)
		EV_VEH_EXIT,       // a player left a vehicle
		EV_VEH_STATE,      // driver state update (veh)
		EV_VEH_DESPAWN,    // server retired a vehicle: drop the puppet (arg = id)
		EV_BANNER,         // large center-screen announcement (text)
		EV_WORLD,          // server world clock/weather: kind=hour arg=minute amount=weather
		EV_RES_LIST,       // mod/resource inventory updated (see resList)
		EV_VOICE,          // voice frame: id = speaker, text = one Opus packet
	};

	struct Event {
		int type = EV_NONE;
		uint8_t id = 0;
		uint8_t kind = 0;
		int32_t amount = 0;
		uint16_t arg = 0;
		uint16_t model = 0;
		std::string text;
		rnet::PlayerState state;
		rnet::VehicleState veh;
		rnet::MsgInventory inv;
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
	// 0xFFFF = new; x/y/z/heading = where the car is (places a new one)
	void SendVehEnter(uint16_t vehicleId, uint8_t seat, uint16_t modelId,
	                  float x, float y, float z, float heading);
	void SendVehExit(uint16_t vehicleId);
	void SendVehState(const rnet::VehicleState& s); // unreliable, 20 Hz, driver only
	void SendInventory(const rnet::MsgInventory& m); // reliable snapshot (profile sync)

	bool Connected() const { return m_connected; }
	bool Spawned() const { return m_spawned; }
	uint8_t MyId() const { return m_myId; }
	const char* Hostname() const { return m_hostname.c_str(); }
	const char* GameMode() const { return m_gameMode.c_str(); }
	const char* MapName() const { return m_mapName.c_str(); }
	uint8_t Players() const { return m_players; }
	uint8_t MaxPlayers() const { return m_maxPlayers; }
	const char* MyName() const { return m_nick.c_str(); }

	bool PopEvent(Event& ev);

	std::map<uint8_t, NetRemotePlayer> remotes;
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
	uint8_t m_myId = 0;
	std::string m_nick;
	std::string m_password;   // claim/restore the server-side profile (sent in MSG_HELLO)
	std::string m_hostname, m_gameMode, m_mapName;
	uint8_t m_players = 0, m_maxPlayers = 0;
	std::deque<Event> m_events;
};
