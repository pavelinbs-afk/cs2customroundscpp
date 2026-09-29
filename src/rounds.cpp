#include "rounds.h"

#include <cstring>

#include <entity2/entitysystem.h>
#include <entityhandle.h>
#include <tier1/utlvector.h>

#include "customrounds.h"
#include "invert_hook.h"
#include "players.h"
#include "schema.h"
#include "sound_hook.h"

static CRRoundMode g_RoundMode = CRRoundMode::None;
static float g_flNextOneBulletTick = 0.0f;

static constexpr int kOneBulletReserve = 120;
static constexpr float kOneBulletInterval = 0.05f;
static constexpr int kMaxWeaponsPerPlayer = 16;

void Rounds_SetMode(CRRoundMode mode)
{
	if (g_RoundMode == mode)
		return;

	g_RoundMode = mode;
	SoundHook_SetMuted(mode == CRRoundMode::NoSound);
	if (mode != CRRoundMode::Invert)
		InvertHook_ResetPlayerState();
	CR_Log("round mode -> %d", static_cast<int>(mode));
}

CRRoundMode Rounds_GetMode()
{
	return g_RoundMode;
}

void Rounds_OnStartupServer()
{
	g_RoundMode = CRRoundMode::None;
	g_flNextOneBulletTick = 0.0f;
	SoundHook_SetMuted(false);
	InvertHook_ResetPlayerState();
}

static bool IsExcludedWeapon(const char* designerName)
{
	if (!designerName || !*designerName)
		return true;

	static const char* kExcluded[] = {
		"weapon_knife",
		"weapon_knife_t",
		"weapon_bayonet",
		"weapon_fists",
		"weapon_hegrenade",
		"weapon_flashbang",
		"weapon_smokegrenade",
		"weapon_molotov",
		"weapon_incgrenade",
		"weapon_decoy",
		"weapon_taser",
		"weapon_healthshot",
		"weapon_c4",
		nullptr,
	};

	for (const char** p = kExcluded; *p; ++p)
	{
		if (!strcmp(designerName, *p))
			return true;
	}

	return strncmp(designerName, "weapon_knife", 12) == 0;
}

static void* GetWeaponServices(CEntityInstance* pPawn)
{
	return Schema_Get<void*>(pPawn, "CBasePlayerPawn", "m_pWeaponServices");
}

static CEntityInstance* HandleToEntity(const CEntityHandle& h)
{
	if (!g_pGameEntitySystem)
		return nullptr;
	return g_pGameEntitySystem->GetEntityInstance(h);
}

static const char* GetDesignerName(CEntityInstance* pEnt)
{
	if (!pEnt)
		return nullptr;
	return pEnt->GetClassname();
}

static void WriteOneBulletReserve(CEntityInstance* pWeapon, void* pWeaponServices)
{
	if (!pWeapon)
		return;

	int32_t reserveOff = Schema_GetOffset("CBasePlayerWeapon", "m_pReserveAmmo");
	if (reserveOff < 0)
		return;

	// m_pReserveAmmo is int32[2] embedded in the weapon — not a pointer.
	int32_t* pReserve = reinterpret_cast<int32_t*>(
		reinterpret_cast<uint8_t*>(pWeapon) + reserveOff);

	if (pReserve[0] >= kOneBulletReserve)
		return;

	pReserve[0] = kOneBulletReserve;

	int16_t ammoType = Schema_Get<int16_t>(pWeapon, "CBasePlayerWeapon", "m_iPrimaryAmmoType", -1);
	if (ammoType < 0)
		ammoType = Schema_Get<int16_t>(pWeapon, "CBasePlayerWeapon", "m_nPrimaryAmmoType", -1);
	if (ammoType >= 0 && ammoType < 2)
		pReserve[ammoType] = kOneBulletReserve;

	if (ammoType >= 0 && pWeaponServices)
	{
		int32_t ammoOff = Schema_GetOffset("CPlayer_WeaponServices", "m_iAmmo");
		// m_iAmmo is an embedded uint16[] — not a pointer.
		if (ammoOff >= 0 && ammoType >= 0 && ammoType < 32)
		{
			auto* pAmmo = reinterpret_cast<uint16_t*>(
				reinterpret_cast<uint8_t*>(pWeaponServices) + ammoOff);
			pAmmo[ammoType] = static_cast<uint16_t>(kOneBulletReserve);
		}
	}

	Schema_NetworkStateChanged(pWeapon, reserveOff);
}

