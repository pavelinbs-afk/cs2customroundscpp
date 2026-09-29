#pragma once

#include <cstdint>

#define CR_MAXPLAYERS 64

class CEntityInstance;

CEntityInstance* GetControllerBySlot(int iSlot);
CEntityInstance* GetPawnBySlot(int iSlot);
int GetPlayerTeamNum(int iSlot);
bool IsPlayerAlive(int iSlot);
