// net/proxy/main.cpp — net-shaper: UDP relay with latency/jitter/loss injection.
//
// Usage:
//   net-shaper -listen 9000 -target 127.0.0.1:7777 [-latency 100] [-jitter 30] [-loss 2]
//
// Point game clients at the -listen port; every datagram is delayed by
// latency ± jitter milliseconds and dropped with probability loss%.
// Each client gets its own target-side socket so multiple clients keep
// distinct identities toward the server (and replies route back correctly).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <queue>
#include <random>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

static double NowMs()
{
	using namespace std::chrono;
	return duration_cast<duration<double, std::milli>>(steady_clock::now().time_since_epoch()).count();
}

struct Pending {
	double dueMs = 0;
	SOCKET sock = INVALID_SOCKET;
	sockaddr_in dest;
	int destLen = 0;
	std::vector<uint8_t> data;
	bool operator>(const Pending& o) const { return dueMs > o.dueMs; }
};

struct Client {
	bool used = false;
	SOCKET targetSock = INVALID_SOCKET;
	sockaddr_in addr;
	int addrLen = 0;
	double lastSeen = 0;
};

static const int MAX_CLIENTS = 16;
static Client g_clients[MAX_CLIENTS];

int main(int argc, char** argv)
{
	uint16_t listenPort = 9000;
	std::string targetHost = "127.0.0.1";
	uint16_t targetPort = 7777;
	double latency = 100.0, jitter = 30.0, loss = 2.0;

	for(int i = 1; i < argc - 1; i++){
		std::string a = argv[i];
		if(a == "-listen") listenPort = (uint16_t)atoi(argv[++i]);
		else if(a == "-target"){
			std::string s = argv[++i];
			size_t c = s.find(':');
			targetHost = s.substr(0, c);
			if(c != std::string::npos) targetPort = (uint16_t)atoi(s.c_str() + c + 1);
		}
		else if(a == "-latency") latency = atof(argv[++i]);
		else if(a == "-jitter") jitter = atof(argv[++i]);
		else if(a == "-loss") loss = atof(argv[++i]);
	}

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	SOCKET listenSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(listenSock == INVALID_SOCKET){ printf("[ERR] socket\n"); return 1; }

	sockaddr_in listenAddr;
	memset(&listenAddr, 0, sizeof listenAddr);
	listenAddr.sin_family = AF_INET;
	listenAddr.sin_addr.s_addr = ADDR_ANY;
	listenAddr.sin_port = htons(listenPort);
	if(bind(listenSock, (sockaddr*)&listenAddr, sizeof listenAddr) != 0){
		printf("[ERR] bind %u\n", (unsigned)listenPort);
		return 1;
	}

	sockaddr_in targetAddr;
	memset(&targetAddr, 0, sizeof targetAddr);
	targetAddr.sin_family = AF_INET;
	targetAddr.sin_port = htons(targetPort);
	inet_pton(AF_INET, targetHost.c_str(), &targetAddr.sin_addr);

	u_long nb = 1;
	ioctlsocket(listenSock, FIONBIO, &nb);

	std::mt19937 rng{ 4242 };
	std::uniform_real_distribution<double> jitDist(-1.0, 1.0);
	std::uniform_real_distribution<double> lossDist(0.0, 100.0);

	std::priority_queue<Pending, std::vector<Pending>, std::greater<Pending>> queue;
	double lastStats = NowMs();
	long long fwd = 0, dropped = 0;

	printf("[shaper] :%u -> %s:%u  latency=%.0fms jitter=+/-%.0fms loss=%.1f%%\n",
	       (unsigned)listenPort, targetHost.c_str(), (unsigned)targetPort, latency, jitter, loss);
	fflush(stdout);

	auto schedule = [&](SOCKET sock, const sockaddr_in& dest, int destLen,
	                    const uint8_t* data, int len){
		if(lossDist(rng) < loss){ dropped++; return; }
		Pending p;
		p.dueMs = NowMs() + latency + (jitter > 0 ? jitDist(rng) * jitter : 0.0);
		p.sock = sock;
		p.dest = dest;
		p.destLen = destLen;
		p.data.assign(data, data + len);
		queue.push(std::move(p));
	};

	while(true){
		// ingest from clients
		for(;;){
			char buf[65536];
			sockaddr_in from;
			int fromLen = sizeof from;
			int n = recvfrom(listenSock, buf, sizeof buf, 0, (sockaddr*)&from, &fromLen);
			if(n <= 0) break;

			// find or create client
			Client* cl = nullptr;
			for(auto& c : g_clients){
				if(c.used && c.addr.sin_addr.s_addr == from.sin_addr.s_addr &&
				   c.addr.sin_port == from.sin_port){ cl = &c; break; }
			}
			if(!cl){
				for(auto& c : g_clients){
					if(!c.used){
						c.used = true;
						c.targetSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
						ioctlsocket(c.targetSock, FIONBIO, &nb);
						c.addr = from;
						c.addrLen = fromLen;
						cl = &c;
						printf("[shaper] client %s:%u -> dedicated socket\n",
						       inet_ntoa(from.sin_addr), (unsigned)ntohs(from.sin_port));
						break;
					}
				}
			}
			if(!cl){ dropped++; continue; }
			cl->lastSeen = NowMs();
			schedule(cl->targetSock, targetAddr, sizeof targetAddr, (uint8_t*)buf, n);
		}

		// ingest from target (per-client sockets)
		for(auto& c : g_clients){
			if(!c.used) continue;
			for(;;){
				char buf[65536];
				sockaddr_in from;
				int fromLen = sizeof from;
				int n = recvfrom(c.targetSock, buf, sizeof buf, 0, (sockaddr*)&from, &fromLen);
				if(n <= 0) break;
				schedule(listenSock, c.addr, c.addrLen, (uint8_t*)buf, n);
			}
		}

		// deliver due
		double now = NowMs();
		while(!queue.empty() && queue.top().dueMs <= now){
			Pending p = queue.top();
			queue.pop();
			sendto(p.sock, (const char*)p.data.data(), (int)p.data.size(), 0,
			       (sockaddr*)&p.dest, p.destLen);
			fwd++;
		}

		if(now - lastStats > 5000.0){
			printf("[shaper] forwarded=%lld dropped=%lld queued=%zu\n",
			       fwd, dropped, queue.size());
			fflush(stdout);
			lastStats = now;
		}

		Sleep(1);
	}
	return 0;
}
