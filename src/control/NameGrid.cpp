#include "common.h"
#include "NameGrid.h"

// TODO: reverse mobile code

CPlayerName::CPlayerName()
{
	// TODO
	x = 0.0f;
	y = 0.0f;
	unk_4c = 0;
}

void
CPlayerName::DisplayName(int)
{
	// TODO
}

CRow::CRow()
{
	// TODO
	unk_0 = 0;
	unk_4 = 0;
	unk_30 = 0;
}

void
CRow::SetLetter(int, wchar *)
{
	// TODO
}

CGrid::CGrid()
{
	// TODO
	unk_int1 = 0;
	unk_int2 = 0;
	unk_int3 = 0;
	unk_float1 = 0.0f;
	unk_float2 = 0.0f;
	for (int i = 0; i < 4; i++) {
		unk2[i] = 0;
		unk3[i] = 0;
	}
}

void
CGrid::ProcessAnyLeftJustDown()
{
	unk_int2--;
}

void
CGrid::ProcessAnyRightJustDown()
{
	unk_int2++;
}

void
CGrid::ProcessAnyUpJustDown()
{
	unk_int1--;
}

void
CGrid::ProcessAnyDownJustDown()
{
	unk_int1++;
}

void
CGrid::AllDoneMakePlayerName()
{
	// TODO
}

void
CGrid::ProcessDPadCrossJustDown()
{
	// TODO
}

void
CGrid::DisplayGrid()
{
	// TODO
}

void
CGrid::ProcessControllerInput()
{
	// TODO
}

void
CGrid::Process()
{
	ProcessControllerInput();
	DisplayGrid();
	playerName.DisplayName(2 * playerName.unk_4c);
}