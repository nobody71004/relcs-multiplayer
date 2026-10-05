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
#include <enet/enet.h>

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

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
	int players = 0, maxPlayers = 0;
	int pingMs = 0;
};

static std::string g_gameExe = "reLCS.exe";
static std::string g_nick = "Player";
static std::string g_rconPw; // server rcon password for in-game admin tools
static std::string g_masterHost = "127.0.0.1";
static uint16_t g_masterPort = DEFAULT_MASTER_PORT;
static std::vector<std::string> g_profiles;   // saved usernames (usernames.ini)

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
static std::vector<ServerRow> QueryMaster()
{
	std::vector<ServerRow> rows;
	ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
	if(!client) return rows;

	ENetAddress addr;
	if(enet_address_set_host(&addr, g_masterHost.c_str()) < 0){
		enet_host_destroy(client);
		return rows;
	}
	addr.port = g_masterPort;
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

static std::vector<ServerRow> QueryLan()
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

	double t0 = GetTickCount64() / 1000.0;
	while(GetTickCount64() / 1000.0 - t0 < 2.0){
		char buf[512];
		sockaddr_in from; int flen = sizeof from;
		int n = recvfrom(s, buf, sizeof buf - 1, 0, (sockaddr*)&from, &flen);
		if(n > 0){
			buf[n] = '\0';
			// RELCS_RESP_V1|hostname|players|max|gamemode|version|gameport
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
				rows.push_back(row);
			}
		}
		Sleep(10);
	}
	closesocket(s);
	return rows;
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
		else if(key == "rcon") g_rconPw = val;
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
}

// ---------------------------------------------------------------------------
// join flow
// ---------------------------------------------------------------------------
static std::string g_lastJoinError;

// find reLCS.exe: explicit config first, then well-known locations
static std::string ResolveGameExe()
{
	std::vector<std::string> cands;
	if(!g_gameExe.empty()) cands.push_back(g_gameExe);
	char dir[320] = {};
	GetModuleFileNameA(nullptr, dir, sizeof dir);
	char* slash = strrchr(dir, '\\');
	if(slash) *slash = 0;
	std::string base = dir;
	cands.push_back(base + "\\reLCS.exe");                            // next to launcher
	cands.push_back(base + "\\..\\..\\..\\..\\reLCS\\reLCS.exe"); // dev layout
	cands.push_back("reLCS.exe");                                     // PATH
	for(const auto& c : cands){
		DWORD a = GetFileAttributesA(c.c_str());
		if(a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)){
			char full[640] = {};
			GetFullPathNameA(c.c_str(), sizeof full, full, nullptr);
			return full;
		}
	}
	return "";
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

	std::string exe = ResolveGameExe();
	if(exe.empty()){
		g_lastJoinError = "game executable not found — set it in Settings";
		Log("[JOIN] %s (looked for: %s, launcher dir, ../../../../reLCS, PATH)",
		    g_lastJoinError.c_str(), g_gameExe.c_str());
		return false;
	}

	// the game must run from its own directory (CEF runtime, gameui.html,
	// AUDIO/ and the game files all resolve against the CWD)
	std::string exeDir = exe;
	size_t slash = exeDir.find_last_of('\\');
	if(slash != std::string::npos) exeDir.resize(slash);

	std::string cmd = "\"" + exe + "\" -connect " + address + " -nick \"" + g_nick + "\"";
	// admin tools (broadcast/give/...) need the server's rcon password in-game
	if(!g_rconPw.empty())
		cmd += " -rcon \"" + g_rconPw + "\"";
	Log("[JOIN] %s  (cwd=%s)", cmd.c_str(), exeDir.c_str());

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	memset(&si, 0, sizeof si);
	memset(&pi, 0, sizeof pi);
	si.cb = sizeof si;
	BOOL ok = CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE,
	                         0, nullptr, exeDir.c_str(), &si, &pi);
	if(!ok){
		char err[128];
		snprintf(err, sizeof err, "CreateProcess failed (error %lu)", GetLastError());
		g_lastJoinError = err;
		Log("[JOIN] %s for %s", err, exe.c_str());
		return false;
	}
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return true;
}

// ---------------------------------------------------------------------------
// JS <-> host plumbing (lcs:// scheme)
// ---------------------------------------------------------------------------
static std::string Jesc(const std::string& s)
{
	std::string o;
	for(char c : s){
		if(c == '\'' || c == '\\' || c == '"'){ o += '\\'; o += c; }
		else if((unsigned char)c < 0x20) continue;
		else o += c;
	}
	return o;
}

static void Reply(const std::string& json)
{
	if(g_exec)
		g_exec(("window.onCefReply && onCefReply('" + json + "')").c_str());
}

