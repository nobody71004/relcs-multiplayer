// net/launcher/main.cpp — reLCS-launcher: Chromium (CEF) server browser + join.
//
// UI      : launcher.html rendered by cefui.dll (windowed Chromium)
// Tabs    : Internet (master list) | LAN (broadcast discovery) | Favorites
// Join    : reLCS.exe -connect <ip:port> -nick <name>
// Config  : launcher.ini    (game=, nick=, master=)
// Profiles: usernames.ini   (saved usernames)
// Log     : launcher.log    (the E2E run book verifies this headlessly)
//
// CLI: -game <path> -nick <name> -master <host:port>
//      -selftest   : boot the CEF UI + query master + LAN, write results to
//                    launcher.log, exit
//      -join <ip:port> : connect flow without GUI (spawns the game directly)
#include "rnet_protocol.h"
#include "rnet_eos.h"
#include "eac.h"
#include "eos_bootstrap.h"
#include <enet/enet.h>

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>

using namespace rnet;

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
struct ServerRow {
	std::string hostname, gameMode, address;
	std::string mapName;              // live map (probe / announce / LAN)
	int players = 0, maxPlayers = 0;
	int pingMs = 0;
	int protocolVersion = 0;          // 0 = unknown, else flag stale servers
	bool favorite = false;            // came from the favorites list
	bool live = false;                // answered a probe
	bool online = false;              // reachable right now
};

static std::string g_gameExe = "reLCS.exe";
static std::string g_nick = "Player";
// server rcon password for in-game admin tools; defaults to the fleet's password so a
// fresh install can open the admin panel without editing launcher.ini
static std::string g_rconPw = DEFAULT_RCON_PASSWORD;
static std::string g_pass;   // profile password (claims/restores the name in-game)
// public staging master (VPS) — the Internet tab lists its servers by default.
// launcher.ini `master = host:port` overrides it; the LAN tab (broadcast) is
// independent of this and keeps finding local servers.
static std::string g_masterHost = "88.214.59.166";
static uint16_t g_masterPort = DEFAULT_MASTER_PORT;
static std::vector<std::string> g_profiles;   // saved usernames (usernames.ini)

