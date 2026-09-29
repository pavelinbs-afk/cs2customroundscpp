#include "players.h"

#include "customrounds.h"
#include "schema.h"

CEntityInstance* GetControllerBySlot(int iSlot)
{
	if (!g_pGameEntitySystem || iSlot < 0 || iSlot >= CR_MAXPLAYERS)
		return nullptr;
	return g_pGameEntitySystem->GetEntityInstance(CEntityIndex(iSlot + 1));
}

CEntityInstance* GetPawnBySlot(int iSlot)
{
	CEntityInstance* pController = GetControllerBySlot(iSlot);
	if (!pController || !g_pGameEntitySystem)
		return nullptr;

	CEntityHandle hPawn = Schema_Get<CEntityHandle>(pController, "CBasePlayerController", "m_hPawn");
	return g_pGameEntitySystem->GetEntityInstance(hPawn);
}

int GetPlayerTeamNum(int iSlot)
{
	CEntityInstance* pController = GetControllerBySlot(iSlot);
	if (!pController)
		return 0;
	return Schema_Get<uint8_t>(pController, "CBaseEntity", "m_iTeamNum");
}

bool IsPlayerAlive(int iSlot)
{
	CEntityInstance* pPawn = GetPawnBySlot(iSlot);
	if (!pPawn)
		return false;
	return Schema_Get<int32_t>(pPawn, "CBaseEntity", "m_iHealth") > 0;
}