// После выстрела clip=0 — не трогаем, игрок перезаряжается вручную.
// Запас всегда подпитываем (бесконечные обоймы).
static void MaintainOneBulletWeapon(CEntityInstance* pWeapon, void* pWeaponServices, bool bAfterReload)
{
	const char* name = GetDesignerName(pWeapon);
	if (IsExcludedWeapon(name))
		return;

	int32_t clip = Schema_Get<int32_t>(pWeapon, "CBasePlayerWeapon", "m_iClip1", -1);
	if (clip < 0)
		return;

	if (bAfterReload)
	{
		if (clip != 1)
			Schema_SetNetworked<int32_t>(pWeapon, "CBasePlayerWeapon", "m_iClip1", 1);
	}
	else if (clip > 1)
	{
		// Купил полный магазин — оставляем 1 патрон до первого выстрела.
		Schema_SetNetworked<int32_t>(pWeapon, "CBasePlayerWeapon", "m_iClip1", 1);
	}

	WriteOneBulletReserve(pWeapon, pWeaponServices);
}

static void ApplyOneBulletToPlayer(int iSlot, bool bAfterReload)
{
	CEntityInstance* pPawn = GetPawnBySlot(iSlot);
	if (!pPawn || !IsPlayerAlive(iSlot))
		return;

	void* pWs = GetWeaponServices(pPawn);
	if (!pWs)
		return;

	int32_t weaponsOff = Schema_GetOffset("CPlayer_WeaponServices", "m_hMyWeapons");
	if (weaponsOff < 0)
		return;

	auto* pWeapons = reinterpret_cast<CUtlVector<CEntityHandle>*>(
		reinterpret_cast<uint8_t*>(pWs) + weaponsOff);
	if (!pWeapons)
		return;

	const int weaponCount = pWeapons->Count();
	if (weaponCount <= 0)
		return;

	const int limit = weaponCount < kMaxWeaponsPerPlayer ? weaponCount : kMaxWeaponsPerPlayer;
	for (int i = 0; i < limit; i++)
	{
		CEntityInstance* pWeapon = HandleToEntity((*pWeapons)[i]);
		if (pWeapon)
			MaintainOneBulletWeapon(pWeapon, pWs, bAfterReload);
	}
}

static void TickRoundMode()
{
	if (g_RoundMode == CRRoundMode::None)
		return;

	CGlobalVars* gv = GetGlobals();
	if (!gv)
		return;

	if (g_RoundMode != CRRoundMode::OneBullet)
		return;

	if (gv->curtime < g_flNextOneBulletTick)
		return;
	g_flNextOneBulletTick = gv->curtime + kOneBulletInterval;

	for (int i = 0; i < CR_MAXPLAYERS; i++)
	{
		if (GetPlayerTeamNum(i) <= 1)
			continue;
		ApplyOneBulletToPlayer(i, false);
	}
}

void Rounds_OnGameFrame()
{
	TickRoundMode();
}

void Rounds_OnWeaponFire(int iSlot)
{
	if (g_RoundMode != CRRoundMode::OneBullet)
		return;

	// Только подпитываем запас; clip=0 после выстрела не трогаем.
	ApplyOneBulletToPlayer(iSlot, false);
}

void Rounds_OnWeaponReload(int iSlot)
{
	if (g_RoundMode != CRRoundMode::OneBullet)
		return;

	// После перезарядки — снова 1 патрон в обойме.
	ApplyOneBulletToPlayer(iSlot, true);
}
