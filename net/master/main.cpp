// net/master/main.cpp — reLCS master server (server list for the launcher).
//
// Protocol (ENet, UDP port 7800 by default):
//   game servers send MSG_MASTER_ANNOUNCE every 30 s
//   launchers send MSG_MASTER_LIST_REQ, get MSG_MASTER_LIST_RESP + entries
// Entries expire after 90 s of silence.
#include "rnet_protocol.h"
#include <enet/enet.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <map>
#include <chrono>

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
	double lastSeen = 0.0;
};

static std::map<uint64_t, Entry> g_entries;
static bool g_running = true;

static uint64_t Key(uint32_t ip, uint16_t port) { return ((uint64_t)ip << 16) | port; }

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
				printf("%s:%u  \"%s\" mode=%s players=%d/%d age=%.0fs\n",
				       ipstr, (unsigned)e.gamePort, e.hostname, e.gameMode,
				       (int)e.players, (int)e.maxPlayers, NowSec() - e.lastSeen);
			}
			printf("%zu servers listed\n", g_entries.size());
			fflush(stdout);
		}else if(line == "count"){
			printf("%zu\n", g_entries.size());
			fflush(stdout);
		}else if(line == "quit" || line == "exit"){
			g_running = false;
		}
	}
}

int main(int argc, char** argv)
{
	uint16_t port = DEFAULT_MASTER_PORT;
	for(int i = 1; i < argc - 1; i++)
		if(strcmp(argv[i], "-port") == 0) port = (uint16_t)atoi(argv[i + 1]);

	if(enet_initialize() != 0){
		printf("[ERR] enet_initialize failed\n");
		return 1;
	}

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
			case ENET_EVENT_TYPE_RECEIVE:{
				BitReader r(ev.packet->data, ev.packet->dataLength);
				MsgType type = (MsgType)r.ReadU8();
				if(type == MSG_MASTER_ANNOUNCE){
					MsgMasterAnnounce a;
					if(ReadMasterAnnounce(r, a)){
						uint32_t ip = ev.peer->address.host;   // observed IP (NAT-safe)
						uint64_t key = Key(ip, a.gamePort);
						Entry& e = g_entries[key];
						e.ip = ip;
						e.gamePort = a.gamePort;
						e.players = a.players;
						e.maxPlayers = a.maxPlayers;
						e.protocolVersion = a.protocolVersion;
						CopyStr(e.hostname, sizeof e.hostname, a.hostname);
						CopyStr(e.gameMode, sizeof e.gameMode, a.gameMode);
						e.lastSeen = NowSec();
						printf("[master] announce %s (%d/%d) mode=%s\n",
						       e.hostname, (int)e.players, (int)e.maxPlayers, e.gameMode);
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
						WriteMasterEntry(w, le);
					}
					ENetPacket* pkt = enet_packet_create(w.Data(), w.ByteSize(),
					                                     ENET_PACKET_FLAG_RELIABLE);
					enet_peer_send(ev.peer, 0, pkt);
					enet_host_flush(host);
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

	enet_host_destroy(host);
	enet_deinitialize();
	printf("[master] bye\n");
	return 0;
}
