#include "rounds.h"

#include <cstdlib>
#include <cstring>

#include <entity2/entitysystem.h>
#include <entityhandle.h>
#include <tier1/utlvector.h>

#include "customrounds.h"
#include "entity_utils.h"
#include "invert_hook.h"
#include "players.h"
#include "schema.h"
#include "sound_hook.h"

static CRRoundMode g_RoundMode = CRRoundMode::None;
static float g_flNextOneBulletTick = 0.0f;
static float g_flNextGrenadeShot[CR_MAXPLAYERS] = {};
static int32_t g_nLastClip[CR_MAXPLAYERS];
static uint32_t g_nLastWeaponIndex[CR_MAXPLAYERS];
static int g_nGrenadeBag[4] = {0, 1, 2, 3};
static int g_nGrenadeBagLeft = 0;

static constexpr int kOneBulletReserve = 120;
static constexpr float kOneBulletInterval = 0.05f;
static constexpr int kMaxWeaponsPerPlayer = 16;
// Only suppress duplicate weapon_fire in the same tick window — not weapon cycle time.
static constexpr float kGrenadeShotDebounce = 0.05f;
static constexpr float kGrenadeThrowSpeed = 900.f;

static void ResetGrenadeShotPlayerState()
{
	std::memset(g_flNextGrenadeShot, 0, sizeof(g_flNextGrenadeShot));
	for (int i = 0; i < CR_MAXPLAYERS; ++i)
	{
		g_nLastClip[i] = -1;
		g_nLastWeaponIndex[i] = 0;
	}
	g_nGrenadeBagLeft = 0;
}

