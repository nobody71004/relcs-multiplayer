// net/master/main.cpp — reLCS master server (server list + favorites for the launcher).
//
// Protocol (ENet, UDP port 7800 by default):
//   game servers send MSG_MASTER_ANNOUNCE every 30 s
//   launchers send MSG_MASTER_LIST_REQ, get MSG_MASTER_LIST_RESP + entries
//   launchers send MSG_MASTER_FAV_LIST / _ADD / _DEL, get MSG_MASTER_FAV_RESP
// Entries expire after 90 s of silence; favorites live in SQLite (master.db).
#include "rnet_protocol.h"
#include "compat.h"
#include "favorites.h"
#include <enet/enet.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <map>
#include <deque>
#include <chrono>
#include <ctime>

using namespace rnet;

static double NowSec()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

struct Entry {
	uint32_t ip = 0;
	uint16_t gamePort = 0;
	uint8_t players = 0, maxPlayers = 0;
	uint16_t protocolVersion = 0;
	char hostname[MAX_HOSTNAME_LEN] = {};
	char gameMode[32] = {};
	char mapName[MAX_MAP_LEN] = {};
	double lastSeen = 0.0;
};

static std::map<uint64_t, Entry> g_entries;
static bool g_running = true;
static FavStore g_favorites;

// per-peer favorites rate limit: a public master must not let one client spin
struct OpWindow { double start = 0.0; int count = 0; };
static std::map<ENetPeer*, OpWindow> g_favWindows;
static const int FAV_OPS_PER_WINDOW = 40;
static const double FAV_WINDOW_SEC = 10.0;

static bool FavRateOk(ENetPeer* peer)
{
	double t = NowSec();
	OpWindow& w = g_favWindows[peer];
	if(t - w.start > FAV_WINDOW_SEC){
		w.start = t;
		w.count = 0;
	}
	return ++w.count <= FAV_OPS_PER_WINDOW;
}

static uint64_t Key(uint32_t ip, uint16_t port) { return ((uint64_t)ip << 16) | port; }

static void SendFavResp(ENetHost* host, ENetPeer* peer, const char* nick, bool ok, const char* err)
{
	MsgFavResp resp;
	resp.ok = ok ? 1 : 0;
	CopyStr(resp.error, sizeof resp.error, err ? err : "");
	std::vector<FavEntry> list;
	if(ok && nick && nick[0]) g_favorites.List(nick, list);
	resp.count = (uint8_t)(list.size() > MAX_FAV_ENTRIES ? MAX_FAV_ENTRIES : list.size());
	for(uint8_t i = 0; i < resp.count; i++){
		CopyStr(resp.entries[i].address, sizeof resp.entries[i].address, list[i].address.c_str());
		CopyStr(resp.entries[i].name, sizeof resp.entries[i].name, list[i].name.c_str());
	}
	BitWriter w;
	BeginMsg(w, MSG_MASTER_FAV_RESP);
	WriteFavResp(w, resp);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send(peer, 0, pkt);
	enet_host_flush(host);
}

static void ConsoleThread()
{
	std::string line;
	while(std::getline(std::cin, line)){
		if(line == "list"){
			for(auto& kv : g_entries){
				Entry& e = kv.second;
				char ipstr[32];
				snprintf(ipstr, sizeof ipstr, "%u.%u.%u.%u",
				         e.ip & 0xFF, (e.ip >> 8) & 0xFF, (e.ip >> 16) & 0xFF, (e.ip >> 24) & 0xFF);
				printf("%s:%u  \"%s\" mode=%s map=%s players=%d/%d age=%.0fs\n",
				       ipstr, (unsigned)e.gamePort, e.hostname, e.gameMode, e.mapName,
				       (int)e.players, (int)e.maxPlayers, NowSec() - e.lastSeen);
			}
			printf("%zu servers listed\n", g_entries.size());
			fflush(stdout);
		}else if(line == "count"){
			printf("%zu\n", g_entries.size());
			fflush(stdout);
		}else if(line == "favcount"){
			auto nicks = g_favorites.Nicks();
			printf("%zu username(s) with favorites\n", nicks.size());
			for(auto& n : nicks) printf("  %s\n", n.c_str());
			fflush(stdout);
		}else if(line.rfind("favs ", 0) == 0){
			std::string nick = line.substr(5);
			while(!nick.empty() && nick.back() == ' ') nick.pop_back();
			std::vector<FavEntry> list;
			if(!g_favorites.List(nick, list)){
				printf("favs: %s\n", g_favorites.LastError());
			}else{
				printf("%s: %zu favorite(s)\n", nick.c_str(), list.size());
				for(auto& f : list) printf("  %s  \"%s\"\n", f.address.c_str(), f.name.c_str());
			}
			fflush(stdout);
		}else if(line == "quit" || line == "exit"){
			g_running = false;
		}
	}
}