static std::string RowsToJson(const std::vector<ServerRow>& rows)
{
	std::string j = "[";
	for(size_t i = 0; i < rows.size(); i++){
		char head[128];
		snprintf(head, sizeof head, "%s{\"hostname\":\"%s\",\"gameMode\":\"%s\",\"address\":\"%s\","
		         "\"players\":%d,\"maxPlayers\":%d,\"pingMs\":%d}",
		         i ? "," : "", Jesc(rows[i].hostname).c_str(), Jesc(rows[i].gameMode).c_str(),
		         Jesc(rows[i].address).c_str(), rows[i].players, rows[i].maxPlayers, rows[i].pingMs);
		j += head;
	}
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

	if(cmd == "loaded"){
		g_loaded = true;
		std::string gameShown = ResolveGameExe();
		if(gameShown.empty()) gameShown = g_gameExe;
		Reply("{\"reply\":\"init\",\"nick\":\"" + Jesc(g_nick) +
		      "\",\"game\":\"" + Jesc(gameShown) +
		      "\",\"master\":\"" + Jesc(g_masterHost + ":" + std::to_string(g_masterPort)) +
		      "\",\"rcon\":\"" + Jesc(g_rconPw) +
		      "\",\"profiles\":" + ProfilesToJson() + "}");
	}else if(cmd.rfind("title ", 0) == 0){
		if(req.find("CEF-OK") != std::string::npos) g_gotTitle = true;
	}else if(cmd == "refresh"){
		std::string which = arg("tab");
		std::thread([which](){
			std::vector<ServerRow> rows;
			const char* source = "internet";
			if(which == "lan"){ rows = QueryLan(); source = "lan"; }
			else if(which == "favorites"){ rows = LoadFavorites(); source = "favorites"; }
			else rows = QueryMaster();
			Log("[QUERY] %s -> %zu servers", source, rows.size());
			Reply("{\"reply\":\"servers\",\"source\":\"" + std::string(source) +
			      "\",\"rows\":" + RowsToJson(rows) + "}");
		}).detach();
	}else if(cmd == "join"){
		std::string address = arg("address");
		std::string nick = arg("nick");
		bool ok = !address.empty() && JoinServer(address, nick);
		if(address.empty()) g_lastJoinError = "no server address";
		Reply(std::string("{\"reply\":\"join\",\"ok\":") + (ok ? "true" : "false") +
		      ",\"address\":\"" + Jesc(address) + "\",\"error\":\"" + Jesc(g_lastJoinError) + "\"}");
	}else if(cmd == "saveuser"){
		std::string nick = arg("nick");
		bool ok = RememberUser(nick);
		if(ok){ g_nick = nick; SaveConfig(); }
		Reply(std::string("{\"reply\":\"saveuser\",\"ok\":") + (ok ? "true" : "false") +
		      ",\"nick\":\"" + Jesc(nick) + "\",\"profiles\":" + ProfilesToJson() + "}");
	}else if(cmd == "config"){
		std::string game = arg("game"), master = arg("master"), nick = arg("nick");
		std::string rcon = arg("rcon");
		if(!game.empty()) g_gameExe = game;
		if(!nick.empty() && ValidUsername(nick)) g_nick = nick;
		if(args.find("rcon=") != std::string::npos) g_rconPw = rcon; // allow clearing
		if(!master.empty()){
			size_t c = master.find(':');
			g_masterHost = master.substr(0, c);
			if(c != std::string::npos) g_masterPort = (uint16_t)atoi(master.c_str() + c + 1);
		}
		SaveConfig();
		Reply("{\"reply\":\"config\",\"ok\":true}");
	}else if(cmd == "addfav"){
		std::string address = arg("address"), name = arg("name");
		bool ok = !address.empty();
		if(ok){
			std::ofstream f("favorites.ini", std::ios::app);
			f << address << " " << (name.empty() ? address : name) << "\n";
		}
		Reply(std::string("{\"reply\":\"addfav\",\"ok\":") + (ok ? "true" : "false") + "}");
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
	ShowWindow(g_hWnd, nCmdShow);
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
	auto master = QueryMaster();
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

	// chromium UI: page loaded (JS->host) + title echo (host->JS->host)
	bool uiLoaded = g_loaded.load();
	bool uiEcho = g_gotTitle.load();
	Log("[SELFTEST] cef: dll=%d page_loaded=%d host_js_host_roundtrip=%d",
	    (int)cefLoaded, (int)uiLoaded, (int)uiEcho);

	bool pass = !master.empty() && !lan.empty() && userOk && cefLoaded && uiLoaded && uiEcho;
	Log("[SELFTEST] RESULT: %s", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
	g_log = fopen("launcher.log", "w");
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

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	enet_initialize();

	// test hook: feed one UI query through the real query handler (e.g.
	// -query "join?address=127.0.0.1%3A7777&nick=X" exercises the GUI join path)
	if(!rawQuery.empty()){
		HandleQuery(rawQuery.c_str(), nullptr);
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
