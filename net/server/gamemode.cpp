// net/server/gamemode.cpp — built-in server gamemodes.
#include "gamemode.h"
#include "server.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <random>

using namespace rnet;

// Tunable Liberty City spawn points (Portland-area). Adjust against the game.
const std::vector<SpawnPoint>& GetSpawnPoints()
{
	static const std::vector<SpawnPoint> points = {
		// clustered within ~150 m so players see each other immediately (AOI 200 m)
		{ 1133.0f, -660.0f, 23.0f, 90.0f },
		{ 1108.0f, -622.0f, 23.5f, 180.0f },
		{ 1166.0f, -618.0f, 24.0f, 270.0f },
		{ 1148.0f, -700.0f, 22.5f, 0.0f },
		{ 1090.0f, -686.0f, 23.0f, 135.0f },
		{ 1120.0f, -566.0f, 25.0f, 45.0f },
		{ 1178.0f, -672.0f, 23.0f, 225.0f },
		{ 1062.0f, -640.0f, 24.5f, 315.0f },
	};
	return points;
}

SpawnPoint RandomSpawnPoint()
{
	static std::mt19937 rng{ std::random_device{}() };
	const auto& pts = GetSpawnPoints();
	std::uniform_int_distribution<size_t> d(0, pts.size() - 1);
	return pts[d(rng)];
}

// ---------------------------------------------------------------------------
// Free roam
// ---------------------------------------------------------------------------
void FreeRoamMode::OnPlayerSpawnRequest(Server& server, uint8_t playerId)
{
	SpawnPoint sp = RandomSpawnPoint();
	server.SpawnPlayer(playerId, sp.x, sp.y, sp.z, sp.heading, 0, 100, 0);
}

// ---------------------------------------------------------------------------
// Liberty City Survivor (official PSP mode #1): deathmatch
// ---------------------------------------------------------------------------
LibertyCitySurvivorMode::LibertyCitySurvivorMode()
{
	m_phase = Phase::Warmup;
	m_phaseTimer = 0.0f;
	m_roundStarted = false;
}

void LibertyCitySurvivorMode::OnPlayerSpawnRequest(Server& server, uint8_t playerId)
{
	SpawnPoint sp = RandomSpawnPoint();
	server.SpawnPlayer(playerId, sp.x, sp.y, sp.z, sp.heading, 0, 100, 0);

	if(m_phase == Phase::Warmup && server.ActivePlayerCount() >= 1)
		OnRoundStart(server);
}

void LibertyCitySurvivorMode::OnPlayerDeath(Server& server, uint8_t victimId, uint8_t killerId, uint8_t weaponId)
{
	(void)victimId; (void)weaponId;
	if(m_phase == Phase::Active && killerId != victimId){
		// scoring already applied by server (score++ per kill); check win
		ServerPlayer* killer = server.FindPlayer(killerId);
		if(killer && killer->score >= scoreLimit)
			OnRoundEnd(server, killerId);
	}
}

void LibertyCitySurvivorMode::OnTick(Server& server, float dt)
{
	if(m_phase == Phase::Active){
		m_phaseTimer += dt;
		if(m_phaseTimer >= roundTime){
			// highest score wins
			uint8_t best = 0;
			int32_t bestScore = -1;
			for(uint8_t id = 1; id <= rnet::MAX_PLAYERS; id++){
				ServerPlayer* p = server.FindPlayer(id);
				if(p && p->score > bestScore){ bestScore = p->score; best = id; }
			}
			OnRoundEnd(server, best);
		}
	}else if(m_phase == Phase::Ended){
		m_phaseTimer += dt;
		if(m_phaseTimer >= 5.0f){
			// new round
			m_phase = Phase::Warmup;
			server.RestartGameMode();
			OnRoundStart(server);
		}
	}
}

void LibertyCitySurvivorMode::OnRoundStart(Server& server)
{
	if(server.ActivePlayerCount() < 1) return;
	m_phase = Phase::Active;
	m_phaseTimer = 0.0f;
	char msg[128];
	snprintf(msg, sizeof msg, "Liberty City Survivor — first to %d kills (%.0f min)",
	         scoreLimit, roundTime / 60.0f);
	server.BroadcastEvent(GMEV_ROUND_START, 0, 0, msg);
}

void LibertyCitySurvivorMode::OnRoundEnd(Server& server, uint8_t winnerId)
{
	m_phase = Phase::Ended;
	m_phaseTimer = 0.0f;
	ServerPlayer* w = server.FindPlayer(winnerId);
	char msg[128];
	snprintf(msg, sizeof msg, "Round over — winner: %s", w ? w->name.c_str() : "nobody");
	server.BroadcastEvent(GMEV_ROUND_END, winnerId, 0, msg);
}

void LibertyCitySurvivorMode::Reset(Server& server)
{
	(void)server;
	m_phase = Phase::Warmup;
	m_phaseTimer = 0.0f;
	m_roundStarted = false;
}