int main(int argc, char** argv)
{
	uint16_t port = DEFAULT_MASTER_PORT;
	std::string favPath = "master.db";
	for(int i = 1; i < argc - 1; i++){
		if(strcmp(argv[i], "-port") == 0) port = (uint16_t)atoi(argv[i + 1]);
		else if(strcmp(argv[i], "-favorites") == 0) favPath = argv[i + 1];
	}

	if(enet_initialize() != 0){
		printf("[ERR] enet_initialize failed\n");
		return 1;
	}

	if(g_favorites.Open(favPath))
		printf("[master] favorites: SQLite store at %s\n", favPath.c_str());
	else
		printf("[ERR] favorites store disabled: %s\n", g_favorites.LastError());

	ENetAddress address;
	address.host = ENET_HOST_ANY;
	address.port = port;
	ENetHost* host = enet_host_create(&address, 64, 2, 0, 0);
	if(!host){
		printf("[ERR] cannot bind UDP %u\n", (unsigned)port);
		return 1;
	}

	printf("[master] listening on UDP %u\n", (unsigned)port);
	fflush(stdout);

	std::thread console(ConsoleThread);
	console.detach();

	double lastSweep = NowSec();

	while(g_running){
		ENetEvent ev;
		while(enet_host_service(host, &ev, 0) > 0){
			switch(ev.type){
			case ENET_EVENT_TYPE_DISCONNECT:
				g_favWindows.erase(ev.peer);
				break;
			case ENET_EVENT_TYPE_RECEIVE:{
				BitReader r(ev.packet->data, ev.packet->dataLength);
				MsgType type = (MsgType)r.ReadU8();
				if(type == MSG_MASTER_ANNOUNCE){
					MsgMasterAnnounce a;
					if(ReadMasterAnnounce(r, a)){
						uint32_t ip = ev.peer->address.host;   // observed IP (NAT-safe)
						// server + master on the same box observe loopback, which is
						// unreachable from other machines — trust the advertised LAN IP
						if((ip & 0xFF) == 127 && a.advertisedIp) ip = a.advertisedIp;
						uint64_t key = Key(ip, a.gamePort);
						Entry& e = g_entries[key];
						e.ip = ip;
						e.gamePort = a.gamePort;
						e.players = a.players;
						e.maxPlayers = a.maxPlayers;
						e.protocolVersion = a.protocolVersion;
						CopyStr(e.hostname, sizeof e.hostname, a.hostname);
						CopyStr(e.gameMode, sizeof e.gameMode, a.gameMode);
						CopyStr(e.mapName, sizeof e.mapName, a.mapName);
						e.lastSeen = NowSec();
						printf("[master] announce %s (%d/%d) mode=%s map=%s\n",
						       e.hostname, (int)e.players, (int)e.maxPlayers, e.gameMode, e.mapName);
						fflush(stdout);
					}
				}else if(type == MSG_MASTER_LIST_REQ){
					BitWriter w;
					BeginMsg(w, MSG_MASTER_LIST_RESP);
					uint16_t count = 0;
					for(auto& kv : g_entries)
						if(NowSec() - kv.second.lastSeen < 90.0) count++;
					w.WriteU16(count);
					for(auto& kv : g_entries){
						if(NowSec() - kv.second.lastSeen >= 90.0) continue;
						MasterListEntry le;
						le.ipv4 = kv.second.ip;
						le.gamePort = kv.second.gamePort;
						le.players = kv.second.players;
						le.maxPlayers = kv.second.maxPlayers;
						le.pingMs = 0;
						CopyStr(le.hostname, sizeof le.hostname, kv.second.hostname);
						CopyStr(le.gameMode, sizeof le.gameMode, kv.second.gameMode);
						CopyStr(le.mapName, sizeof le.mapName, kv.second.mapName);
						le.protocolVersion = kv.second.protocolVersion;
						WriteMasterEntry(w, le);
					}
					ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(),
					                                     ENET_PACKET_FLAG_RELIABLE);
					enet_peer_send(ev.peer, 0, pkt);
					enet_host_flush(host);
				}else if(type == MSG_MASTER_FAV_LIST || type == MSG_MASTER_FAV_ADD ||
				         type == MSG_MASTER_FAV_DEL){
					if(!FavRateOk(ev.peer)){
						SendFavResp(host, ev.peer, nullptr, false, "too many requests");
					}else if(!g_favorites.Ready()){
						SendFavResp(host, ev.peer, nullptr, false, "favorites storage unavailable");
					}else{
						bool parsed = true;
						std::string err, nick, address, name;
						if(type == MSG_MASTER_FAV_LIST){
							MsgFavReq req;
							parsed = ReadFavReq(r, req);
							nick = req.nick;
						}else{
							// ADD and DEL share the add layout (DEL ignores the name)
							MsgFavAdd req;
							parsed = ReadFavAdd(r, req);
							nick = req.nick;
							address = req.address;
							name = req.name;
						}
						bool ok = parsed;
						if(!parsed) err = "malformed request";
						else if(!FavStore::ValidNick(nick)){ ok = false; err = "invalid username"; }
						else if(type == MSG_MASTER_FAV_ADD) ok = g_favorites.Add(nick, address, name, err);
						else if(type == MSG_MASTER_FAV_DEL) ok = g_favorites.Del(nick, address, err);
						SendFavResp(host, ev.peer, nick.c_str(), ok, err.c_str());
						printf("[master] fav %s nick=%s addr=%s ok=%d%s\n",
						       type == MSG_MASTER_FAV_ADD ? "add" :
						       type == MSG_MASTER_FAV_DEL ? "del" : "list",
						       nick.c_str(), address.c_str(), (int)ok,
						       ok ? "" : (" err=" + err).c_str());
						fflush(stdout);
					}
				}
				enet_packet_destroy(ev.packet);
				break;
			}
			default:
				break;
			}
		}

		double t = NowSec();
		if(t - lastSweep > 10.0){
			lastSweep = t;
			for(auto it = g_entries.begin(); it != g_entries.end(); ){
				if(t - it->second.lastSeen >= 90.0){
					printf("[master] expired %s\n", it->second.hostname);
					it = g_entries.erase(it);
				}else
					++it;
			}
		}

		Sleep(5);
	}

	g_favorites.Close();
	enet_host_destroy(host);
	enet_deinitialize();
	printf("[master] bye\n");
	return 0;
}
