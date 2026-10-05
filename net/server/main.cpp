// net/server/main.cpp — reLCS dedicated server entry point + console.
#include "server.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <fstream>
#include <sstream>

static Server* g_server = nullptr;

static BOOL WINAPI ConsoleHandler(DWORD type)
{
	if(type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT){
		if(g_server) g_server->Shutdown();
		return TRUE;
	}
	return FALSE;
}

static void ConsoleThread()
{
	std::string line;
	while(std::getline(std::cin, line)){
		if(g_server) g_server->PushConsoleLine(line);
		else break;
	}
}

static void Trim(std::string& s)
{
	while(!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
	size_t start = s.find_first_not_of(" \t");
	if(start != std::string::npos) s = s.substr(start);
}

static void ApplyKeyVal(ServerConfig& cfg, const std::string& key, const std::string& val)
{
	if(key == "hostname") cfg.hostname = val;
	else if(key == "port") cfg.port = (uint16_t)atoi(val.c_str());
	else if(key == "maxplayers") cfg.maxPlayers = atoi(val.c_str());
	else if(key == "gamemode") cfg.gamemode = val;
	else if(key == "announce") cfg.announce = (atoi(val.c_str()) != 0);
	else if(key == "master") cfg.master = val;
	else if(key == "master_port") cfg.masterPort = (uint16_t)atoi(val.c_str());
	else if(key == "lan_discover") cfg.lanDiscover = (atoi(val.c_str()) != 0);
	else if(key == "rcon_password") cfg.rconPassword = val;
	else if(key == "stream_distance") cfg.streamDistance = (float)atof(val.c_str());
	else if(key == "tick_rate") cfg.tickRate = atoi(val.c_str());
	else if(key == "hour") cfg.hour = (uint8_t)atoi(val.c_str());
	else if(key == "minute") cfg.minute = (uint8_t)atoi(val.c_str());
	else if(key == "weather") cfg.weather = (uint8_t)atoi(val.c_str());
	else if(key == "netstats_interval") cfg.netstatsInterval = atoi(val.c_str());
}

static bool LoadConfigFile(ServerConfig& cfg, const std::string& path)
{
	std::ifstream f(path);
	if(!f) return false;
	std::string line;
	while(std::getline(f, line)){
		Trim(line);
		if(line.empty() || line[0] == '#') continue;
		size_t eq = line.find('=');
		if(eq == std::string::npos) continue;
		std::string key = line.substr(0, eq), val = line.substr(eq + 1);
		Trim(key); Trim(val);
		ApplyKeyVal(cfg, key, val);
	}
	return true;
}

int main(int argc, char** argv)
{
	ServerConfig cfg;
	std::string configPath = "server.cfg";

	// first pass: find -config
	for(int i = 1; i < argc - 1; i++)
		if(strcmp(argv[i], "-config") == 0) configPath = argv[i + 1];

	LoadConfigFile(cfg, configPath);

	// second pass: command line overrides
	for(int i = 1; i < argc - 1; i++){
		if(strcmp(argv[i], "-config") == 0) continue;
		if(argv[i][0] == '-'){
			std::string key = argv[i] + 1;
			ApplyKeyVal(cfg, key, argv[i + 1]);
			i++;
		}
	}

	SetConsoleCtrlHandler(ConsoleHandler, TRUE);

	Server server;
	g_server = &server;
	if(!server.Init(cfg))
		return 1;

	std::thread console(ConsoleThread);
	console.detach();

	server.Run();

	g_server = nullptr;
	enet_deinitialize();
	std::printf("bye\n");
	return 0;
}
