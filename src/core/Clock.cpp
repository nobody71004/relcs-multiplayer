#include "common.h"

#include "Timer.h"
#include "Pad.h"
#include "Clock.h"
#include "Stats.h"
#include "VarConsole.h"

_TODO("gbFastTime");
bool gbFastTime;

uint8  CClock::ms_nGameClockHours;
uint8  CClock::ms_nGameClockMinutes;
uint16 CClock::ms_nGameClockSeconds;
uint8  CClock::ms_Stored_nGameClockHours;
uint8  CClock::ms_Stored_nGameClockMinutes;
uint16 CClock::ms_Stored_nGameClockSeconds;
uint32 CClock::ms_nMillisecondsPerGameMinute;
uint32  CClock::ms_nLastClockTick;
bool   CClock::ms_bClockHasBeenStored;
float  CClock::ms_EnvMapTimeMultiplicator;

#ifndef MASTER
bool gbFreezeTime;
#endif

void
CClock::Initialise(uint32 scale)
{
	debug("Initialising CClock...\n");
	ms_nGameClockHours = 12;
	ms_nGameClockMinutes = 0;
	ms_nGameClockSeconds = 0;
	ms_nMillisecondsPerGameMinute = scale;
	ms_nLastClockTick = CTimer::GetTimeInMilliseconds();
	ms_bClockHasBeenStored = false;
	ms_EnvMapTimeMultiplicator = 1.0f;
	debug("CClock ready\n");
#ifndef MASTER
	VarConsole.Add("Time (hour of day)", &ms_nGameClockHours, 1, 0, 23, true);
	VarConsole.Add("Freeze time", &gbFreezeTime, true);
#endif
}

void
CClock::Update(void)
{
	if(CPad::GetPad(1)->GetRightShoulder1())
	{
		ms_nGameClockMinutes += 8;
		ms_nLastClockTick = CTimer::GetTimeInMilliseconds();
		
		if(ms_nGameClockMinutes >= 60)
		{
			ms_nGameClockHours++;
			ms_nGameClockMinutes = 0;
			if(ms_nGameClockHours >= 24)
				ms_nGameClockHours = 0;
		}
		
	}
#ifndef MASTER
	else if (gbFreezeTime)
		ms_nLastClockTick = CTimer::GetTimeInMilliseconds();
#endif
	else if(CTimer::GetTimeInMilliseconds() - ms_nLastClockTick > ms_nMillisecondsPerGameMinute || gbFastTime)
	{
		ms_nGameClockMinutes++;
		ms_nLastClockTick += ms_nMillisecondsPerGameMinute;
		
		if ( gbFastTime )
			ms_nLastClockTick = CTimer::GetTimeInMilliseconds();
		
		if(ms_nGameClockMinutes >= 60)
		{
			ms_nGameClockHours++;
			ms_nGameClockMinutes = 0;
			if(ms_nGameClockHours >= 24)
			{
				CStats::DaysPassed++;
				ms_nGameClockHours = 0;
			}
		}
	}
	ms_nGameClockSeconds = 60 * (CTimer::GetTimeInMilliseconds() - ms_nLastClockTick) / ms_nMillisecondsPerGameMinute;
}

void
CClock::SetGameClock(uint8 h, int32 m)
{
	// robust against junk/partial values from config or net data: use signed
	// math and wrap properly (a bad time must never corrupt the clock state)
	int32 hh = (int32)h, mm = m;
	if(mm < 0){ int32 k = (-mm + 59) / 60; mm += 60*k; hh -= k; }
	while (mm >= 60) { mm -= 60; hh++; }
	ms_nGameClockMinutes = (uint8)mm;
	hh %= 24;
	if(hh < 0) hh += 24;
	ms_nGameClockHours = (uint8)hh;
	ms_nGameClockSeconds = 0;
	ms_nLastClockTick = CTimer::GetTimeInMilliseconds();
}

int32
CClock::GetGameClockMinutesUntil(uint8 h, uint8 m)
{
	int32 now, then;
	now = ms_nGameClockHours*60 + ms_nGameClockMinutes;
	then = h*60 + m;
	if(then < now)
		then += 24*60;
	return then-now;
}

bool
CClock::GetIsTimeInRange(uint8 h1, uint8 h2)
{
	if(h1 > h2)
		return ms_nGameClockHours >= h1 || ms_nGameClockHours < h2;
	else
		return ms_nGameClockHours >= h1 && ms_nGameClockHours < h2;
}

void
CClock::StoreClock(void)
{
	ms_Stored_nGameClockHours = ms_nGameClockHours;
	ms_Stored_nGameClockMinutes = ms_nGameClockMinutes;
	ms_Stored_nGameClockSeconds = ms_nGameClockSeconds;
	ms_bClockHasBeenStored = true;
}

void
CClock::RestoreClock(void)
{
	ms_nGameClockHours = ms_Stored_nGameClockHours;
	ms_nGameClockMinutes = ms_Stored_nGameClockMinutes;
	ms_nGameClockSeconds = ms_Stored_nGameClockSeconds;
}

void
CClock::CalcEnvMapTimeMultiplicator(void)
{
	float nightness = Abs(ms_nGameClockHours/24.0f - 0.5f);
	ms_EnvMapTimeMultiplicator = SQR(1.0f - nightness);;
}