void Rounds_SetMode(CRRoundMode mode)
{
	if (g_RoundMode == mode)
		return;

	const CRRoundMode prev = g_RoundMode;
	g_RoundMode = mode;
	SoundHook_SetMuted(mode == CRRoundMode::NoSound);
	if (mode != CRRoundMode::Invert)
		InvertHook_ResetPlayerState();
	if (prev == CRRoundMode::GrenadeShot || mode == CRRoundMode::GrenadeShot)
		ResetGrenadeShotPlayerState();
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
	ResetGrenadeShotPlayerState();
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

static CEntityInstance* GetActiveWeapon(CEntityInstance* pPawn)
{
	void* pWs = GetWeaponServices(pPawn);
	if (!pWs)
		return nullptr;

	CEntityHandle hActive = Schema_Get<CEntityHandle>(pWs, "CPlayer_WeaponServices", "m_hActiveWeapon");
	return HandleToEntity(hActive);
}

static CRGrenadeType PickRandomGrenadeType()
{
	// Fair bag over types that have a resolved native Create factory.
	if (g_nGrenadeBagLeft <= 0)
	{
		static constexpr CRGrenadeType kAll[] = {
			CRGrenadeType::HE, CRGrenadeType::Molotov, CRGrenadeType::Decoy, CRGrenadeType::Flash
		};
		int n = 0;
		for (CRGrenadeType t : kAll)
		{
			if (EntityUtils_HasFactoryForType(t))
				g_nGrenadeBag[n++] = static_cast<int>(t);
		}
		if (n <= 0)
			return CRGrenadeType::HE;

		for (int i = n - 1; i > 0; --i)
		{
			const int j = static_cast<unsigned>(std::rand()) % (i + 1);
			const int tmp = g_nGrenadeBag[i];
			g_nGrenadeBag[i] = g_nGrenadeBag[j];
			g_nGrenadeBag[j] = tmp;
		}
		g_nGrenadeBagLeft = n;
	}

	return static_cast<CRGrenadeType>(g_nGrenadeBag[--g_nGrenadeBagLeft]);
}

static bool IsEmptyDryFire(int iSlot, CEntityInstance* pWeapon)
{
	if (iSlot < 0 || iSlot >= CR_MAXPLAYERS || !pWeapon)
		return true;

	const int32_t clip = Schema_Get<int32_t>(pWeapon, "CBasePlayerWeapon", "m_iClip1", -1);
	if (clip < 0)
		return true;

	const uint32_t weaponIndex = pWeapon->GetRefEHandle().GetEntryIndex();
	const int32_t prevClip = g_nLastClip[iSlot];
	const uint32_t prevWeapon = g_nLastWeaponIndex[iSlot];

	g_nLastClip[iSlot] = clip;
	g_nLastWeaponIndex[iSlot] = weaponIndex;

	// True empty click: clip stayed at 0. Do NOT require clip decrease —
	// VIP / refill plugins often restore m_iClip1 before weapon_fire runs.
	if (prevWeapon == weaponIndex && prevClip == 0 && clip == 0)
		return true;

	return false;
}

static void RememberClipAfterReload(int iSlot, CEntityInstance* pWeapon)
{
	if (iSlot < 0 || iSlot >= CR_MAXPLAYERS || !pWeapon)
		return;
	const int32_t clip = Schema_Get<int32_t>(pWeapon, "CBasePlayerWeapon", "m_iClip1", -1);
	if (clip < 0)
		return;
	g_nLastClip[iSlot] = clip;
	g_nLastWeaponIndex[iSlot] = pWeapon->GetRefEHandle().GetEntryIndex();
}

static void SpawnGrenadeShotForPlayer(int iSlot, const char* weaponName)
{
	CEntityInstance* pPawn = GetPawnBySlot(iSlot);
	if (!pPawn || !IsPlayerAlive(iSlot))
		return;

	CEntityInstance* pWeapon = GetActiveWeapon(pPawn);
	const char* activeName = GetDesignerName(pWeapon);
	if (!pWeapon || IsExcludedWeapon(activeName))
		return;

	// Event weapon name (without "weapon_" prefix sometimes) — reject mismatches / empty.
	if (weaponName && *weaponName)
	{
		if (!strcmp(weaponName, "knife") || !strncmp(weaponName, "knife_", 6))
			return;
		if (strstr(weaponName, "hegrenade") || strstr(weaponName, "flashbang")
			|| strstr(weaponName, "smokegrenade") || strstr(weaponName, "molotov")
			|| strstr(weaponName, "incgrenade") || strstr(weaponName, "decoy")
			|| strstr(weaponName, "taser") || strstr(weaponName, "c4"))
			return;
	}

	if (IsEmptyDryFire(iSlot, pWeapon))
		return;

	CGlobalVars* gv = GetGlobals();
	if (!gv)
		return;

	if (iSlot < 0 || iSlot >= CR_MAXPLAYERS)
		return;
	// Debounce duplicate events only — must stay below fastest weapon cycle (~0.08–0.1s).
	if (gv->curtime < g_flNextGrenadeShot[iSlot])
		return;
	g_flNextGrenadeShot[iSlot] = gv->curtime + kGrenadeShotDebounce;

	if (!EntityUtils_HasGrenadeFactories())
	{
		EntityUtils_RetryGrenadeFactories();
		if (!EntityUtils_HasGrenadeFactories())
		{
			CR_Log("GrenadeShot: entity spawn helpers not ready");
			return;
		}
	}

	CRVec3 origin{};
	CRVec3 viewOff{};
	CRQAngle eye{};
	if (!Entity_GetAbsOrigin(pPawn, origin))
		return;
	Entity_GetViewOffset(pPawn, viewOff);
	if (!Entity_GetEyeAngles(pPawn, eye))
		return;

	CRVec3 eyePos = CR_VecAdd(origin, viewOff);
	CRVec3 forward{};
	CR_AngleVectors(eye, forward);
	eyePos = CR_VecAdd(eyePos, CR_VecScale(forward, 16.f));
	CRVec3 velocity = CR_VecScale(forward, kGrenadeThrowSpeed);

	const int team = Schema_Get<uint8_t>(pPawn, "CBaseEntity", "m_iTeamNum");
	const CRGrenadeType type = PickRandomGrenadeType();

	CEntityInstance* pNade = Entity_CreateGrenadeProjectile(type, eyePos, eye, velocity, pPawn, team);
	if (!pNade)
		CR_Log("GrenadeShot: spawn failed (type=%d)", static_cast<int>(type));
}

void Rounds_OnWeaponFire(int iSlot)
{
	Rounds_OnWeaponFireEvent(iSlot, nullptr);
}

void Rounds_OnWeaponFireEvent(int iSlot, const char* weaponName)
{
	if (g_RoundMode == CRRoundMode::GrenadeShot)
	{
		SpawnGrenadeShotForPlayer(iSlot, weaponName);
		return;
	}

	if (g_RoundMode != CRRoundMode::OneBullet)
		return;

	ApplyOneBulletToPlayer(iSlot, false);
}

void Rounds_OnWeaponReload(int iSlot)
{
	if (g_RoundMode == CRRoundMode::GrenadeShot)
	{
		CEntityInstance* pPawn = GetPawnBySlot(iSlot);
		if (pPawn)
			RememberClipAfterReload(iSlot, GetActiveWeapon(pPawn));
		return;
	}

	if (g_RoundMode != CRRoundMode::OneBullet)
		return;

	ApplyOneBulletToPlayer(iSlot, true);
}
