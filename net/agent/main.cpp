// net/agent/main.cpp — reLCS-agentd: HTTP control bridge for real-world LLM
// agent models + the CEF-style web HUD.
//
// The bridge holds one persistent ENet control connection to the game server
// (logged in as player "Agent", authenticated for RCON) and exposes a small
// JSON/HTTP API so any LLM agent harness, the web HUD, or curl can drive the
// game and the server:
//
//   GET  /            -> web HUD (HTML: server HUD + inventory + admin panel)
//   GET  /v1/tools    -> OpenAI-style tool schema (drop into any LLM tool API)
//   GET  /v1/status   -> live server state (status + inventory + netstats)
//   POST /v1/rcon     -> { "command": "..." }
//   POST /v1/say      -> { "text": "..." }                 broadcast
//   POST /v1/give     -> { "target": "id|name",
//                         "kind": "money|weapon|god|health", "value": N, "arg": N }
//   POST /v1/bot      -> { "name": "AgentBot",
//                         "scenario": "roam|combat|drive|chaos", "duration": N }
//
// Auth on /v1/*: header "X-Rcon: <password>" or body field "password".
// CLI: -http <port> -server <host:port> -rcon <pw> [-bot_exe <path>] [-web <path>]
//      -selftest  : connect + rcon "version", print [SELFTEST] RESULT, exit
#include "rnet_protocol.h"
#include <enet/enet.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

using namespace rnet;

// ---------------------------------------------------------------------------
// config + state
// ---------------------------------------------------------------------------
static std::string g_serverHost = "127.0.0.1";
static uint16_t g_serverPort = DEFAULT_PORT;
static uint16_t g_httpPort = 7801;
static std::string g_rconPw;
static std::string g_botExe = "relcs-netbot.exe";
static std::string g_webPath = "webhud.html";

static ENetHost* g_net = nullptr;
static ENetPeer* g_peer = nullptr;
static bool g_welcomed = false, g_rconOk = false;
static std::vector<std::string> g_resp;

static double NowSec()
{
	return GetTickCount64() / 1000.0;
}

static void SendHello();

static void PumpNet(int timeoutMs)
{
	if(!g_net) return;
	ENetEvent ev;
	while(enet_host_service(g_net, &ev, (uint32_t)timeoutMs) > 0){
		timeoutMs = 0;
		switch(ev.type){
		case ENET_EVENT_TYPE_CONNECT:
			SendHello();
			break;
		case ENET_EVENT_TYPE_RECEIVE:{
			BitReader r(ev.packet->data, ev.packet->dataLength);
			MsgType type = (MsgType)r.ReadU8();
			if(type == MSG_WELCOME){
				g_welcomed = true;
				BitWriter w;
				BeginMsg(w, MSG_RCON_AUTH);
				WriteStringMsg(w, g_rconPw.c_str(), 255);
				ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
				enet_peer_send(g_peer, 0, pkt);
			}else if(type == MSG_RCON_OK){
				g_rconOk = true;
			}else if(type == MSG_RCON_RESP){
				char text[512] = {};
				ReadStringMsg(r, text, sizeof text, 511);
				g_resp.push_back(text);
			}
			enet_packet_destroy(ev.packet);
			break;
		}
		case ENET_EVENT_TYPE_DISCONNECT:
			g_welcomed = false;
			g_rconOk = false;
			g_peer = nullptr;
			break;
		default:
			break;
		}
	}
}

static bool ConnectControl()
{
	if(g_net){ enet_host_destroy(g_net); g_net = nullptr; }
	ENetHost* client = enet_host_create(nullptr, 1, 2, 0, 0);
	if(!client) return false;
	client->maximumPacketSize = 32 * 1024;
	ENetAddress addr;
	if(enet_address_set_host(&addr, g_serverHost.c_str()) < 0){ enet_host_destroy(client); return false; }
	addr.port = g_serverPort;
	ENetPeer* peer = enet_host_connect(client, &addr, 2, 0);
	if(!peer){ enet_host_destroy(client); return false; }
	g_net = client;
	g_peer = peer;
	g_welcomed = g_rconOk = false;

	double t0 = NowSec();
	while(NowSec() - t0 < 5.0 && !g_rconOk)
		PumpNet(50);
	return g_rconOk;
}