// EOS client credentials live in eos.ini next to the launcher (never committed:
// the dev portal value is per-product and rotating it here needs no rebuild).
static void ReadEosCredentials(std::string& clientId, std::string& clientSecret)
{
	clientId.clear();
	clientSecret.clear();
	FILE* f = fopen("eos.ini", "r");
	if(!f) return;
	char line[512];
	while(fgets(line, sizeof line, f)){
		std::string l = line;
		size_t hash = l.find_first_of("#;");
		if(hash != std::string::npos) l.resize(hash);
		size_t eq = l.find('=');
		if(eq == std::string::npos) continue;
		std::string k = l.substr(0, eq), v = l.substr(eq + 1);
		auto trim = [](std::string& s){
			while(!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
			while(!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
		};
		trim(k); trim(v);
		if(k == "client_id") clientId = v;
		else if(k == "client_secret") clientSecret = v;
	}
	fclose(f);
}

static FILE* g_log = nullptr;
static void Log(const char* fmt, ...)
{
	char buf[1024];
	va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	if(g_log){ fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

// ---------------------------------------------------------------------------
// cefui.dll (loaded dynamically — the launcher has no build-time CEF dep)
// ---------------------------------------------------------------------------
typedef int  (*fn_api_version)(void);
typedef int  (*fn_execute_process)(void*);
typedef int  (*fn_init_windowed)(void*, int, int, int, int, const char*);
typedef void (*fn_run_loop)(void);
typedef void (*fn_tick)(void);
typedef void (*fn_exec)(const char*);
typedef void (*fn_query_handler)(void (*)(const char*, void*), void*);
typedef void (*fn_resize)(int, int);
typedef void (*fn_close)(void);
typedef void (*fn_shutdown)(void);

static HMODULE g_dll = nullptr;
static fn_execute_process g_execute_process = nullptr;
static fn_init_windowed g_init_windowed = nullptr;
static fn_run_loop     g_run_loop = nullptr;
static fn_tick         g_tick = nullptr;
static fn_exec         g_exec = nullptr;
static fn_query_handler g_set_handler = nullptr;
static fn_resize       g_resize = nullptr;
static fn_close        g_close = nullptr;
static fn_shutdown     g_shutdown = nullptr;

static std::atomic<bool> g_loaded{false};      // page's "loaded" message seen
static std::atomic<bool> g_gotTitle{false};    // host->JS->host round trip seen

static bool LoadCefUi()
{
	const char* names[] = { "cefui.dll", "..\\bin\\win-amd64-librw_d3d9-oal\\Release\\cefui.dll" };
	for(const char* n : names){
		g_dll = LoadLibraryA(n);
		if(g_dll) break;
	}
	if(!g_dll){
		Log("[CEF] cefui.dll not found (own dir or ..\\bin\\win-amd64-librw_d3d9-oal\\Release)");
		return false;
	}
	fn_api_version p_version = (fn_api_version)GetProcAddress(g_dll, "cefui_api_version");
	g_execute_process = (fn_execute_process)GetProcAddress(g_dll, "cefui_execute_process");
	g_init_windowed = (fn_init_windowed)GetProcAddress(g_dll, "cefui_init_windowed");
	g_run_loop      = (fn_run_loop)GetProcAddress(g_dll, "cefui_run_loop");
	g_tick          = (fn_tick)GetProcAddress(g_dll, "cefui_tick");
	g_exec          = (fn_exec)GetProcAddress(g_dll, "cefui_exec");
	g_set_handler   = (fn_query_handler)GetProcAddress(g_dll, "cefui_set_query_handler");
	g_resize        = (fn_resize)GetProcAddress(g_dll, "cefui_resize");
	g_close         = (fn_close)GetProcAddress(g_dll, "cefui_close_browser");
	g_shutdown      = (fn_shutdown)GetProcAddress(g_dll, "cefui_shutdown");
	if(!p_version || !g_init_windowed || !g_run_loop || !g_exec || !g_set_handler){
		Log("[CEF] cefui.dll is missing exports");
		return false;
	}
	Log("[CEF] cefui.dll loaded (api v%d)", p_version());
	return true;
}

// ---------------------------------------------------------------------------
// queries (worker threads)
// ---------------------------------------------------------------------------
static std::vector<ServerRow> QueryMaster(const std::string& host, uint16_t port)
{
	std::vector<ServerRow> rows;
	ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
	if(!client) return rows;

	ENetAddress addr;
	if(enet_address_set_host(&addr, host.c_str()) < 0){
		enet_host_destroy(client);
		return rows;
	}
	addr.port = port;
	ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
	if(!peer){ enet_host_destroy(client); return rows; }

	double t0 = GetTickCount64() / 1000.0;
	bool sent = false;
	while(GetTickCount64() / 1000.0 - t0 < 3.0){
		ENetEvent ev;
		while(enet_host_service(client, &ev, 0) > 0){
			if(ev.type == ENET_EVENT_TYPE_CONNECT && !sent){
				BitWriter w;
				BeginMsg(w, MSG_MASTER_LIST_REQ);
				ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(),
				                                     ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send(peer, 0, pkt);
				sent = true;
			}
			if(ev.type == ENET_EVENT_TYPE_RECEIVE){
				BitReader r(ev.packet->data, ev.packet->dataLength);
				MsgType type = (MsgType)r.ReadU8();
				if(type == MSG_MASTER_LIST_RESP){
					uint16_t count = r.ReadU16();
					for(uint16_t i = 0; i < count; i++){
						MasterListEntry e;
						if(!ReadMasterEntry(r, e)) break;
						ServerRow row;
						row.hostname = e.hostname;
						row.gameMode = e.gameMode;
						row.players = e.players;
						row.maxPlayers = e.maxPlayers;
						row.pingMs = e.pingMs;
						char ipstr[32];
						snprintf(ipstr, sizeof ipstr, "%u.%u.%u.%u",
						         e.ipv4 & 0xFF, (e.ipv4 >> 8) & 0xFF,
						         (e.ipv4 >> 16) & 0xFF, (e.ipv4 >> 24) & 0xFF);
						char addrstr[48];
						snprintf(addrstr, sizeof addrstr, "%s:%u", ipstr, (unsigned)e.gamePort);
						row.address = addrstr;
						rows.push_back(row);
					}
				}
				enet_packet_destroy(ev.packet);
			}
		}
		if(sent && !rows.empty()) break;
		Sleep(10);
	}
	enet_host_destroy(client);
	return rows;
}

// reachability of a master: a bare ENet connect (no request), 3 s budget
static bool ProbeMaster(const std::string& host, uint16_t port, std::string& err, int& ms)
{
	ms = -1;
	ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
	if(!client){ err = "no network stack"; return false; }
	ENetAddress addr;
	if(enet_address_set_host(&addr, host.c_str()) < 0){
		enet_host_destroy(client);
		err = "cannot resolve " + host;
		return false;
	}
	addr.port = port;
	ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
	if(!peer){ enet_host_destroy(client); err = "cannot start a connection"; return false; }

	ULONGLONG t0 = GetTickCount64();
	bool connected = false;
	while(!connected && GetTickCount64() - t0 < 3000){
		ENetEvent ev;
		int n = enet_host_service(client, &ev, 50);
		if(n < 0) break;
		if(n == 0) continue;
		if(ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
		else if(ev.type == ENET_EVENT_TYPE_DISCONNECT){ err = "connection refused"; break; }
		else if(ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
	}
	if(connected){
		ms = (int)(GetTickCount64() - t0);
		enet_peer_disconnect_now(peer, 0);
	}else{
		if(err.empty()) err = "no answer within 3 s";
		enet_peer_reset(peer);
	}
	enet_host_destroy(client);
	return connected;
}

static std::vector<ServerRow> QueryMasterDefault()
{
	return QueryMaster(g_masterHost, g_masterPort);
}

// masterHints (optional): "ip:port" of masters hinted by LAN responders
static std::vector<ServerRow> QueryLan(std::vector<std::string>* masterHints = nullptr)
{
	std::vector<ServerRow> rows;
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(s == INVALID_SOCKET) return rows;
	BOOL bc = 1;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&bc, sizeof bc);
	u_long nb = 1;
	ioctlsocket(s, FIONBIO, &nb);

	sockaddr_in dst;
	memset(&dst, 0, sizeof dst);
	dst.sin_family = AF_INET;
	dst.sin_port = htons(DISCOVERY_PORT);
	dst.sin_addr.s_addr = INADDR_BROADCAST;
	sendto(s, LAN_DISCOVER_MAGIC, (int)strlen(LAN_DISCOVER_MAGIC), 0,
	       (sockaddr*)&dst, sizeof dst);
	inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
	sendto(s, LAN_DISCOVER_MAGIC, (int)strlen(LAN_DISCOVER_MAGIC), 0,
	       (sockaddr*)&dst, sizeof dst);
	// subnet-directed broadcasts: the limited broadcast (255.255.255.255)
	// often leaves through the wrong NIC on multi-homed machines
	{
		ULONG asz = 16 * 1024;
		std::vector<uint8_t> abuf(asz);
		IP_ADAPTER_ADDRESSES* addrs = (IP_ADAPTER_ADDRESSES*)abuf.data();
		ULONG rc = GetAdaptersAddresses(AF_INET,
		         GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
		         nullptr, addrs, &asz);
		if(rc == ERROR_BUFFER_OVERFLOW){
			abuf.resize(asz);
			addrs = (IP_ADAPTER_ADDRESSES*)abuf.data();
			rc = GetAdaptersAddresses(AF_INET,
			         GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
			         nullptr, addrs, &asz);
		}
		if(rc == NO_ERROR){
			for(IP_ADAPTER_ADDRESSES* a = addrs; a; a = a->Next){
				if(a->OperStatus != IfOperStatusUp) continue;
				for(IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next){
					if(u->Address.lpSockaddr->sa_family != AF_INET) continue;
					uint32_t ip = ((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr;
					if((ip & 0xFF) == 127) continue;
					uint8_t prefix = u->OnLinkPrefixLength ? u->OnLinkPrefixLength : 24;
					// network-order-on-little-endian: the low `prefix` bits are network
					uint32_t mask = prefix >= 32 ? 0xFFFFFFFFu : ((1u << prefix) - 1u);
					dst.sin_addr.s_addr = ip | ~mask;
					sendto(s, LAN_DISCOVER_MAGIC, (int)strlen(LAN_DISCOVER_MAGIC), 0,
					       (sockaddr*)&dst, sizeof dst);
				}
			}
		}
	}

	double t0 = GetTickCount64() / 1000.0;
	while(GetTickCount64() / 1000.0 - t0 < 2.0){
		char buf[512];
		sockaddr_in from; int flen = sizeof from;
		int n = recvfrom(s, buf, sizeof buf - 1, 0, (sockaddr*)&from, &flen);
		if(n > 0){
			buf[n] = '\0';
			// RELCS_RESP_V1|hostname|players|max|gamemode|version|gameport|masterport|map
			std::vector<std::string> parts;
			std::string s2 = buf, cur;
			for(char c : s2){
				if(c == '|'){ parts.push_back(cur); cur.clear(); }
				else cur.push_back(c);
			}
			parts.push_back(cur);
			if(parts.size() >= 7 && parts[0] == LAN_RESP_MAGIC){
				ServerRow row;
				row.hostname = parts[1];
				row.players = atoi(parts[2].c_str());
				row.maxPlayers = atoi(parts[3].c_str());
				row.gameMode = parts[4];
				char ipstr[32];
				snprintf(ipstr, sizeof ipstr, "%s", inet_ntoa(from.sin_addr));
				char addrstr[48];
				snprintf(addrstr, sizeof addrstr, "%s:%s", ipstr, parts[6].c_str());
				row.address = addrstr;
				// a LAN responder advertises its protocol version and map — the
				// browser shows both without probing
				row.protocolVersion = atoi(parts[5].c_str());
				if(parts.size() >= 9) row.mapName = parts[8];
				rows.push_back(row);
				// optional tail: the responder's master port — lets the Internet
				// tab find the list with zero configuration on other machines
				if(masterHints && parts.size() >= 8 && !parts[7].empty()){
					std::string hint = std::string(ipstr) + ":" + parts[7];
					bool dup = false;
					for(const std::string& h : *masterHints) if(h == hint){ dup = true; break; }
					if(!dup) masterHints->push_back(hint);
				}
			}
		}
		Sleep(10);
	}
	closesocket(s);
	// the same server answers on several broadcast paths — one row each
	std::vector<ServerRow> unique;
	for(const ServerRow& r : rows){
		bool dup = false;
		for(const ServerRow& u : unique) if(u.address == r.address){ dup = true; break; }
		if(!dup) unique.push_back(r);
	}
	return unique;
}

// ---------------------------------------------------------------------------
// live server info: a HELLO_PROBE handshake
// ---------------------------------------------------------------------------
// The server answers MSG_WELCOME (hostname, mode, map, players) and allocates no
// player slot, so the launcher can sample every listed server safely. The
// HELLO -> WELCOME round trip is also a real ping sample for the graph.
struct ServerInfo {
	bool ok = false;
	std::string error;
	std::string hostname, gameMode, mapName;
	int players = 0, maxPlayers = 0;
	int pingMs = -1;
	int protocolVersion = PROTOCOL_VERSION;
	bool versionMismatch = false;
};

static bool SplitAddress(const std::string& address, std::string& host, uint16_t& port)
{
	size_t c = address.rfind(':');
	if(c == std::string::npos) return false;
	host = address.substr(0, c);
	int p = atoi(address.c_str() + c + 1);
	if(host.empty() || p <= 0 || p > 65535) return false;
	port = (uint16_t)p;
	return true;
}

static ServerInfo ProbeServer(const std::string& address, double budgetSec = 2.5)
{
	ServerInfo info;
	std::string host;
	uint16_t port = 0;
	if(!SplitAddress(address, host, port)){ info.error = "bad address"; return info; }

	ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
	if(!client){ info.error = "enet host"; return info; }
	ENetAddress addr;
	if(enet_address_set_host(&addr, host.c_str()) < 0){
		enet_host_destroy(client);
		info.error = "cannot resolve host";
		return info;
	}
	addr.port = port;
	ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
	if(!peer){ enet_host_destroy(client); info.error = "cannot connect"; return info; }

	double t0 = GetTickCount64() / 1000.0, sentAt = 0.0;
	bool connected = false, sentHello = false, gotWelcome = false;
	while(GetTickCount64() / 1000.0 - t0 < budgetSec){
		ENetEvent ev;
		while(enet_host_service(client, &ev, 0) > 0){
			switch(ev.type){
			case ENET_EVENT_TYPE_CONNECT:
				connected = true;
				break;
			case ENET_EVENT_TYPE_RECEIVE:{
				BitReader r(ev.packet->data, ev.packet->dataLength);
				MsgType type = (MsgType)r.ReadU8();
				if(type == MSG_WELCOME){
					MsgWelcome wel;
					if(ReadWelcome(r, wel)){
						gotWelcome = true;
						info.hostname = wel.hostname;
						info.gameMode = wel.gameMode;
						info.mapName = wel.mapName;
						info.players = wel.players;
						info.maxPlayers = wel.maxPlayers;
						if(sentAt > 0.0)
							info.pingMs = (int)((GetTickCount64() / 1000.0 - sentAt) * 1000.0 + 0.5);
					}
				}else if(type == MSG_KICK || type == MSG_DISCONNECT){
					MsgKick k;
					if(ReadKick(r, k)){
						info.error = k.reason;
						if(strstr(k.reason, "protocol")) info.versionMismatch = true;
					}
				}
				enet_packet_destroy(ev.packet);
				break;
			}
			default: break;
			}
		}
		if(connected && !sentHello){
			sentHello = true;
			BitWriter w;
			BeginMsg(w, MSG_HELLO);
			MsgHello h;
			h.protocolVersion = PROTOCOL_VERSION;
			h.flags = HELLO_PROBE;
			h.gameVersion = GAME_VERSION_TAG;
			CopyStr(h.name, sizeof h.name, "launcher");
			WriteHello(w, h);
			ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
			enet_peer_send(peer, 0, pkt);
			enet_host_flush(client);
			sentAt = GetTickCount64() / 1000.0;
		}
		if(gotWelcome) break;
		Sleep(5);
	}

	if(!gotWelcome && info.error.empty())
		info.error = connected ? "no reply from the server" : "no route to the server";
	info.ok = gotWelcome;
	// ENet keeps its own estimate; fall back to it when timing was too coarse
	if(info.ok && info.pingMs <= 0)
		info.pingMs = (int)peer->roundTripTime;
	if(peer) enet_peer_disconnect_now(peer, 0);
	enet_host_destroy(client);
	return info;
}

// refresh a row from a probe: live map/mode/players + a ping sample
static bool ProbeIntoRow(const std::string& address, ServerRow& row, std::string& err,
                         double budgetSec = 2.5)
{
	ServerInfo info = ProbeServer(address, budgetSec);
	row.online = info.ok;
	if(!info.ok){
		err = info.error.empty() ? "unreachable" : info.error;
		return false;
	}
	row.live = true;
	if(!info.hostname.empty()) row.hostname = info.hostname;
	if(!info.gameMode.empty()) row.gameMode = info.gameMode;
	row.mapName = info.mapName;
	row.players = info.players;
	row.maxPlayers = info.maxPlayers;
	row.pingMs = info.pingMs;
	row.protocolVersion = info.protocolVersion;
	return true;
}

// ---------------------------------------------------------------------------
// favorites: the master stores them (per username); favorites.ini is a cache
// ---------------------------------------------------------------------------
static std::mutex g_rowMutex;
static std::vector<ServerRow> g_lastRows;   // the list the UI is showing

static void SaveFavoritesCache(const std::vector<ServerRow>& rows)
{
	std::ofstream f("favorites.ini");
	f << "# cached copy of the master-side favorites (server is authoritative)\n";
	for(const auto& r : rows)
		f << r.address << " " << (r.hostname.empty() ? r.address : r.hostname) << "\n";
}

// one request/response exchange with the master; op = FAV_LIST/ADD/DEL
static bool MasterFavOp(MsgType op, const std::string& address, const std::string& name,
                        std::vector<ServerRow>& out, std::string& err)
{
	out.clear();
	ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
	if(!client){ err = "enet host"; return false; }
	ENetAddress addr;
	if(enet_address_set_host(&addr, g_masterHost.c_str()) < 0){
		enet_host_destroy(client);
		err = "cannot resolve master";
		return false;
	}
	addr.port = g_masterPort;
	ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
	if(!peer){ enet_host_destroy(client); err = "cannot reach master"; return false; }

	double t0 = GetTickCount64() / 1000.0;
	bool connected = false, sent = false, done = false;
	while(GetTickCount64() / 1000.0 - t0 < 3.5 && !done){
		ENetEvent ev;
		while(enet_host_service(client, &ev, 0) > 0){
			if(ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
			else if(ev.type == ENET_EVENT_TYPE_RECEIVE){
				BitReader r(ev.packet->data, ev.packet->dataLength);
				if((MsgType)r.ReadU8() == MSG_MASTER_FAV_RESP){
					MsgFavResp resp;
					if(ReadFavResp(r, resp)){
						done = true;
						if(!resp.ok){
							err = resp.error;
							if(err.empty()) err = "master rejected the request";
						}else{
							for(uint8_t i = 0; i < resp.count; i++){
								ServerRow row;
								row.address = resp.entries[i].address;
								row.hostname = resp.entries[i].name[0] ? resp.entries[i].name : row.address;
								row.gameMode = "-";
								row.favorite = true;
								out.push_back(row);
							}
						}
					}
				}
				enet_packet_destroy(ev.packet);
			}
		}
		if(connected && !sent){
			sent = true;
			BitWriter w;
			if(op == MSG_MASTER_FAV_LIST){
				BeginMsg(w, MSG_MASTER_FAV_LIST);
				MsgFavReq req;
				CopyStr(req.nick, sizeof req.nick, g_nick.c_str());
				WriteFavReq(w, req);
			}else{
				BeginMsg(w, op);
				MsgFavAdd req;
				CopyStr(req.nick, sizeof req.nick, g_nick.c_str());
				CopyStr(req.address, sizeof req.address, address.c_str());
				CopyStr(req.name, sizeof req.name, name.c_str());
				WriteFavAdd(w, req);
			}
			ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
			enet_peer_send(peer, 0, pkt);
			enet_host_flush(client);
		}
		Sleep(5);
	}
	if(!done && err.empty()) err = connected ? "master did not answer" : "master unreachable";
	if(peer) enet_peer_disconnect_now(peer, 0);
	enet_host_destroy(client);
	return done;
}

static std::vector<ServerRow> LoadFavorites()
{
	std::vector<ServerRow> rows;
	std::ifstream f("favorites.ini");
	std::string line;
	while(std::getline(f, line)){
		if(line.empty() || line[0] == '#') continue;
		ServerRow row;
		size_t sp = line.find(' ');
		if(sp == std::string::npos){
			row.address = line;
			row.hostname = line;
		}else{
			row.address = line.substr(0, sp);
			row.hostname = line.substr(sp + 1);
		}
		row.gameMode = "-";
		rows.push_back(row);
	}
	return rows;
}

// ---------------------------------------------------------------------------
// username profiles
// ---------------------------------------------------------------------------
static bool ValidUsername(const std::string& u)
{
	if(u.size() < 3 || u.size() > 20) return false;
	for(char c : u)
		if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		     (c >= '0' && c <= '9') || c == '_'))
			return false;
	return true;
}

static void LoadProfiles()
{
	g_profiles.clear();
	std::ifstream f("usernames.ini");
	std::string line;
	while(std::getline(f, line)){
		while(!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		if(ValidUsername(line)) g_profiles.push_back(line);
	}
}

static void SaveProfiles()
{
	std::ofstream f("usernames.ini");
	for(auto& u : g_profiles)
		f << u << "\n";
}

// remember a username (dedup, keep newest last)
static bool RememberUser(const std::string& u)
{
	if(!ValidUsername(u)) return false;
	for(size_t i = 0; i < g_profiles.size(); i++)
		if(g_profiles[i] == u){
			g_profiles.erase(g_profiles.begin() + i);
			break;
		}
	g_profiles.push_back(u);
	SaveProfiles();
	return true;
}

// ---------------------------------------------------------------------------
// config
// ---------------------------------------------------------------------------
static void LoadConfig()
{
	std::ifstream f("launcher.ini");
	std::string line;
	auto trim = [](std::string& s){
		while(!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
		size_t p = s.find_first_not_of(" \t");
		s = (p == std::string::npos) ? "" : s.substr(p);
	};
	while(std::getline(f, line)){
		size_t eq = line.find('=');
		if(eq == std::string::npos) continue;
		std::string key = line.substr(0, eq), val = line.substr(eq + 1);
		trim(key);
		trim(val);
		if(key == "game") g_gameExe = val;
		else if(key == "nick") g_nick = val;
		else if(key == "rcon") g_rconPw = val.empty() ? std::string(DEFAULT_RCON_PASSWORD) : val;
		else if(key == "pass") g_pass = val;
		else if(key == "master"){
			size_t c = val.find(':');
			g_masterHost = val.substr(0, c);
			if(c != std::string::npos) g_masterPort = (uint16_t)atoi(val.c_str() + c + 1);
		}
	}
}

static void SaveConfig()
{
	std::ofstream f("launcher.ini");
	f << "game = " << g_gameExe << "\n";
	f << "nick = " << g_nick << "\n";
	f << "master = " << g_masterHost << ":" << g_masterPort << "\n";
	f << "rcon = " << g_rconPw << "\n";
	// profile password, plaintext in the local config (same trust level as rcon)
	f << "pass = " << g_pass << "\n";
}

// ---------------------------------------------------------------------------
// join flow
// ---------------------------------------------------------------------------
static std::string g_lastJoinError;

// ---------------------------------------------------------------------------
// game install auto-detection
// ---------------------------------------------------------------------------
static bool FileExists(const std::string& p)
{
	DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirExists(const std::string& p)
{
	DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string EnvDir(const char* name)
{
	char buf[MAX_PATH] = {};
	DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
	return (n > 0 && n < sizeof buf) ? std::string(buf) : std::string();
}

// a directory is a usable install when it holds the game data the engine needs;
// cefui.dll is required for the in-game UI, so report it separately
static bool GameDirOk(const std::string& dir, bool& dataOk, bool& cefOk)
{
	std::string exe = dir + "\\reLCS.exe";
	if(!FileExists(exe)) return false;
	dataOk = FileExists(dir + "\\models\\gta3.img") ||
	         FileExists(dir + "\\data\\default.ide") ||
	         FileExists(dir + "\\DATA\\DEFAULT.IDE");
	cefOk = FileExists(dir + "\\cefui.dll");
	return true;
}

static bool RegReadString(HKEY root, const char* sub, const char* value, std::string& out)
{
	HKEY key = nullptr;
	if(RegOpenKeyExA(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
	char buf[1024] = {};
	DWORD size = sizeof buf, type = 0;
	LONG rc = RegQueryValueExA(key, value, nullptr, &type, (LPBYTE)buf, &size);
	RegCloseKey(key);
	if(rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
	out = buf;
	return !out.empty();
}

// one level of uninstall entries: DisplayName -> InstallLocation
static void RegUninstallDirs(std::vector<std::string>& out)
{
	const char* roots[] = {
		"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
		"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
	};
	HKEY hives[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
	for(HKEY hive : hives){
		for(const char* r : roots){
			HKEY key = nullptr;
			if(RegOpenKeyExA(hive, r, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
			char sub[256];
			DWORD idx = 0, subLen = sizeof sub;
			while(RegEnumKeyExA(key, idx++, sub, &subLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS){
				subLen = sizeof sub;
				std::string display, loc;
				std::string path = std::string(r) + "\\" + sub;
				if(RegReadString(hive, path.c_str(), "DisplayName", display) &&
				   (display.find("reLCS") != std::string::npos ||
				    display.find("Liberty City Stories") != std::string::npos) &&
				   RegReadString(hive, path.c_str(), "InstallLocation", loc) && !loc.empty())
					out.push_back(loc);
			}
			RegCloseKey(key);
		}
	}
}

// bounded directory walk (depth 2, capped) for "somewhere in Downloads"
static void ScanForGame(const std::string& root, int depth, std::vector<std::string>& out, int& budget)
{
	if(depth < 0 || budget <= 0) return;
	std::string pattern = root + "\\*";
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if(h == INVALID_HANDLE_VALUE){
		// the root itself may be the install
		bool dataOk = false, cefOk = false;
		if(GameDirOk(root, dataOk, cefOk)) out.push_back(root);
		return;
	}
	do{
		if(budget-- <= 0) break;
		if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
		if(fd.cFileName[0] == '.') continue;
		std::string sub = root + "\\" + fd.cFileName;
		bool dataOk = false, cefOk = false;
		if(GameDirOk(sub, dataOk, cefOk)){ out.push_back(sub); continue; }
		if(depth > 0) ScanForGame(sub, depth - 1, out, budget);
	}while(FindNextFileA(h, &fd));
	FindClose(h);
}

// detection order: configured path, registry, common install dirs, next to the
// launcher, a bounded scan of the usual download/game folders, then PATH.
// Returns the exe path ("") when nothing looks like an install.
static std::string DetectGameExe(std::string& how, bool& dataOk, std::string& err)
{
	how.clear();
	err.clear();
	dataOk = false;
	std::vector<std::pair<std::string, const char*>> cands;   // path or dir + source

	if(!g_gameExe.empty())
		cands.push_back({ g_gameExe, "settings" });

	// registry: launcher-written hints, App Paths, uninstall entries
	const char* regKeys[] = { "SOFTWARE\\reLCS", "SOFTWARE\\WOW6432Node\\reLCS" };
	HKEY hives[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
	for(HKEY hive : hives)
		for(const char* k : regKeys){
			std::string dir;
			if(RegReadString(hive, k, "InstallDir", dir)) cands.push_back({ dir + "\\reLCS.exe", "registry" });
		}
	{
		std::string ap;
		if(RegReadString(HKEY_LOCAL_MACHINE,
		     "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\reLCS.exe", "", ap))
			cands.push_back({ ap, "registry" });
	}
	{
		std::vector<std::string> dirs;
		RegUninstallDirs(dirs);
		for(const auto& d : dirs) cands.push_back({ d + "\\reLCS.exe", "registry (uninstall entry)" });
	}

	// common install locations
	char self[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, self, sizeof self);
	std::string selfDir = self;
	size_t selfSlash = selfDir.find_last_of('\\');
	if(selfSlash != std::string::npos) selfDir.resize(selfSlash);

	std::vector<std::string> commonDirs;
	for(const char* env : { "ProgramFiles", "ProgramFiles(x86)", "ProgramW6432" }){
		std::string b = EnvDir(env);
		if(!b.empty()) commonDirs.push_back(b + "\\reLCS");
	}
	for(const char* env : { "LOCALAPPDATA", "APPDATA" }){
		std::string b = EnvDir(env);
		if(!b.empty()) commonDirs.push_back(b + "\\reLCS");
	}
	std::string up = EnvDir("USERPROFILE");
	if(!up.empty()){
		commonDirs.push_back(up + "\\Downloads\\reLCS");
		commonDirs.push_back(up + "\\Desktop\\reLCS");
		commonDirs.push_back(up + "\\Documents\\reLCS");
	}
	commonDirs.push_back("C:\\Games\\reLCS");
	for(const auto& d : commonDirs) cands.push_back({ d + "\\reLCS.exe", "common path" });

	// next to the launcher (shipped builds) and the dev tree
	cands.push_back({ selfDir + "\\reLCS.exe", "launcher directory" });
	cands.push_back({ selfDir + "\\..\\..\\..\\..\\reLCS\\reLCS.exe", "dev tree" });
	cands.push_back({ selfDir + "\\..\\..\\..\\..\\bin\\win-amd64-librw_d3d9-oal\\Release\\reLCS.exe", "dev build" });
	cands.push_back({ "reLCS.exe", "PATH" });

	for(auto& c : cands){
		bool data = false, cef = false;
		std::string dir = c.first;
		char full[MAX_PATH * 2] = {};
		if(dir.find('\\') != std::string::npos && dir.find(".exe") == std::string::npos){
			GetFullPathNameA(dir.c_str(), sizeof full, full, nullptr);
			dir = full;
			if(GameDirOk(dir, data, cef)){
				dir += "\\reLCS.exe";
				how = c.second;
				dataOk = data;
				if(!data) err = "game data missing (models/gta3.img)";
				return dir;
			}
			continue;
		}
		if(!FileExists(dir)) continue;
		GetFullPathNameA(dir.c_str(), sizeof full, full, nullptr);
		std::string exe = full, exeDir = exe;
		size_t s = exeDir.find_last_of('\\');
		exeDir = (s == std::string::npos) ? std::string() : exeDir.substr(0, s);
		bool dirOk = !exeDir.empty() && GameDirOk(exeDir, data, cef);
		how = c.second;
		dataOk = data;
		if(!dirOk) err = "found the exe but its data files are not next to it";
		else if(!data) err = "game data missing (models/gta3.img)";
		else if(!cef) err = "cefui.dll is missing (the in-game UI will not show)";
		return exe;
	}

	// bounded scan of the folders games actually get extracted to
	std::vector<std::string> roots;
	if(!up.empty()){
		roots.push_back(up + "\\Downloads");
		roots.push_back(up + "\\Desktop");
		roots.push_back(up + "\\Documents");
		roots.push_back(up);
	}
	roots.push_back("C:\\Games");
	std::vector<std::string> found;
	for(const auto& r : roots){
		if(!DirExists(r)) continue;
		int budget = 300;
		ScanForGame(r, 1, found, budget);
		if(!found.empty()) break;
	}
	if(!found.empty()){
		bool data = false, cef = false;
		GameDirOk(found[0], data, cef);
		how = "folder scan";
		dataOk = data;
		if(!data) err = "game data missing (models/gta3.img)";
		else if(!cef) err = "cefui.dll is missing (the in-game UI will not show)";
		return found[0] + "\\reLCS.exe";
	}

	if(err.empty()) err = "no game install found";
	return "";
}

// find reLCS.exe: explicit config first, then well-known locations
static std::string ResolveGameExe()
{
	std::string how, err;
	bool dataOk = false;
	return DetectGameExe(how, dataOk, err);
}

static bool JoinServer(const std::string& address, const std::string& nickIn)
{
	g_lastJoinError.clear();
	std::string nick = nickIn.empty() ? g_nick : nickIn;
	if(!ValidUsername(nick)){
		Log("[JOIN] rejected username \"%s\" (3-20 chars: letters, digits, _)", nick.c_str());
		nick = "Player";
	}
	g_nick = nick;
	RememberUser(g_nick);
	SaveConfig();

	std::string how, detErr;
	bool dataOk = false;
	std::string exe = DetectGameExe(how, dataOk, detErr);
	if(exe.empty()){
		g_lastJoinError = "game install not found (" + detErr + ") — set the path in Settings";
		Log("[JOIN] %s", g_lastJoinError.c_str());
		return false;
	}
	Log("[JOIN] game detected via %s: %s (data=%d)", how.c_str(), exe.c_str(), (int)dataOk);

	// the game must run from its own directory (CEF runtime, gameui.html,
	// AUDIO/ and the game files all resolve against the CWD)
	std::string exeDir = exe;
	size_t slash = exeDir.find_last_of('\\');
	if(slash != std::string::npos) exeDir.resize(slash);

	std::string gameArgs = "-connect " + address + " -nick \"" + g_nick + "\"";
	// profile password: claims the name / restores the saved wallet + weapons
	if(!g_pass.empty())
		gameArgs += " -password \"" + g_pass + "\"";
	// admin tools (broadcast/give/...) need the server's rcon password in-game
	if(!g_rconPw.empty())
		gameArgs += " -rcon \"" + g_rconPw + "\"";

	// Easy Anti-Cheat: refresh the runtime + product config, then start through
	// the protected bootstrapper so the game is actually covered by EAC.
	std::string exeName = exe.substr(exe.find_last_of('\\') == std::string::npos ? 0 : exe.find_last_of('\\') + 1);
	eac::Status ac;
	eac::Prepare(exeDir, exeName, ac);
	Log("[EAC] %s", ac.log.empty() ? "prepare skipped" : ac.log.c_str());
	if(!ac.lastError.empty()) Log("[EAC] %s", ac.lastError.c_str());

	std::string launchErr;
	bool protectedLaunch = false;
	Log("[JOIN] args %s  (cwd=%s)", gameArgs.c_str(), exeDir.c_str());
	if(!eac::Launch(exeDir, exeName, gameArgs, launchErr, protectedLaunch)){
		g_lastJoinError = launchErr;
		Log("[JOIN] %s for %s", launchErr.c_str(), exe.c_str());
		return false;
	}
	Log("[JOIN] started %s", protectedLaunch ? "EAC-protected build (start_protected_game.exe)" : "bare game (EAC bootstrapper missing)");
	return true;
}

// ---------------------------------------------------------------------------
// JS <-> host plumbing (lcs:// scheme)
// ---------------------------------------------------------------------------
// JSON string escaping (the value goes between double quotes in a JSON text).
// A single quote is legal JSON and must NOT be backslash-escaped here: `\'` is
// not a JSON escape and made JSON.parse reject any value containing one.
static std::string Jesc(const std::string& s)
{
	std::string o;
	for(char c : s){
		if(c == '\\' || c == '"'){ o += '\\'; o += c; }
		else if((unsigned char)c < 0x20) continue;
		else o += c;
	}
	return o;
}

// The JSON text travels inside a single-quoted JavaScript string literal, so it
// needs a second escaping layer of its own. With one layer the JS literal ate
// the JSON escapes: a game path reached JSON.parse with bare backslashes (an
// invalid escape), so the init reply never parsed and the page stayed on
// "Game: detecting" / "Player —" whenever a game install was found.
static void Reply(const std::string& json)
{
	if(!g_exec) return;
	std::string lit;
	lit.reserve(json.size() + 16);
	for(char c : json){
		if(c == '\\' || c == '\''){ lit += '\\'; lit += c; }
		else lit += c;
	}
	g_exec(("window.onCefReply && onCefReply('" + lit + "')").c_str());
}

static std::string RowToJson(const ServerRow& r)
{
	char buf[512];
	snprintf(buf, sizeof buf, "{\"hostname\":\"%s\",\"gameMode\":\"%s\",\"address\":\"%s\","
	         "\"map\":\"%s\",\"players\":%d,\"maxPlayers\":%d,\"pingMs\":%d,"
	         "\"protocolVersion\":%d,\"favorite\":%s,\"live\":%s,\"online\":%s}",
	         Jesc(r.hostname).c_str(), Jesc(r.gameMode).c_str(), Jesc(r.address).c_str(),
	         Jesc(r.mapName).c_str(), r.players, r.maxPlayers, r.pingMs, r.protocolVersion,
	         r.favorite ? "true" : "false", r.live ? "true" : "false", r.online ? "true" : "false");
	return buf;
}

static std::string RowsToJson(const std::vector<ServerRow>& rows)
{
	std::string j = "[";
	for(size_t i = 0; i < rows.size(); i++)
		j += (i ? "," : "") + RowToJson(rows[i]);
	j += "]";
	return j;
}

static std::string ProfilesToJson()
{
	std::string j = "[";
	for(size_t i = 0; i < g_profiles.size(); i++)
		j += (i ? ",\"" : "\"") + Jesc(g_profiles[i]) + "\"";
	j += "]";
	return j;
}

static void HandleQuery(const char* request, void*)
{
	if(!request) return;
	std::string req(request);
	size_t q = req.find('?');
	std::string cmd = req.substr(0, q);
	std::string args = q == std::string::npos ? "" : req.substr(q + 1);
	// proper query-string parsing: params are &-separated
	// the UI percent-encodes every value (encodeURIComponent): "127.0.0.1:7777"
	// arrives as "127.0.0.1%3A7777" — decode before use (was the reason GUI
	// joins never connected: the game got a still-encoded address)
	auto urldecode = [](std::string s){
		auto hex = [](char c) -> int {
			if(c >= '0' && c <= '9') return c - '0';
			if(c >= 'a' && c <= 'f') return c - 'a' + 10;
			if(c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};
		std::string o;
		for(size_t i = 0; i < s.size(); i++){
			if(s[i] == '%' && i + 2 < s.size()){
				int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
				if(hi >= 0 && lo >= 0){ o += (char)((hi << 4) | lo); i += 2; continue; }
			}
			if(s[i] == '+'){ o += ' '; continue; }
			o += s[i];
		}
		return o;
	};
	auto arg = [&](const char* key) -> std::string {
		std::string k = std::string(key) + "=";
		size_t pos = 0;
		while(pos <= args.size()){
			if((pos == 0 || args[pos - 1] == '&') && args.rfind(k, pos) == pos){
				std::string v = args.substr(pos + k.size());
				size_t amp = v.find('&');
				if(amp != std::string::npos) v.resize(amp);
				return urldecode(v);
			}
			size_t next = args.find('&', pos);
			if(next == std::string::npos) break;
			pos = next + 1;
		}
		return "";
	};

	Log("[CEF] query: %s", request);

	if(cmd == "eosstatus"){
		Reply("{\"reply\":\"eosstatus\",\"eos\":" + eosboot::Json() + "}");
	}else if(cmd == "eacstatus"){
		std::string how2, gErr2;
		bool dataOk2 = false;
		std::string game = DetectGameExe(how2, dataOk2, gErr2);
		std::string dir = game;
		size_t s2 = dir.find_last_of('\\');
		std::string name = "reLCS.exe";
		if(s2 != std::string::npos){ name = dir.substr(s2 + 1); dir.resize(s2); }
		eac::Status ac;
		if(!dir.empty()) eac::Prepare(dir, name, ac);
		std::string js = "{\"reply\":\"eacstatus\",\"ok\":" + std::string(ac.runtimeInstalled && ac.configWritten ? "true" : "false") +
		                 ",\"runtime\":" + std::string(ac.runtimeInstalled ? "true" : "false") +
		                 ",\"config\":" + std::string(ac.configWritten ? "true" : "false") +
		                 ",\"integrity\":" + std::string(ac.integrityBuilt ? "true" : "false") +
		                 ",\"version\":\"" + Jesc(ac.version) + "\"" +
		                 ",\"error\":\"" + Jesc(ac.lastError.empty() ? gErr2 : ac.lastError) + "\"" +
		                 ",\"log\":\"" + Jesc(ac.log) + "\"}";
		Reply(js);
	}else if(cmd == "eacinstall"){
		std::string how3, gErr3;
		bool dataOk3 = false;
		std::string game = DetectGameExe(how3, dataOk3, gErr3);
		std::string dir = game;
		size_t s3 = dir.find_last_of('\\');
		if(s3 != std::string::npos) dir.resize(s3);
		eac::Status ac;
		bool ok = !dir.empty() && eac::InstallService(dir, ac);
		Reply("{\"reply\":\"eacinstall\",\"ok\":" + std::string(ok ? "true" : "false") +
		      ",\"error\":\"" + Jesc(ac.lastError) + "\",\"log\":\"" + Jesc(ac.log) + "\"}");
	}else if(cmd == "eacintegrity"){
		std::string how4, gErr4;
		bool dataOk4 = false;
		std::string game = DetectGameExe(how4, dataOk4, gErr4);
		std::string dir = game, name = "reLCS.exe";
		size_t s4 = dir.find_last_of('\\');
		if(s4 != std::string::npos){ name = dir.substr(s4 + 1); dir.resize(s4); }
		eac::Status ac;
		bool ok = !dir.empty() && eac::BuildIntegrity(dir, name, ac);
		Reply("{\"reply\":\"eacintegrity\",\"ok\":" + std::string(ok ? "true" : "false") +
		      ",\"signed\":" + std::string(ac.integrityBuilt ? "true" : "false") +
		      ",\"error\":\"" + Jesc(ac.lastError) + "\",\"log\":\"" + Jesc(ac.log) + "\"}");
	}else if(cmd == "loaded"){
		g_loaded = true;
		std::string how, gErr;
		bool dataOk = false;
		std::string gameShown = DetectGameExe(how, dataOk, gErr);
		bool cefOk = false;
		if(!gameShown.empty()){
			std::string d = gameShown;
			size_t s = d.find_last_of('\\');
			if(s != std::string::npos){
				d.resize(s);
				bool data2 = false;
				GameDirOk(d, data2, cefOk);
			}
		}
		Reply("{\"reply\":\"init\",\"nick\":\"" + Jesc(g_nick) +
		      "\",\"game\":\"" + Jesc(gameShown) +
		      "\",\"gameHow\":\"" + Jesc(how) +
		      "\",\"gameData\":" + (dataOk ? "true" : "false") +
		      "\",\"gameCef\":" + (cefOk ? "true" : "false") +
		      "\",\"gameError\":\"" + Jesc(gErr) +
		      "\",\"version\":" + std::to_string((int)PROTOCOL_VERSION) +
		      "\",\"master\":\"" + Jesc(g_masterHost + ":" + std::to_string(g_masterPort)) +
		      "\",\"rcon\":\"" + Jesc(g_rconPw) +
		      "\",\"pass\":\"" + Jesc(g_pass) +
		      "\",\"profiles\":" + ProfilesToJson() + "}");
	}else if(cmd.rfind("title ", 0) == 0){
		if(req.find("CEF-OK") != std::string::npos) g_gotTitle = true;
	}else if(cmd == "refresh"){
		std::string which = arg("tab");
		std::thread([which](){
			std::vector<ServerRow> rows;
			std::string source = "internet", note;
			if(which == "lan"){ rows = QueryLan(); source = "lan"; }
			else if(which == "favorites"){
				// favorites live on the master (per username); the local file is
				// only a cache for when the master cannot be reached
				std::vector<ServerRow> favs;
				std::string err;
				if(MasterFavOp(MSG_MASTER_FAV_LIST, "", "", favs, err)){
					rows = favs;
					source = "master";
					SaveFavoritesCache(rows);
				}else{
					rows = LoadFavorites();
					source = "local cache";
					note = "master unavailable (" + err + ") — showing the cached list";
					Log("[FAV] %s", note.c_str());
				}
			}
			else {
				rows = QueryMasterDefault();
				if(rows.empty()){
					// configured master unreachable (fresh machines default to
					// 127.0.0.1) — fall back to a master discovered on the LAN
					std::vector<std::string> hints;
					QueryLan(&hints);
					for(const std::string& h : hints){
						size_t c = h.rfind(':');
						if(c == std::string::npos) continue;
						rows = QueryMaster(h.substr(0, c), (uint16_t)atoi(h.substr(c + 1).c_str()));
						if(!rows.empty()){
							Log("[QUERY] master fallback via %s", h.c_str());
							break;
						}
					}
				}
			}
			{
				// remember what the UI is showing: probe/pingall address rows by index
				std::lock_guard<std::mutex> lk(g_rowMutex);
				g_lastRows = rows;
			}
			Log("[QUERY] %s -> %zu servers", source.c_str(), rows.size());
			Reply("{\"reply\":\"servers\",\"source\":\"" + source +
			      "\",\"note\":\"" + Jesc(note) +
			      "\",\"rows\":" + RowsToJson(rows) + "}");
		}).detach();
	}else if(cmd == "join"){
		std::string address = arg("address");
		std::string nick = arg("nick");
		std::string pass = arg("pass");
		if(args.find("pass=") != std::string::npos) g_pass = pass; // allow clearing
		bool ok = !address.empty() && JoinServer(address, nick);
		if(address.empty()) g_lastJoinError = "no server address";
		Reply(std::string("{\"reply\":\"join\",\"ok\":") + (ok ? "true" : "false") +
		      ",\"address\":\"" + Jesc(address) + "\",\"error\":\"" + Jesc(g_lastJoinError) + "\"}");
	}else if(cmd == "addfav"){
		// legacy local-only add (kept for older pages / offline use)
		std::string address = arg("address"), name = arg("name");
		bool ok = !address.empty();
		if(ok){
			std::ofstream f("favorites.ini", std::ios::app);
			f << address << " " << (name.empty() ? address : name) << "\n";
		}
		Reply(std::string("{\"reply\":\"addfav\",\"ok\":") + (ok ? "true" : "false") + "}");
	}else if(cmd == "saveuser"){
		std::string nick = arg("nick");
		bool ok = RememberUser(nick);
		if(ok){ g_nick = nick; SaveConfig(); }
		Reply(std::string("{\"reply\":\"saveuser\",\"ok\":") + (ok ? "true" : "false") +
		      ",\"nick\":\"" + Jesc(nick) + "\",\"profiles\":" + ProfilesToJson() + "}");
	}else if(cmd == "config"){
		std::string game = arg("game"), master = arg("master"), nick = arg("nick");
		std::string rcon = arg("rcon");
		std::string pass = arg("pass");
		if(!game.empty()) g_gameExe = game;
		if(!nick.empty() && ValidUsername(nick)) g_nick = nick;
		if(args.find("rcon=") != std::string::npos) g_rconPw = rcon; // allow clearing
		if(args.find("pass=") != std::string::npos) g_pass = pass; // allow clearing
		if(!master.empty()){
			size_t c = master.find(':');
			g_masterHost = master.substr(0, c);
			if(c != std::string::npos) g_masterPort = (uint16_t)atoi(master.c_str() + c + 1);
		}
		SaveConfig();
		Reply("{\"reply\":\"config\",\"ok\":true}");
	}else if(cmd == "favadd" || cmd == "favdel"){
		std::string address = arg("address"), name = arg("name");
		bool add = (cmd == "favadd");
		if(address.empty()){
			Reply("{\"reply\":\"fav\",\"ok\":false,\"error\":\"select a server first\"}");
		}else std::thread([add, address, name](){
			std::vector<ServerRow> rows;
			std::string err;
			bool ok = MasterFavOp(add ? MSG_MASTER_FAV_ADD : MSG_MASTER_FAV_DEL, address, name, rows, err);
			std::string note;
			if(ok){
				SaveFavoritesCache(rows);
			}else{
				// master unreachable: keep the local cache usable and say so
				std::vector<ServerRow> local = LoadFavorites();
				std::vector<ServerRow> kept;
				bool have = false;
				for(const auto& r : local){
					if(r.address == address){
						have = true;
						if(!add) continue;   // delete locally
					}
					kept.push_back(r);
				}
				if(add && !have){
					ServerRow nr;
					nr.address = address;
					nr.hostname = name.empty() ? address : name;
					kept.push_back(nr);
				}
				SaveFavoritesCache(kept);
				rows = kept;
				note = "master unreachable (" + err + ") — kept in the local cache only";
				Log("[FAV] %s", note.c_str());
			}
			Reply("{\"reply\":\"fav\",\"ok\":" + std::string(ok ? "true" : "false") +
			      ",\"add\":" + (add ? "true" : "false") +
			      ",\"address\":\"" + Jesc(address) +
			      "\",\"error\":\"" + Jesc(note) +
			      "\",\"rows\":" + RowsToJson(rows) + "}");
		}).detach();
	}else if(cmd == "detectgame"){
		std::thread([](){
			std::string how, err;
			bool dataOk = false, cefOk = false;
			std::string exe = DetectGameExe(how, dataOk, err);
			if(!exe.empty()){
				std::string d = exe;
				size_t s = d.find_last_of('\\');
				if(s != std::string::npos){
					d.resize(s);
					bool data2 = false;
					GameDirOk(d, data2, cefOk);
				}
			}
			Log("[DETECT] game=%s how=%s data=%d cef=%d (%s)",
			    exe.empty() ? "-" : exe.c_str(), how.empty() ? "-" : how.c_str(),
			    (int)dataOk, (int)cefOk, err.c_str());
			Reply("{\"reply\":\"game\",\"ok\":" + std::string(exe.empty() ? "false" : "true") +
			      ",\"path\":\"" + Jesc(exe) + "\",\"how\":\"" + Jesc(how) +
			      "\",\"data\":" + (dataOk ? "true" : "false") +
			      "\",\"cef\":" + (cefOk ? "true" : "false") +
			      ",\"error\":\"" + Jesc(err) + "\"}");
		}).detach();
	}else if(cmd == "diag"){
		// the join path writes g_lastJoinError on the UI thread: copy the
		// settings we need before the worker starts
		const std::string lastJoin = g_lastJoinError;
		const std::string mHost = g_masterHost;
		const uint16_t mPort = g_masterPort;
		std::thread([lastJoin, mHost, mPort](){
			std::string mErr, how, gErr;
			int mMs = -1;
			bool mOk = ProbeMaster(mHost, mPort, mErr, mMs);
			std::string exe, mAddr = mHost + ":" + std::to_string((int)mPort);
			bool dataOk = false, cefOk = false;
			exe = DetectGameExe(how, dataOk, gErr);
			if(!exe.empty()){
				std::string d = exe;
				size_t s = d.find_last_of('\\');
				if(s != std::string::npos){
					d.resize(s);
					bool data2 = false;
					GameDirOk(d, data2, cefOk);
				}
			}
			Log("[DIAG] master=%s ok=%d ms=%d err=%s | game=%s how=%s data=%d cef=%d err=%s | lastjoin=%s",
			    mAddr.c_str(), (int)mOk, mMs, mErr.c_str(),
			    exe.empty() ? "-" : exe.c_str(), how.empty() ? "-" : how.c_str(),
			    (int)dataOk, (int)cefOk, gErr.c_str(),
			    lastJoin.empty() ? "-" : lastJoin.c_str());
			Reply("{\"reply\":\"diag\",\"master\":\"" + Jesc(mAddr) +
			      "\",\"masterOk\":" + (mOk ? "true" : "false") +
			      ",\"masterMs\":" + std::to_string(mMs) +
			      ",\"masterError\":\"" + Jesc(mErr) +
			      "\",\"game\":\"" + Jesc(exe) + "\",\"gameOk\":" + (exe.empty() ? "false" : "true") +
			      ",\"gameHow\":\"" + Jesc(how) +
			      "\",\"gameData\":" + (dataOk ? "true" : "false") +
			      ",\"gameCef\":" + (cefOk ? "true" : "false") +
			      ",\"gameError\":\"" + Jesc(gErr) +
			      "\",\"lastJoinError\":\"" + Jesc(lastJoin) + "\"}");
		}).detach();
	}else if(cmd == "probe"){
		std::string address = arg("address");
		std::thread([address](){
			ServerRow row;
			row.address = address;
			std::string err;
			bool alive = !address.empty() && ProbeIntoRow(address, row, err);
			Reply("{\"reply\":\"info\",\"address\":\"" + Jesc(address) +
			      "\",\"ok\":" + (alive ? "true" : "false") +
			      ",\"error\":\"" + Jesc(err) + "\",\"row\":" + RowToJson(row) + "}");
		}).detach();
	}else if(cmd == "pingall"){
		std::thread([](){
			std::vector<ServerRow> rows;
			{
				std::lock_guard<std::mutex> lk(g_rowMutex);
				rows = g_lastRows;
			}
			// short budget per server: the point is to fill the table, not to be exact
			int probed = 0, online = 0;
			for(size_t i = 0; i < rows.size() && probed < 24; i++, probed++){
				ServerRow row = rows[i];
				std::string err;
				bool alive = ProbeIntoRow(row.address, row, err, 1.0);
				if(alive) online++;
				Reply("{\"reply\":\"ping\",\"address\":\"" + Jesc(row.address) +
				      "\",\"ok\":" + (alive ? "true" : "false") +
				      ",\"error\":\"" + Jesc(err) + "\",\"row\":" + RowToJson(row) + "}");
			}
			Reply("{\"reply\":\"pingdone\",\"probed\":" + std::to_string(probed) +
			      ",\"online\":" + std::to_string(online) + "}");
		}).detach();
	}else if(cmd == "selftest"){
		Reply("{\"reply\":\"selftest\",\"ok\":true,\"detail\":\"bridge ok\"}");
	}else if(cmd == "quit"){
		if(g_close) g_close();
		PostQuitMessage(0);
	}
}

// ---------------------------------------------------------------------------
// host window
// ---------------------------------------------------------------------------
static HWND g_hWnd = nullptr;

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch(msg){
	case WM_SIZE:
		if(g_resize) g_resize(LOWORD(lParam), HIWORD(lParam));
		break;
	case WM_DPICHANGED:{
		// moved to a monitor with another scale: take the size Windows suggests
		const RECT* r = (const RECT*)lParam;
		SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
		             SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_DESTROY:
		if(g_close) g_close();
		PostQuitMessage(0);
		break;
	}
	return DefWindowProcA(hWnd, msg, wParam, lParam);
}

static HWND MakeHostWindow(HINSTANCE hInst, int nCmdShow, int w, int h)
{
	WNDCLASSA wc;
	memset(&wc, 0, sizeof wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = "reLCSLauncherCef";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	RegisterClassA(&wc);

	RECT rc = { 0, 0, w, h };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	g_hWnd = CreateWindowA("reLCSLauncherCef",
	                       "Liberty City Stories Online — Launcher",
	                       WS_OVERLAPPEDWINDOW,
	                       CW_USEDEFAULT, CW_USEDEFAULT,
	                       rc.right - rc.left, rc.bottom - rc.top,
	                       nullptr, nullptr, hInst, nullptr);
	// the A call above cannot carry the em dash (it showed as "â€”")
	SetWindowTextW(g_hWnd, L"Liberty City Stories Online \x2014 Launcher");
	// open filling the screen (the selftest keeps it hidden); the CEF view
	// follows the size through WM_SIZE -> cefui_resize
	ShowWindow(g_hWnd, nCmdShow == SW_HIDE ? SW_HIDE : SW_SHOWMAXIMIZED);
	return g_hWnd;
}

static std::string HtmlUrl()
{
	char dir[320] = {};
	GetModuleFileNameA(nullptr, dir, sizeof dir);
	char* slash = strrchr(dir, '\\');
	if(slash) *slash = 0;
	char url[512];
	snprintf(url, sizeof url, "file:///%s/launcher.html", dir);
	for(char* c = url; *c; c++) if(*c == '\\') *c = '/';
	return url;
}

// ---------------------------------------------------------------------------
// selftest (E2E) — backend queries AND the CEF UI round trip
// ---------------------------------------------------------------------------
static int SelfTest(bool cefLoaded)
{
	auto master = QueryMasterDefault();
	if(master.empty()){
		// same fallback as the Internet tab: a LAN-discovered master
		std::vector<std::string> hints;
		QueryLan(&hints);
		for(const std::string& h : hints){
			size_t c = h.rfind(':');
			if(c == std::string::npos) continue;
			master = QueryMaster(h.substr(0, c), (uint16_t)atoi(h.substr(c + 1).c_str()));
			if(!master.empty()) break;
		}
	}
	Log("[SELFTEST] master -> %zu servers", master.size());
	for(auto& r : master)
		Log("[SELFTEST] master: \"%s\" %s %d/%d mode=%s",
		    r.hostname.c_str(), r.address.c_str(), r.players, r.maxPlayers, r.gameMode.c_str());

	auto lan = QueryLan();
	Log("[SELFTEST] lan -> %zu servers", lan.size());
	for(auto& r : lan)
		Log("[SELFTEST] lan: \"%s\" %s %d/%d mode=%s",
		    r.hostname.c_str(), r.address.c_str(), r.players, r.maxPlayers, r.gameMode.c_str());

	// username system: validation + persistence round trip
	bool userOk = ValidUsername("Test_User_1") && !ValidUsername("x") && !ValidUsername("has space");
	std::string probe = "E2E_User";
	bool saved = RememberUser(probe);
	LoadProfiles();
	bool found = false;
	for(auto& u : g_profiles) if(u == probe) found = true;
	userOk = userOk && saved && found;
	Log("[SELFTEST] username: valid=%d saved=%d reloaded=%d profiles=%zu",
	    (int)(ValidUsername("Test_User_1") && !ValidUsername("has space")),
	    (int)saved, (int)found, g_profiles.size());

	// game install auto-detection
	std::string how, gErr;
	bool dataOk = false, cefOk = false;
	std::string gamePath = DetectGameExe(how, dataOk, gErr);
	if(!gamePath.empty()){
		std::string d = gamePath;
		size_t s = d.find_last_of('\\');
		if(s != std::string::npos){
			d.resize(s);
			bool data2 = false;
			GameDirOk(d, data2, cefOk);
		}
	}
	Log("[SELFTEST] game: path=\"%s\" how=%s data=%d cef=%d (%s)",
	    gamePath.c_str(), how.empty() ? "-" : how.c_str(), (int)dataOk, (int)cefOk, gErr.c_str());

	// favorites: server-side persistence round trip on the master
	bool favOk = true;
	{
		std::string favErr;
		std::vector<ServerRow> rows;
		const char* probeAddr = "127.0.0.1:65000";
		bool added = MasterFavOp(MSG_MASTER_FAV_ADD, probeAddr, "E2E Favourite", rows, favErr);
		bool found = false;
		for(auto& r : rows) if(r.address == probeAddr) found = true;
		bool delOk = MasterFavOp(MSG_MASTER_FAV_DEL, probeAddr, "", rows, favErr);
		bool gone = true;
		for(auto& r : rows) if(r.address == probeAddr) gone = false;
		if(master.empty()){
			Log("[SELFTEST] favorites: SKIPPED (no master reachable)");
		}else{
			Log("[SELFTEST] favorites: add=%d listed=%d found=%d removed=%d gone=%d total=%zu",
			    (int)added, (int)rows.size(), (int)found, (int)delOk, (int)gone, rows.size());
			favOk = added && found && delOk && gone;
		}
	}

	// live probe: real map/mode/players/ping from a server we can see.
	// probe a protocol-compatible server — probing an outdated one is expected to
	// fail with "protocol version mismatch" (and the browser flags it as stale)
	ServerInfo liveInfo;
	std::string probeTarget;
	bool haveCompatible = false;
	for(const auto& r : lan)
		if(r.protocolVersion == PROTOCOL_VERSION){ probeTarget = r.address; haveCompatible = true; break; }
	if(!haveCompatible)
		for(const auto& r : master)
			if(r.protocolVersion == PROTOCOL_VERSION){ probeTarget = r.address; haveCompatible = true; break; }
	if(probeTarget.empty() && !lan.empty()) probeTarget = lan[0].address;   // report why it fails
	if(!probeTarget.empty()){
		liveInfo = ProbeServer(probeTarget);
		Log("[SELFTEST] probe %s -> ok=%d hostname=\"%s\" mode=%s map=%s players=%d/%d ping=%dms (%s)",
		    probeTarget.c_str(), (int)liveInfo.ok, liveInfo.hostname.c_str(), liveInfo.gameMode.c_str(),
		    liveInfo.mapName.c_str(), liveInfo.players, liveInfo.maxPlayers, liveInfo.pingMs,
		    liveInfo.error.c_str());
	}else{
		Log("[SELFTEST] probe: SKIPPED (no server in the list)");
	}
	// a probe that correctly diagnoses an outdated server still proves the path
	bool probeOk = probeTarget.empty() || liveInfo.ok || (!haveCompatible && liveInfo.versionMismatch);
	if(!liveInfo.ok && liveInfo.versionMismatch && !haveCompatible)
		Log("[SELFTEST] probe: only stale servers reachable — mismatch reported as expected");

	// chromium UI: page loaded (JS->host) + title echo (host->JS->host)
	bool uiLoaded = g_loaded.load();
	bool uiEcho = g_gotTitle.load();
	Log("[SELFTEST] cef: dll=%d page_loaded=%d host_js_host_roundtrip=%d",
	    (int)cefLoaded, (int)uiLoaded, (int)uiEcho);

	bool pass = !master.empty() && !lan.empty() && userOk && cefLoaded && uiLoaded && uiEcho &&
	            favOk && probeOk;
	Log("[SELFTEST] RESULT: %s", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
	// Per-monitor DPI awareness, before any window exists. CEF renders DPI
	// aware; a DPI-unaware host got a scaled-down client size while the page
	// laid out at the real scale, so on a 125-150% display the right side and
	// the bottom of the page (most of the server list) was cut off.
	{
		typedef BOOL (WINAPI* SetDpiCtxFn)(HANDLE);
		SetDpiCtxFn setCtx = (SetDpiCtxFn)GetProcAddress(GetModuleHandleA("user32.dll"),
		                                                "SetProcessDpiAwarenessContext");
		if(setCtx) setCtx((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
	}
	g_log = fopen("launcher.log", "w");

	// Epic Online Services bootstrap: start before any CLI path so -eostest can
	// observe it; the UI polls eosboot::Json() for the step list.
	std::string eosClientId, eosClientSecret;
	ReadEosCredentials(eosClientId, eosClientSecret);
	eosboot::Start(rnet::EOS_PRODUCT_ID, rnet::EOS_SANDBOX_ID, rnet::EOS_DEPLOYMENT_ID,
	               eosClientId, eosClientSecret);
	Log("[EOS] bootstrap started (product=%s, client credentials %s)", rnet::EOS_PRODUCT_ID,
	    eosClientId.empty() ? "MISSING (see eos.ini.example)" : "loaded");

	LoadConfig();
	LoadProfiles();

	std::string selftest, joinAddr, gameOverride, rawQuery;
	for(int i = 1; i < __argc; i++){
		std::string a = __argv[i];
		if(a == "-selftest") selftest = "1";
		else if(a == "-join" && i + 1 < __argc) joinAddr = __argv[++i];
		else if(a == "-query" && i + 1 < __argc) rawQuery = __argv[++i];
		else if(a == "-game" && i + 1 < __argc) gameOverride = __argv[++i];
		else if(a == "-nick" && i + 1 < __argc) g_nick = __argv[++i];
		else if(a == "-master" && i + 1 < __argc){
			std::string v = __argv[++i];
			size_t c = v.find(':');
			g_masterHost = v.substr(0, c);
			if(c != std::string::npos) g_masterPort = (uint16_t)atoi(v.c_str() + c + 1);
		}
	}
	if(!gameOverride.empty()) g_gameExe = gameOverride;

	// headless EOS bootstrap check (CI/runbook)
	for(int i = 1; i < __argc; i++){
		if(std::string(__argv[i]) != "-eostest") continue;
		for(int t = 0; t < 300; t++){
			eosboot::Status st = eosboot::Get();
			if(st.loggedIn || !st.error.empty()) break;
			Sleep(100);
		}
		eosboot::Status st = eosboot::Get();
		Log("[EOS] sdk=%d platform=%d login=%d version=%s puid=%s steps=%s pending=%s error=%s",
		    (int)st.sdkLoaded, (int)st.platformReady, (int)st.loggedIn, st.version.c_str(),
		    st.productUserId.c_str(), st.steps.c_str(), st.pendingStep.c_str(), st.error.c_str());
		if(g_log) fclose(g_log);
		return st.loggedIn ? 0 : 1;
	}

	// headless anti-cheat maintenance (E2E + release prep)
	for(int i = 1; i < __argc; i++){
		std::string a = __argv[i];
		if(a == "-eacinstall" || a == "-eacintegrity"){
			std::string how, gErr;
			bool dataOk = false;
			std::string game = DetectGameExe(how, dataOk, gErr);
			std::string dir = game, name = "reLCS.exe";
			size_t s = dir.find_last_of('\\');
			if(s != std::string::npos){ name = dir.substr(s + 1); dir.resize(s); }
			if(dir.empty()){
				Log("[EAC] %s", gErr.c_str());
				if(g_log) fclose(g_log);
				return 2;
			}
			eac::Status ac;
			bool prepped = eac::Prepare(dir, name, ac);
			Log("[EAC] prepare %s: %s", prepped ? "ok" : "failed", ac.log.c_str());
			bool ok = prepped;
			if(ok && a == "-eacinstall") ok = eac::InstallService(dir, ac);
			if(ok && a == "-eacintegrity") ok = eac::BuildIntegrity(dir, name, ac);
			Log("[EAC] %s %s: %s", a.c_str(), ok ? "ok" : "failed",
			    ac.lastError.empty() ? ac.log.c_str() : ac.lastError.c_str());
			if(g_log) fclose(g_log);
			return ok ? 0 : 1;
		}
	}

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	enet_initialize();

	// test hook: feed one UI query through the real query handler (e.g.
	// -query "join?address=127.0.0.1%3A7777&nick=X" exercises the GUI join path)
	if(!rawQuery.empty()){
		HandleQuery(rawQuery.c_str(), nullptr);
		// the handlers that talk to the master run on worker threads: give them
		// time to finish before this headless invocation exits
		Sleep(4000);
		if(g_log) fclose(g_log);
		return 0;
	}

	if(!joinAddr.empty()){
		bool ok = JoinServer(joinAddr, g_nick);
		if(g_log) fclose(g_log);
		return ok ? 0 : 1;
	}

	bool cefLoaded = LoadCefUi();
	if(cefLoaded && g_execute_process){
		// standard CEF entry: run the subprocess path first (returns -1 for the
		// browser process, >= 0 when THIS process is a CEF subprocess)
		int sub = g_execute_process(GetModuleHandle(nullptr));
		if(sub >= 0){
			if(g_log) fclose(g_log);
			return sub;
		}
	}
	if(cefLoaded){
		g_set_handler(HandleQuery, nullptr);
		bool hidden = !selftest.empty();
		HWND wnd = MakeHostWindow(hInst, hidden ? SW_HIDE : nCmdShow, 1160, 760);
		RECT rc;
		GetClientRect(wnd, &rc);
		std::string url = HtmlUrl();
		Log("[CEF] boot: %s", url.c_str());
		if(!g_init_windowed(wnd, 0, 0, rc.right - rc.left, rc.bottom - rc.top, url.c_str())){
			Log("[CEF] init_windowed failed");
			cefLoaded = false;
		}
	}

	if(!selftest.empty()){
		// pump until the page's loaded + title-echo round trip (or timeout)
		double t0 = GetTickCount64() / 1000.0;
		MSG msg;
		while(GetTickCount64() / 1000.0 - t0 < 30.0 &&
		      !(g_loaded.load() && g_gotTitle.load())){
			while(PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)){
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			if(cefLoaded && g_exec && g_loaded.load() && !g_gotTitle.load()){
				// host -> JS -> host probe (fires once)
				static bool sent = false;
				if(!sent){ sent = true; g_exec("document.title='CEF-OK'"); }
			}
			if(cefLoaded && g_tick) g_tick();
			Sleep(20);
		}
		int rc = SelfTest(cefLoaded);
		if(cefLoaded){
			// close the browser cleanly and let CEF wind down before shutdown
			if(g_close) g_close();
			double t1 = GetTickCount64() / 1000.0;
			while(GetTickCount64() / 1000.0 - t1 < 3.0){
				MSG msg2;
				while(PeekMessage(&msg2, nullptr, 0, 0, PM_REMOVE)){
					TranslateMessage(&msg2);
					DispatchMessage(&msg2);
				}
				if(g_tick) g_tick();
				Sleep(20);
			}
			if(g_shutdown) g_shutdown();
		}
		if(g_log) fclose(g_log);
		return rc;
	}

	if(!cefLoaded){
		Log("[CEF] no chromium — cannot show the launcher UI");
		MessageBoxA(nullptr, "cefui.dll (or the CEF runtime) is missing.\n"
		            "Copy the CEF runtime next to reLCS-launcher.exe.",
		            "LCS Online Launcher", MB_ICONERROR);
		if(g_log) fclose(g_log);
		return 1;
	}

	g_run_loop();
	if(g_shutdown) g_shutdown();
	if(g_log) fclose(g_log);
	return 0;
}