// send HELLO as soon as the peer connects (called from the connect path below)
static bool ConnectControlHello()
{
	if(!ConnectControl()){
		// retry once: the very first packets can race the handshake
		Sleep(200);
		if(!ConnectControl()) return false;
	}
	return true;
}

static void SendHello()
{
	BitWriter w;
	BeginMsg(w, MSG_HELLO);
	MsgHello h;
	h.flags = 0;
	CopyStr(h.name, sizeof h.name, "Agent");
	WriteHello(w, h);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send(g_peer, 0, pkt);
}

// run one rcon command and collect response lines
static std::string RconExec(const std::string& cmd, int waitMs = 1200)
{
	g_resp.clear();
	if(!g_peer || !g_rconOk) return "";
	BitWriter w;
	BeginMsg(w, MSG_RCON_CMD);
	WriteStringMsg(w, cmd.c_str(), 511);
	ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send(g_peer, 0, pkt);
	enet_host_flush(g_net);

	double t0 = NowSec();
	while(NowSec() - t0 < waitMs / 1000.0){
		PumpNet(25);
		if(!g_resp.empty() && NowSec() - t0 > 0.15) break; // first line usually suffices
	}
	std::string out;
	for(size_t i = 0; i < g_resp.size(); i++){
		if(i) out += "\n";
		out += g_resp[i];
	}
	return out;
}

// ---------------------------------------------------------------------------
// tiny JSON helpers (tolerant, for tool payloads)
// ---------------------------------------------------------------------------
static std::string HdrVal(const std::string& req, const char* name)
{
	std::string pat = std::string(name) + ":";
	size_t k = req.find(pat);
	if(k == std::string::npos) return "";
	size_t e = req.find('\r', k);
	std::string v = req.substr(k + pat.size(), e == std::string::npos ? std::string::npos : e - k - pat.size());
	while(!v.empty() && v[0] == ' ') v.erase(0, 1);
	while(!v.empty() && (v.back() == ' ' || v.back() == '\n')) v.pop_back();
	return v;
}

static std::string JsonStr(const std::string& body, const char* key)
{
	std::string pat = std::string("\"") + key + "\"";
	size_t k = body.find(pat);
	if(k == std::string::npos) return "";
	size_t c = body.find(':', k + pat.size());
	if(c == std::string::npos) return "";
	size_t q1 = body.find('"', c + 1);
	if(q1 == std::string::npos) return "";
	size_t q2 = body.find('"', q1 + 1);
	if(q2 == std::string::npos) return "";
	return body.substr(q1 + 1, q2 - q1 - 1);
}

static double JsonNum(const std::string& body, const char* key, double dflt = 0.0)
{
	std::string pat = std::string("\"") + key + "\"";
	size_t k = body.find(pat);
	if(k == std::string::npos) return dflt;
	size_t c = body.find(':', k + pat.size());
	if(c == std::string::npos) return dflt;
	return atof(body.c_str() + c + 1);
}

static std::string JsonEscape(const std::string& s)
{
	std::string out;
	for(char c : s){
		if(c == '"' || c == '\\'){ out += '\\'; out += c; }
		else if(c == '\n') out += "\\n";
		else if(c == '\r') continue;
		else out += c;
	}
	return out;
}

static std::string JsonLines(const std::string& text)
{
	std::string out = "[";
	bool first = true;
	size_t pos = 0;
	while(pos <= text.size()){
		size_t nl = text.find('\n', pos);
		std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		if(!line.empty()){
			if(!first) out += ",";
			out += "\"" + JsonEscape(line) + "\"";
			first = false;
		}
		if(nl == std::string::npos) break;
		pos = nl + 1;
	}
	out += "]";
	return out;
}

// ---------------------------------------------------------------------------
// tools schema (OpenAI function-calling format)
// ---------------------------------------------------------------------------
static const char* kToolsJson =
	"{\"tools\":["
	"{\"type\":\"function\",\"function\":{\"name\":\"server_status\","
	"\"description\":\"Get live reLCS multiplayer server state: players, inventories (money, health, weapons), ping and traffic.\","
	"\"parameters\":{\"type\":\"object\",\"properties\":{},\"required\":[]}}},"
	"{\"type\":\"function\",\"function\":{\"name\":\"server_rcon\","
	"\"description\":\"Run a raw server console command (e.g. teleport, loadmode, spawnprop, setr).\"," 
	"\"parameters\":{\"type\":\"object\",\"properties\":{"
	"\"command\":{\"type\":\"string\",\"description\":\"the console command line\"}},\"required\":[\"command\"]}}},"
	"{\"type\":\"function\",\"function\":{\"name\":\"server_say\","
	"\"description\":\"Broadcast a message to every player (server announcement).\","
	"\"parameters\":{\"type\":\"object\",\"properties\":{"
	"\"text\":{\"type\":\"string\",\"description\":\"message text\"}},\"required\":[\"text\"]}}},"
	"{\"type\":\"function\",\"function\":{\"name\":\"server_give\","
	"\"description\":\"Give a player money, a weapon, godmode or health.\","
	"\"parameters\":{\"type\":\"object\",\"properties\":{"
	"\"target\":{\"type\":\"string\",\"description\":\"player id or name\"},"
	"\"kind\":{\"type\":\"string\",\"enum\":[\"money\",\"weapon\",\"god\",\"health\"]},"
	"\"value\":{\"type\":\"integer\",\"description\":\"amount (money/hp), 0/1 for god, eWeaponType for weapon\"},"
	"\"arg\":{\"type\":\"integer\",\"description\":\"weapon ammo (weapons only)\"}},"
	"\"required\":[\"target\",\"kind\",\"value\"]}}},"
	"{\"type\":\"function\",\"function\":{\"name\":\"server_spawn_bot\","
	"\"description\":\"Spawn an NPC bot player (joins the game with a nametag, acts out a scenario).\","
	"\"parameters\":{\"type\":\"object\",\"properties\":{"
	"\"name\":{\"type\":\"string\",\"description\":\"bot name\"},"
	"\"scenario\":{\"type\":\"string\",\"enum\":[\"roam\",\"combat\",\"drive\",\"chaos\"]},"
	"\"duration\":{\"type\":\"integer\",\"description\":\"seconds to stay (default 600)\"}},"
	"\"required\":[\"name\"]}}}"
	"]}";

// ---------------------------------------------------------------------------
// bot spawning
// ---------------------------------------------------------------------------
static bool SpawnBot(const std::string& name, const std::string& scenario, int duration)
{
	char args[512];
	snprintf(args, sizeof args, "\"%s\" -name %s -server %s:%u -scenario %s -duration %d",
	         g_botExe.c_str(), name.c_str(), g_serverHost.c_str(), (unsigned)g_serverPort,
	         scenario.c_str(), duration);
	STARTUPINFOA si; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
	PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
	if(!CreateProcessA(NULL, args, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
		return false;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
static void SendHttp(SOCKET s, int code, const char* ctype, const std::string& body)
{
	char hdr[256];
	snprintf(hdr, sizeof hdr,
	         "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
	         "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: X-Rcon, Content-Type\r\n"
	         "Connection: close\r\n\r\n",
	         code, code == 200 ? "OK" : code == 400 ? "Bad Request" : "Error",
	         ctype, (unsigned)body.size());
	std::string all = std::string(hdr) + body;
	send(s, all.c_str(), (int)all.size(), 0);
}

static std::string StatusJson()
{
	std::string status = RconExec("status");
	std::string inv = RconExec("inventory");
	std::string net = RconExec("netstats");
	std::string out = "{\"ok\":true,\"status\":" + JsonLines(status) +
	                  ",\"inventory\":" + JsonLines(inv) +
	                  ",\"netstats\":" + JsonLines(net) + "}";
	return out;
}

static void HandleHttp(SOCKET s)
{
	DWORD timeoutMs = 3000;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof timeoutMs);

	// read the full request: headers+body can arrive in separate segments
	std::string req;
	char chunk[4096];
	int n = recv(s, chunk, sizeof chunk, 0);
	if(n <= 0){ closesocket(s); return; }
	req.assign(chunk, n);
	for(;;){
		size_t hdrEnd = req.find("\r\n\r\n");
		if(hdrEnd != std::string::npos){
			int need = (int)(hdrEnd + 4) + atoi(HdrVal(req, "Content-Length").c_str());
			if((int)req.size() >= need) break;
		}
		if(req.size() > (1 << 20)) break; // sanity cap
		n = recv(s, chunk, sizeof chunk, 0);
		if(n <= 0) break;
		req.append(chunk, n);
	}
	size_t sp1 = req.find(' ');
	size_t sp2 = req.find(' ', sp1 + 1);
	if(sp1 == std::string::npos || sp2 == std::string::npos){ closesocket(s); return; }
	std::string method = req.substr(0, sp1);
	std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
	size_t hdrEnd = req.find("\r\n\r\n");
	std::string body = hdrEnd == std::string::npos ? "" : req.substr(hdrEnd + 4);

	std::string auth = HdrVal(req, "X-Rcon");
	if(auth.empty()) auth = JsonStr(body, "password");
	bool authed = !g_rconPw.empty() && auth == g_rconPw;

	if(method == "GET" && (path == "/" || path == "/index.html")){
		// web HUD: prefer the file beside the exe, fall back to a stub
		FILE* f = fopen(g_webPath.c_str(), "rb");
		std::string page;
		if(f){
			char chunk[4096]; size_t got;
			while((got = fread(chunk, 1, sizeof chunk, f)) > 0) page.append(chunk, got);
			fclose(f);
		}
		if(page.empty())
			page = "<html><body><h1>LCS ONLINE</h1><p>webhud.html not found beside reLCS-agentd.</p></body></html>";
		SendHttp(s, 200, "text/html", page);
	}else if(method == "GET" && path == "/v1/tools"){
		SendHttp(s, 200, "application/json", kToolsJson);
	}else if(method == "GET" && path == "/v1/status"){
		if(!authed) SendHttp(s, 401, "application/json", "{\"ok\":false,\"error\":\"bad or missing X-Rcon\"}");
		else SendHttp(s, 200, "application/json", StatusJson());
	}else if(method == "POST" && path == "/v1/rcon"){
		std::string cmd = JsonStr(body, "command");
		if(!authed) SendHttp(s, 401, "application/json", "{\"ok\":false,\"error\":\"bad or missing X-Rcon\"}");
		else if(cmd.empty()) SendHttp(s, 400, "application/json", "{\"ok\":false,\"error\":\"missing command\"}");
		else{
			std::string resp = RconExec(cmd);
			std::string out = "{\"ok\":true,\"command\":\"" + JsonEscape(cmd) + "\",\"lines\":" + JsonLines(resp) + "}";
			SendHttp(s, 200, "application/json", out);
		}
	}else if(method == "POST" && path == "/v1/say"){
		std::string text = JsonStr(body, "text");
		if(!authed) SendHttp(s, 401, "application/json", "{\"ok\":false,\"error\":\"bad or missing X-Rcon\"}");
		else if(text.empty()) SendHttp(s, 400, "application/json", "{\"ok\":false,\"error\":\"missing text\"}");
		else{
			std::string resp = RconExec("broadcast " + text);
			SendHttp(s, 200, "application/json",
			         "{\"ok\":true,\"lines\":" + JsonLines(resp) + "}");
		}
	}else if(method == "POST" && path == "/v1/give"){
		std::string target = JsonStr(body, "target");
		std::string kind = JsonStr(body, "kind");
		int value = (int)JsonNum(body, "value");
		int arg = (int)JsonNum(body, "arg", 200);
		if(!authed) SendHttp(s, 401, "application/json", "{\"ok\":false,\"error\":\"bad or missing X-Rcon\"}");
		else if(target.empty() || kind.empty()) SendHttp(s, 400, "application/json", "{\"ok\":false,\"error\":\"missing target/kind\"}");
		else{
			char cmd[160];
			snprintf(cmd, sizeof cmd, "give %s %s %d %d", target.c_str(), kind.c_str(), value, arg);
			std::string resp = RconExec(cmd);
			SendHttp(s, 200, "application/json",
			         "{\"ok\":true,\"lines\":" + JsonLines(resp) + "}");
		}
	}else if(method == "POST" && path == "/v1/bot"){
		std::string name = JsonStr(body, "name");
		std::string scenario = JsonStr(body, "scenario");
		int duration = (int)JsonNum(body, "duration", 600);
		if(scenario.empty()) scenario = "roam";
		if(name.empty()) name = "AgentBot";
		if(!authed) SendHttp(s, 401, "application/json", "{\"ok\":false,\"error\":\"bad or missing X-Rcon\"}");
		else if(SpawnBot(name, scenario, duration))
			SendHttp(s, 200, "application/json",
			         "{\"ok\":true,\"spawned\":\"" + JsonEscape(name) + "\"}");
		else
			SendHttp(s, 500, "application/json", "{\"ok\":false,\"error\":\"CreateProcess failed\"}");
	}else if(method == "OPTIONS"){
		SendHttp(s, 200, "text/plain", "");
	}else{
		SendHttp(s, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
	}
	closesocket(s);
}

// ---------------------------------------------------------------------------
// selftest (E2E): connect, auth, rcon version
// ---------------------------------------------------------------------------
static int SelfTest()
{
	if(!ConnectControlHello()){
		std::printf("[SELFTEST] connect/auth failed\n");
		std::printf("[SELFTEST] RESULT: FAIL\n");
		return 1;
	}
	std::string resp = RconExec("version");
	std::printf("[SELFTEST] version -> %s\n", resp.c_str());
	bool pass = resp.find("protocol=") != std::string::npos;
	std::printf("[SELFTEST] RESULT: %s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}

int main(int argc, char** argv)
{
	for(int i = 1; i < argc; i++){
		std::string a = argv[i];
		if(a == "-http" && i + 1 < argc) g_httpPort = (uint16_t)atoi(argv[++i]);
		else if(a == "-server" && i + 1 < argc){
			std::string v = argv[++i];
			size_t c = v.find(':');
			g_serverHost = v.substr(0, c);
			if(c != std::string::npos) g_serverPort = (uint16_t)atoi(v.c_str() + c + 1);
		}
		else if(a == "-rcon" && i + 1 < argc) g_rconPw = argv[++i];
		else if(a == "-bot_exe" && i + 1 < argc) g_botExe = argv[++i];
		else if(a == "-web" && i + 1 < argc) g_webPath = argv[++i];
	}

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	enet_initialize();

	bool selftest = false;
	for(int i = 1; i < argc; i++) if(!strcmp(argv[i], "-selftest")) selftest = true;
	if(selftest) return SelfTest();

	// control connection (retry: server may still be booting)
	for(int attempt = 0; attempt < 10 && !g_rconOk; attempt++){
		ConnectControlHello();
		if(!g_rconOk) Sleep(500);
	}
	if(!g_rconOk) std::printf("[AGENT] warning: no control connection yet (will retry per request)\n");

	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(listenSock == INVALID_SOCKET){ std::printf("[AGENT] socket failed\n"); return 1; }
	BOOL yes = 1;
	setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
	sockaddr_in addr; memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(g_httpPort);
	if(bind(listenSock, (sockaddr*)&addr, sizeof addr) != 0 ||
	   listen(listenSock, 8) != 0){
		std::printf("[AGENT] cannot bind http port %u\n", (unsigned)g_httpPort);
		return 1;
	}
	std::printf("[AGENT] http bridge on http://127.0.0.1:%u  (server %s:%u)\n",
	            (unsigned)g_httpPort, g_serverHost.c_str(), (unsigned)g_serverPort);
	std::printf("[AGENT] tools: GET /v1/tools | status: GET /v1/status | HUD: GET /\n");
	fflush(stdout);

	for(;;){
		fd_set rfds; FD_ZERO(&rfds); FD_SET(listenSock, &rfds);
		timeval tv; tv.tv_sec = 0; tv.tv_usec = 50000;
		int r = select(0, &rfds, nullptr, nullptr, &tv);
		PumpNet(0); // keep the control connection alive
		if(r > 0 && FD_ISSET(listenSock, &rfds)){
			SOCKET cs = accept(listenSock, nullptr, nullptr);
			if(cs != INVALID_SOCKET) HandleHttp(cs);
		}
		// auto-reconnect the control link if it dropped
		if(!g_rconOk) ConnectControlHello();
	}
}
